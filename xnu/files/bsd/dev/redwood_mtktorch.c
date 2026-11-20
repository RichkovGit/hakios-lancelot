/*
 * The flashlight of the Redmi 9 (lancelot): the LED191 flash module, two GPIOs of the MT6768 pin
 * controller -- EN=GPIO152, SEL=GPIO153 (device tree flashlights_led191; Xiaomi's lancelot overlay).
 * No I2C/PMIC writes: SEL picks the torch (low-current) position, EN switches the LED. The iOS side
 * is the same as the Qualcomm torch's: libredwood-media.dylib hands FigFlashlight a level written to
 * /dev/redwood-torch (0..1000, 0 off). This is the MTK GPIO backend; the Qualcomm redwood_torch.c
 * (SPMI PM8350C) does not apply here. Own boot-arg (torch= maps Qualcomm addresses):
 *
 *   mtktorch=1  a look, reading only: the pins' mode, direction and output as they are
 *   mtktorch=2  ... and the light on once in the torch position for mtktorch_secs (default 2), then off
 *   mtktorch=3  ... the light for iOS until the run ends (/dev/redwood-torch)
 *
 * Limits: the torch position only (SEL held there), on for mtktorch_max_s at a time (default 30,
 * never more than 120), battery at most 45 C; off at the run's end and on a panic's way out.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void redwood_keep(const char *format, ...) __printflike(1, 2);
extern int redwood_battery_temperature(void);

#define GPIO_PHYS       0x10005000
#define GPIO_DIR_SET(b) (0x004 + 0x10 * (b))
#define GPIO_DIR_CLR(b) (0x008 + 0x10 * (b))
#define GPIO_DOUT_SET(b) (0x104 + 0x10 * (b))
#define GPIO_DOUT_CLR(b) (0x108 + 0x10 * (b))
#define GPIO_DIN(b)     (0x200 + 0x10 * (b))
#define GPIO_MODE(p)    (0x300 + 0x10 * ((p) / 8))

#define PIN_EN          152
#define PIN_SEL         153
#define BANK_EN         (PIN_EN / 32)
#define BIT_EN          (1u << (PIN_EN % 32))
#define BIT_SEL         (1u << (PIN_SEL % 32))
#define MODE_SHIFT_EN   ((PIN_EN % 8) * 4)
#define MODE_SHIFT_SEL  ((PIN_SEL % 8) * 4)

static vm_offset_t gpio;
static uint32_t level;                  /* the boot-arg mtktorch */
static volatile uint32_t wanted, shown;
static uint32_t max_secs = 30;
static bool lit, leaving;

static inline uint32_t rd(uint32_t at) { return *(volatile uint32_t *)(gpio + at); }
static inline void wr(uint32_t at, uint32_t v) { *(volatile uint32_t *)(gpio + at) = v; }

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
wait_ms(uint32_t ms)
{
	uint64_t until = now_ms() + ms;

	while (now_ms() < until) {
		delay(1000);
	}
}

/* SEL to the torch position (low current), EN on/off. The pins are set to GPIO mode and output once. */
static void
pins_setup(void)
{
	uint32_t mode = rd(GPIO_MODE(PIN_EN));

	mode &= ~((0xfu << MODE_SHIFT_EN) | (0xfu << MODE_SHIFT_SEL));   /* function 0 = GPIO */
	wr(GPIO_MODE(PIN_EN), mode);
	wr(GPIO_DIR_SET(BANK_EN), BIT_EN | BIT_SEL);
	wr(GPIO_DOUT_CLR(BANK_EN), BIT_SEL);                            /* SEL low = torch position */
	wr(GPIO_DOUT_CLR(BANK_EN), BIT_EN);                             /* EN low = dark */
}

static void
en(bool on)
{
	if (on) {
		wr(GPIO_DOUT_SET(BANK_EN), BIT_EN);
	} else {
		wr(GPIO_DOUT_CLR(BANK_EN), BIT_EN);
	}
	lit = on;
}

static const char *
trouble(void)
{
	int t = redwood_battery_temperature();

	if (t > 45) {
		return "the battery is over 45 C";
	}
	return NULL;
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		wanted = value > 1000 ? 1000 : value;
	}
	return 0;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
serve(void)
{
	uint32_t applied = 0;
	uint64_t since = 0, last_look = 0;
	bool waiting_for_off = false;
	int major = cdevsw_add(-1, &torch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-torch") == NULL) {
		redwood_keep("mtktorch: /dev/redwood-torch cannot be made");
		return;
	}
	redwood_keep("mtktorch: the light is iOS's (/dev/redwood-torch), at most %u s at a time", max_secs);
	while (!leaving) {
		uint32_t want = wanted;
		uint64_t now = now_ms();

		if (want == 0) {
			waiting_for_off = false;
		}
		if (waiting_for_off) {
			want = 0;
		}
		if (want != applied) {
			en(want != 0);
			if (applied == 0 && want != 0) {
				since = now;
			}
			redwood_keep("mtktorch: %s (iOS asked %u of 1000)", want ? "ON" : "off", wanted);
			applied = want;
			shown = want;
		}
		if (applied != 0 && now - since >= max_secs * 1000ull) {
			redwood_keep("mtktorch: on for %u s, the limit: off until iOS asks again", max_secs);
			waiting_for_off = true;
			en(false);
			applied = 0;
			shown = 0;
			continue;
		}
		if (applied != 0 && now - last_look >= 100) {
			const char *why;

			last_look = now;
			if ((why = trouble()) != NULL) {
				redwood_keep("mtktorch: OFF: %s", why);
				en(false);
				break;
			}
		}
		wait_ms(20);
	}
	en(false);
	shown = 0;
}

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	gpio = ml_io_map(GPIO_PHYS, 0x1000);
	redwood_keep("mtktorch: level %u; EN(GPIO%d) mode %#x dir %#x dout %#x, SEL(GPIO%d) dout %#x", level, PIN_EN,
	    rd(GPIO_MODE(PIN_EN)), rd(GPIO_DIR_SET(BANK_EN)) & (BIT_EN | BIT_SEL), rd(GPIO_DIN(BANK_EN)) & (BIT_EN | BIT_SEL),
	    PIN_SEL, rd(GPIO_DIN(BANK_EN)) & BIT_SEL);
	if (level < 2) {
		goto out;
	}
	pins_setup();
	PE_parse_boot_argn("mtktorch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 1 ? 1 : max_secs > 120 ? 120 : max_secs;
	if (level == 2) {
		uint32_t secs = 2;
		uint64_t until;

		PE_parse_boot_argn("mtktorch_secs", &secs, sizeof(secs));
		secs = secs < 1 ? 1 : secs > max_secs ? max_secs : secs;
		redwood_keep("mtktorch: the light ON in the torch position for %u s", secs);
		en(true);
		for (until = now_ms() + secs * 1000ull; now_ms() < until && trouble() == NULL;) {
			wait_ms(100);
		}
		en(false);
		redwood_keep("mtktorch: off again after %u s", secs);
	} else {
		serve();
	}
out:
	redwood_keep("mtktorch: done");
	thread_terminate(current_thread());
}

void
redwood_mtktorch_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mtktorch", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 3 ? 3 : level;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
redwood_mtktorch_quiesce(void)
{
	if (level < 2 || gpio == 0) {
		return;
	}
	leaving = true;
	if (lit) {
		en(false);
		redwood_keep("mtktorch: at the run's end: the light switched off");
	}
}
