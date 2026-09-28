/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi_loopback — what does the AgRV2K SPI engine actually transmit?
 *
 * Wire the fabric patch's MISO input to its own MOSI output (a jumper
 * between the two pins, or SPI1_SI_IO0 -> so_io1 inside the bitstream) and
 * the bytes the engine clocks out come back through the patch's capture
 * register. That turns the fabric into a witness for the transmit path,
 * which is otherwise invisible without a logic analyser.
 *
 * Four transfers, all of them clocked by the on-die engine:
 *
 *   1. 4 bytes through the driver  — the plain register-fed path;
 *   2. 32 bytes through the driver — eight register-fed phases, the
 *      driver's documented maximum;
 *   3. 36 bytes through the driver — past the register-fed maximum, so
 *      spi_agm hands phase1 to the DMAC. This is the shape the SDK's
 *      SPI_Send_Long/SPI_FLASH_WritePage build;
 *   4. the same 36 bytes with the engine and the DMAC programmed by hand
 *      (characterisation probe, off by default): the two paths must agree.
 *
 * Each test prints the transmitted and the captured bytes, so shifts,
 * reversal or a completely idle MOSI are all visible.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/spi_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_APP_LOOP_CONTROLLER_SPI1)
#define LOOP_NODE DT_NODELABEL(spi1)
#else
#define LOOP_NODE DT_NODELABEL(spi0)
#endif

#if !DT_NODE_HAS_STATUS(LOOP_NODE, okay)
#error "the selected SPI controller is disabled: build with a board overlay that sets it okay"
#endif

#if !DT_NODE_HAS_STATUS(DT_NODELABEL(cpld0), okay)
#error "cpld0 is disabled: the fabric capture needs the CPLD window"
#endif

static const struct device *const spi = DEVICE_DT_GET(LOOP_NODE);
static const struct device *const cpld = DEVICE_DT_GET(DT_NODELABEL(cpld0));
static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(dma0));

static uintptr_t fabric_base;

/* Register map of the engine (see drivers/spi/spi_agm.c). */
#define ENGINE_BASE  ((uintptr_t)DT_REG_ADDR(LOOP_NODE))
#define SPI_CTRL     (ENGINE_BASE + 0x00U)
#define PHASE_CTRL(n) (ENGINE_BASE + 0x10U + 4U * (n))
#define PHASE_DATA(n) (ENGINE_BASE + 0x30U + 4U * (n))

#define CTRL_START      BIT(0)
#define CTRL_DONE       BIT(1)
#define CTRL_ERROR      BIT(2)
#define CTRL_PHASE_CNT_SHIFT 4
#define CTRL_USE_DMA    BIT(8)
#define CTRL_LE         BIT(10)
#define CTRL_RESET      BIT(31)

#define ACT_TX      (0U << 4)
#define ACT_RX      (2U << 4)

/* Fabric patch control register bits (vendor full_duplex_spi.v). */
#define FABRIC_CTRL_ENDIAN BIT(10)
#define FABRIC_CTRL_DMA_EN BIT(8)
#define FABRIC_CTRL_CPOL   BIT(24)
#define FABRIC_CTRL_CPHA   BIT(25)

#define TX4_LEN  4U
#define TX32_LEN 32U
#define TX36_LEN 36U
/* An int Kconfig can be 0 to disable the test; the arrays still need a size. */
#define TXL_LEN  ((CONFIG_APP_LOOP_LONG_BYTES > 0) ? CONFIG_APP_LOOP_LONG_BYTES : 1)

static const struct spi_config spi_cfg = {
	.frequency = 1000000U,
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = { .cs_is_gpio = false },
};

static uint8_t tx4[TX4_LEN];
static uint8_t tx32[TX32_LEN];
static uint8_t tx36[TX36_LEN];
/* The long pair: >32 bytes out of the engine (so the DMAC feeds it) while
 * the fabric streams the same number of captured bytes into memory. */
static uint8_t txl[TXL_LEN] __aligned(4);
static uint8_t cap4[TX4_LEN] __aligned(4);
static uint8_t cap32[TX32_LEN] __aligned(4);
static uint8_t cap36[TX36_LEN] __aligned(4);
static uint8_t capl[TXL_LEN] __aligned(4);

static void print_bytes(const uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		printk(" %02x", buf[i]);
	}
}

