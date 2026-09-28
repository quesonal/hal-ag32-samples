/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 40-pin LCD adapter board (FPC4301MS-L190-40C-9A / HX8369A, 480x800) on the
 * AgRV2K: 16-bit 8080 bus bit-banged from GPIOs + backlight.
 *
 * First bring-up revision. It drives the bus, runs the part of the panel
 * init that the vendor's own C51 example starts with, and paints colour
 * bands. What it deliberately does NOT do yet:
 *   - the vendor's full register sequence (gamma/power tables, from
 *     LCM/C51调试例程/8369A_LG4.3 MCU 16bit ...c),
 *   - any read-back of the panel ID (needs the RD path + tri-stating DB,
 *     which the stellaris GPIO driver can do but which is untested here),
 *   - the touch section of the adapter (T_CLK/T_MOSI/RT_MISO/T_CS/T_PEN/
 *     RT_BUSY are routed in the pin map, nothing drives them yet).
 * See samples/lcd_40pin/README.rst and the the development notes (not published here).
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util_macro.h>

#include "hx8369a_init.h"

#define LCD_NODE DT_NODELABEL(lcd0)
#define DATA_PINS DT_PROP_LEN(LCD_NODE, data_gpios)

#define DATA_SPEC(i) GPIO_DT_SPEC_GET_BY_IDX(LCD_NODE, data_gpios, i)

static const struct gpio_dt_spec cs_pin = GPIO_DT_SPEC_GET(LCD_NODE, cs_gpios);
static const struct gpio_dt_spec rs_pin = GPIO_DT_SPEC_GET(LCD_NODE, rs_gpios);
static const struct gpio_dt_spec wr_pin = GPIO_DT_SPEC_GET(LCD_NODE, wr_gpios);
static const struct gpio_dt_spec rd_pin = GPIO_DT_SPEC_GET(LCD_NODE, rd_gpios);
static const struct gpio_dt_spec rst_pin =
	GPIO_DT_SPEC_GET(LCD_NODE, reset_gpios);
static const struct gpio_dt_spec bl_pin =
	GPIO_DT_SPEC_GET_OR(LCD_NODE, backlight_gpios, {0});

/* Panel geometry is a datasheet constant here, not a read-back (TODO). */
#define PANEL_WIDTH   480U     /* panel width  */
#define PANEL_HEIGHT  800U     /* panel height */
#define PANEL_X_OFF   0        /* shift the window by N columns (0 = raw)     */
#define PANEL_MIRROR  0        /* 1 = emit CASET reversed (mirror-like swap)  */
/* The visible area of this 480x800 module does not start at row 0 of the
 * controller's frame memory: y must be offset by 20 (the reference driver
 * carries the same value in its commented-out esp_lcd_panel_set_gap(0, 20)).
 * Without it the image sits 20 rows low and a black bar shows at the top. */
#define LCD_Y_GAP 0U

/* HX8369A command set, as used by the vendor's C51 example. */
#define HX_SWRESET 0x01
#define HX_SLPOUT 0x11
#define HX_DISPON 0x29
#define HX_CASET 0x2A
#define HX_PASET 0x2B
#define HX_RAMWR 0x2C
#define HX_COLMOD 0x3A
#define HX_EXTC 0xB9

/* Register readback without a debugger: the SoC's APB clock gate register
 * and the GPIO blocks this sample drives. AGM_APB_CLK_GPIO(n) = 1 << (4 + n),
 * so gpio1/2/3/5/6 show up as 0x7e0 (plus gpio4 = bit 8 for the board LEDs). */
#define REG_APB_CLKENABLE 0x03000060U
#define REG_GPIO_DIR(n)   (0x40014000U + (n) * 0x1000U + 0x400U)
/* The stellaris/TM4C GPIO block is *mask-addressed*: the low address bits
 * select which byte lanes are touched, so the whole-port read lives at
 * DATA + (0xff << 2) = +0x3fc. Reading plain +0x000 reads "no lane
 * selected" and always returns 0 -- which is what made the first
 * self-check print DATA=00 for a port that had just been driven to 0xf8. */
