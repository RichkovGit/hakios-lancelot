/*
 * The battery as iOS reads it, on the MT6358's own gauge: written for the Infinix Hot 20 (x6826b) and used on
 * lancelot with two numbers of that phone changed (CELL_UAH, CELL_MOHM, see below).
 * Boot-arg mtkbatt=1; mtkbatt=2 says every reading in the log, every 2 s
 * instead of every 10; mtkbatt=3 takes the percentage from the voltage alone (the way without Android's number).
 *
 * The percentage is Android's own, carried on by the PMIC's counter of charge:
 *  - Android's gauge is a program, and it leaves the percentage it shows in the PMIC's clock, bits 14:8 of RTC_AL_MTH (0x5aa):
 *    0xe401 while Android said 100, 0xe301 while it said 99 (10 November 2026, read from Android through pmic_access).
 *  - The MT6358's fuel gauge counts the charge through the phone's 5 mOhm (R_FG_VALUE of its device tree) in FGADC_CAR_CON0/1
 *    (0xd14, 0xd16), and Android does not start the counter again: it went on from XNU's last number after run 764. Read after
 *    a latch (FGADC_CON1, 0xd0a: bit 0 asks, bit 15 says latched, bit 3 lets go) with the current, FGADC_CUR_CON0 (0xd8a):
 *    16 bits with a sign, 250 mV / 65536 a count over 5 mOhm = 0.763 mA. Run 764 saw -240..-437 mA on a computer's port
 *    (Android says about the same there, -357 mA), the voltage lower the more negative it was, and the counter falling:
 *    less than 0 is out of the battery. The counter fell 92546 in the 90.8 s that the current summed to 9.94 mAh:
 *    0.1074 uAh a count. The cell is 5132 mAh on the x6826b and 5020 on lancelot (Android's CHARGE_FULL_DESIGN there),
 *    51 and 50 mAh a point; CELL_UAH says which.
 *  - So: the percentage in the clock when the kernel starts, plus the counter's change since. Not written back to the clock:
 *    Android's gauge makes its own on its next start.
 * Checked against the voltage, and without Android's number (0, or over 100) it is the voltage alone:
 *  - The MT6358's BATADC, channel 0 of its AUXADC. A conversion is asked for (bit 0 of AUXADC_RQST0, 0x1108) and its result
 *    is AUXADC_ADC0 (0x1088: 15 bits, bit 15 "ready"). Without the request the register keeps the last conversion somebody
 *    made: run 754 read 26788 counts at every look for a minute, which was Android's last. A count is 1.8 V x 3 / 32768 =
 *    0.16479 mV: 26775 counts while Android's own gauge said 4414 mV.
 *  - The cell at rest by a curve of a 4.45 V Li-ion cell, checked at ten points against the phone's profile: 4345 mV is 90%,
 *    3885 mV 50%, 3690 mV 10%. Under a load the voltage is brought back by the current over 0.28 Ohm, which is what run
 *    764's own pairs give (4306 mV at -240 mA, 4254 mV at -422 mA) and more than the profile's 0.13: it is rough, 4276 mV
 *    at -357 mA is 94% by it where Android says 99. If the two ways are more than 30 points apart the voltage is believed.
 * A charger on the port: RGS_CHRDET, bit 5 of CHR_CON0 (0xa88), the name Android's PMIC header gives it; 0x62 with the cable
 * in. It has not been seen at 0 yet (the cable is how the phone is run). "Charging" is said only while the current goes in.
 * The port's voltage, which AdapterDetails shows: AUXADC channel 2, VCDT, VBUS through the phone's own divider. It is the
 * channel Android's mt_charger reads for usb/voltage_now, and one count of it is 4.157 mV of the port: twenty pairs of
 * usb/voltage_now and in_voltage2_VCDT_raw read together over a minute (19 November 2026) give 4.130..4.197, mean 4.157,
 * which is the channel's 0.4385 mV a count (1.8 V over 12 bits, as Android's own in_voltage2_VCDT_input has it) times a
 * divider of 9.48. The gain that Android's pmic_get_vbus applies comes from the PMIC's efuse calibration, which lives in
 * its kernel's BSS and cannot be read from here; this one number, taken from Android's own answer, stands in for it. The
 * port's current is its limit rather than a measurement: usb/current_max on Android and lk_charger's usb_charger_current
 * are both 500 mA. Not done: the charger itself (RT9471 on I2C5, SMB1351 on I2C7: nothing sets or feeds them here), and
 * which family of port it is -- Android says real_type USB on a Type-C sink, and usb_type stays 0 (unknown) until it is
 * known which of iOS's families is right.
 * The cell's temperature: the same AUXADC, channel 3 (BAT_TEMP), 12 bits over a 2:1 divider, and channel 11, which answers
 * with the 2.8 V the thermistor is pulled up to. The phone's own device tree books VBIF as channel 14 and channel 11 as
 * TSX_TEMP, but request bit 11 comes back with 2791-2800 mV over run 45 (the same as Android's in_voltage14_VBIF of 2795-2797,
 * where its in_voltage11_TSX_TEMP is 836), so it is VBIF28 that bit 11 reads on this PMIC. The NTC is 100 kOhm at 25 degC and
 * the pull-up is 100 kOhm, so
 * R = 100000 x V / (Vbif - V) and a table of the phone's own kernel turns R into degrees (lancelot's mt6358-auxadc.c; see
 * bat_ntc below). Checked against Android on 18 November 2026: 1523 counts (1339 mV) of VBIF 2796 mV is 91849 Ohm and
 * 26.6 degC where Android's power_supply/battery/temp said 26.5; the fuel gauge's own interrupts in Android's log agree
 * (h[12799 11903] are its 28 and 30 degree trip points in 0.1 mV, and the table above makes them 1280.4 and 1190.6).
 * Not done: the charger's own regulation and its modes, and the port's kind (see above).
 * One transaction at a time on the wrapper: redwood_pwrap_lock() is the camera's and CONNSYS's too.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define PWRAP_BASE              0x1000d000ULL
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)

#define MT6358_RTC_AL_MTH       0x05aa          /* bits 14:8: the percentage Android's gauge shows */
#define MT6358_CHR_CON0         0x0a88
#define CHR_CON0_CHRDET         (1u << 5)
#define MT6358_FGADC_CON0       0x0d08
#define MT6358_FGADC_CON1       0x0d0a
#define FGADC_CON1_READ         (1u << 0)
#define FGADC_CON1_CLEAR        (1u << 3)
#define FGADC_CON1_LATCHED      (1u << 15)
#define MT6358_FGADC_CAR_CON0   0x0d14
#define MT6358_FGADC_CAR_CON1   0x0d16
#define MT6358_FGADC_CUR_CON0   0x0d8a
#define MT6358_AUXADC_ADC(n)    (0x1088 + 2 * (n))      /* ADC0..: one 16-bit answer each, bit 15 "ready" */
#define MT6358_AUXADC_RQST0     0x1108

