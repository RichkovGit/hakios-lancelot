/*
 * The touch screen of the Redmi 9 (lancelot): a FocalTech FT8719P on SPI0 of the MT6768, as a
 * source of touches for /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg
 * touchscreen=3: every address below belongs to this phone.
 *
 * The controller has no flash. Its firmware lives in PRAM and is downloaded at every boot by
 * whoever brings the chip up -- on the way to Android that is focaltech_core.c, which resets the
 * chip, reads its boot ID and writes a hundred kilobytes. Nothing here resets the chip, on
 * purpose: if the firmware survived the reboot the chip is already reporting and all that is left
 * is to read it. Whether it does survive is exactly what the first lines of the log say: the chip
 * ID at 0xA3/0x9F (0x87, 0x19 when the application is running) or the boot ID from command 0x90
 * (also 0x87, 0x19, but from the boot ROM).
 *
 * There is no clock, pin or SPI framework in this kernel, so the driver reaches the registers
 * itself:
 *   pins     GPIO32 is data in, 33 the chip select, 34 the data out and 35 the clock, all at
 *            function 1 (the MT6768 pin table: SPI0, unlike the SPI2 of the other phone);
 *            GPIO92 is the reset line, held up by the board and left alone; GPIO1 is the
 *            interrupt line, low while the chip has something to say
 *   SPI      driven by hand, mode 1: the clock idles low, data is shifted out on the rising edge
 *            and sampled on the falling one. The chip allows 8 MHz and does not mind going
 *            slower; a report is under a millisecond this way.
 *   chip     FTS 3.1 packages, as focaltech_spi.c has them: a four-byte header whose first byte
 *            says status (0x05), command (0xC0) or data (0x3F); the byte after the header is the
 *            control byte on a command (0x00 write, 0xA0 read with CRC) or the first byte of the
 *            data. A command gets 30 us on its own, then the rest; the chip select stays low for
 *            the whole package and high for at least 150 us after it.
 *
 * A report is read with a bare data package, no command before it, exactly as
 * fts_read_touchdata() does: 63 bytes for ten points of six. The first byte is 0x9x, the low
 * nibble of the second is how many points follow, and then for each point
 *
 *   x high nibble and the event in its top two bits
 *   x low
 *   y high nibble and the finger's number in its top four bits
 *   y low
 *   pressure
 *   the area in the top nibble
 *
 * events 0 and 2 mean down and contact, 1 means lifted. Reading only when the interrupt line is
 * low matters here as much as it does for the other controller: a read with nothing waiting puts
 * the chip's side of the bus out of step.
 *
 * Register layouts: drivers/spi/spi-mt65xx.c and drivers/pinctrl/mediatek/pinctrl-mtk-mt6768.h in
 * Linux, and for the chip's own language the FT8719P driver in the Redmi 9 kernel source
 * (drivers/input/touchscreen/mediatek/FT8719P/), which is where the numbers below come from.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000

static vm_offset_t gpio;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

/* ---- the pins ---- */

/*
 * The MT6768 pin controller puts 32 pins in a bank, four bits of function per pin in a MODE
 * register that covers eight, and gives each bank three pairs of set/clear direction and output
 * registers. The numbers are the same on every MediaTek of this generation.
 */
#define GPIO_DIR_SET(bank)      (0x004 + 0x10 * (bank))
#define GPIO_DIR_CLR(bank)      (0x008 + 0x10 * (bank))
#define GPIO_DOUT_SET(bank)     (0x104 + 0x10 * (bank))
#define GPIO_DOUT_CLR(bank)     (0x108 + 0x10 * (bank))
#define GPIO_DIN(bank)          (0x200 + 0x10 * (bank))
#define GPIO_MODE(pin)          (0x300 + 0x10 * ((pin) / 8))

#define PIN_MI          32              /* SPI0_MI */
#define PIN_CS          33              /* SPI0_CSB, low while a package is on the bus */
#define PIN_MO          34              /* SPI0_MO */
#define PIN_CLK         35              /* SPI0_CLK */
#define PIN_FUNC_SPI0   1