#define REG_GPIO_DATA(n)  (0x40014000U + (n) * 0x1000U + 0x3FCU)
#define REG_GPIO_AFSEL(n) (0x40014000U + (n) * 0x1000U + 0x420U)

static uint32_t rd(uint32_t addr)
{
	return *(volatile uint32_t *)addr;
}

/* The panel is driven in **8-bit** MCU mode (IM[2:0] = 100): the working
 * ESP-IDF reference for this module creates an i80 bus with `.bus_width = 8`
 * and only DATA0..DATA7, like the vendor's own "MCU 8bit, select DB0-7" C51
 * example. Commands, parameters and each half of an RGB565 pixel go out as
 * one byte on DB0..DB7; DB8..DB15 stay unused (they are still routed, so the
 * pin map does not have to change if 16-bit mode is ever needed). */
BUILD_ASSERT(DATA_PINS >= 8, "the 8080 bus needs at least DB0..DB7");
#define BUS_BITS 8

/* Fast path: the stellaris GPIO block is mask-addressed, so one store to
 * DATA + (mask << 2) writes all eight data lines / the four control lines at
 * once, instead of ~15 gpio_pin_set_dt() calls per byte. */
#define GPIO1_BASE 0x40015000U          /* DB0..DB7   */
#define GPIO3_BASE 0x40017000U          /* CS RS WR RD */
#define REG_MASK(base, mask) (*(volatile uint32_t *)((base) + 0x000U + ((mask) << 2)))
#define CTRL_CS  (1U << 0)              /* raw pin levels (active low) */
#define CTRL_RS  (1U << 1)
#define CTRL_WR  (1U << 2)              /* high = idle */
#define CTRL_RD  (1U << 3)              /* high = idle */

static const struct gpio_dt_spec data_pins[] = {
	DATA_SPEC(0), DATA_SPEC(1), DATA_SPEC(2), DATA_SPEC(3),
	DATA_SPEC(4), DATA_SPEC(5), DATA_SPEC(6), DATA_SPEC(7),
	DATA_SPEC(8), DATA_SPEC(9), DATA_SPEC(10), DATA_SPEC(11),
	DATA_SPEC(12), DATA_SPEC(13), DATA_SPEC(14), DATA_SPEC(15),
};

/* 8080 timing is stretched by software; the panel tolerates tens of ns, and
 * the GPIO writes alone are far slower than that. */
static inline void bus_delay(void)
{
	k_busy_wait(1);
}

static void bus_write(bool is_data, uint8_t value)
{
	uint32_t ctrl = CTRL_RD | CTRL_WR;              /* RD/WR idle high */

	if (is_data) {
		ctrl |= CTRL_RS;
	}

	REG_MASK(GPIO1_BASE, 0xffU) = value;            /* data valid first */
	REG_MASK(GPIO3_BASE, 0x0fU) = ctrl;             /* CS low, WR high */
	REG_MASK(GPIO3_BASE, 0x0fU) = ctrl & ~CTRL_WR;  /* WR low, data already valid */
	__asm__ volatile("nop; nop; nop; nop");
	REG_MASK(GPIO3_BASE, 0x0fU) = ctrl;             /* WR high: latch */
	REG_MASK(GPIO3_BASE, 0x0fU) = ctrl | CTRL_CS;   /* CS high */
}

static void write_cmd(uint8_t cmd)
{
	bus_write(false, cmd);
}

static void write_data(uint8_t value)
{
	bus_write(true, value);
}




