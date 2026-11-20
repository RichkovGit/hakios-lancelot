/*
 * The touch screen of the Redmi 9 (lancelot): a FocalTech FT8719P on SPI0 of the MT6768, brought
 * up before XNU starts.
 *
 * The controller has no flash. Its firmware lives in PRAM and is lost on every reset, so whoever
 * wants a working touch screen has to send it again -- Android's driver does it at probe, and this
 * does it on the way to Darwin, where the kernel (bsd/dev/redwood_fts.c) then only reads reports.
 *
 * What the vendor's driver does, in order (drivers/input/touchscreen/mediatek/FT8719P):
 *   power    the rail is "vtouch", the PMIC's VLDO28 at 2.8 V. Linux's regulator driver has it
 *            default-on; nothing here does, so it is switched on (and off first, for a clean start)
 *            through the PMIC wrapper.
 *   reset    GPIO92, active low, then 8 ms -- upgrade_setting_list[0].delay_init.
 *   boot id  0x55 wakes the ROM, 0xAA follows, and command 0x90 answers 0x87 0x19.
 *   download 0xAE packets -- address and length in the command, 4092 bytes of data after it -- and
 *            three milliseconds between them, because this chip is not asked whether it supports
 *            fast download. The image's own header says how much goes: code_len at 0x100.
 *   verify   ECC over the whole PRAM range: 0xCC starts it, 0xCE says when it is done, 0xCD gives
 *            the chip's value to compare with the one computed here.
 *   run      0x08, then the chip answers status 0x01 and its ID at 0xA3/0x9F.
 *
 * The bus is driven by hand, mode 1 (clock idles low, data changes on the rising edge and is read
 * on the falling one -- SPI_MODE_1, focaltech_core.c sets it): GPIO32 is data in, 33 the chip
 * select, 34 the data out, 35 the clock, all at function 0, with input enable and the pulls set
 * in the pads' own io-config group (see fts_pads below -- a GPIO read is zero without it). The
 * controller at 0x1100a000 would need its clock mux and FIFO set up first, and the firmware goes
 * out at a few hundred kilobytes a second either way.
 *
 * The firmware itself comes from the phone's own /vendor/firmware through tools/mkftsfw.py into
 * ftsfw.h, which is generated and never committed.
 *
 * Register layouts: Linux drivers/pinctrl/mediatek/pinctrl-mtk-mt6768.h for the pins,
 * include/linux/mfd/mt6358/registers.h for VLDO28, and focaltech_spi.c / focaltech_flash.c for
 * the chip's own language.
 */
#ifndef FTS_H
#define FTS_H

#include <stdint.h>
#include "mtkpwrap.h"

#if defined(__has_include)
#  if __has_include("ftsfw.h")
#    include "ftsfw.h"
#    define FTS_HAVE_FIRMWARE 1
#  endif
#endif
#ifndef FTS_HAVE_FIRMWARE
#  define FTS_HAVE_FIRMWARE 0
#endif

#if FTS_HAVE_FIRMWARE

/* ---- the pins ---- */

/*
 * The MT6768 pin controller: a bank is 32 pins, a MODE register covers eight with four bits each,
 * and every bank has a pair of set/clear direction and output registers. GPIO32-35 are in bank 1,
 * the reset line (GPIO92) in bank 2.
 */
#define FTS_GPIO_BASE        0x10005000ULL
#define FTS_DIR_SET(bank)    (0x004 + 0x10 * (bank))
#define FTS_DIR_CLR(bank)    (0x008 + 0x10 * (bank))
#define FTS_DOUT_SET(bank)   (0x104 + 0x10 * (bank))
#define FTS_DOUT_CLR(bank)   (0x108 + 0x10 * (bank))
#define FTS_DIN(bank)        (0x200 + 0x10 * (bank))
#define FTS_MODE(pin)        (0x300 + 0x10 * ((pin) / 8))

#define FTS_PIN_MI           32      /* SPI0_MI: the chip drives it */
#define FTS_PIN_CS           33      /* SPI0_CSB, low for every package */
#define FTS_PIN_MO           34      /* SPI0_MO */
#define FTS_PIN_CLK          35      /* SPI0_CLK */
#define FTS_PIN_RST          92      /* the chip's reset, active low */
#define FTS_PIN_IRQ          1       /* low while the chip has something to say */