#define AUXADC_CH_BATADC        0
#define AUXADC_CH_VCDT          2               /* the port's voltage: VBUS through the phone's divider */
#define AUXADC_CH_BAT_TEMP      3
#define AUXADC_CH_VBIF          11
#define VBIF_MV_NOMINAL         2795            /* VBIF28, what Android reads: 2794..2798 mV */
#define RBAT_PULL_UP_OHM        100000          /* VBIF28 to the thermistor on BAT_TEMP, as this phone's kernel has it */
#define VBUS_NV_PER_COUNT       4157000         /* 4.157 mV: see "the port" in the comment above */
#define VBUS_MV_NOMINAL         5000            /* what a USB port gives, when VCDT cannot be read */
#define USB_CURRENT_UA          500000          /* usb/current_max on Android, lk_charger's usb_charger_current */

#define COUNT_NV                164795          /* 1.8 V x 3 / 32768, in nV a count */
#define FG_COUNT_NA             762939          /* 250 mV / 65536 over 5 mOhm, in nA a count */
#define CAR_COUNT_PAH           107425          /* run 764: 9.94 mAh by the current while the counter fell 92546, in pAh a count */
#define CELL_UAH                5020000         /* lancelot: POWER_SUPPLY_CHARGE_FULL_DESIGN on Android; 5132000 was the x6826b's */
#define CELL_MOHM               140             /* lancelot: POWER_SUPPLY_RESISTANCE on Android's bms; 280 was the x6826b's */
#define FULL_MV                 4410
#define APART_HUNDREDTHS        3000