#define PIN_RST         92              /* the chip's reset, pulled up by the board */
#define PIN_IRQ         1               /* low while a report waits */

static inline void
pin_set(unsigned pin, bool high)
{
	wr(gpio, high ? GPIO_DOUT_SET(pin / 32) : GPIO_DOUT_CLR(pin / 32), 1u << (pin % 32));
}

static inline bool
pin_get(unsigned pin)
{
	return (rd(gpio, GPIO_DIN(pin / 32)) >> (pin % 32)) & 1u;
}

/* Long enough for a pad to settle; one register read is already most of a microsecond. */
static inline void
pin_settle(void)
{
	(void)rd(gpio, GPIO_DIN(0));
	(void)rd(gpio, GPIO_DIN(0));
}

static void
pins_init(void)
{
	uint32_t mode;

	/*
	 * The four SPI0 pins become plain GPIOs: the controller at 0x1100a000 is not used, and its
	 * clock would have to be brought up first. The reset line is made an input so that the
	 * board's pull-up keeps the chip out of reset; the interrupt line is an input too.
	 */
	mode = rd(gpio, GPIO_MODE(PIN_MI));
	for (unsigned pin = PIN_MI; pin <= PIN_CLK; pin++) {
		mode &= ~(0xfu << ((pin % 8) * 4));
	}
	wr(gpio, GPIO_MODE(PIN_MI), mode);

	mode = rd(gpio, GPIO_MODE(PIN_IRQ));
	wr(gpio, GPIO_MODE(PIN_IRQ), mode & ~(0xfu << ((PIN_IRQ % 8) * 4)));

	mode = rd(gpio, GPIO_MODE(PIN_RST));
	wr(gpio, GPIO_MODE(PIN_RST), mode & ~(0xfu << ((PIN_RST % 8) * 4)));

	wr(gpio, GPIO_DIR_CLR(PIN_IRQ / 32), 1u << (PIN_IRQ % 32));
	wr(gpio, GPIO_DIR_CLR(PIN_RST / 32), 1u << (PIN_RST % 32));
	wr(gpio, GPIO_DIR_SET(PIN_CS / 32), (1u << (PIN_CS % 32)) | (1u << (PIN_MO % 32)) | (1u << (PIN_CLK % 32)));
	wr(gpio, GPIO_DIR_CLR(PIN_MI / 32), 1u << (PIN_MI % 32));

	/* mode 1: the clock idles low, the chip select high */
	wr(gpio, GPIO_DOUT_CLR(PIN_CLK / 32), 1u << (PIN_CLK % 32));
	wr(gpio, GPIO_DOUT_SET(PIN_CS / 32), 1u << (PIN_CS % 32));
	__asm__ volatile ("dsb sy");
}

static void
chip_reset(void)
{
	wr(gpio, GPIO_DIR_SET(PIN_RST / 32), 1u << (PIN_RST % 32));
	pin_set(PIN_RST, false);
	delay(1000);
	pin_set(PIN_RST, true);
	delay(12000);                   /* FTS_CMD_START_DELAY */
	wr(gpio, GPIO_DIR_CLR(PIN_RST / 32), 1u << (PIN_RST % 32));
}

/* ---- one package on the bus ---- */

/*
 * Mode 1 by hand: the data out changes while the clock is low, the clock rises, then falls and
 * the chip's answer is read. The chip select is low for the whole package and left high for
 * 150 us afterwards, and the first byte sits alone for 30 us, both as focaltech_spi.c does it.
 *
 * The four-byte header in the buffers is one byte of wire, the package type: focaltech_spi.c's
 * message is tx[0] and then tx[4 .. length-1], so offsets 1..3 are padding and stay at home.
 */
#define CS_HIGH_DELAY   150
#define FIRST_BYTE_US   30

