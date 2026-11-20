/*
 * The audio of the Redmi 9 (lancelot): MT6768 AFE + MT6358 codec, the speaker an analog Awinic
 * AW87519 on MT6358 LINEOUT L (DL1 -> ADDA DAC -> LOL -> AW87519 -> speaker), the microphone on
 * MT6358 ADC1/AIN0. See docs/lancelot-audio-backend.md for the confirmed route and register plan.
 * The Qualcomm LPASS/APR path (redwood_adsp.c / redwood_mic.c) does not apply here.
 *
 *   mtkafe=1  a look, reading only: the audio clocks, the AFE, the MT6358 audio registers (PWRAP),
 *             the amplifier's reset/I2C pins -- nothing enabled, nothing written
 *
 * Higher stages (clocks+digital, analog+amp tone, microphone) come later; this is the read-only
 * foundation that confirms the hardware state before anything is driven.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void redwood_keep(const char *format, ...) __printflike(1, 2);
extern void redwood_pwrap_lock(void);
extern void redwood_pwrap_unlock(void);

#define TOPCKGEN        0x10000000ULL
#define APMIXED         0x1000c000ULL
#define INFRACFG        0x1020e000ULL
#define AFE             0x11220000ULL
#define PWRAP           0x1000d000ULL
#define GPIO            0x10005000ULL

#define WACS2_CMD       0xc20
#define WACS2_RDATA     0xc24
#define WACS2_VLDCLR    0xc28
#define FSM(r)          (((r) >> 16) & 7u)

static volatile uint32_t *pwrap;
static vm_offset_t gpio_base;
static uint32_t level;

static inline uint32_t rd(vm_offset_t base, uint32_t at) { return *(volatile uint32_t *)(base + at); }
static inline void wr(vm_offset_t base, uint32_t at, uint32_t v) { *(volatile uint32_t *)(base + at) = v; }
/* ---- I2C7 by hand on the MT6768 pin controller (GPIO175 SCL, GPIO176 SDA) ----------------------
 * Open-drain: a line is pulled low (pin output, drives 0) or let go (pin input, the bus pull-up
 * raises it). The pins are taken from their I2C function to GPIO for one transaction, given back. */
#define PIN_SCL 175
#define PIN_SDA 176
#define DIR_SET(b)  (0x004 + 0x10 * (b))
#define DIR_CLR(b)  (0x008 + 0x10 * (b))
#define DOUT_CLR(b) (0x108 + 0x10 * (b))
#define DIN(b)      (0x200 + 0x10 * (b))
#define MODE(p)     (0x300 + 0x10 * ((p) / 8))

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
line(unsigned pin, bool high)
{
	uint32_t bank = pin / 32, bit = 1u << (pin % 32), mode = rd(gpio_base, MODE(pin));

	mode &= ~(0xfu << ((pin % 8) * 4));            /* function 0 = GPIO */
	wr(gpio_base, MODE(pin), mode);
	if (high) {
		wr(gpio_base, DIR_CLR(bank), bit);     /* input: the pull-up raises it */
	} else {
		wr(gpio_base, DOUT_CLR(bank), bit);
		wr(gpio_base, DIR_SET(bank), bit);     /* output, drives 0 */
	}
}

static bool
pin_level(unsigned pin)
{
	return rd(gpio_base, DIN(pin / 32)) & (1u << (pin % 32));
}

static void i2c_half(void) { spin_us(5); }