static int fabric_configure(bool dma_en)
{
	uint32_t ctrl = FABRIC_CTRL_ENDIAN;

	if (dma_en) {
		ctrl |= FABRIC_CTRL_DMA_EN;
	}

	if (IS_ENABLED(CONFIG_APP_LOOP_CPOL)) {
		ctrl |= FABRIC_CTRL_CPOL;
	}

	if (IS_ENABLED(CONFIG_APP_LOOP_CPHA)) {
		ctrl |= FABRIC_CTRL_CPHA;
	}

	return agm_cpld_write32(cpld, CONFIG_APP_LOOP_CTRL_OFFSET, ctrl);
}

/* Read a short capture (<= 4 bytes) out of the patch's register. */
static int capture_short(uint8_t *rx, size_t len)
{
	uint32_t word = 0U;
	int ret;

	ret = agm_cpld_read32(cpld, CONFIG_APP_LOOP_DATA_OFFSET, &word);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0U; i < len; i++) {
		rx[i] = (uint8_t)(word >> (8U * i));
	}

	return 0;
}

/* Long capture: the patch pushes a word per four bytes into its FIFO while
 * DMA_EN is set, and this channel drains it. */
struct loop_dma_ctx {
	struct k_sem done;
	volatile int status;
};

static void loop_dma_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	struct loop_dma_ctx *ctx = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	ctx->status = status;
	k_sem_give(&ctx->done);
}

static int capture_long_start(uint8_t *rx, size_t len, struct loop_dma_ctx *ctx)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	int ret;

	k_sem_init(&ctx->done, 0, 1);
	ctx->status = -1;

	block.source_address = (uint32_t)(fabric_base + CONFIG_APP_LOOP_DATA_OFFSET);
	block.dest_address = (uint32_t)(uintptr_t)rx;
	block.block_size = len;
	block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.source_burst_length = 1U;
	cfg.dest_burst_length = 1U;
	cfg.block_count = 1U;
	cfg.head_block = &block;
	cfg.dma_callback = loop_dma_cb;
	cfg.user_data = ctx;
	cfg.complete_callback_en = 1U;
	cfg.dma_slot = CONFIG_APP_LOOP_FIFO_DMA_REQUEST;

	ret = dma_config(dma, CONFIG_APP_LOOP_FIFO_DMA_CHANNEL, &cfg);
	if (ret < 0) {
		return ret;
	}

	return dma_start(dma, CONFIG_APP_LOOP_FIFO_DMA_CHANNEL);
}

static int capture_long_wait(struct loop_dma_ctx *ctx)
{
	if (k_sem_take(&ctx->done, K_MSEC(1000)) != 0) {
		(void)dma_stop(dma, CONFIG_APP_LOOP_FIFO_DMA_CHANNEL);

		return -ETIMEDOUT;
	}

	return ctx->status;
}

static int engine_write(const uint8_t *buf, size_t len)
{
	struct spi_buf tx_buf = { .buf = (void *)buf, .len = len };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };

	return spi_write(spi, &spi_cfg, &tx_set);
}

/* The SDK's TX-DMA shape, programmed directly: phase0 = 4 bytes from the
 * data register, phase1 = the rest streamed in by the DMAC with the
 * peripheral as flow controller (flow 5, transferSize 0). */