static void
spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t wire = length > 4 ? length - 3 : 1;

	pin_set(PIN_CS, false);
	pin_settle();

	for (uint32_t at_wire = 0; at_wire < wire; at_wire++) {
		uint32_t at = at_wire == 0 ? 0 : at_wire + 3;
		uint8_t out = tx ? tx[at] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(PIN_MO, (out >> bit) & 1);
			pin_set(PIN_CLK, true);
			pin_settle();
			pin_set(PIN_CLK, false);
			pin_settle();
			in = (uint8_t)((in << 1) | (pin_get(PIN_MI) ? 1 : 0));
		}
		if (rx) {
			rx[at] = in;
		}
		if (at_wire == 0) {
			delay(FIRST_BYTE_US);
		}
	}
	pin_set(PIN_CS, true);
	pin_settle();
	delay(CS_HIGH_DELAY);
}

/* ---- the chip's language ---- */

#define PKG_STATUS      0x05
#define PKG_COMMAND     0xC0
#define PKG_DATA        0x3F

#define CTRL_WRITE      0x00
#define CTRL_READ       0xA0            /* READ_CMD (0x80) | DATA_CRC_EN (0x20) */

#define STATUS_BUSY     0x80
#define STATUS_APP      0x01            /* the application is running and the bus is free */

static uint32_t packets, crc_errors, timeouts;

