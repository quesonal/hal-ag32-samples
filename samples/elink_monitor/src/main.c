/* SPDX-License-Identifier: Apache-2.0 */
/*
 * elink_monitor -- a "monitoring console" on the same Pervasive E2271CS091
 * 2.7" e-paper panel that samples/elink drives. Where elink runs once and
 * exits, this one loops:
 *
 *   for (cycle = 0; ; cycle++) {
 *     metrics = synth_metrics(cycle);          // PRNG, deterministic per cycle
 *     render(metrics, cycle, uptime);
 *     push_two_frames_then_refresh();          // borrowed from samples/elink
 *     sleep_ms(15000 - elapsed);
 *   }
 *
 * The panel only supports full refresh (no partial / fast), so the user sees
 * a single frame for ~15 s between paints -- that *is* the monitoring tick.
 * No fake countdown is rendered, since anything written on screen would just
 * lie while the next refresh is brewing.
 *
 * The bus, init and update flow are the elink sample's, unchanged -- this
 * sample is a rendering loop on top of the same primitives. SPI engine,
 * GPIOs and overlay come from samples/elink/boards/agrv2k_407.overlay.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>

#include "prng.h"
#include "render.h"

#define EPD_NODE DT_NODELABEL(elink0)

#define EPD_W DT_PROP(EPD_NODE, width)
#define EPD_H DT_PROP(EPD_NODE, height)

#define EPD_LINE_BYTES  ((EPD_H + 7U) / 8U)     /* 22 bytes = 176 pixels */
#define EPD_LINES       EPD_W                   /* 264 lines */
#define EPD_FRAME_BYTES (EPD_LINE_BYTES * EPD_LINES)

BUILD_ASSERT(EPD_FRAME_BYTES == 5808U,
	     "the 2.7\" frame is 5808 bytes (176 x 264 / 8)");

/* Mirror render.c's geometry so layout maths lives here. */
#define CANVAS_W  176U
#define CANVAS_H  264U

/* Refresh cadence. A full refresh on this family is ~5 s on top of the bus
 * transfers, so 15 s leaves the user ~10 s of "fresh frame" between paints. */
#define REFRESH_INTERVAL_MS 15000

/* EPD register indices (application note sections 2..5). */
#define EPD_REG_PSR         0x00U
#define EPD_REG_POF         0x02U
#define EPD_REG_PON         0x04U
#define EPD_REG_DTM1        0x10U
#define EPD_REG_DRF         0x12U
#define EPD_REG_DTM2        0x13U
#define EPD_REG_ACTIVE_TEMP 0xE0U
#define EPD_REG_TSSET       0xE5U

#define EPD_SOFT_RESET  0x0EU
#define EPD_TSSET_25C   0x19U
#define EPD_ACTIVE_TEMP 0x02U
#define EPD_PSR_0       0xCFU
#define EPD_PSR_1       0x8DU

#define EPD_BUSY_ASSERT_MS 100U