static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	/*
	 * Plain window: CASET 0..width, PASET 0..height, plus the two knobs
	 * above. The 2026-09-16 dev board session measured a column order that did
	 * not match the frame memory (mirror-like and not monotonic) with only
	 * ~400 of the 480 driver columns visible; those two facts need
	 * re-checking on a known-good panel, so the default here stays raw.
	 */
	x0 += PANEL_X_OFF;
	x1 += PANEL_X_OFF;
	if (PANEL_MIRROR) {
		uint16_t t0 = (uint16_t)(479U - x1), t1 = (uint16_t)(479U - x0);

		x0 = t0;
		x1 = t1;
	}
	write_cmd(HX_CASET);
	write_data(x0 >> 8);
	write_data(x0 & 0xff);
	write_data(x1 >> 8);
	write_data(x1 & 0xff);
	write_cmd(HX_PASET);
	write_data(y0 >> 8);
	write_data(y0 & 0xff);
	write_data(y1 >> 8);
	write_data(y1 & 0xff);
	write_cmd(HX_RAMWR);
}

/* Test image: white 1-px frame, red verticals 20 columns inside the left and
 * right edges, green horizontals 20 rows inside the top and bottom edges. */
static void pattern_ruler(void)
{
	for (uint16_t y = 0; y < PANEL_HEIGHT; y++) {
		set_window(0, y, PANEL_WIDTH - 1U, y);
		for (uint16_t x = 0; x < PANEL_WIDTH; x++) {
			uint16_t c = 0x001F;

			if (y == 0U || y == PANEL_HEIGHT - 1U ||
			    x == 0U || x == PANEL_WIDTH - 1U) {
				c = 0xFFFF;
			} else if (x == 20U || x == PANEL_WIDTH - 21U) {
				c = 0xF800;
			} else if (y == 20U || y == PANEL_HEIGHT - 21U) {
				c = 0x07E0;
			}
			write_data(c >> 8);
			write_data(c & 0xff);
		}
	}
	printk("lcd_40pin: ruler painted (white frame, red verticals, "
	       "green horizontals)\n");
}


/* --- 8080-side "hram_test" equivalents --------------------------------- */
#define GPIO1_DIR_MASK(mask)  (*(volatile uint32_t *)(GPIO1_BASE + 0x400U + ((mask) << 2)))
#define DB_AS_OUTPUT() (GPIO1_DIR_MASK(0xffU) = 0xffU)
#define DB_AS_INPUT()  (GPIO1_DIR_MASK(0xffU) = 0x00U)

/* One byte from the panel: CS low, RS=1, WR high, RD low (asserted). */
static uint8_t bus_read_byte(void)
{
	uint8_t v;

	REG_MASK(GPIO3_BASE, 0x0fU) = CTRL_RS | CTRL_WR;            /* RD low */
	__asm__ volatile("nop; nop; nop; nop");
	v = (uint8_t)REG_MASK(GPIO1_BASE, 0xffU);
	REG_MASK(GPIO3_BASE, 0x0fU) = CTRL_RS | CTRL_WR | CTRL_RD;  /* RD idle */
	REG_MASK(GPIO3_BASE, 0x0fU) = CTRL_RS | CTRL_WR | CTRL_RD | CTRL_CS;
	return v;
}

/* Values the controller must answer on its own (HX8369A). */
static void panel_selftest(void)
{
	uint8_t id[4], st[5];

	write_cmd(0x04);                        /* RDDID */
	DB_AS_INPUT();
	for (int i = 0; i < 4; i++) {
		id[i] = bus_read_byte();
	}
	DB_AS_OUTPUT();
	printk("lcd_40pin: RDDID = %02x %02x %02x %02x (dummy,ID1..ID3)\n",
	       id[0], id[1], id[2], id[3]);

	write_cmd(0x09);                        /* RDDST */
	DB_AS_INPUT();
	for (int i = 0; i < 5; i++) {
		st[i] = bus_read_byte();
	}
	DB_AS_OUTPUT();
	printk("lcd_40pin: RDDST = %02x %02x %02x %02x %02x\n",
	       st[0], st[1], st[2], st[3], st[4]);
}