/*
 * The pads themselves: the pin controller's other half. Input enable, schmitt trigger and the
 * pulls are not in the GPIO banks but in the io-config group the pads physically sit in -- here
 * iocfg_rm (0x10002800), and the bit numbers come from pinctrl-mtk-mt6768.h's
 * mt6768_pin_{ies,smt,pu}_range[].
 *
 * Android never needs these: its touch node has no pinctrl states (fts_spi_parse_dt says
 * "Cannot find touch pc" and carries on), so the pads stay as the device tree's gpio_init_default
 * left them -- function 1, input enable 0 -- and the SPI controller reads the pad through its own
 * input path, which the pin controller does not gate. A GPIO read is gated: Linux's
 * mtk_gpio_direction_input() sets IES for exactly that reason, and without it the DIN register
 * reads a flat zero, which is what the run before this one saw (status 0x00, never a boot ID).
 */
#define FTS_IOCFG_RM         0x10002800ULL
#define FTS_IOCFG_BIT(off, bit, on) \
	wr32(FTS_IOCFG_RM + (off), (on) ? (rd32(FTS_IOCFG_RM + (off)) | (1u << (bit))) \
	                                : (rd32(FTS_IOCFG_RM + (off)) & ~(1u << (bit))))

struct fts_pad {
	uint8_t pin;
	uint16_t ies_off;
	uint8_t ies_bit;              /* input enable: without it DIN is always 0 */
	uint16_t smt_off;
	uint8_t smt_bit;              /* schmitt trigger, for a clock that is driven by hand */
	uint16_t pu_off;
	uint8_t pu_bit;               /* pull-up, so the chip's idle line has a level */
};

static const struct fts_pad fts_pads[] = {
	{ FTS_PIN_IRQ, 0x60, 8,  0x100, 5,  0xa0, 8  },
	{ FTS_PIN_MI,  0x60, 31, 0x100, 18, 0xa0, 27 },
	{ FTS_PIN_CS,  0x60, 30, 0x100, 18, 0xa0, 26 },
	{ FTS_PIN_MO,  0x70, 0,  0x100, 18, 0xa0, 28 },
	{ FTS_PIN_CLK, 0x60, 29, 0x100, 18, 0xa0, 25 },
};

static void fts_pin_set(uint32_t pin, int high)
{
	wr32(FTS_GPIO_BASE + (high ? FTS_DOUT_SET(pin / 32) : FTS_DOUT_CLR(pin / 32)),
	    1u << (pin % 32));
}

static int fts_pin_get(uint32_t pin)
{
	return (int)((rd32(FTS_GPIO_BASE + FTS_DIN(pin / 32)) >> (pin % 32)) & 1u);
}

/* Long enough for a pad to settle; one register read is already most of a microsecond. */
static void fts_pin_settle(void)
{
	(void)rd32(FTS_GPIO_BASE + FTS_DIN(0));
	(void)rd32(FTS_GPIO_BASE + FTS_DIN(0));
}

static void fts_pin_function(uint32_t pin, uint32_t function)
{
	uint32_t at = FTS_MODE(pin), mode = rd32(FTS_GPIO_BASE + at);
	uint32_t shift = (pin % 8) * 4;

	wr32(FTS_GPIO_BASE + at, (mode & ~(0xfu << shift)) | ((function & 0xfu) << shift));
}