static const struct device *const bus = DEVICE_DT_GET(DT_BUS(EPD_NODE));
static const struct gpio_dt_spec dc   = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec rst  = GPIO_DT_SPEC_GET(EPD_NODE, reset_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

static uint8_t framebuffer[EPD_FRAME_BYTES];

/* "Every byte gets its own CS# pulse" (application note, SPI Timing). */
static const struct spi_config epd_spi_cfg = {
	.frequency = DT_PROP(EPD_NODE, spi_max_frequency),
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
		     SPI_TRANSFER_MSB,
	.slave = DT_REG_ADDR(EPD_NODE),
};

/* --------------------------------------------------------------------------
 * Synthesised metrics + helpers
 * --------------------------------------------------------------------------*/

typedef struct {
	uint8_t  cpu_pct;
	uint8_t  mem_kb;
	uint8_t  temp_c;
	uint8_t  load_q8;
	uint16_t net_tx_pkts;
	uint16_t net_rx_pkts;
	uint16_t net_tx_kb;
	uint16_t net_rx_kb;
	uint16_t err_count;
	bool     can_active;
	bool     can_err;
} metrics_t;

#define SPARK_LEN  170U   /* 1 px per sample, 170 px wide plot */

static uint8_t  cpu_history[SPARK_LEN];
static uint16_t cpu_history_count;
static uint16_t net_tx_pkts_state;
static uint16_t net_rx_pkts_state;
static uint16_t net_tx_kb_state;
static uint16_t net_rx_kb_state;
static uint16_t err_count_state;

/*
 * Cycle-deterministic synthesiser. Same cycle -> same numbers, which is
 * handy for dev board testing; deltas look plausible on the dashboard.
 */
static void synth_metrics(uint32_t cycle, metrics_t *m)
{
	uint32_t base = cycle * 2654435761U;   /* Knuth's multiplicative hash */
	uint32_t s;

	prng_seed(base ? base : 1U);

	m->cpu_pct = (uint8_t)prng_range(5U, 95U);
	m->mem_kb  = (uint8_t)prng_range(8U, 60U);
	m->temp_c  = (uint8_t)prng_range(20U, 55U);
	m->load_q8 = (uint8_t)prng_range(0U, 200U);

	s = prng_range(0U, 9U);
	m->can_active = (s < 7U);
	m->can_err    = (s >= 8U);

	{
		uint16_t d_tx_pkts = (uint16_t)prng_range(0U, 30U);
		uint16_t d_rx_pkts = (uint16_t)prng_range(0U, 18U);
		uint16_t d_tx_kb   = (uint16_t)prng_range(0U, 6U);
		uint16_t d_rx_kb   = (uint16_t)prng_range(0U, 4U);
		uint16_t d_err     = (s == 9U)
				   ? (uint16_t)prng_range(1U, 3U) : 0U;

		net_tx_pkts_state = (uint16_t)(net_tx_pkts_state + d_tx_pkts);
		net_rx_pkts_state = (uint16_t)(net_rx_pkts_state + d_rx_pkts);
		net_tx_kb_state   = (uint16_t)(net_tx_kb_state + d_tx_kb);
		net_rx_kb_state   = (uint16_t)(net_rx_kb_state + d_rx_kb);
		err_count_state   = (uint16_t)(err_count_state + d_err);

		m->net_tx_pkts = net_tx_pkts_state;
		m->net_rx_pkts = net_rx_pkts_state;
		m->net_tx_kb   = net_tx_kb_state;
		m->net_rx_kb   = net_rx_kb_state;
		m->err_count   = err_count_state;
	}

	/* Push to circular sparkline buffer (oldest gets overwritten). */
	{
		uint16_t slot = (cpu_history_count < SPARK_LEN)
			      ? cpu_history_count
			      : (uint16_t)(cpu_history_count % SPARK_LEN);

		cpu_history[slot] = m->cpu_pct;
		cpu_history_count++;
	}
}

/*
 * Walk the circular buffer in chronological order (oldest first) into a
 * linear snapshot that the renderer can plot left-to-right.
 */
static void snapshot_cpu_history(uint8_t *out)
{
	uint16_t i;
	uint16_t start;
	uint16_t count = (cpu_history_count < SPARK_LEN)
		       ? cpu_history_count : SPARK_LEN;

	start = (cpu_history_count < SPARK_LEN)
	      ? 0U : (uint16_t)(cpu_history_count % SPARK_LEN);

	for (i = 0; i < count; i++) {
		out[i] = cpu_history[(uint16_t)(start + i) % SPARK_LEN];
	}
	/* Pad the rest with zeros so the renderer always sees SPARK_LEN. */
	for (; i < SPARK_LEN; i++) {
		out[i] = 0U;
	}
}

/* --------------------------------------------------------------------------
 * Layout
 * --------------------------------------------------------------------------*/

static void layout_top(uint32_t cycle, int64_t uptime_ms)
{
	char buf[24];
	uint32_t total_s = (uint32_t)(uptime_ms / 1000U);
	uint32_t hh = (total_s / 3600U) % 100U;
	uint32_t mm = (total_s / 60U)   % 60U;
	uint32_t ss =  total_s          % 60U;

	render_text(framebuffer, 2, 2, "ZEPHYR MON", 2);
	snprintf(buf, sizeof(buf), "#%04lu %02lu:%02lu:%02lu",
		 (unsigned long)cycle,
		 (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
	render_text(framebuffer, 2, 14, buf, 1);
	render_hline(framebuffer, 2, 26, (uint16_t)(CANVAS_W - 4));
}

static void layout_tile(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
			const char *label, const char *value, uint8_t pct)
{
	render_rect(framebuffer, x, y, w, h, false);
	render_text(framebuffer, (uint16_t)(x + 2), (uint16_t)(y + 2),
		    label, 1);
	render_text(framebuffer, (uint16_t)(x + 2), (uint16_t)(y + 9),
		    value, 2);
	render_bar_h(framebuffer, (uint16_t)(x + 2),
		     (uint16_t)(y + h - 8), (uint16_t)(w - 4U), 6, pct);
}

static void layout_rows(const metrics_t *m)
{
	char buf[8];
	uint8_t spark_snapshot[SPARK_LEN];
	uint16_t tile_w = 84U;
	uint16_t tile_h = 40U;
	uint16_t col1_x = 2U;
	uint16_t col2_x = 90U;
	uint16_t row1_y = 29U;
	uint16_t row2_y = 71U;
	uint16_t row3_y = 175U;

	/* Row 1: CPU + MEM */
	snprintf(buf, sizeof(buf), "%u%%", m->cpu_pct);
	layout_tile(col1_x, row1_y, tile_w, tile_h, "CPU", buf, m->cpu_pct);
	snprintf(buf, sizeof(buf), "%uKB", m->mem_kb);
	layout_tile(col2_x, row1_y, tile_w, tile_h, "MEM", buf,
		    (uint8_t)((uint32_t)m->mem_kb * 100U / 64U));

	/* Row 2: TEMP + LOAD */
	snprintf(buf, sizeof(buf), "%uC", m->temp_c);
	layout_tile(col1_x, row2_y, tile_w, tile_h, "TEMP", buf,
		    (uint8_t)((uint32_t)(m->temp_c - 20U) * 100U / 35U));
	snprintf(buf, sizeof(buf), "%u.%02u",
		 m->load_q8 / 100U, m->load_q8 % 100U);
	layout_tile(col2_x, row2_y, tile_w, tile_h, "LOAD", buf,
		    (uint8_t)(m->load_q8 / 2U));   /* 0..200 -> 0..100 */

	/* Sparkline */
	render_text(framebuffer, 2, 114, "CPU 60s", 1);
	snapshot_cpu_history(spark_snapshot);
	render_sparkline(framebuffer, 2, 124, (uint16_t)(CANVAS_W - 4), 48,
			 spark_snapshot, SPARK_LEN, 100);

	/* Row 3: NET TX + NET RX */
	snprintf(buf, sizeof(buf), "%u", m->net_tx_pkts);
	layout_tile(col1_x, row3_y, tile_w, tile_h, "TX", buf,
		    (uint8_t)(m->net_tx_pkts % 100U));
	snprintf(buf, sizeof(buf), "%u", m->net_rx_pkts);
	layout_tile(col2_x, row3_y, tile_w, tile_h, "RX", buf,
		    (uint8_t)(m->net_rx_pkts % 100U));
}

static void layout_status(const metrics_t *m)
{
	char buf[24];

	render_hline(framebuffer, 2, 217, (uint16_t)(CANVAS_W - 4));

	snprintf(buf, sizeof(buf), "err %u  can %s",
		 m->err_count,
		 m->can_err ? "ERR" : (m->can_active ? "ACT" : "OFF"));
	render_text(framebuffer, 2, 220, buf, 1);

	snprintf(buf, sizeof(buf), "tx %uKB rx %uKB",
		 m->net_tx_kb, m->net_rx_kb);
	render_text(framebuffer, 2, 232, buf, 1);

	render_hline(framebuffer, 2, 244, (uint16_t)(CANVAS_W - 4));
	render_text_center(framebuffer, 2, (uint16_t)(CANVAS_W - 2),
			   250, "PASS", 2);
}

static void layout_render(const metrics_t *m, uint32_t cycle,
			  int64_t uptime_ms)
{
	render_clear(framebuffer);
	layout_top(cycle, uptime_ms);
	layout_rows(m);
	layout_status(m);
}

/* --------------------------------------------------------------------------
 * EPD bus plumbing (lifted from samples/elink/src/main.c).
 * --------------------------------------------------------------------------*/

static int epd_byte(bool is_data, uint8_t value)
{
	struct spi_buf buf = { .buf = &value, .len = 1 };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };

	gpio_pin_set_dt(&dc, is_data ? 1 : 0);
	return spi_write(bus, &epd_spi_cfg, &tx);
}

static int epd_write(uint8_t index, const uint8_t *data, size_t len)
{
	int rc = epd_byte(false, index);

	for (size_t i = 0; rc == 0 && i < len; i++) {
		rc = epd_byte(true, data[i]);
	}
	return rc;
}

static int epd_cmd(uint8_t index)
{
	return epd_byte(false, index);
}

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
			printk("elink_monitor: BUSY_N still low %u ms after %s\n",
			       (unsigned int)release_ms, what);
			return -ETIMEDOUT;
		}
	}

	if (!asserted) {
		printk("elink_monitor: %s: BUSY_N never went low\n", what);
	}
	return 0;
}