static int engine_write_dma_direct(const uint8_t *buf, size_t len)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	uint32_t waited = 0U;
	uint32_t ctrl;
	int ret;

	sys_write32(CTRL_RESET, SPI_CTRL);
	sys_write32(0U, SPI_CTRL);

	/* phase 0: first four bytes from the register */
	sys_write32(ACT_TX | (4U << 8), PHASE_CTRL(0));
	sys_write32((uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
			    ((uint32_t)buf[3] << 24),
		    PHASE_DATA(0));

	/* phase 1: the rest, fed by DMA into its data register */
	sys_write32(ACT_TX | ((uint32_t)(len - 4U) << 8), PHASE_CTRL(1));

	block.source_address = (uint32_t)(uintptr_t)(buf + 4U);
	block.dest_address = (uint32_t)PHASE_DATA(1);
	/* The peripheral controls the count in flow 5, but dma_agm still needs a
	 * non-zero size to build its descriptor (and it is what the engine is
	 * clocking anyway). */
	block.block_size = len - 4U;
	block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;

	cfg.channel_direction = MEMORY_TO_PERIPHERAL;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.source_burst_length = 1U;
	cfg.dest_burst_length = 1U;
	cfg.block_count = 1U;
	cfg.head_block = &block;
	cfg.complete_callback_en = 0U;
	cfg.dma_slot = CONFIG_APP_LOOP_TX_DMA_REQUEST;

	ret = dma_config(dma, CONFIG_APP_LOOP_TX_DMA_CHANNEL, &cfg);
	if (ret < 0) {
		return ret;
	}

	ret = dma_start(dma, CONFIG_APP_LOOP_TX_DMA_CHANNEL);
	if (ret < 0) {
		return ret;
	}

	/* Slow clock (SYSCLK / 256) and the engine's own DONE as completion. */
	sys_write32(CTRL_START | (1U << CTRL_PHASE_CNT_SHIFT) | CTRL_LE | CTRL_USE_DMA, SPI_CTRL);

	while (((sys_read32(SPI_CTRL) & CTRL_DONE) == 0U) && (waited < 100000U)) {
		k_busy_wait(10);
		waited++;
	}

	ctrl = sys_read32(SPI_CTRL);
	(void)dma_stop(dma, CONFIG_APP_LOOP_TX_DMA_CHANNEL);

	if ((ctrl & CTRL_DONE) == 0U) {
		printk("loopback: engine never finished (ctrl=%08x, err=%u)\n", ctrl,
		       (ctrl & CTRL_ERROR) ? 1U : 0U);

		return -ETIMEDOUT;
	}

	return 0;
}

/* Does `cap` contain `tx` (accepting a one-byte shift or reversal)? */
static bool capture_matches(const uint8_t *cap, size_t cap_len, const uint8_t *tx, size_t tx_len)
{
	if (memcmp(cap, tx, tx_len) == 0) {
		return true;
	}

	for (size_t shift = 1U; (shift + tx_len) <= cap_len; shift++) {
		if (memcmp(&cap[shift], tx, tx_len) == 0) {
			return true;
		}
	}

	for (size_t i = 0U; i < tx_len; i++) {
		if (cap[tx_len - 1U - i] != tx[i]) {
			return false;
		}
	}

	return true; /* reversed */
}