static void fts_pins_init(void)
{
	uint32_t ies_before = rd32(FTS_IOCFG_RM + 0x60);
	uint32_t din_before = rd32(FTS_GPIO_BASE + FTS_DIN(0)) & 0x3u;

	/* plain GPIOs, function 0: the SPI controller is not used */
	for (uint32_t pin = FTS_PIN_MI; pin <= FTS_PIN_CLK; pin++) {
		fts_pin_function(pin, 0);
	}
	fts_pin_function(FTS_PIN_RST, 0);
	fts_pin_function(FTS_PIN_IRQ, 0);

	/*
	 * What the preloader left behind, before any of this is changed: if the IES bits are
	 * already set the reads that follow mean something, and if they are not, that alone
	 * explains a bus that answers zero.
	 */
	out("touch: pads were IES ");
	out_hex(ies_before);
	out(" (MI 32 bit 31, IRQ 1 bit 8), DIN0 ");
	out_hex(din_before);
	out("\n");

	for (unsigned i = 0; i < sizeof(fts_pads) / sizeof(fts_pads[0]); i++) {
		const struct fts_pad *pad = &fts_pads[i];

		FTS_IOCFG_BIT(pad->ies_off, pad->ies_bit, 1);
		FTS_IOCFG_BIT(pad->smt_off, pad->smt_bit, 1);
		FTS_IOCFG_BIT(pad->pu_off, pad->pu_bit, 1);
	}

	wr32(FTS_GPIO_BASE + FTS_DIR_SET(FTS_PIN_CS / 32),
	    1u << (FTS_PIN_CS % 32));
	wr32(FTS_GPIO_BASE + FTS_DIR_SET(FTS_PIN_MO / 32),
	    1u << (FTS_PIN_MO % 32));
	wr32(FTS_GPIO_BASE + FTS_DIR_SET(FTS_PIN_CLK / 32),
	    1u << (FTS_PIN_CLK % 32));
	wr32(FTS_GPIO_BASE + FTS_DIR_SET(FTS_PIN_RST / 32),
	    1u << (FTS_PIN_RST % 32));
	wr32(FTS_GPIO_BASE + FTS_DIR_CLR(FTS_PIN_MI / 32),
	    1u << (FTS_PIN_MI % 32));
	wr32(FTS_GPIO_BASE + FTS_DIR_CLR(FTS_PIN_IRQ / 32),
	    1u << (FTS_PIN_IRQ % 32));

	fts_pin_set(FTS_PIN_CLK, 0);    /* mode 1: the clock idles low */
	fts_pin_set(FTS_PIN_CS, 1);
	fts_pin_set(FTS_PIN_RST, 1);    /* out of reset */
	__asm__ volatile("dsb sy");
	fts_pin_settle();

	out("touch: pads now IES ");
	out_hex(rd32(FTS_IOCFG_RM + 0x60));
	out(", MI ");
	out(fts_pin_get(FTS_PIN_MI) ? "high" : "low");
	out(", IRQ ");
	out(fts_pin_get(FTS_PIN_IRQ) ? "high" : "low");
	out(", CS ");
	out(fts_pin_get(FTS_PIN_CS) ? "high" : "low");
	out("\n");
}

static void fts_chip_reset(void)
{
	fts_pin_set(FTS_PIN_RST, 0);
	dbg_delay_ms(2);
	fts_pin_set(FTS_PIN_RST, 1);
	dbg_delay_ms(8);                /* upgrade_setting_list[0].delay_init */
}

/* ---- one package on the bus ---- */

#define FTS_FIRST_BYTE_US    30     /* DELAY_AFTER_FIRST_BYTE */
#define FTS_CS_HIGH_US       150    /* CS_HIGH_DELAY */

/*
 * One package on the wire. The four-byte header in the buffers is only *one* byte of wire: the
 * package type. fts_spi_transfer() in focaltech_spi.c builds the message out of two transfers --
 * tx[0] alone, then tx[4 .. len-1] -- so the three bytes at offsets 1..3 are buffer padding and are
 * never clocked out. Clocking them (as this did until run #9) puts every command three bytes late:
 * the chip hears `C0 00 00 00 01 55` instead of `C0 01 55`, ignores it, and leaves the data line to
 * whatever the pads do, which is the flat 0x00 status and the 0xef data answer of run #8.
 *
 * The chip select is low for the whole package and high for 150 us after it, and the type byte sits
 * alone for 30 us -- both as focaltech_spi.c has them.
 */
static void fts_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint32_t wire = len > 4 ? len - 3 : 1;

	fts_pin_set(FTS_PIN_CS, 0);
	fts_pin_settle();

	for (uint32_t at_wire = 0; at_wire < wire; at_wire++) {
		uint32_t at = at_wire == 0 ? 0 : at_wire + 3;
		uint8_t out = tx ? tx[at] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			fts_pin_set(FTS_PIN_MO, (out >> bit) & 1);
			fts_pin_set(FTS_PIN_CLK, 1);
			fts_pin_settle();
			fts_pin_set(FTS_PIN_CLK, 0);            /* the edge that is read */
			fts_pin_settle();
			in = (uint8_t)((in << 1) | (fts_pin_get(FTS_PIN_MI) ? 1 : 0));
		}
		if (rx) {
			rx[at] = in;
		}
		if (at_wire == 0) {
			dbg_delay_us(FTS_FIRST_BYTE_US);
		}
	}
	fts_pin_set(FTS_PIN_CS, 1);
	fts_pin_settle();
	dbg_delay_us(FTS_CS_HIGH_US);
}

