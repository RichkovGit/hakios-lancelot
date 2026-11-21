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
#include <sys/redwood_audio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void redwood_keep(const char *format, ...) __printflike(1, 2);
extern void redwood_pwrap_lock(void);
extern void redwood_pwrap_unlock(void);
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void delay_for_interval(uint32_t interval, uint32_t scale);
#include <mach/vm_param.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
extern vm_map_t kernel_map;

#define TOPCKGEN        0x10000000ULL
#define APMIXED         0x1000c000ULL
#define INFRACFG        0x10001000ULL   /* same base as SCP/mtkcam; 0x1020e000 was wrong (read 0) */
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
static uint32_t tone_secs = 6;
static uint32_t lol_gain = 0x1f;   /* LOL PGA index: 0x1f=-40 dB (safe), 0x12=18~-10 dB; mtkafe_gain=N */

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
	/* infra audio bus (infracfg_ao@10001000): the axi gate CLK_IFR_AUDIO bit25 is active-low
	 * in effect -- default (0) already has the AFE register + CONN interconnect bus ON; setting
	 * bit25 gates it OFF (AFE reg0 and CONN read 0). Isolation test proved this. Leave both
	 * gates at default so registers and the DL1->ADDA_DL route stay accessible. */
	redwood_keep("mtkafe: infra STA1(0x94) %#x STA2(0xac) %#x (left at default, bus on)", rd(infra, 0x94), rd(infra, 0xac));
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
	v = rd(apl, 0x318); wr(apl, 0x318, v | 1u);                /* APLL1 power on (CON4 bit0) */
	v = rd(apl, 0x318); wr(apl, 0x318, v & ~2u);               /* ISO off (CON4 bit1) */
	spin_us(1);
	v = rd(apl, 0x308); wr(apl, 0x308, v | 1u);                /* PLL_EN (CON0 bit0) */
	spin_us(20);
	redwood_keep("mtkafe: APLL1 PCW %#x CON1 %#x CON0 %#x PWR %#x", rd(apl, 0x310), rd(apl, 0x30c), rd(apl, 0x308), rd(apl, 0x318));
	/* audiosyscon gates (reg 0, RMW): afe, 24m, tuner, adc, dac, dac_predis, tml */
	v = rd(afe, 0x000);
	/* gates may be active-low (like the infra axi gate): default reg0 already has the audio
	 * clocks on; setting the bits gated them OFF (IRQ0_CNT_MON stayed 0). Keep default. */
	redwood_keep("mtkafe: audiosyscon reg0 %#x (kept default)", rd(afe, 0x000));
	/* AFE tuner + 24M enable */
	wr(afe, 0x3f0, 0x434 | 1u);
	v = rd(afe, 0xdd0); wr(afe, 0xdd0, v | (1u << 1));
	redwood_keep("mtkafe: AFE APLL_TUNER %#x HD_ENGEN %#x", rd(afe, 0x3f0), rd(afe, 0xdd0));
}

/* ---- stage 4/5: playback. DL1 -> ADDA DAC -> MT6358 LOL -> AW87519 -> speaker ----------------
 * Register sequences from docs/lancelot-audio-backend.md (vendor mt6768-mt6358 machine +
 * aw87519_audio_kspk). Every PMIC write is read back; mismatches are kept for the host. The
 * LOL starts at -40 dB (g=0x1f) and the tone is low amplitude: nothing loud until verified. */
static vm_offset_t g_afe;
static vm_offset_t g_top, g_apl, g_infra;
static uint64_t ring_phys;
static vm_offset_t ring_va, ring_wc;
static uint32_t ring_frames = 3840;            /* 8 periods x 480 frames (10 ms) at 48k */
static unsigned dl1_running;
static uint32_t wr_frame;
static uint64_t sink_taken, sink_dropped;
static const int16_t tone48[48] = {0,392,776,1148,1500,1826,2121,2380,2598,2772,2898,2974,3000,2974,2898,2772,2598,2380,2121,1826,1500,1148,776,392,0,-392,-776,-1148,-1500,-1826,-2121,-2380,-2598,-2772,-2898,-2974,-3000,-2974,-2898,-2772,-2598,-2380,-2121,-1826,-1500,-1148,-776,-392};