static volatile uint32_t *pwrap;

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(pwrap[PWRAP_WACS2_RDATA / 4]) == state) {
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
		pwrap[PWRAP_WACS2_CMD / 4] = (reg >> 1) << 16;
		if (pwrap_wait(6)) {
			*value = (uint16_t)pwrap[PWRAP_WACS2_RDATA / 4];
			pwrap[PWRAP_WACS2_VLDCLR / 4] = 1;
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

static bool
pmic_write(uint32_t reg, uint16_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (pwrap_wait(0)) {
		pwrap[PWRAP_WACS2_CMD / 4] = (1u << 31) | ((reg >> 1) << 16) | value;
		ok = pwrap_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* the register's bits (mask) at `want`, looked at every millisecond for `ms` */
static bool
pmic_until(uint32_t reg, uint16_t mask, uint16_t want, unsigned ms, uint16_t *last)
{
	for (unsigned t = 0; t <= ms; t++) {
		if (pmic_read(reg, last) && (*last & mask) == want) {
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return false;
}

/* one conversion of any AUXADC channel, asked for now: its counts (12 bits unless told otherwise), or -1 */
static int32_t
auxadc_convert(unsigned channel, unsigned bits)
{
	static bool said[16];
	uint16_t raw = 0;

	if (channel < 16 && pmic_write(MT6358_AUXADC_RQST0, (uint16_t)(1u << channel))) {
		delay_for_interval(3, NSEC_PER_MSEC);
		if (pmic_until(MT6358_AUXADC_ADC(channel), 0x8000, 0x8000, 50, &raw)) {
			return raw & (int32_t)((1u << bits) - 1);
		}
	}
	if (!said[channel & 15]) {
		said[channel & 15] = true;
		printf("mtkbatt: no conversion of AUXADC channel %u in 50 ms after the request (ADC %#x): it is not given\n",
		    channel, raw);
	}
	return -1;
}

/*
 * The NTC on BAT_TEMP as the phone's own kernel has it (mt6358-auxadc.c): ohms and tenths of a degree, 100 kOhm at 25.
 * The lines between the points are straight in R, which is what BattThermistorConverTemp() does there too, and
 * beyond the table's ends the temperature stands at -40 and 90.
 */
static const struct { int32_t ohm, tenths; } bat_ntc[] = {
	{ 4251000, -400 }, { 3005000, -350 }, { 2149000, -300 }, { 1554000, -250 }, { 1016120, -200 },
	{ 718920, -150 }, { 562100, -100 }, { 460000, -50 }, { 338840, 0 }, { 261800, 50 },
	{ 199400, 100 }, { 152500, 150 }, { 125000, 200 }, { 100000, 250 }, { 74230, 300 },
	{ 60180, 350 }, { 48680, 400 }, { 39900, 450 }, { 30990, 500 }, { 25090, 550 },
	{ 21620, 600 }, { 18020, 650 }, { 14980, 700 }, { 12640, 750 }, { 10508, 800 },
	{ 8887, 850 }, { 7500, 900 },
};

static int32_t
ntc_tenths(int32_t ohm)
{
	unsigned last = sizeof(bat_ntc) / sizeof(bat_ntc[0]) - 1;

	if (ohm >= bat_ntc[0].ohm) {
		return bat_ntc[0].tenths;
	}
	for (unsigned i = 0; i < last; i++) {
		if (ohm >= bat_ntc[i + 1].ohm) {
			return bat_ntc[i].tenths + (int32_t)((int64_t)(ohm - bat_ntc[i].ohm) *
			    (bat_ntc[i + 1].tenths - bat_ntc[i].tenths) / (bat_ntc[i + 1].ohm - bat_ntc[i].ohm));
		}
	}
	return bat_ntc[last].tenths;
}

/*
 * The cell's temperature in hundredths of a degree, or -1; the counts, millivolts, the pull-up's voltage and the
 * thermistor's resistance come back too, for the log. A count of a 12-bit channel is 1800 mV x 2 / 4096 = 0.879 mV.
 */
static int32_t
batt_temp_hundredths(int32_t *counts_out, int32_t *mv_out, int32_t *vref_out, int32_t *ohm_out)
{
	int32_t counts = auxadc_convert(AUXADC_CH_BAT_TEMP, 12);
	int32_t vbif = auxadc_convert(AUXADC_CH_VBIF, 12);
	int32_t mv, vref;

	*counts_out = counts;
	if (counts < 0) {
		return -1;
	}
	mv = counts * 3600 / 4096;
	vref = vbif >= 0 ? vbif * 3600 / 4096 : VBIF_MV_NOMINAL;
	*mv_out = mv;
	*vref_out = vref;
	if (mv <= 0 || vref <= mv) {
		return -1;
	}
	*ohm_out = (int32_t)((int64_t)RBAT_PULL_UP_OHM * mv / (vref - mv));
	return ntc_tenths(*ohm_out) * 10;
}

/*
 * The fuel gauge's current and its counter of charge, latched together. False, with the reason said once, if the gauge does
 * not latch (it is Android that switched it on; nothing here does).
 */
static bool
gauge_latch(int16_t *current, uint32_t *charge)
{
	static bool said;
	uint16_t con1 = 0, got = 0, low = 0, high = 0, cur = 0;
	bool ok;

	if (!pmic_read(MT6358_FGADC_CON1, &con1)) {
		return false;
	}
	con1 &= (uint16_t)~(FGADC_CON1_READ | FGADC_CON1_CLEAR | FGADC_CON1_LATCHED);
	ok = pmic_write(MT6358_FGADC_CON1, con1 | FGADC_CON1_READ) &&
	    pmic_until(MT6358_FGADC_CON1, FGADC_CON1_LATCHED, FGADC_CON1_LATCHED, 100, &got) &&
	    pmic_read(MT6358_FGADC_CUR_CON0, &cur) && pmic_read(MT6358_FGADC_CAR_CON0, &low) && pmic_read(MT6358_FGADC_CAR_CON1, &high);
	/* let go, whatever happened */
	(void)pmic_write(MT6358_FGADC_CON1, con1 | FGADC_CON1_CLEAR);
	(void)pmic_until(MT6358_FGADC_CON1, FGADC_CON1_LATCHED, 0, 100, &got);
	(void)pmic_write(MT6358_FGADC_CON1, con1);
	if (!ok) {
		if (!said) {
			uint16_t con0 = 0;

			said = true;
			(void)pmic_read(MT6358_FGADC_CON0, &con0);
			printf("mtkbatt: the fuel gauge does not latch (FGADC_CON0 %#x, CON1 %#x): no current\n", con0, got);
		}
		return false;
	}
	*current = (int16_t)cur;
	*charge = ((uint32_t)high << 16) | low;
	return true;
}

/* the cell's charge at rest by its voltage, in hundredths of a percent: (mV, hundredths) */
static int32_t
percent_of(int32_t mv)
{
	static const struct { int32_t mv, hundredths; } curve[] = {
		{ 3400, 0 }, { 3620, 500 }, { 3690, 1000 }, { 3745, 2000 }, { 3785, 3000 }, { 3825, 4000 }, { 3885, 5000 },
		{ 3985, 6000 }, { 4100, 7000 }, { 4225, 8000 }, { 4345, 9000 }, { 4395, 9700 }, { FULL_MV, 10000 },
	};

	if (mv <= curve[0].mv) {
		return 0;
	}
	for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv <= curve[i].mv) {
			return curve[i - 1].hundredths + (mv - curve[i - 1].mv) * (curve[i].hundredths - curve[i - 1].hundredths) / (curve[i].mv - curve[i - 1].mv);
		}
	}
	return 10000;
}

/* the port's voltage: AUXADC channel 2 is VCDT, VBUS through the phone's divider, and it is the channel
 * Android's mt_charger reads for usb/voltage_now */
static int32_t
vbus_mv(int32_t *counts_out)
{
	int32_t counts = auxadc_convert(AUXADC_CH_VCDT, 12);

	*counts_out = counts;
	if (counts < 0) {
		return -1;
	}
	return (int32_t)((int64_t)counts * VBUS_NV_PER_COUNT / 1000000);
}

static void
mtkbatt_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t level = 1;
	uint32_t looks = 0;
	int32_t kept_mv = 0;            /* the cell's voltage at rest, smoothed over four looks */
	int32_t kept_ma = 0;            /* the current, the same way: "charging" does not flicker with it */
	int32_t start_hundredths = -1;  /* Android's percentage, or -1 */
	int32_t left_hundredths = -1;   /* the same, kept even after the charge stops being believed */
	uint32_t start_charge = 0;
	uint16_t rtc = 0;
	bool counted = false, apart_said = false;

	PE_parse_boot_argn("mtkbatt", &level, sizeof(level));
	pwrap = (volatile uint32_t *)ml_io_map(PWRAP_BASE, 0x1000);
	if (pmic_read(MT6358_RTC_AL_MTH, &rtc) && ((rtc >> 8) & 0x7f) >= 1 && ((rtc >> 8) & 0x7f) <= 100) {
		start_hundredths = (int32_t)((rtc >> 8) & 0x7f) * 100;
		left_hundredths = start_hundredths;
	}
	delay_for_interval(20000, NSEC_PER_MSEC);
	for (;; delay_for_interval(level == 2 ? 2000 : 10000, NSEC_PER_MSEC)) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		int32_t sum = 0, good = 0, low = 0x7fff, high = 0, mv, rest_mv, ma = 0, by_voltage, by_charge = -1;
		int32_t tcounts = -1, tmv = 0, tvref = 0, tohm = 0, temp = -1;
		int32_t vcounts = -1, vbus = -1;
		uint16_t chr = 0;
		int16_t gauge = 0;
		uint32_t charge = 0;
		bool charger, current;

		for (unsigned i = 0; i < 4; i++) {
			int32_t counts = auxadc_convert(AUXADC_CH_BATADC, 15);

			if (counts >= 0) {
				sum += counts;
				good++;
				low = counts < low ? counts : low;
				high = counts > high ? counts : high;
			}
		}
		if (good == 0) {
			continue;       /* nothing is said to iOS on a look that read nothing: it keeps what it had */
		}
		mv = (int32_t)((int64_t)(sum / good) * COUNT_NV / 1000000);
		if (mv < 2500 || mv > 4700) {
			printf("mtkbatt: BATADC %d counts would be %d mV: not a cell's voltage, not given\n", sum / good, mv);
			continue;
		}
		charger = pmic_read(MT6358_CHR_CON0, &chr) && (chr & CHR_CON0_CHRDET);
		vbus = vbus_mv(&vcounts);
		current = gauge_latch(&gauge, &charge);
		if (current) {
			ma = (int32_t)((int64_t)gauge * FG_COUNT_NA / 1000000);
		}
		rest_mv = mv - ma * CELL_MOHM / 1000;
		kept_mv = kept_mv == 0 ? rest_mv : (kept_mv * 3 + rest_mv) / 4;
		kept_ma = (kept_ma * 3 + ma) / 4;
		temp = batt_temp_hundredths(&tcounts, &tmv, &tvref, &tohm);
		by_voltage = percent_of(kept_mv);
		if (current && start_hundredths >= 0 && level != 3) {
			if (!counted) {
				counted = true;
				start_charge = charge;
				printf("mtkbatt: Android left %d%% (RTC_AL_MTH %#x); the charge is counted from %#x\n", start_hundredths / 100, rtc, charge);
			}
			/* hundredths of a percent: counts x pAh / (uAh x 100) */
			by_charge = start_hundredths + (int32_t)((int64_t)(int32_t)(charge - start_charge) * CAR_COUNT_PAH / ((int64_t)CELL_UAH * 100));
			by_charge = by_charge < 0 ? 0 : by_charge > 10000 ? 10000 : by_charge;
			if (by_charge - by_voltage > APART_HUNDREDTHS || by_voltage - by_charge > APART_HUNDREDTHS) {
				if (!apart_said) {
					apart_said = true;
					printf("mtkbatt: %d%% by the charge and %d%% by the voltage (%d mV at rest): the voltage is believed from here\n",
					    by_charge / 100, by_voltage / 100, kept_mv);
				}
				start_hundredths = -1;
				by_charge = -1;
			}
		} else if (!counted && !apart_said && looks == 0) {
			printf("mtkbatt: the percentage is by the voltage alone (RTC_AL_MTH %#x, %s, mtkbatt=%u)\n", rtc,
			    current ? "the gauge answers" : "no gauge", level);
		}
		b.percent_hundredths = by_charge >= 0 ? by_charge : by_voltage;
		b.voltage_uv = mv * 1000;
		b.current_ua = current ? ma * 1000 : 0;
		b.temperature_hundredths = temp >= 0 ? temp : 2500;     /* 25 degrees only when the thermistor could not be read */
		b.present = 1;
		b.external = charger;
		/*
		 * What the port gives, for AdapterDetails (iokit/Kernel/RedwoodBattery.cpp turns these into watts,
		 * milliamps and millivolts). The voltage is measured; the current is the port's limit, which is what
		 * Android's usb/current_max says (500 mA) and what lk_charger asks for a USB charger.
		 */
		if (charger) {
			b.usb_type = 0;         /* (SDP, DCP, Type-C, PD: not told yet; Android's real_type is plain "USB") */
			b.usb_voltage_uv = (vbus > 0 ? vbus : VBUS_MV_NOMINAL) * 1000;
			b.usb_current_ua = USB_CURRENT_UA;
		}
		/* Linux's numbers: 1 charging, 2 discharging, 3 not charging, 4 full.
		 * Full is the charger's termination -- current tapered (<150 mA) at the float
		 * voltage (>=4.2 V) -- not a high SOC alone; SOC>=99.5% stays as a fallback.
		 * (The RT9471's own charge-state flag is the next refinement.) */
		if (!charger) {
			b.status = 2;
		} else if (current && kept_ma >= 0 && kept_ma < 150 && kept_mv >= 4200) {
			b.status = 4;
		} else if (b.percent_hundredths >= 9950) {
			b.status = 4;
		} else if (current && kept_ma <= 0) {
			b.status = 3;
		} else {
			b.status = 1;
		}
		redwood_battery_report(&b, false);
		if (level >= 2 || (looks % 30) == 0) {
			if (left_hundredths >= 0) {
				printf("mtkbatt: Android left %d%% (RTC_AL_MTH %#x)%s\n", left_hundredths / 100, rtc,
				    counted ? "" : "; the gauge has not answered yet, so the percentage is by the voltage");
			}
			printf("mtkbatt: %d.%02d%% (%s), BATADC %d counts (%d..%d of %d) = %d mV, charger %s (CHR_CON0 %#x), port %d mV (VCDT %d counts), status %d",
			    b.percent_hundredths / 100, b.percent_hundredths % 100, by_charge >= 0 ? "by the charge" : "by the voltage",
			    sum / good, low, high, good, mv, charger ? "in" : "out", chr, vbus, vcounts, b.status);
			if (temp >= 0) {
				printf(", %d.%02d C (BAT_TEMP %d counts = %d mV of %d mV = %d Ohm)", temp / 100, temp % 100,
				    tcounts, tmv, tvref, tohm);
			} else {
				printf(", no temperature (BAT_TEMP %d counts)", tcounts);
			}
			if (current) {
				printf("; gauge %d counts = %d mA, charge counter %#x, at rest %d mV = %d.%02d%% by the voltage\n", gauge, ma, charge,
				    kept_mv, by_voltage / 100, by_voltage % 100);
			} else {
				printf("; no current\n");
			}
		}
		looks++;
	}
}

void
redwood_mtkbatt_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkbatt", &level, sizeof(level)) || level < 1) {
		return;
	}
	if (kernel_thread_start(mtkbatt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