/* ---- the chip's language ---- */

#define FTS_PKG_STATUS       0x05
#define FTS_PKG_COMMAND      0xC0
#define FTS_PKG_DATA         0x3F

#define FTS_CTRL_WRITE       0x00
#define FTS_CTRL_READ        0xA0    /* READ_CMD (0x80) | DATA_CRC_EN (0x20) */

#define FTS_BUSY_TRIES       100     /* BUSY_QUERY_TIMEOUT */
#define FTS_BUSY_DELAY_US    150     /* BUSY_QUERY_DELAY */

/*
 * Everything that is not a report goes through three packages: a status read to see whether the
 * chip is free, a command, and -- when there is data -- a data package. In boot mode the status
 * reads 0x00 when free, in the running application 0x01; bit 7 is busy either way.
 */
static uint8_t fts_status(void)
{
	uint8_t tx[5] = { FTS_PKG_STATUS, 0, 0, 0, 0 }, rx[5];

	fts_spi_xfer(tx, rx, sizeof(tx));
	return rx[4];
}

/* Free in either mode: the boot ROM and the application idle differently, both are a yes here. */
static int fts_wait_free(void)
{
	for (unsigned try = 0; try < FTS_BUSY_TRIES; try++) {
		uint8_t status;

		dbg_delay_us(FTS_BUSY_DELAY_US);
		status = fts_status();
		if ((status & 0x81) == 0x00 || (status & 0x81) == 0x01) {
			return (int)status;
		}
	}
	return -1;
}

static void fts_command(uint8_t ctrl, const uint8_t *command, uint32_t len)
{
	uint8_t tx[16] = { FTS_PKG_COMMAND, 0, 0, 0, 0 };

	tx[4] = (uint8_t)(ctrl | (len & 0x0f));
	for (uint32_t i = 0; i < len; i++) {
		tx[5 + i] = command[i];
	}
	fts_spi_xfer(tx, 0, 5 + len);
}

/*
 * The largest data package the driver sends without fast download: FTS_FLASH_PACKET_LENGTH_SPI_LOW,
 * 4 * 1024 - 4. One buffer for the package (its four-byte header and the data) is all the loader
 * gives this.
 */
#define FTS_PACKET_MAX       4092

static uint8_t fts_packet[4 + FTS_PACKET_MAX];