int main(void)
{
	struct loop_dma_ctx fifo_ctx;
	uintptr_t base = 0U;
	int ret;

	printk("\nspi_loopback: MOSI->MISO through the fabric capture\n");

	if (!device_is_ready(spi) || !device_is_ready(cpld) || !device_is_ready(dma)) {
		printk("spi_loopback: a device is not ready\n");
		goto idle;
	}

	if (agm_cpld_get_base(cpld, &base) < 0) {
		printk("spi_loopback: cannot query the window\n");
		goto idle;
	}

	fabric_base = base;

	printk("spi_loopback: %s + %s, window CTRL +%#x DATA +%#x, mode %u%u\n", spi->name,
	       cpld->name, (unsigned int)CONFIG_APP_LOOP_CTRL_OFFSET,
	       (unsigned int)CONFIG_APP_LOOP_DATA_OFFSET, IS_ENABLED(CONFIG_APP_LOOP_CPOL) ? 1U : 0U,
	       IS_ENABLED(CONFIG_APP_LOOP_CPHA) ? 1U : 0U);

	/* Known patterns: byte i = 0xA0 + i, so a shift or reversal is obvious. */
	for (size_t i = 0U; i < TX36_LEN; i++) {
		if (i < TX4_LEN) {
			tx4[i] = (uint8_t)(0xA0U + i);
		}

		if (i < TX32_LEN) {
			tx32[i] = (uint8_t)(0xA0U + i);
		}

		tx36[i] = (uint8_t)(0xA0U + i);
	}

	for (size_t i = 0U; i < TXL_LEN; i++) {
		txl[i] = (uint8_t)(0xA0U + i);
	}

	/* --- sanity: is the patch where we think it is? -------------------- */
	{
		uint32_t ctrl_readback = 0U;

		ret = fabric_configure(false);
		if (ret < 0) {
			printk("spi_loopback: fabric CTRL write failed (%d)\n", ret);
			goto idle;
		}

		ret = agm_cpld_read32(cpld, CONFIG_APP_LOOP_CTRL_OFFSET, &ctrl_readback);
		if (ret < 0) {
			printk("spi_loopback: fabric CTRL read failed (%d)\n", ret);
			goto idle;
		}

		printk("spi_loopback: fabric CTRL readback = 0x%08x %s\n", ctrl_readback,
		       (ctrl_readback & FABRIC_CTRL_ENDIAN) ? "(patch answers)"
							    : "(no patch at this offset -> give up)");

		if ((ctrl_readback & FABRIC_CTRL_ENDIAN) == 0U) {
			goto idle;
		}
	}

	/* --- 0. direct loopback, no fabric involved ------------------------ */
	/* The engine is half duplex: MISO is only sampled once the TX phases
	 * are done, so this captures what the line carries *after* the
	 * pattern -- not the pattern itself. The RX list covers the whole
	 * frame (the command's own slots stay 0xFF), which is the shape the
	 * SPI API describes. It still shows whether the jumper sits between
	 * the two pins the engine uses. */
	{
		uint8_t echo[sizeof(tx4)];
		struct spi_buf tx_buf = { .buf = tx4, .len = sizeof(tx4) };
		struct spi_buf rx_bufs[2] = {
			{ .buf = echo, .len = sizeof(echo) },
			{ .buf = cap4, .len = sizeof(cap4) },
		};
		const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
		const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

		memset(cap4, 0, sizeof(cap4));

		ret = spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);

		printk("spi_loopback: [0] direct TX4 + RX4 (no fabric): ret=%d RX =", ret);
		print_bytes(cap4, sizeof(cap4));
		printk(" -> %s\n",
		       capture_matches(cap4, sizeof(cap4), tx4, sizeof(tx4)) ? "MATCH (full duplex?)"
									    : "no pattern (half duplex, as expected)");
	}

	/* Does the captured value depend on what we transmit? Three patterns
	 * through the same direct loopback answer that. */
	{
		static const uint8_t patterns[3][4] = {
			{ 0x00U, 0x00U, 0x00U, 0x00U },
			{ 0xFFU, 0xFFU, 0xFFU, 0xFFU },
			{ 0x5AU, 0xA5U, 0x00U, 0xFFU },
		};

		for (size_t p = 0U; p < ARRAY_SIZE(patterns); p++) {
			uint8_t echo[4];
			struct spi_buf tx_buf = { .buf = (void *)patterns[p], .len = 4U };
			struct spi_buf rx_bufs[2] = {
				{ .buf = echo, .len = sizeof(echo) },
				{ .buf = cap4, .len = 4U },
			};
			const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
			const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

			memset(cap4, 0, sizeof(cap4));
			ret = spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);

			printk("spi_loopback: [0b] TX");
			print_bytes(patterns[p], 4U);
			printk(" -> RX");
			print_bytes(cap4, 4U);
			printk(" (ret=%d)\n", ret);
		}
	}

	/* --- 1. four bytes through the driver ------------------------------ */
	ret = fabric_configure(false);
	if (ret < 0) {
		printk("spi_loopback: fabric CTRL write failed (%d)\n", ret);
		goto idle;
	}

	ret = engine_write(tx4, sizeof(tx4));
	printk("spi_loopback: [1] TX 4 B via driver: ret=%d\n", ret);

	ret = capture_short(cap4, sizeof(cap4));
	if (ret < 0) {
		printk("spi_loopback: [1] capture failed (%d)\n", ret);
		goto idle;
	}

	printk("spi_loopback: [1] TX =");
	print_bytes(tx4, sizeof(tx4));
	printk("\nspi_loopback: [1] RX =");
	print_bytes(cap4, sizeof(cap4));
	printk(" -> %s\n", capture_matches(cap4, sizeof(cap4), tx4, sizeof(tx4)) ? "MATCH" : "mismatch");

	/* --- 2. 32 bytes through the driver (8 register phases) ------------ */
	/* Capture the *whole* stream through the fabric FIFO, not just the last
	 * register word: an order or alignment problem in the register-fed
	 * phases only shows up in the full capture. */
	ret = fabric_configure(true);
	if (ret == 0) {
		ret = capture_long_start(cap32, sizeof(cap32), &fifo_ctx);
	}

	ret = (ret < 0) ? ret : engine_write(tx32, sizeof(tx32));
	printk("spi_loopback: [2] TX 32 B via driver: ret=%d\n", ret);

	if (ret == 0) {
		ret = capture_long_wait(&fifo_ctx);
	}

	if (ret == 0) {
		printk("spi_loopback: [2] TX =");
		print_bytes(tx32, sizeof(tx32));
		printk("\nspi_loopback: [2] RX =");
		print_bytes(cap32, sizeof(cap32));
		printk(" -> %s\n",
		       capture_matches(cap32, sizeof(cap32), tx32, sizeof(tx32)) ? "MATCH"
										: "mismatch");
	} else {
		printk("spi_loopback: [2] capture failed (%d)\n", ret);
	}

	(void)fabric_configure(false);

	/* --- 3. 36 bytes through the driver (engine TX DMA) ---------------- */
	/* >32 B makes spi_agm build phase0 from the data register and hand the
	 * rest to the DMAC (flow 5), i.e. the SDK's SPI_Send_Long shape. */
	if (IS_ENABLED(CONFIG_APP_LOOP_EXPERIMENTAL_TX_DMA)) {
		ret = fabric_configure(true);
		if (ret < 0) {
			printk("spi_loopback: [3] fabric CTRL failed (%d)\n", ret);
			goto idle;
		}

		ret = capture_long_start(cap36, sizeof(cap36), &fifo_ctx);
		if (ret < 0) {
			printk("spi_loopback: [3] FIFO DMA start failed (%d)\n", ret);
			goto idle;
		}

		ret = engine_write(tx36, sizeof(tx36));
		printk("spi_loopback: [3] TX 36 B via driver: ret=%d\n", ret);

		if (ret == 0) {
			ret = capture_long_wait(&fifo_ctx);
		}

		if (ret < 0) {
			printk("spi_loopback: [3] capture failed (%d)\n", ret);
		} else {
			printk("spi_loopback: [3] TX =");
			print_bytes(tx36, sizeof(tx36));
			printk("\nspi_loopback: [3] RX =");
			print_bytes(cap36, sizeof(cap36));
			printk(" -> %s\n",
			       capture_matches(cap36, sizeof(cap36), tx36, sizeof(tx36)) ? "MATCH"
											: "mismatch");
		}

		(void)fabric_configure(false);
	}

	/* --- 4. the same 36 bytes with the engine programmed by hand ------- */
	if (IS_ENABLED(CONFIG_APP_LOOP_DIRECT_TX_DMA)) {
		/* Long capture: DMA_EN set, FIFO drained by the DMA. */
		ret = fabric_configure(true);
		if (ret < 0) {
			printk("spi_loopback: [4] fabric CTRL failed (%d)\n", ret);
			goto idle;
		}

		ret = capture_long_start(cap36, sizeof(cap36), &fifo_ctx);
		if (ret < 0) {
			printk("spi_loopback: [4] FIFO DMA start failed (%d)\n", ret);
			goto idle;
		}

		ret = engine_write_dma_direct(tx36, sizeof(tx36));
		printk("spi_loopback: [4] TX 36 B via hand-programmed engine: ret=%d\n", ret);

		ret = capture_long_wait(&fifo_ctx);
		if (ret < 0) {
			printk("spi_loopback: [4] FIFO DMA wait failed (%d)\n", ret);
		} else {
			printk("spi_loopback: [4] TX =");
			print_bytes(tx36, sizeof(tx36));
			printk("\nspi_loopback: [4] RX =");
			print_bytes(cap36, sizeof(cap36));
			printk(" -> %s\n",
			       capture_matches(cap36, sizeof(cap36), tx36, sizeof(tx36)) ? "MATCH"
										: "mismatch");
		}

		(void)fabric_configure(false);
	}