/* Write an incrementing pattern into a window, read it back with 0x2E and
 * compare -- the 8080 equivalent of hram_test's pattern test. */
static void gram_memtest(void)
{
	const uint16_t W = 64U, H = 64U;
	uint32_t bad = 0U, t0;
	uint16_t n = 0U;

	set_window(0, 0, W - 1U, H - 1U);
	DB_AS_OUTPUT();
	for (uint16_t y = 0U; y < H; y++) {
		for (uint16_t x = 0U; x < W; x++) {
			uint16_t v = (uint16_t)(0x1000U + n++);

			write_data(v >> 8);
			write_data(v & 0xff);
		}
	}

	set_window(0, 0, W - 1U, H - 1U);
	write_cmd(0x2E);                        /* RAMRD */
	t0 = k_cycle_get_32();
	DB_AS_INPUT();
	n = 0U;
	for (uint16_t y = 0U; y < H; y++) {
		for (uint16_t x = 0U; x < W; x++) {
			uint16_t want = (uint16_t)(0x1000U + n++);
			uint8_t hi = bus_read_byte();
			uint8_t lo = bus_read_byte();

			if ((((uint16_t)hi << 8) | lo) != want) {
				bad++;
			}
		}
	}
	DB_AS_OUTPUT();
	printk("lcd_40pin: GRAM memtest %ux%u: %s (%u mismatch) in %u ms\n",
	       W, H, bad ? "FAIL" : "PASS", (unsigned)bad,
	       (unsigned)((k_cycle_get_32() - t0) / (sys_clock_hw_cycles_per_sec() / 1000U)));
}

/* Leftover check: after wiping the frame memory, read a region we did not
 * write -- anything non-zero there is stale content from an earlier run. */
static void leftover_check(void)
{
	uint32_t t0 = k_cycle_get_32();
	uint8_t v;

	set_window(0, 0, 479, 863);
	DB_AS_OUTPUT();
	for (uint32_t i = 0; i < 480U * 864U; i++) {
		write_data(0x00);
		write_data(0x00);
	}
	printk("lcd_40pin: GRAM wipe %u ms (%u KB/s)\n",
	       (unsigned)((k_cycle_get_32() - t0) / (sys_clock_hw_cycles_per_sec() / 1000U)),
	       (unsigned)((480U * 864U * 2U) /
	                  ((k_cycle_get_32() - t0) / (sys_clock_hw_cycles_per_sec() / 1000U) + 1U)));

	set_window(400, 800, 440, 840);         /* outside the visible 800 rows */
	write_cmd(0x2E);
	DB_AS_INPUT();
	v = bus_read_byte();
	DB_AS_OUTPUT();
	printk("lcd_40pin: leftover probe at (400,800) = %02x (expect 00)\n", v);
}



static int configure_output(const struct gpio_dt_spec *spec, const char *name)
{
	if (!gpio_is_ready_dt(spec)) {
		printk("lcd_40pin: %s: gpio controller not ready\n", name);
		return -ENODEV;
	}
	return gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
}




/* Mapping probe: black field with one red 8-column bar at each end of the
 * nominal 480 columns (raw coordinates: 0x36=0x00, no mirror, no offset).
 * Where those two bars appear on the glass gives the visible window, the
 * offset and the column order in one look. */

/* Mapping probe, 8-column granularity: 60 groups of 8 columns, six colours
 * cycling (red, green, blue, yellow, magenta, cyan). Reporting the first
 * three colours from the left gives the visible-window start and the column
 * order in one look: red/green/blue = not mirrored starting at 0;
 * cyan/magenta/yellow = mirrored, starting at the far end. */