static uint16_t fts_crc(const uint8_t *data, uint32_t len)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < len; i++) {
		crc = (uint16_t)(crc ^ data[i]);
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static int fts_data_write(const uint8_t *data, uint32_t len)
{
	if (len > FTS_PACKET_MAX) {
		return -1;
	}
	fts_packet[0] = FTS_PKG_DATA;
	fts_packet[1] = 0;
	fts_packet[2] = 0;
	fts_packet[3] = 0;
	for (uint32_t i = 0; i < len; i++) {
		fts_packet[4 + i] = data[i];
	}
	fts_spi_xfer(fts_packet, 0, 4 + len);
	return 0;
}

/* A read: the answer and its two bytes of CRC; the chip owns them, so what goes out is zeros. */
static int fts_data_read(uint8_t *data, uint32_t len)
{
	uint16_t want, got;

	if (len + 6 > sizeof(fts_packet)) {
		return -1;
	}
	fts_packet[0] = FTS_PKG_DATA;
	fts_packet[1] = 0;
	fts_packet[2] = 0;
	fts_packet[3] = 0;
	for (uint32_t i = 0; i < len + 2; i++) {
		fts_packet[4 + i] = 0;
	}
	fts_spi_xfer(fts_packet, fts_packet, 4 + len + 2);
	for (uint32_t i = 0; i < len; i++) {
		data[i] = fts_packet[4 + i];
	}
	want = (uint16_t)(fts_packet[4 + len] | (fts_packet[5 + len] << 8));
	got = fts_crc(fts_packet + 4, len);
	return want == got ? 0 : -1;
}

/* ---- boot mode: the ROM's own protocol ---- */

static int fts_boot_write(const uint8_t *command, uint32_t cmdlen,
    const uint8_t *data, uint32_t datalen)
{
	if (fts_wait_free() < 0) {
		return -1;
	}
	fts_command(FTS_CTRL_WRITE, command, cmdlen);
	if (data && datalen) {
		if (fts_wait_free() < 0) {
			return -1;
		}
		return fts_data_write(data, datalen);
	}
	return 0;
}

static int fts_boot_read(const uint8_t *command, uint32_t cmdlen,
    uint8_t *data, uint32_t datalen)
{
	if (fts_wait_free() < 0) {
		return -1;
	}
	fts_command(FTS_CTRL_READ, command, cmdlen);
	if (fts_wait_free() < 0) {
		return -1;
	}
	return fts_data_read(data, datalen);
}

/*
 * 0x55 wakes the ROM, 0xAA follows it, and a moment later 0x90 answers two bytes of ID. This is
 * fts_read_bootid(), which is the call Android's probe uses -- and what it gets on this phone is
 * 0x87 0x19 before any firmware has been downloaded.
 */
static int fts_boot_id(uint8_t id[2])
{
	uint8_t start[2] = { 0x55, 0xaa };
	uint8_t read[4] = { 0x90, 0, 0, 0 };

	if (fts_wait_free() < 0) {
		return -1;
	}
	fts_command(FTS_CTRL_WRITE, start, 1);
	if (fts_wait_free() < 0) {
		return -1;
	}
	if (fts_data_write(&start[1], 1) < 0) {
		return -1;
	}
	dbg_delay_ms(12);               /* FTS_CMD_START_DELAY */

	if (fts_wait_free() < 0) {
		return -1;
	}
	fts_command(FTS_CTRL_READ, read, sizeof(read));
	if (fts_wait_free() < 0) {
		return -1;
	}
	if (fts_data_read(id, 2) < 0) {
		/*
		 * What actually came back on the data package: two bytes and the chip's CRC. All
		 * zeros means the line never moved, all 0xff means it never came down, and
		 * anything else means the clock and the wire are working and only the timing is
		 * wrong.
		 */
		out("touch: boot ID answer");
		for (unsigned i = 0; i < 4; i++) {
			out(" ");
			out_hex(fts_packet[4 + i]);
		}
		out(" (2 data, 2 crc)\n");
		return -1;
	}
	return 0;
}

static int fts_wait_boot_id(uint8_t id[2])
{
	for (int attempt = 1; attempt <= 3; attempt++) {
		fts_chip_reset();
		out("touch: after reset ");
		out_dec((uint32_t)attempt);
		out(", IRQ ");
		out(fts_pin_get(FTS_PIN_IRQ) ? "high" : "low");
		out(", status ");
		out_hex((uint32_t)fts_status());
		out("\n");
		for (int try = 0; try < 10; try++) {
			if (fts_boot_id(id) == 0 && !(id[0] == 0 && id[1] == 0) &&
			    !(id[0] == 0xff && id[1] == 0xff)) {
				return 0;
			}
			dbg_delay_ms(8);
		}
		out("touch: no boot ID after reset, attempt ");
		out_dec((uint32_t)attempt);
		out(" of 3\n");
	}
	return -1;
}

/* ---- the firmware ---- */

/*
 * One 0xAE command carries the address and the length, and the data package that follows carries
 * the bytes. Without fast download the chip wants three milliseconds between packets.
 */
static int fts_firmware_write(const uint8_t *fw, uint32_t len)
{
	uint8_t command[6];

	for (uint32_t offset = 0; offset < len; offset += FTS_PACKET_MAX) {
		uint32_t chunk = len - offset;
		int result;

		if (chunk > FTS_PACKET_MAX) {
			chunk = FTS_PACKET_MAX;
		}
		command[0] = 0xae;
		command[1] = (uint8_t)(offset >> 16);
		command[2] = (uint8_t)(offset >> 8);
		command[3] = (uint8_t)offset;
		command[4] = (uint8_t)(chunk >> 8);
		command[5] = (uint8_t)chunk;

		if (offset % (16 * FTS_PACKET_MAX) == 0) {
			out("touch: PRAM at ");
			out_hex(offset);
			out(" of ");
			out_hex(len);
			out("\n");
		}
		result = fts_boot_write(command, sizeof(command), fw + offset, chunk);
		if (result < 0) {
			out("touch: packet at ");
			out_hex(offset);
			out(" was refused\n");
			return -1;
		}
		dbg_delay_ms(3);
	}
	return 0;
}

/*
 * AL2_FCS_COEF, as fts_ecc_cal_host(): the image goes in as sixteen-bit words, high byte first,
 * and each word is run through sixteen rounds of the 0x8408 polynomial.
 */
static uint16_t fts_ecc_host(const uint8_t *data, uint32_t len)
{
	uint16_t ecc = 0;

	for (uint32_t i = 0; i + 1 < len; i += 2) {
		ecc = (uint16_t)(ecc ^ ((uint16_t)(data[i] << 8) | data[i + 1]));
		for (unsigned bit = 0; bit < 16; bit++) {
			ecc = (ecc & 1) ? (uint16_t)((ecc >> 1) ^ 0x8408) : (uint16_t)(ecc >> 1);
		}
	}
	return ecc;
}

/*
 * 0xCC asks the ROM to compute the same over PRAM, 0xCE is polled until it says 0x00 (eccok_val),
 * and 0xCD gives its answer, two bytes high first.
 */
static int fts_ecc_check(uint32_t address, uint32_t len, uint16_t host)
{
	uint8_t command[7];
	uint8_t value = 0xff, chip[2];
	uint16_t ecc;
	int done = 0;

	command[0] = 0xcc;
	command[1] = (uint8_t)(address >> 16);
	command[2] = (uint8_t)(address >> 8);
	command[3] = (uint8_t)address;
	command[4] = (uint8_t)(len >> 16);
	command[5] = (uint8_t)(len >> 8);
	command[6] = (uint8_t)len;
	if (fts_boot_write(command, sizeof(command), 0, 0) < 0) {
		out("touch: the ECC command was refused\n");
		return -1;
	}
	dbg_delay_ms(2);

	for (unsigned try = 0; try < 100; try++) {
		uint8_t poll = 0xce;

		if (fts_boot_read(&poll, 1, &value, 1) < 0) {
			out("touch: the ECC poll was refused\n");
			return -1;
		}
		if (value == 0x00) {
			done = 1;
			break;
		}
		dbg_delay_ms(1);
	}
	if (!done) {
		out("touch: the chip never finished its ECC (last ");
		out_hex(value);
		out(")\n");
		return -1;
	}

	{
		uint8_t read = 0xcd;

		if (fts_boot_read(&read, 1, chip, sizeof(chip)) < 0) {
			out("touch: the ECC value was refused\n");
			return -1;
		}
	}
	ecc = (uint16_t)((chip[0] << 8) | chip[1]);
	out("touch: ECC from the chip ");
	out_hex(ecc);
	out(", from here ");
	out_hex(host);
	if (ecc != host) {
		out(" -- they differ\n");
		return -1;
	}
	out("\n");
	return 0;
}

/* ---- the running application ---- */

/* In application mode a command carries the register and the number of bytes wanted after it. */
static int fts_reg_read(uint8_t address, uint8_t *value)
{
	uint8_t command[3] = { address, 0, 1 };

	if (fts_wait_free() < 0) {
		return -1;
	}
	fts_command(FTS_CTRL_READ, command, sizeof(command));
	if (fts_wait_free() < 0) {
		return -1;
	}
	return fts_data_read(value, 1);
}

static int fts_app_id(uint8_t id[2])
{
	if (fts_reg_read(0xa3, &id[0]) < 0) {
		return -1;
	}
	return fts_reg_read(0x9f, &id[1]);
}

/* ---- vldo28: the chip's supply ---- */

#define MT6358_LDO_VLDO28_CON0_0  0x1d32   /* bit 0: on */
#define MT6358_LDO_VLDO28_CON1    0x1d42   /* bit 0: really on */
#define MT6358_VLDO28_ANA_CON0    0x1e3c   /* bits 9..8: VOSEL, 1 = 2.8 V */

/*
 * Android's regulator driver has the rail default-on and never turns it off; the preloader may or
 * may not have left it on. Switch it off and on again at 2.8 V: a rail in an unknown state is the
 * one thing a chip that answers nothing could be complaining about.
 */
static void fts_power_on(void)
{
	uint16_t con = 0, con1 = 0, ana = 0;

	if (!pwrap_read(MT6358_LDO_VLDO28_CON0_0, &con)) {
		out("touch: the PMIC wrapper does not answer; vldo28 not looked at\n");
		return;
	}
	pwrap_read(MT6358_VLDO28_ANA_CON0, &ana);
	pwrap_read(MT6358_LDO_VLDO28_CON1, &con1);

	out("touch: vldo28 was ");
	out(con & 1 ? "on" : "off");
	out(", VOSEL ");
	out_dec((ana >> 8) & 3);
	out(", status ");
	out_hex(con1 & 1);

	pwrap_write(MT6358_LDO_VLDO28_CON0_0, (uint16_t)(con & ~1u));
	dbg_delay_ms(5);
	pwrap_write(MT6358_VLDO28_ANA_CON0, (uint16_t)((ana & ~(3u << 8)) | (1u << 8)));
	pwrap_write(MT6358_LDO_VLDO28_CON0_0, (uint16_t)(con | 1u));
	dbg_delay_ms(30);               /* the chip's own power-on reset, run to completion */

	pwrap_read(MT6358_LDO_VLDO28_CON0_0, &con);
	pwrap_read(MT6358_LDO_VLDO28_CON1, &con1);
	out("; now ");
	out(con & 1 ? "on" : "still off");
	out(" at 2.8 V, status ");
	out_hex(con1 & 1);
	out("\n");
}

/* ---- the whole sequence ---- */

static uint8_t fts_bring_up(void)
{
	uint8_t id[2] = { 0, 0 };
	uint16_t host_ecc;
	int status;

	out("touch: FT8719P on SPI0 (MI 32, CS 33, MO 34, CLK 35), reset 92, interrupt 1\n");
	fts_power_on();
	fts_pins_init();

	status = fts_status();
	out("touch: status ");
	out_hex((uint32_t)status);
	out(", interrupt line ");
	out(fts_pin_get(FTS_PIN_IRQ) ? "high" : "low");
	out("\n");

	if (fts_wait_boot_id(id) < 0) {
		out("touch: the chip says nothing in boot mode either; is its rail really on?\n");
		return 0;
	}
	out("touch: boot ID ");
	out_hex(id[0]);
	out(" ");
	out_hex(id[1]);
	if (id[0] != 0x87 || id[1] != 0x19) {
		out(" -- not the 0x87 0x19 this phone's chip should say\n");
		return 0;
	}
	out("\n");

	out("touch: sending ");
	out_dec(FTS_FW_SIZE);
	out(" bytes of firmware (");
	out_dec((FTS_FW_SIZE + FTS_PACKET_MAX - 1) / FTS_PACKET_MAX);
	out(" packets)\n");
	if (fts_firmware_write(FTS_FW, FTS_FW_SIZE) < 0) {
		out("touch: the firmware did not go in\n");
		return 0;
	}

	host_ecc = fts_ecc_host(FTS_FW, FTS_FW_SIZE);
	if (fts_ecc_check(0x000000, FTS_FW_SIZE, host_ecc) < 0) {
		out("touch: the firmware is not what the chip received\n");
		return 0;
	}

	{
		uint8_t start = 0x08;           /* FTS_ROMBOOT_CMD_START_APP */

		if (fts_boot_write(&start, 1, 0, 0) < 0) {
			out("touch: the start command was refused\n");
			return 0;
		}
	}
	dbg_delay_ms(50);

	if (fts_app_id(id) < 0) {
		out("touch: the application does not answer after the start command\n");
		return 0;
	}
	out("touch: running, chip ID ");
	out_hex(id[0]);
	out(" ");
	out_hex(id[1]);
	if (id[0] != 0x87 || id[1] != 0x19) {
		out(" -- not 0x87 0x19, so the kernel will find nothing to read\n");
		return 0;
	}
	out(": the kernel only has to read reports from here\n");
	return 0x01;                            /* TCM_MODE_APPLICATION */
}

#else /* FTS_HAVE_FIRMWARE */

static uint8_t fts_bring_up(void)
{
	out("touch: FT8719P: no firmware in this build (tools/mkftsfw.py); bring-up skipped\n");
	return 0;
}

#endif /* FTS_HAVE_FIRMWARE */

#endif /* FTS_H */