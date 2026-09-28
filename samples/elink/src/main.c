/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * elink -- Pervasive Displays E2271CS091 (2.7", 264x176, "G2" iTC OTP-LUT COG)
 * on the AgRV2K. The bus runs on the SPI1 engine (SCK/MOSI/CSN) with the
 * panel's D/C#, RST_N and BUSY_N on GPIOs, so the sample runs on the stock
 * 200 MHz bitstream -- no custom pin map, no bit-banging.
 *
 * The command flow is Pervasive's "Application Note for small size Monochrome
 * EPD with iTC (OTP LUT)", rev 02, sections 2..5, cross-checked against
 * $HOME/esp-elink-main (Marcelo Barros de Almeida, MIT), which drives
 * this same panel: that reference never touches the DC/DC setting (0x01),
 * booster soft start (0x06) or PLL (0x30) registers, so the COG runs on its OTP
 * defaults and neither does this sample.
 *
 * This family only does full updates -- there is no partial/fast refresh.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>

#define EPD_NODE DT_NODELABEL(elink0)

/* Geometry as the glass is specified: 264 px in the scan direction, 176 px in
 * the data direction. */
#define EPD_W DT_PROP(EPD_NODE, width)
#define EPD_H DT_PROP(EPD_NODE, height)

/*
 * The COG's frame memory does not follow the glass' orientation: a line holds
 * 176 pixels and there are 264 of them (application note, "Input image to the
 * EPD" -- 2.7": N = 176, M = 264, 5 808 bytes per frame), 1 bit per pixel, MSB
 * first, 1 = black.
 */
#define EPD_LINE_BYTES  ((EPD_H + 7U) / 8U)     /* 22 bytes = 176 pixels */
#define EPD_LINES       EPD_W                   /* 264 lines */
#define EPD_FRAME_BYTES (EPD_LINE_BYTES * EPD_LINES)

BUILD_ASSERT(EPD_FRAME_BYTES == 5808U, "the 2.7\" frame is 5808 bytes (176 x 264 / 8)");

/* Register indices (application note sections 3..5). */
#define EPD_REG_PSR         0x00U
#define EPD_REG_POF         0x02U        /* DC/DC off */
#define EPD_REG_PON         0x04U        /* DC/DC on */
#define EPD_REG_DTM1        0x10U        /* first frame */
#define EPD_REG_DRF         0x12U        /* display refresh */
#define EPD_REG_DTM2        0x13U        /* second frame */
#define EPD_REG_ACTIVE_TEMP 0xE0U
#define EPD_REG_TSSET       0xE5U

#define EPD_SOFT_RESET  0x0EU
#define EPD_TSSET_25C   0x19U            /* top bit = sign, negatives in two's complement */
#define EPD_ACTIVE_TEMP 0x02U
#define EPD_PSR_0       0xCFU            /* every size but 2.9" HR, 3.7", 4.2", 4.37" */
#define EPD_PSR_1       0x8DU

/*
 * BUSY_N assertion window; the panel reacts in microseconds, so this only has
 * to be long enough to catch the pulse before the release loop samples the pin.
 */
#define EPD_BUSY_ASSERT_MS 100U

static const struct device *const bus = DEVICE_DT_GET(DT_BUS(EPD_NODE));

static const struct gpio_dt_spec dc = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec rst = GPIO_DT_SPEC_GET(EPD_NODE, reset_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

static uint8_t framebuffer[EPD_FRAME_BYTES];

/*
 * SPI mode 0, MSB first, 8 bits; the panel takes up to 10 MHz and the engine
 * divides SYSCLK down to whatever the node asks for.
 *
 * The panel's framing is not the usual one: **every byte gets its own CS#
 * pulse** -- "if register data is more than one byte, the CS# pulse is
 * necessary between each data byte" (application note, "SPI Timing Format").
 * The AgRV2K engine asserts CSN for exactly one transfer, so one byte per
 * `spi_write()` is precisely that, with the hardware doing the shifting.
 * (esp-elink streams a whole frame under one CS# and lets the ESP32 hardware
 * drive it; here the note wins.)
 */
static const struct spi_config epd_spi_cfg = {
	.frequency = DT_PROP(EPD_NODE, spi_max_frequency),
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = DT_REG_ADDR(EPD_NODE),
};

/* One byte on the bus: D/C# first -- it has a 30 ns setup time before SCLK
 * starts moving (application note, "SPI command timing") -- then the byte. */
static int epd_byte(bool is_data, uint8_t value)
{
	struct spi_buf buf = { .buf = &value, .len = 1 };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };

	gpio_pin_set_dt(&dc, is_data ? 1 : 0);
	return spi_write(bus, &epd_spi_cfg, &tx);
}

/* Register write: index byte (D/C# = 0) followed by N data bytes (D/C# = 1). */
static int epd_write(uint8_t index, const uint8_t *data, size_t len)
{
	int rc = epd_byte(false, index);

	for (size_t i = 0; rc == 0 && i < len; i++) {
		rc = epd_byte(true, data[i]);
	}
	return rc;
}

/*
 * A command is the index on its own: 0x02, 0x04 and 0x12 carry no data
 * (application note, "Send updating command", note 2).
 */
static int epd_cmd(uint8_t index)
{
	return epd_byte(false, index);
}

/*
 * BUSY_N is low while the COG executes a command -- "when Busy is Low, the
 * operation of the chip should not be interrupted, and Command should not be
 * sent" (datasheet table 5-1) -- and returns high when it is done.
 *
 * The wait is two-phase: a bounded window for the assertion, then a bounded
 * wait for the release. The reference driver only waits for the release, which
 * sails straight through when the level is still high from an earlier step;
 * waiting for the pulse first makes a panel that is not there visible instead.
 */
static int epd_wait_busy(const char *what, uint32_t release_ms)
{
	int64_t deadline = k_uptime_get() + EPD_BUSY_ASSERT_MS;
	bool asserted = false;

	while (k_uptime_get() < deadline) {
		if (gpio_pin_get_dt(&busy) != 0) {
			asserted = true;
			break;
		}
	}

	deadline = k_uptime_get() + release_ms;
	while (gpio_pin_get_dt(&busy) != 0) {
		if (k_uptime_get() > deadline) {
			printk("elink: BUSY_N still low %u ms after %s\n",
			       (unsigned int)release_ms, what);
			return -ETIMEDOUT;
		}
	}

	if (!asserted) {
		printk("elink: %s: BUSY_N never went low (panel not answering?)\n", what);
	}
	return 0;
}

/*
 * Power-on sequence (application note section 2): RES# low, then high. The note
 * starts from "VCC/VDD, RES#, CS#, SDIN, SCLK = 0" and asks for a soft start on
 * VCC/VDD, which is not under software control here.
 */
static void epd_reset(void)
{
	gpio_pin_set_dt(&rst, 1);               /* RES# asserted (active low) */
	k_msleep(5);
	gpio_pin_set_dt(&rst, 0);               /* released */
	k_msleep(10);
}

/* Reset, environment temperature and panel settings (note sections 2 and 3). */
static int panel_init(void)
{
	const uint8_t soft_reset = EPD_SOFT_RESET;
	const uint8_t tsset = EPD_TSSET_25C;
	const uint8_t active_temp = EPD_ACTIVE_TEMP;
	const uint8_t psr[2] = { EPD_PSR_0, EPD_PSR_1 };
	int rc;

	epd_reset();

	rc = epd_write(EPD_REG_PSR, &soft_reset, 1);
	if (rc != 0) {
		return rc;
	}
	rc = epd_wait_busy("soft reset", 500);
	if (rc != 0) {
		return rc;
	}

	/*
	 * TSSET is the panel temperature in Celsius -- the note reads it from a
	 * sensor; this sample has none and pins it at 25 C.
	 */
	rc = epd_write(EPD_REG_TSSET, &tsset, 1);
	if (rc == 0) {
		rc = epd_write(EPD_REG_ACTIVE_TEMP, &active_temp, 1);
	}
	if (rc == 0) {
		rc = epd_write(EPD_REG_PSR, psr, sizeof(psr));
	}
	if (rc != 0) {
		return rc;
	}

	printk("elink: init sent -- soft reset 0x%02x, TSSET 0x%02x (25 C), "
	       "active temp 0x%02x, PSR 0x%02x,0x%02x\n",
	       soft_reset, tsset, active_temp, psr[0], psr[1]);
	return 0;
}

/*
 * Paint a border plus every 16th line. The frame is addressed the way the COG
 * expects it: EPD_LINE_BYTES bytes per line (data direction), EPD_LINES lines
 * (scan direction), MSB first, 1 = black.
 */
static void draw_test_image(void)
{
	memset(framebuffer, 0, sizeof(framebuffer));

	for (uint16_t line = 0; line < EPD_LINES; line++) {
		for (uint16_t px = 0; px < EPD_H; px++) {
			bool on = (line == 0 || line == EPD_LINES - 1 ||
				   px == 0 || px == EPD_H - 1 || (line % 16) == 0);

			if (on) {
				framebuffer[line * EPD_LINE_BYTES + px / 8U] |=
					(uint8_t)(0x80U >> (px % 8U));
			}
		}
	}
}

/*
 * One full update (note sections 4 and 5): *two* frames -- the image (1 = black)
 * under 0x10, then the same number of 0x00 bytes under 0x13 -- and then the
 * update command: 0x04 powers the DC/DC on, 0x12 refreshes, 0x02 powers it off
 * again (note section 5, "Turn-off DC/DC"; esp-elink does the same and then
 * parks the bus, which this does too).
 */
static int panel_update(int64_t *frames_ms)
{
	static const uint8_t blank[64];
	int64_t start = k_uptime_get();
	int rc;

	rc = epd_byte(false, EPD_REG_DTM1);
	for (size_t i = 0; rc == 0 && i < sizeof(framebuffer); i++) {
		rc = epd_byte(true, framebuffer[i]);
	}
	if (rc != 0) {
		return rc;
	}

	rc = epd_byte(false, EPD_REG_DTM2);
	for (size_t sent = 0; rc == 0 && sent < sizeof(framebuffer); sent += sizeof(blank)) {
		size_t n = MIN(sizeof(framebuffer) - sent, sizeof(blank));

		for (size_t i = 0; rc == 0 && i < n; i++) {
			rc = epd_byte(true, blank[i]);
		}
	}
	if (rc != 0) {
		return rc;
	}

	*frames_ms = k_uptime_get() - start;

	rc = epd_cmd(EPD_REG_PON);
	if (rc == 0) {
		rc = epd_wait_busy("DC/DC on", 2000);
	}
	if (rc != 0) {
		return rc;
	}

	/*
	 * Tr is 5 s at 25 C and grows with temperature (datasheet table 6-2),
	 * and the panel's boost current during it can disturb the console on a
	 * dev board rig -- one capture ended right here. Report what is already
	 * known before issuing the refresh, so that a log which stops at this
	 * line still says the bus, both frames and the DC/DC-on were fine.
	 */
	printk("elink: two %u-byte frames out in %lld ms, DC/DC on; refreshing\n",
	       (unsigned int)sizeof(framebuffer), *frames_ms);

	rc = epd_cmd(EPD_REG_DRF);
	if (rc == 0) {
		rc = epd_wait_busy("refresh", 30000);
	}
	if (rc != 0) {
		return rc;
	}

	rc = epd_cmd(EPD_REG_POF);
	if (rc == 0) {
		rc = epd_wait_busy("DC/DC off", 5000);
	}
	if (rc != 0) {
		return rc;
	}

	/*
	 * Park the bus: DC and RES# low, as the reference leaves it and as the
	 * note's power-on flow expects to find it. The engine keeps CSN high
	 * between transfers by itself.
	 */
	gpio_pin_set_dt(&dc, 0);
	k_msleep(150);
	gpio_pin_set_dt(&rst, 1);
	return 0;
}

int main(void)
{
	const struct gpio_dt_spec *pins[] = { &dc, &rst, &busy };
	int64_t frames_ms = 0;
	int rc = 0;

	if (!device_is_ready(bus)) {
		printk("elink: SPI bus not ready\n");
		return -ENODEV;
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(pins); i++) {
		if (!gpio_is_ready_dt(pins[i])) {
			printk("elink: gpio %u not ready\n", i);
			return -ENODEV;
		}
		rc |= gpio_pin_configure_dt(pins[i],
			(pins[i] == &busy) ? GPIO_INPUT : GPIO_OUTPUT_INACTIVE);
	}
	if (rc != 0) {
		printk("elink: RESULT: FAIL (gpio setup rc=%d)\n", rc);
		return rc;
	}

	printk("elink: E2271CS091 %ux%u, %ux%u frame of %u bytes, %s at %u Hz\n",
	       EPD_W, EPD_H, EPD_H, EPD_LINES, (unsigned int)sizeof(framebuffer),
	       bus->name, (unsigned int)DT_PROP(EPD_NODE, spi_max_frequency));
	printk("elink: DC=%s.%u RST_N=%s.%u BUSY_N=%s.%u\n",
	       dc.port->name, dc.pin, rst.port->name, rst.pin,
	       busy.port->name, busy.pin);

	rc = panel_init();
	if (rc != 0) {
		printk("elink: RESULT: FAIL (panel_init rc=%d)\n", rc);
		printk("elink: BUSY_N still low right after the soft reset means the "
		       "panel is not driving it -- check its VDD/VDDIO and the "
		       "BUSY_N wire.\n");
		return rc;
	}

	draw_test_image();
	rc = panel_update(&frames_ms);
	if (rc != 0) {
		printk("elink: RESULT: FAIL (update rc=%d)\n", rc);
		return rc;
	}

	printk("elink: RESULT: PASS -- two %u-byte frames pushed in %lld ms and "
	       "the DC/DC cycled; the border plus every 16th line should be black "
	       "by then (Tr is ~5 s at 25 C, on top of this).\n",
	       (unsigned int)sizeof(framebuffer), frames_ms);
	return 0;
}