static uint16_t
crc_kermit(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

/* The status package is five bytes; the chip's is the last one, and bit 7 says it is busy. */
static int
status_package(void)
{
	uint8_t tx[5] = { PKG_STATUS, 0, 0, 0, 0 }, rx[5];

	spi_xfer(tx, rx, sizeof(tx));
	packets++;
	return rx[4];
}

static bool
wait_idle(void)
{
	for (unsigned try = 0; try < 100; try++) {
		int status;

		delay(150);             /* BUSY_QUERY_DELAY */
		status = status_package();
		if ((status & 0x81) == 0x00 || (status & 0x81) == STATUS_APP) {
			return true;
		}
	}
	timeouts++;
	return false;
}

static void
command_package(uint8_t ctrl, const uint8_t *command, uint32_t length)
{
	uint8_t tx[16] = { PKG_COMMAND, 0, 0, 0, 0 };

	tx[4] = (uint8_t)(ctrl | (length & 0x0f));
	for (uint32_t i = 0; i < length; i++) {
		tx[5 + i] = command[i];
	}
	spi_xfer(tx, NULL, 5 + length);
	packets++;
}

/* A data package, read: the chip drives `length` bytes and two of CRC after the header. */
static bool
data_package(uint8_t *data, uint32_t length)
{
	uint8_t tx[128] = { 0 }, rx[128];
	uint32_t total = 4 + length + 2;

	if (total > sizeof(tx)) {
		return false;
	}
	tx[0] = PKG_DATA;
	spi_xfer(tx, rx, total);
	packets++;
	for (uint32_t i = 0; i < length; i++) {
		data[i] = rx[4 + i];
	}
	if (crc_kermit(rx + 4, length) != (uint16_t)(rx[4 + length] | (rx[4 + length + 1] << 8))) {
		crc_errors++;
		return false;
	}
	return true;
}

/* A data package, write: no CRC, and the chip only takes it in boot mode. */
static void
data_package_write(const uint8_t *data, uint32_t length)
{
	uint8_t tx[16] = { 0 };

	if (4 + length > sizeof(tx)) {
		return;
	}
	tx[0] = PKG_DATA;
	for (uint32_t i = 0; i < length; i++) {
		tx[4 + i] = data[i];
	}
	spi_xfer(tx, NULL, 4 + length);
	packets++;
}

/* A register in the running application: the command carries the address and the length. */
static bool
read_reg(uint8_t address, uint8_t *value)
{
	uint8_t command[3] = { address, 0, 1 };

	if (!wait_idle()) {
		return false;
	}
	command_package(CTRL_READ, command, sizeof(command));
	if (!wait_idle()) {
		return false;
	}
	if (!data_package(value, 1)) {
		return false;
	}
	return true;
}

static bool
write_reg(uint8_t address, uint8_t value)
{
	uint8_t command[3] = { address, 0, 1 };

	if (!wait_idle()) {
		return false;
	}
	command_package(CTRL_WRITE, command, sizeof(command));
	if (!wait_idle()) {
		return false;
	}
	data_package_write(&value, 1);
	return true;
}

/*
 * The boot ROM's own protocol, as fts_read_bootid(): 0x55 0xAA wakes it, and 0x90 answers with
 * two bytes of ID. Writes in boot mode are a command package and then a data package, with no
 * length appended to the command -- the four bytes of {0x90, 0, 0, 0} are all the ROM wants.
 */
static bool
boot_id(uint8_t id[2])
{
	uint8_t start = 0x55, more = 0xaa, command[4] = { 0x90, 0, 0, 0 };

	if (!wait_idle()) {
		return false;
	}
	command_package(CTRL_WRITE, &start, 1);
	if (!wait_idle()) {
		return false;
	}
	data_package_write(&more, 1);
	delay(12000);

	if (!wait_idle()) {
		return false;
	}
	command_package(CTRL_READ, command, sizeof(command));
	if (!wait_idle()) {
		return false;
	}
	return data_package(id, 2);
}

/* ---- what a report holds ---- */

#define PANEL_W         1080
#define PANEL_H         2340
#define MAX_FINGERS     10
#define ONE_POINT       6
/* The chip streams one fixed-size report: the touch part, then FTS_GESTURE_DATA_LEN of gestures.
 * The CRC covers both, so a read of only the touch part always fails. */
#define GESTURE_LEN     28              /* FTS_GESTURE_POINTS_MAX * 4 + 4 */
#define REPORT_LEN      (MAX_FINGERS * ONE_POINT + 2 + GESTURE_LEN)
#define REPORT_HEADER   0x90

static bool down[MAX_FINGERS];
static uint32_t last_x[MAX_FINGERS], last_y[MAX_FINGERS];
static uint32_t reports, touches, bad_reports, shown;

static uint32_t
picture_x(uint32_t x)
{
	if (x >= PANEL_W) {
		x = PANEL_W - 1;
	}
	return x * 65535 / (PANEL_W - 1);
}

static uint32_t
picture_y(uint32_t y)
{
	if (y >= PANEL_H) {
		y = PANEL_H - 1;
	}
	return y * 65535 / (PANEL_H - 1);
}

/*
 * A report turned into what /dev/redwood-touch wants: a landing and a lifting of its own rather
 * than a state to be compared, and a position across the panel. The report names every finger it
 * can see, lifted ones included, so a finger that is not named has lifted without saying so.
 */
static void
report(const uint8_t *buffer)
{
	bool seen[MAX_FINGERS] = { false };
	uint32_t points = buffer[1] & 0x0f;

	if ((buffer[0] & 0xf0) != REPORT_HEADER || points > MAX_FINGERS) {
		bad_reports++;
		return;
	}
	for (uint32_t i = 0; i < points; i++) {
		const uint8_t *point = buffer + 2 + i * ONE_POINT;
		uint32_t event = point[0] >> 6;
		uint32_t x = ((uint32_t)(point[0] & 0x0f) << 8) | point[1];
		uint32_t y = ((uint32_t)(point[2] & 0x0f) << 8) | point[3];
		uint32_t which = point[2] >> 4;

		if (which >= MAX_FINGERS) {
			continue;
		}
		if (event == 1) {               /* lifted */
			if (down[which]) {
				redwood_touch_post(REDWOOD_TOUCH_UP, which,
				    picture_x(last_x[which]), picture_y(last_y[which]));
				down[which] = false;
			}
			continue;
		}
		seen[which] = true;
		last_x[which] = x;
		last_y[which] = y;
		touches++;
		redwood_touch_post(down[which] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, which,
		    picture_x(x), picture_y(y));
		down[which] = true;
		if (shown < 8) {
			printf("fts: point %u at %u, %u (event %u), %u points\n",
			    which, x, y, event, points);
			shown++;
		}
	}
	for (unsigned which = 0; which < MAX_FINGERS; which++) {
		if (down[which] && !seen[which]) {
			redwood_touch_post(REDWOOD_TOUCH_UP, which,
			    picture_x(last_x[which]), picture_y(last_y[which]));
			down[which] = false;
		}
	}
	reports++;
}

/* ---- the thread that watches the line ---- */

static void
fts_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t id[2] = { 0, 0 }, version = 0, vendor = 0, power = 0;
	uint32_t poll_us = 4000, reset = 0;
	bool running = false;
	int status;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_reset", &reset, sizeof(reset));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 4000;
	}

	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	pins_init();
	if (reset) {
		printf("fts: resetting the chip on GPIO%u (its firmware, if it was running, is gone)\n", PIN_RST);
		chip_reset();
	}

	status = status_package();
	printf("fts: status 0x%02x; pins MI %u, CS %u, MO %u, CLK %u; reset GPIO%u, interrupt GPIO%u\n",
	    status, PIN_MI, PIN_CS, PIN_MO, PIN_CLK, PIN_RST, PIN_IRQ);

	if ((status & 0x81) == STATUS_APP) {
		if (read_reg(0xa3, &id[0]) && read_reg(0x9f, &id[1])) {
			printf("fts: chip ID 0x%02x%02x from the application\n", id[0], id[1]);
			if (read_reg(0xa6, &version) && read_reg(0xa8, &vendor)) {
				printf("fts: firmware 0x%02x, vendor 0x%02x\n", version, vendor);
			}
			if (read_reg(0xa5, &power)) {
				printf("fts: power mode 0x%02x%s\n", power, power == 0x03 ? " (asleep)" : "");
				if (power == 0x03 && write_reg(0xa5, 0x00)) {
					printf("fts: asked it to wake up\n");
				}
			}
			running = (id[0] == 0x87 && id[1] == 0x19);
		} else {
			printf("fts: the status says the application runs, but its ID does not read\n");
		}
	}

	if (!running) {
		if (boot_id(id)) {
			printf("fts: boot ID 0x%02x%02x (the running application would answer 0x8719)\n",
			    id[0], id[1]);
		} else {
			printf("fts: no boot ID either; the bus may be dead or the chip unpowered\n");
		}
		printf("fts: no firmware to report with -- the chip has to be brought up before this runs;\n"
		    "fts: leaving the pins alone and looking at the bus once every five seconds\n");
		for (unsigned tick = 1;; tick++) {
			delay(1000000);
			if (tick % 5 != 0) {
				continue;
			}
			status = status_package();
			if ((status & 0x81) == STATUS_APP && read_reg(0xa3, &id[0]) && read_reg(0x9f, &id[1])
			    && id[0] == 0x87 && id[1] == 0x19) {
				printf("fts: the application has appeared (chip ID 0x8719); reading touches\n");
				running = true;
				break;
			}
			if (tick % 30 == 0) {
				printf("fts: still nothing: status 0x%02x, %u packets, %u timeouts, %u CRC errors\n",
				    status, packets, timeouts, crc_errors);
			}
		}
	}

	printf("fts: the touch screen's interrupt line is looked at every %u us\n", poll_us);
	for (unsigned tick = 1;; tick++) {
		uint8_t buffer[REPORT_LEN];

		while (!pin_get(PIN_IRQ)) {
			if (!data_package(buffer, sizeof(buffer))) {
				if (bad_reports < 4) {
					printf("fts: a report did not arrive whole: %02x %02x %02x %02x "
					    "%02x %02x %02x %02x, %u bytes\n",
					    buffer[0], buffer[1], buffer[2], buffer[3],
					    buffer[4], buffer[5], buffer[6], buffer[7],
					    (unsigned)sizeof(buffer));
				}
				break;
			}
			report(buffer);
		}
		if (tick % (10 * (1000000 / poll_us)) == 0) {
			printf("fts: so far %u reports and %u finger positions, %u bad reports, "
			    "%u packets, %u timeouts, %u CRC errors; the interrupt line is %s\n",
			    reports, touches, bad_reports, packets, timeouts, crc_errors,
			    pin_get(PIN_IRQ) ? "idle" : "asserted");
		}
		delay(poll_us);
	}
}

void
redwood_fts_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	/* 3 is this phone's FocalTech; 1 is the other one's Goodix and 2 its TCM controller. */
	if (!PE_parse_boot_argn("touchscreen", &which, sizeof(which)) || which != 3) {
		return;
	}
	if (kernel_thread_start(fts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("fts: cannot start the thread that reads the touch screen\n");
	}
}