int main(void)
{
	int rc;

	printk("lcd_40pin: 40-pin adapter -> HX8369A 480x800, %d-bit 8080 bus "
	       "(%d data lines routed)\n", BUS_BITS, DATA_PINS);

	rc = configure_output(&rst_pin, "reset");
	rc |= configure_output(&cs_pin, "cs");
	rc |= configure_output(&rs_pin, "rs");
	rc |= configure_output(&wr_pin, "wr");
	rc |= configure_output(&rd_pin, "rd");
	for (int i = 0; i < DATA_PINS; i++) {
		rc |= configure_output(&data_pins[i], "db");
	}
	if (bl_pin.port != NULL) {
		rc |= configure_output(&bl_pin, "backlight");
	}
	if (rc != 0) {
		printk("lcd_40pin: RESULT: FAIL (gpio setup, rc=%d)\n", rc);
		return rc;
	}

	printk("lcd_40pin: APB_CLKENABLE=%08x (gpio1/2/3/5/6 want 0x7e0)\n",
	       rd(REG_APB_CLKENABLE));
	/* Self-check the write path inside the SoC: drive DB0..DB7 = 0xF8 and
	 * read the GPIO block's DATA register back. If this prints f8, the SoC
	 * really is driving the data lines and any remaining doubt is outside
	 * the chip (harness / adapter / panel). */
	for (int i = 0; i < BUS_BITS; i++) {
		gpio_pin_set_dt(&data_pins[i], (0xF8 >> i) & 0x1);
	}
	printk("lcd_40pin: DB self-check: gpio1 DATA=%02x (want f8)\n",
	       rd(REG_GPIO_DATA(1)));
	for (int b = 1; b <= 6; b++) {
		if (b == 4) {
			continue;               /* the board's LED bank, not ours */
		}
		printk("lcd_40pin: gpio%d DIR=%02x DATA=%02x AFSEL=%02x\n", b,
		       rd(REG_GPIO_DIR(b)), rd(REG_GPIO_DATA(b)),
		       rd(REG_GPIO_AFSEL(b)));
	}

	/* Reset pulse, then the head of the vendor sequence. */
	gpio_pin_set_dt(&rst_pin, 1);           /* assert reset (active low) */
	/* 10 ms is plenty for the panel; drive it low for a second instead if
	 * you want to verify the reset line with a multimeter. */
	k_msleep(10);
	gpio_pin_set_dt(&rst_pin, 0);           /* release */
	k_msleep(120);

	/* The vendor's own sequence: EXTC unlock, power, panel timing, gamma
	 * (transcribed in src/hx8369a_init.h). Without it the panel stays
	 * blank -- backlight only, which is what the first revision showed. */
	for (unsigned i = 0; i < ARRAY_SIZE(hx8369a_init); i++) {
		const struct hx_cmd *c = &hx8369a_init[i];

		write_cmd(c->cmd);
		for (unsigned j = 0; j < c->len; j++) {
			write_data(c->data[j]);
		}
		if (c->delay_ms) {              /* vendor settle times (SLPOUT) */
			k_msleep(c->delay_ms);
		}
	}
	write_cmd(0x29);                        /* DISPON: the reference driver
						 * sends it from display_on() */
	k_msleep(50);
	printk("lcd_40pin: vendor init sequence written (%d commands)\n",
	       (int)ARRAY_SIZE(hx8369a_init));

	/* read_id() disabled: it flips DB directions and leaves the bus contended */

	if (bl_pin.port != NULL) {
		gpio_pin_set_dt(&bl_pin, 1);
	}

	panel_selftest();
	gram_memtest();
	leftover_check();
	pattern_ruler();              /* red */

	printk("lcd_40pin: touch: gt911 %s\n",
	       device_is_ready(DEVICE_DT_GET(DT_NODELABEL(gt911))) ? "ready"
								: "not ready");

	printk("lcd_40pin: NOTE: vendor full init sequence, panel ID read-back\n");
	printk("lcd_40pin:       and touch reporting are still to come\n");
	printk("lcd_40pin: RESULT: INCONCLUSIVE (needs eyes on the panel -- the\n");
	printk("lcd_40pin:       bands should have been visible; no read-back yet)\n");
	return 0;
}