static void epd_reset(void)
{
	gpio_pin_set_dt(&rst, 1);
	k_msleep(5);
	gpio_pin_set_dt(&rst, 0);
	k_msleep(10);
}

static int panel_init(void)
{
	const uint8_t soft_reset = EPD_SOFT_RESET;
	const uint8_t tsset = EPD_TSSET_25C;
	const uint8_t active_temp = EPD_ACTIVE_TEMP;
	const uint8_t psr[2] = { EPD_PSR_0, EPD_PSR_1 };
	int rc;

	epd_reset();

	rc = epd_write(EPD_REG_PSR, &soft_reset, 1);
	if (rc == 0) {
		rc = epd_wait_busy("soft reset", 500);
	}
	if (rc != 0) {
		return rc;
	}
	rc = epd_write(EPD_REG_TSSET, &tsset, 1);
	if (rc == 0) {
		rc = epd_write(EPD_REG_ACTIVE_TEMP, &active_temp, 1);
	}
	if (rc == 0) {
		rc = epd_write(EPD_REG_PSR, psr, sizeof(psr));
	}
	return rc;
}

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
	for (size_t sent = 0; rc == 0 && sent < sizeof(framebuffer);
	     sent += sizeof(blank)) {
		size_t n = MIN(sizeof(framebuffer) - sent, sizeof(blank));

		for (size_t i = 0; i < n; i++) {
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

	gpio_pin_set_dt(&dc, 0);
	gpio_pin_set_dt(&rst, 0);   /* release RES# between cycles */
	k_msleep(150);
	return 0;
}

/* --------------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------------*/

int main(void)
{
	const struct gpio_dt_spec *pins[] = { &dc, &rst, &busy };
	int rc;

	if (!device_is_ready(bus)) {
		printk("elink_monitor: SPI bus not ready\n");
		return -ENODEV;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(pins); i++) {
		if (!gpio_is_ready_dt(pins[i])) {
			printk("elink_monitor: gpio %u not ready\n", i);
			return -ENODEV;
		}
		rc = gpio_pin_configure_dt(pins[i],
			(pins[i] == &busy) ? GPIO_INPUT
					   : GPIO_OUTPUT_INACTIVE);
		if (rc != 0) {
			printk("elink_monitor: gpio setup rc=%d\n", rc);
			return rc;
		}
	}

	rc = panel_init();
	if (rc != 0) {
		printk("elink_monitor: panel_init failed (%d)\n", rc);
		return rc;
	}

	for (uint32_t cycle = 0; ; cycle++) {
		int64_t cycle_start = k_uptime_get();
		metrics_t m;
		int64_t frames_ms = 0;
		int64_t update_ms;

		synth_metrics(cycle, &m);
		layout_render(&m, cycle, cycle_start);

		rc = panel_update(&frames_ms);
		update_ms = k_uptime_get() - cycle_start;
		if (rc != 0) {
			printk("elink_monitor: cycle %u FAIL rc=%d "
			       "(frames=%lld ms, total=%lld ms)\n",
			       (unsigned int)cycle, rc,
			       (long long)frames_ms,
			       (long long)update_ms);
			break;
		}
		printk("elink_monitor: cycle %u OK frames=%lld ms total=%lld ms\n",
		       (unsigned int)cycle,
		       (long long)frames_ms, (long long)update_ms);

		int32_t due = (int32_t)REFRESH_INTERVAL_MS
			    - (int32_t)update_ms;
		if (due > 0) {
			k_msleep((uint32_t)due);
		}
	}
	return 0;
}