#if CONFIG_APP_LOOP_LONG_BYTES > 0
	/* --- 5. long TX and long RX at the same time ---------------------- */
	/* The driver's own RX cannot follow a DMA-fed TX (its DMA phase has to
	 * be the last one), but here the RX goes through the fabric: the patch
	 * captures MISO into its FIFO and a second DMAC drains it while the
	 * engine is still clocking. Two DMACs, one chip-select window. */
	{
		printk("spi_loopback: [5] long TX + long RX: %u B each\n",
		       (unsigned int)TXL_LEN);

		ret = fabric_configure(true);
		if (ret < 0) {
			printk("spi_loopback: [5] fabric CTRL failed (%d)\n", ret);
			goto idle;
		}

		ret = capture_long_start(capl, sizeof(capl), &fifo_ctx);
		if (ret < 0) {
			printk("spi_loopback: [5] FIFO DMA start failed (%d)\n", ret);
			goto idle;
		}

		ret = engine_write(txl, sizeof(txl));
		printk("spi_loopback: [5] TX via driver (engine TX DMA): ret=%d\n", ret);

		if (ret == 0) {
			ret = capture_long_wait(&fifo_ctx);
		}

		if (ret < 0) {
			printk("spi_loopback: [5] capture failed (%d)\n", ret);
		} else {
			printk("spi_loopback: [5] TX =");
			print_bytes(txl, 16U);
			printk(" ... (%u bytes)\n", (unsigned int)TXL_LEN);
			printk("spi_loopback: [5] RX =");
			print_bytes(capl, 16U);
			printk(" ...\n");
			printk("spi_loopback: [5] captured %u bytes -> %s\n",
			       (unsigned int)TXL_LEN,
			       capture_matches(capl, sizeof(capl), txl, sizeof(txl)) ? "MATCH"
										     : "mismatch");
		}

		(void)fabric_configure(false);
	}