static void
i2c_start(void)
{
	line(PIN_SDA, true); line(PIN_SCL, true); i2c_half();
	line(PIN_SDA, false); i2c_half();
	line(PIN_SCL, false); i2c_half();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false); i2c_half();
	line(PIN_SCL, true); i2c_half();
	line(PIN_SDA, true); i2c_half();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out); i2c_half();
	line(PIN_SCL, true);
	for (unsigned s = 0; !pin_level(PIN_SCL) && s < 100; s++) {
		i2c_half();
	}
	i2c_half();
	in = pin_level(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

/* the AW87519's chip id (reg 0x00, expected 0x59), read over I2C7 (7-bit address 0x58) */
static void
amp_id(void)
{
	uint32_t scl_mode = rd(gpio_base, MODE(PIN_SCL)), sda_mode = rd(gpio_base, MODE(PIN_SDA));
	uint32_t scl_dir = rd(gpio_base, DIR_SET(PIN_SCL / 32)), sda_dir = rd(gpio_base, DIR_SET(PIN_SDA / 32));
	uint8_t id = 0xff;
	bool ok = false;

	line(PIN_SDA, true); line(PIN_SCL, true); i2c_half();
	if (!pin_level(PIN_SDA) || !pin_level(PIN_SCL)) {
		redwood_keep("mtkafe: I2C7 not at rest (SDA %d SCL %d): amp id not read", pin_level(PIN_SDA), pin_level(PIN_SCL));
		goto give_back;
	}
	i2c_start();
	if (i2c_write(0x58 << 1) && i2c_write(0x00)) {
		i2c_start();
		if (i2c_write((0x58 << 1) | 1)) {
			id = i2c_read(false);
			ok = true;
		}
	}
	i2c_stop();
	if (ok) {
		redwood_keep("mtkafe: AW87519 chip id (reg 0x00) = %#04x%s", id, id == 0x59 ? " (as expected)" : " (NOT 0x59)");
	} else {
		redwood_keep("mtkafe: AW87519 did not acknowledge on I2C7");
	}
give_back:
	wr(gpio_base, MODE(PIN_SCL), scl_mode);
	wr(gpio_base, MODE(PIN_SDA), sda_mode);
	wr(gpio_base, DIR_SET(PIN_SCL / 32), scl_dir);
	wr(gpio_base, DIR_SET(PIN_SDA / 32), sda_dir);
}


static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (FSM(pwrap[WACS2_RDATA / 4]) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint16_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[WACS2_CMD / 4] = (reg >> 1) << 16;
		if (pwrap_wait(6)) {
			*value = (uint16_t)pwrap[WACS2_RDATA / 4];
			pwrap[WACS2_VLDCLR / 4] = 1;
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

static void
say_pmic(const char *name, uint32_t reg)
{
	uint16_t v;

	if (pmic_read(reg, &v)) {
		redwood_keep("mtkafe: MT6358 %s (%#x) = %#06x", name, reg, v);
	} else {
		redwood_keep("mtkafe: MT6358 %s (%#x): read failed", name, reg);
	}
}


/* ---- stage 2: the audio clocks (mtkafe=3), read back at each step ------------------------------
 * The audio muxes (audio_sel, aud_intbus, aud_1, aud_engen1) are clr/set immediate-effect
 * (INVALID_UPDATE_REG in clk-mt6768.c), so no CLK_CFG_UPDATE bit is needed. Parents confirmed:
 * audio_sel 0=clk26m; aud_intbus 1=syspll1_d4; aud_1 1=apll1_ck; aud_engen1 3=apll1_d8. */
static void
clock_enable(vm_offset_t top, vm_offset_t apl, vm_offset_t infra, vm_offset_t afe)
{
	uint32_t v;

	/* infra: bring the audio bus and its 26M out of clock gating (SET registers) */
	wr(infra, 0x88, 1u << 25);
	wr(infra, 0xa4, 1u << 4);
	redwood_keep("mtkafe: infra CG_1 %#x CG_2 %#x", rd(infra, 0x88), rd(infra, 0xa4));
	/* top muxes: clear then set, immediate */
	wr(top, 0x78, 3u << 24); wr(top, 0x74, 0u << 24);          /* audio_sel = clk26m */
	wr(top, 0x88, 3u << 0);  wr(top, 0x84, 1u << 0);           /* aud_intbus = syspll1_d4 */
	wr(top, 0x88, 1u << 8);  wr(top, 0x84, 1u << 8);           /* aud_1 = apll1_ck */
	wr(top, 0x88, 3u << 16); wr(top, 0x84, 3u << 16);          /* aud_engen1 = apll1_d8 */
	v = rd(top, 0x320); wr(top, 0x320, (v & ~(0xfu << 24)) | (7u << 24));  /* CLK_AUDDIV_0 ÷8 */
	redwood_keep("mtkafe: top CLK_CFG_3 %#x CLK_CFG_4 %#x CLK_AUDDIV_0 %#x", rd(top, 0x70), rd(top, 0x80), rd(top, 0x320));
	/* APLL1 = 196.608 MHz for the 48k family */
	v = rd(apl, 0x0c); wr(apl, 0x0c, v | 1u);                  /* AP_PLL_CON3 tuner on */
	wr(apl, 0x310, 0x078FD526);                                /* APLL1 PCW */
	v = rd(apl, 0x30c); wr(apl, 0x30c, v & ~(7u << 24));       /* postdiv 0 */
	v = rd(apl, 0x318); wr(apl, 0x318, v | 1u);                /* APLL1 power on */
	redwood_keep("mtkafe: APLL1 PCW %#x CON1 %#x PWR %#x", rd(apl, 0x310), rd(apl, 0x30c), rd(apl, 0x318));
	/* audiosyscon gates (reg 0, RMW): afe, 24m, tuner, adc, dac, dac_predis, tml */
	v = rd(afe, 0x000);
	wr(afe, 0x000, v | (1u << 2) | (1u << 9) | (1u << 19) | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27));
	redwood_keep("mtkafe: audiosyscon reg0 %#x -> %#x", v, rd(afe, 0x000));
	/* AFE tuner + 24M enable */
	wr(afe, 0x3f0, 0x434 | 1u);
	v = rd(afe, 0xdd0); wr(afe, 0xdd0, v | (1u << 1));
	redwood_keep("mtkafe: AFE APLL_TUNER %#x HD_ENGEN %#x", rd(afe, 0x3f0), rd(afe, 0xdd0));
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t top = ml_io_map(TOPCKGEN, 0x1000), apl = ml_io_map(APMIXED, 0x1000);
	vm_offset_t infra = ml_io_map(INFRACFG, 0x1000), afe = ml_io_map(AFE, 0x1000), gpio = ml_io_map(GPIO, 0x1000);
	uint32_t bank;

	pwrap = (volatile uint32_t *)ml_io_map(PWRAP, 0x1000);
	gpio_base = gpio;
	redwood_keep("mtkafe: level %u (%s)", level, level >= 3 ? "clock enable" : "read-only probe");
	if (level >= 3) {
		clock_enable(top, apl, infra, afe);
	}
	redwood_keep("mtkafe: topckgen CLK_CFG_3 %#x CLK_CFG_4 %#x CLK_AUDDIV_0 %#x", rd(top, 0x70), rd(top, 0x80), rd(top, 0x320));
	redwood_keep("mtkafe: apmixed APLL1 CON2(PCW) %#x CON1(postdiv) %#x PWR %#x", rd(apl, 0x310), rd(apl, 0x30c), rd(apl, 0x318));
	redwood_keep("mtkafe: infracfg CG_1 %#x CG_2 %#x", rd(infra, 0x88), rd(infra, 0xa4));
	redwood_keep("mtkafe: audiosyscon/AFE reg0 %#x DAC_CON0 %#x DAC_CON1 %#x AUD_PAD_TOP %#x APLL_TUNER %#x HD_ENGEN %#x",
	    rd(afe, 0x000), rd(afe, 0x010), rd(afe, 0x014), rd(afe, 0xe40), rd(afe, 0x3f0), rd(afe, 0xdd0));
	say_pmic("AUD_TOP_ID", 0x2200);
	say_pmic("AUDDEC_ANA_CON13", 0x2422);
	say_pmic("AUDENC_ANA_CON9 (MICBIAS0)", 0x239a);
	say_pmic("AUDENC_ANA_CON0 (ADC1 PGA)", 0x2388);
	say_pmic("PMIC_AUDIO_TOP_CON0", 0x2292);
	/* the amplifier's pins: reset GPIO22, I2C7 SCL GPIO175 / SDA GPIO176 (bank = pin/32) */
	bank = 22 / 32;
	redwood_keep("mtkafe: GPIO22 (amp reset) mode %#x dir %#x din %#x", rd(gpio, 0x300 + 0x10 * (22 / 8)),
	    rd(gpio, 0x004 + 0x10 * bank) & (1u << (22 % 32)), rd(gpio, 0x200 + 0x10 * bank) & (1u << (22 % 32)));
	bank = 175 / 32;
	redwood_keep("mtkafe: GPIO175/176 (I2C7) mode %#x dir %#x din %#x", rd(gpio, 0x300 + 0x10 * (175 / 8)),
	    rd(gpio, 0x004 + 0x10 * bank) & ((1u << (175 % 32)) | (1u << (176 % 32))),
	    rd(gpio, 0x200 + 0x10 * bank) & ((1u << (175 % 32)) | (1u << (176 % 32))));
	if (level >= 2) {
		amp_id();
	}
	redwood_keep("mtkafe: probe done (nothing enabled)");
	thread_terminate(current_thread());
}

void
redwood_mtkafe_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mtkafe", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
redwood_mtkafe_quiesce(void)
{
	/* read-only probe: nothing to turn off */
}