static bool
pmic_write(uint32_t reg, uint16_t value)
{
	bool ok = false;
	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[WACS2_CMD / 4] = (1u << 31) | ((reg >> 1) << 16) | value;
		ok = pwrap_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* write a plain value, read it back, keep any mismatch (diagnosis, not a hard stop) */
static void
pw(uint32_t reg, uint16_t val)
{
	uint16_t got = 0;
	if (!pmic_write(reg, val)) { redwood_keep("mtkafe: PMIC write %#x FAILED", reg); return; }
	if (pmic_read(reg, &got) && got != val)
		redwood_keep("mtkafe: PMIC %#x wrote %#06x read %#06x (mismatch)", reg, val, got);
}

/* read-modify-write: clear mask, set want, verify (got & mask) == want */
static void
pwm(uint32_t reg, uint16_t mask, uint16_t want)
{
	uint16_t cur = 0, got = 0;
	if (!pmic_read(reg, &cur)) { redwood_keep("mtkafe: PMIC %#x read failed (rmw)", reg); return; }
	uint16_t val = (uint16_t)((cur & ~mask) | want);
	if (!pmic_write(reg, val)) { redwood_keep("mtkafe: PMIC write %#x FAILED (rmw)", reg); return; }
	if (pmic_read(reg, &got) && (got & mask) != want)
		redwood_keep("mtkafe: PMIC %#x mask %#x want %#x got %#x (mismatch)", reg, mask, want, got & mask);
}

/* AW87519 over I2C7: write/read one 8-bit register (7-bit address 0x58) */
static bool
amp_write(uint8_t reg, uint8_t val)
{
	bool ok;
	i2c_start();
	ok = i2c_write(0x58 << 1) && i2c_write(reg) && i2c_write(val);
	i2c_stop();
	return ok;
}

static uint8_t
amp_read(uint8_t reg)
{
	uint8_t id = 0xff;
	i2c_start();
	if (i2c_write(0x58 << 1) && i2c_write(reg)) {
		i2c_start();
		if (i2c_write((0x58 << 1) | 1)) id = i2c_read(false);
	}
	i2c_stop();
	return id;
}

/* GPIO22 (amp reset): function 0, output, drive low/high */
static void
gpio22(bool high)
{
	uint32_t mode = rd(gpio_base, MODE(22));
	mode &= ~(0xfu << ((22 % 8) * 4));
	wr(gpio_base, MODE(22), mode);
	if (high) wr(gpio_base, 0x104, 1u << 22);   /* DOUT_SET bank0 */
	else wr(gpio_base, 0x108, 1u << 22);        /* DOUT_CLR bank0 */
	wr(gpio_base, 0x004, 1u << 22);             /* DIR_SET bank0: output */
}


/* MT6358 digital playback path (PWRAP), from the vendor machine driver */
static void
codec_digital(void)
{
	pw(0x00d8, 0x0249); pwm(0x07ac, 1u << 13, 1u << 13);
	pw(0x0030, 0x0ff0); pw(0x003c, 0x8888);
	pw(0x2394, 0x0001); pwm(0x2422, 1u << 4, 0);
	pwm(0x220c, 0x66, 0); pwm(0x2292, 0xc5, 0); spin_us(250);
	pw(0x229a, 0x6); pw(0x229a, 0x3); pw(0x229a, 0xB);
	pw(0x2296, 0xCBA1); pwm(0x2288, 0x4001, 0x0001);
	pw(0x228a, 0x0001); pw(0x2290, 0x0000);
	redwood_keep("mtkafe: MT6358 digital playback path set");
}

/* MT6358 LINEOUT L analog (Speaker_Amp_Change), gain g (0x1f = -40 dB start) */
static void
lol_analog(uint16_t g)
{
	uint16_t got = 0;
	pw(0x240c, 0x4000);
	pw(0x223c, 0x0001); pw(0x223e, 0x002c); pw(0x223a, 0x0001);
	pw(0x2242, 0x0003); pw(0x2240, 0x0000); spin_us(250);
	pwm(0x2424, 0x1055, 0x1055); pw(0x2426, 0x0001); spin_us(100);
	pw(0x2488, 0x0000);
	pw(0x2416, 0x0010); pw(0x2416, 0x0110); pw(0x2416, 0x0112); pw(0x2416, 0x0113);
	pw(0x2420, 0x0055); pw(0x241e, 0x4900);
	pw(0x241a, 0x0000); pw(0x241c, 0x0000);
	pw(0x248a, (uint16_t)((g << 7) | g));
	pwm(0x2422, 1u << 0, 1u << 0);
	pw(0x2408, 0x0009); pw(0x241a, 0x0001); pw(0x2416, 0x011b);
	uint16_t c7 = 0, c0 = 0;
	pmic_read(0x248a, &got); pmic_read(0x2416, &c7); pmic_read(0x2408, &c0);
	redwood_keep("mtkafe: LOL analog up, gain ZCD_CON1 %#06x (g %#x), CON7 %#06x CON0 %#06x", got, g, c7, c0);
}

/* AW87519 reset + kspk profile (vendor aw87519_audio_kspk, 21 register pairs) */
static bool
amp_kspk(void)
{
	static const uint8_t prof[][2] = {
		{0x69,0x80},{0x69,0xB7},{0x01,0xF0},{0x02,0x09},{0x03,0xE8},{0x04,0x11},{0x05,0x10},
		{0x06,0x43},{0x07,0x4E},{0x08,0x03},{0x09,0x08},{0x0A,0x4A},{0x60,0x16},{0x61,0x20},
		{0x62,0x01},{0x63,0x0B},{0x64,0xC5},{0x65,0xA4},{0x66,0x78},{0x67,0xC4},{0x68,0x90} };
	gpio22(false); spin_us(2000); gpio22(true); spin_us(2000);
	amp_write(0x64, 0x2C);
	uint8_t id = amp_read(0x00);
	redwood_keep("mtkafe: AW87519 after reset id %#04x", id);
	if (id != 0x59) { redwood_keep("mtkafe: amp id not 0x59, kspk aborted"); return false; }
	for (unsigned i = 0; i < sizeof(prof) / sizeof(prof[0]); i++)
		if (!amp_write(prof[i][0], prof[i][1])) { redwood_keep("mtkafe: amp reg %#x write failed", prof[i][0]); return false; }
	redwood_keep("mtkafe: AW87519 kspk profile written (21 regs)");
	return true;
}

/* the DL1 ring: one contiguous piece, a write-combine alias for the CPU writes, filled with a tone */
static bool
dl1_alloc(bool tone)
{
	vm_offset_t mem = 0;
	uint32_t bytes = ring_frames * 4;
	if (kmem_alloc_contig(kernel_map, &mem, round_page(bytes), PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		redwood_keep("mtkafe: no memory for the DL1 ring"); return false;
	}
	ring_va = mem; ring_phys = kvtophys(mem); ring_wc = ml_io_map_wcomb(ring_phys, round_page(bytes));
	volatile int16_t *p = (volatile int16_t *)ring_wc;
	for (uint32_t f = 0; f < ring_frames; f++) { int16_t s = tone ? tone48[f % 48] : 0; p[2 * f] = s; p[2 * f + 1] = s; }
	redwood_keep("mtkafe: DL1 ring %u frames, phys %#llx, wc %#llx", ring_frames, ring_phys, (uint64_t)ring_wc);
	return true;
}

static void
dl1_start(void)
{
	uint32_t bytes = ring_frames * 4, v;
	wr(g_afe, 0x040, (uint32_t)ring_phys);
	wr(g_afe, 0x048, (uint32_t)(ring_phys + bytes - 1));   /* END = last byte */
	wr(g_afe, 0xb00, (uint32_t)(ring_phys >> 32));
	wr(g_afe, 0xb08, (uint32_t)((ring_phys + bytes) >> 32));
	wr(g_afe, 0xb04, (uint32_t)(ring_phys >> 32));
	v = rd(g_afe, 0x014); wr(g_afe, 0x014, (v & ~0xfu) | 10u);
	v = rd(g_afe, 0x014); wr(g_afe, 0x014, v & ~(1u << 21));
	v = rd(g_afe, 0x3d8); wr(g_afe, 0x3d8, (v & ~3u) | 3u);
	v = rd(g_afe, 0x3d0); wr(g_afe, 0x3d0, (v & ~0xfu) | 3u);
	v = rd(g_afe, 0x3f8); wr(g_afe, 0x3f8, v & ~1u);
	wr(g_afe, 0x260, 0); wr(g_afe, 0x264, 0);
	wr(g_afe, 0x108, 0x83001803); wr(g_afe, 0x10c, 0xf74f0000);
	v = rd(g_afe, 0xc50); wr(g_afe, 0xc50, (v & ~0x3fu) | 0x1du);
	v = rd(g_afe, 0x124); wr(g_afe, 0x124, v | 1u);
	/* 24-bit output ports + 8_24 memif format (vendor mt6768_afe_runtime_resume) */
	wr(g_afe, 0x06c, 0xffffffffu);                 /* AFE_CONN_24BIT */
	wr(g_afe, 0x994, 0xffffffffu);                 /* AFE_CONN_24BIT_1 */
	v = rd(g_afe, 0x0cc); wr(g_afe, 0x0cc, v & ~(1u << 28));  /* AFE_MEMIF_MSB: force 8_24 */
	v = rd(g_afe, 0xe40); wr(g_afe, 0xe40, v | 1u);           /* AFE_AUD_PAD_TOP bit0 */
	/* master AFE enable (DAC_CON0 bit0), then the DL1 memif (bit1) */
	v = rd(g_afe, 0x010); wr(g_afe, 0x010, v | 1u);
	spin_us(200);
	v = rd(g_afe, 0x010); wr(g_afe, 0x010, v | (1u << 1));
	dl1_running = 1;
	/* IRQ0 paces the DL1 memif (vendor trigger): counter=480 (10 ms), fs=10 (48k), MCU on */
	wr(g_afe, 0x300, 480u);                          /* AFE_IRQ_MCU_CNT0 */
	v = rd(g_afe, 0x2e4); wr(g_afe, 0x2e4, (v & ~0xfu) | 10u);   /* AFE_IRQ_MCU_CON1 fs */
	wr(g_afe, 0x3a8, 1u);                            /* clear IRQ0 status */
	v = rd(g_afe, 0x3a0); wr(g_afe, 0x3a0, v | 1u);   /* AFE_IRQ_MCU_CON0 IRQ0_MCU_ON */
	/* route DL1 -> ADDA_DL (sink ports 3/4 <- I_DL1_CH1=5 / CH2=6), infra audio bus now on */
	v = rd(g_afe, 0x02c); wr(g_afe, 0x02c, v | (1u << 5));
	v = rd(g_afe, 0x030); wr(g_afe, 0x030, v | (1u << 6));
	redwood_keep("mtkafe: route CONN3 %#x CONN4 %#x", rd(g_afe, 0x02c), rd(g_afe, 0x030));
	spin_us(1000);
	redwood_keep("mtkafe: DL1 on DAC_CON0 %#x DAC_MON %#x IRQ_CON0 %#x CNT0 %#x (AFE_ON_RETM bit0=%u) BASE %#x END %#x CUR %#x",
	    rd(g_afe, 0x010), rd(g_afe, 0x2ec), rd(g_afe, 0x3a0), rd(g_afe, 0x300), rd(g_afe, 0x2ec) & 1u, rd(g_afe, 0x040), rd(g_afe, 0x048), rd(g_afe, 0x044));
}

/* everything off, without sleeping: at the run's end and on a panic's way out */
static void
playback_down(void)
{
	if (g_afe == 0) return;
	uint32_t v = rd(g_afe, 0x010); wr(g_afe, 0x010, v & ~3u);  /* DL1 off + AFE master off */
	dl1_running = 0;
	pw(0x248a, (uint16_t)((0x1f << 7) | 0x1f)); pw(0x2416, 0x0000);
	amp_write(0x01, 0x00); gpio22(false);
	redwood_keep("mtkafe: playback down (DL1 off, LOL muted, amp off); sink taken %llu dropped %llu", sink_taken, sink_dropped);
}

static bool
playback_up(void)
{
	codec_digital();
	lol_analog((uint16_t)lol_gain);
	if (!amp_kspk()) return false;
	dl1_start();
	return true;
}

/* one-shot register dump to diagnose the DL1 memif (CUR stuck) */
/* ---- mtkafe=5: the iOS audio sink. mediaserverd writes stereo s16le frames here; we copy
 * them into the DL1 ring ahead of the hardware read pointer (DL1_CUR). Non-blocking. ---- */
static uint32_t
mtk_afe_sink(const int16_t *frames, uint32_t count)
{
	if (!dl1_running || ring_wc == 0) { sink_dropped += count; return 0; }
	uint32_t cur = rd(g_afe, 0x044);                       /* hardware read pointer (byte addr) */
	uint32_t hw_frame = ((cur - (uint32_t)ring_phys) >> 2) % ring_frames;
	uint32_t used = (wr_frame - hw_frame + ring_frames) % ring_frames;
	uint32_t free = ring_frames - 1 - used;
	uint32_t take = count < free ? count : free;
	volatile int16_t *p = (volatile int16_t *)ring_wc;
	for (uint32_t i = 0; i < take; i++) {
		uint32_t idx = (wr_frame + i) % ring_frames;
		p[2 * idx] = frames[2 * i];
		p[2 * idx + 1] = frames[2 * i + 1];
	}
	wr_frame = (wr_frame + take) % ring_frames;
	sink_taken += take;
	sink_dropped += count - take;
	return take;
}

static void
dump_afe(void)
{
	redwood_keep("mtkafe-dump: top CFG3 %#x CFG4 %#x AUDDIV %#x | infra STA1 %#x STA2 %#x",
	    rd(g_top, 0x70), rd(g_top, 0x80), rd(g_top, 0x320), rd(g_infra, 0x94), rd(g_infra, 0xac));
	redwood_keep("mtkafe-dump: apll CON0 %#x CON1 %#x CON2 %#x CON4 %#x tuner %#x",
	    rd(g_apl, 0x308), rd(g_apl, 0x30c), rd(g_apl, 0x310), rd(g_apl, 0x318), rd(g_apl, 0x40));
	redwood_keep("mtkafe-dump: syscon0 %#x syscon4 %#x | DAC_CON0 %#x CON1 %#x CON2 %#x",
	    rd(g_afe, 0x000), rd(g_afe, 0x004), rd(g_afe, 0x010), rd(g_afe, 0x014), rd(g_afe, 0x2e0));
	redwood_keep("mtkafe-dump: DL1 BASE %#x CUR %#x END %#x CUR_MSB %#x BASE_MSB %#x END_MSB %#x",
	    rd(g_afe, 0x040), rd(g_afe, 0x044), rd(g_afe, 0x048), rd(g_afe, 0xb04), rd(g_afe, 0xb00), rd(g_afe, 0xb08));
	redwood_keep("mtkafe-dump: MEMIF_MSB %#x HD_MODE %#x PBUF %#x MINLEN %#x | CONN3 %#x CONN4 %#x",
	    rd(g_afe, 0x0cc), rd(g_afe, 0x3f8), rd(g_afe, 0x3d8), rd(g_afe, 0x3d0), rd(g_afe, 0x02c), rd(g_afe, 0x030));
	redwood_keep("mtkafe-dump: DL_SRC2_CON0 %#x UL_DL_CON0 %#x PREDIS0 %#x PREDIS1 %#x AUD_PAD_TOP %#x",
	    rd(g_afe, 0x108), rd(g_afe, 0x124), rd(g_afe, 0x260), rd(g_afe, 0x264), rd(g_afe, 0xe40));
	redwood_keep("mtkafe-dump: IRQ CON0 %#x CON1 %#x CNT0 %#x EN %#x STATUS %#x | DAC_MON %#x",
	    rd(g_afe, 0x3a0), rd(g_afe, 0x2e4), rd(g_afe, 0x300), rd(g_afe, 0x3b4), rd(g_afe, 0x3a4), rd(g_afe, 0x2ec));
	redwood_keep("mtkafe-mon: BUS_MON0 %#x ADDA_SRC_MON0 %#x MON1 %#x UL_SRC_MON0 %#x IRQ0_CNT_MON %#x SINEGEN_CON0 %#x CON2 %#x",
	    rd(g_afe, 0x244), rd(g_afe, 0x130), rd(g_afe, 0x134), rd(g_afe, 0x148), rd(g_afe, 0x310), rd(g_afe, 0x1f0), rd(g_afe, 0x1dc));
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t top = ml_io_map(TOPCKGEN, 0x1000), apl = ml_io_map(APMIXED, 0x1000);
	vm_offset_t infra = ml_io_map(INFRACFG, 0x1000), afe = ml_io_map(AFE, 0x1000), gpio = ml_io_map(GPIO, 0x1000);
	uint32_t bank;

	pwrap = (volatile uint32_t *)ml_io_map(PWRAP, 0x1000);
	gpio_base = gpio;
	g_afe = afe; g_top = top; g_apl = apl; g_infra = infra;
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
	if (level >= 4) {
		bool tone = (level == 4);
		if (dl1_alloc(tone) && playback_up()) {
			dump_afe();
			if (tone) {
				redwood_keep("mtkafe: TONE 1 kHz at -40 dB LOL playing for %u s", tone_secs);
				uint32_t cur0 = rd(g_afe, 0x044);
				delay_for_interval(tone_secs, NSEC_PER_SEC);
				uint32_t cur1 = rd(g_afe, 0x044);
				if (cur1 != cur0) redwood_keep("mtkafe: DL1 DMA advanced (CUR %#x -> %#x)", cur0, cur1);
				else redwood_keep("mtkafe: DL1 DMA NOT advancing (CUR %#x)", cur0);
				playback_down();
				redwood_keep("mtkafe: tone done");
			} else {
				wr_frame = 0;
				redwood_audio_set_sink("mtk-afe", mtk_afe_sink);
				redwood_keep("mtkafe: iOS audio sink registered (mtk-afe), DL1 running at -40 dB LOL");
				/* stays running until quiesce; report sink throughput once a second */
				for (unsigned t = 0; t < tone_secs; t++) {
					delay_for_interval(1, NSEC_PER_SEC);
					redwood_keep("mtkafe: sink taken %llu dropped %llu (CUR %#x)", sink_taken, sink_dropped, rd(g_afe, 0x044));
				}
				redwood_keep("mtkafe: sink session done (taken %llu dropped %llu)", sink_taken, sink_dropped);
			}
		} else {
			redwood_keep("mtkafe: playback bring-up FAILED");
		}
	}
	redwood_keep("mtkafe: probe done (level %u)", level);
	thread_terminate(current_thread());
}

void
redwood_mtkafe_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mtkafe", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("mtkafe_secs", &tone_secs, sizeof(tone_secs));
	if (tone_secs == 0 || tone_secs > 30) tone_secs = 6;
	PE_parse_boot_argn("mtkafe_gain", &lol_gain, sizeof(lol_gain));
	if (lol_gain < 0x12) lol_gain = 0x12;   /* lower index = louder; cap at -10 dB (0x12) until physically verified */
	if (lol_gain > 0x1f) lol_gain = 0x1f;   /* 0x1f = -40 dB, the quietest */
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
redwood_mtkafe_quiesce(void)
{
	playback_down();
}