#endif

#if CONFIG_APP_LOOP_DUMMY_BYTES > 0
	/* --- 6. filler bytes with no buffer: the engine's DUMMY TX phase ---- */
	/* PHASE_ACTION = DUMMY TX clocks N bytes without a data register and
	 * without DMA. The fabric counts clocks, so a completed capture of N
	 * bytes is the witness that N bytes really went out. */
	{
		static uint8_t cap_dummy[CONFIG_APP_LOOP_DUMMY_BYTES] __aligned(4);

		ret = fabric_configure(true);
		if (ret == 0) {
			ret = capture_long_start(cap_dummy, sizeof(cap_dummy), &fifo_ctx);
		}
		if (ret == 0) {
			ret = spi_agm_clock_dummy(spi, &spi_cfg, sizeof(cap_dummy));
		}

		printk("spi_loopback: [6] %u filler bytes via DUMMY TX: ret=%d\n",
		       (unsigned int)sizeof(cap_dummy), ret);

		if (ret == 0) {
			ret = capture_long_wait(&fifo_ctx);
		}

		if (ret < 0) {
			printk("spi_loopback: [6] the fabric did not see the filler (%d)\n", ret);
		} else {
			printk("spi_loopback: [6] fabric captured %u bytes -> PASS\n",
			       (unsigned int)sizeof(cap_dummy));
		}

		(void)fabric_configure(false);
	}
#endif

#if defined(CONFIG_PM_DEVICE_RUNTIME)
	/* --- 7. runtime PM on the controller that clocks the patch --------- */
	{
		const uint32_t apb_bit = BIT(DT_PROP(LOOP_NODE, agm_apb_clkenable_bit));
		uintptr_t base = (uintptr_t)DT_REG_ADDR(LOOP_NODE);
		uint32_t clk_gated;
		uint32_t clk_resumed;
		uint32_t ctrl_gated;
		uint32_t ctrl_resumed;
		int put_ret;

		/* A balanced get/put pair is what suspends the device. */
		ret = pm_device_runtime_get(spi);
		put_ret = pm_device_runtime_put(spi);
		clk_gated = sys_read32(0x03000060U);
		ctrl_gated = sys_read32(base);

		/* No get() here on purpose: the driver has to resume itself. */
		ret = engine_write(tx4, sizeof(tx4));

		/* Hold a reference while looking at both registers again. */
		(void)pm_device_runtime_get(spi);
		clk_resumed = sys_read32(0x03000060U);
		ctrl_resumed = sys_read32(base);
		(void)pm_device_runtime_put(spi);

		printk("spi_loopback: [7] runtime PM: put=%d, APB_CLKENABLE 0x%08x -> 0x%08x "
		       "(bit %u), CTRL 0x%08x -> 0x%08x, write ret=%d -> %s\n",
		       put_ret, clk_gated, clk_resumed,
		       (unsigned int)DT_PROP(LOOP_NODE, agm_apb_clkenable_bit), ctrl_gated,
		       ctrl_resumed, ret,
		       ((put_ret == 0) && ((clk_gated & apb_bit) == 0U) &&
			((clk_resumed & apb_bit) != 0U) && (ret == 0))
			       ? "PASS"
			       : "FAIL");
	}
#endif

	printk("spi_loopback: done\n");

#if IS_ENABLED(CONFIG_APP_LOOP_CONTINUOUS)
	/* Bench aid: keep the engine clocking so a meter (or a scope) sees an
	 * intermediate level on the patch's MOSI pin instead of the idle state
	 * it settles into after a single transfer. */
	printk("spi_loopback: continuous 4-byte TX loop - probe the patch MOSI/MISO pins now\n");

	while (true) {
		(void)fabric_configure(false);
		(void)engine_write(tx4, sizeof(tx4));
		k_msleep(1);
	}
#endif

idle:
	while (true) {
		k_msleep(1000);
	}
}
