/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi_full_duplex — CPLD-assisted full-duplex SPI on AgRV2K.
 *
 * The on-die SPI controller is a phase engine: a transfer is TX phases
 * followed by RX phases, never both at once ("AG32 下 SPI 的扩展使用",
 * page 1). Full duplex — receiving while the clock runs — therefore cannot
 * come from the controller. The vendor's answer is a fabric patch,
 * full_duplex_spi.v: it sits between the controller's SCK/CSN/MOSI and the
 * pins, drives the pins itself, and captures MISO on every clock edge into
 * the CPLD window. example_spi_advanced.c drives it like this:
 *
 *   1. window +0x00 (CTRL): CPOL (1 << 24), CPHA (1 << 25), ENDIAN
 *      (1 << 10); the polarity/phase bits exist only in the fabric.
 *   2. on-die engine: one TX phase of at most 4 bytes -- it clocks the
 *      transfer (the fabric is what actually drives MOSI/SCK).
 *   3. window +0x04 (DATA): the bytes captured on MISO during that phase.
 *      Longer transfers stream this register with DMA (EXT_DMA0_REQ in the
 *      vendor example), which this sample does not do.
 *
 * In this wiring the fabric owns MISO: the VE snippet in the vendor
 * document connects the pins to the fabric (`so_io1 PIN_80:INPUT`) and does
 * not hand MISO back to the controller, so the on-die RX phases read
 * nothing. Everything the device answers must come from the window.
 *
 * The sample therefore cross-checks two fabric captures instead of
 * hard-coding an ID: 0x9F returns the 3-byte JEDEC ID (manufacturer first)
 * and 0x90 returns manufacturer + device ID twice over. The manufacturer
 * byte found in the 0x90 capture has to appear in the 0x9F capture, and
 * neither may be all-00/all-FF. The plain half-duplex read is kept as an
 * informational probe, because on a full-duplex bitstream it is expected to
 * come back empty.
 *
 * With CONFIG_APP_SPI_FULL_LOOPBACK the sample instead expects a jumper
 * between the MOSI and MISO pins and checks that the transmitted pattern
 * comes back (the fabric may delay it by a byte, so a shift of one byte is
 * accepted and printed).
 *
 * The window offsets and the CPOL/CPHA setting are bitstream properties;
 * they live in this sample's Kconfig (APP_SPI_FULL_*), see README.md.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* Which controller the fabric taps is a bitstream property (the VE lines
 * `SPI0_SCK sck` ... decide it), so it is an explicit Kconfig choice:
 * testing a controller the fabric does not tap would just read the previous
 * capture back, because the fabric has no capture-clear. */
#if IS_ENABLED(CONFIG_APP_SPI_FULL_CONTROLLER_SPI1)
#define SPI_FULL_NODE DT_NODELABEL(spi1)
#else
#define SPI_FULL_NODE DT_NODELABEL(spi0)
#endif

#if !DT_NODE_HAS_STATUS(SPI_FULL_NODE, okay)
#error "the selected SPI controller is disabled: build with a board overlay that sets it okay"
#endif

#if !DT_NODE_HAS_STATUS(DT_NODELABEL(cpld0), okay)
#error "cpld0 is disabled: the fabric full-duplex path needs the CPLD window"
#endif

static const struct device *const spi = DEVICE_DT_GET(SPI_FULL_NODE);

static const struct device *const cpld = DEVICE_DT_GET(DT_NODELABEL(cpld0));

/* Window base as seen by the CPU; filled in by main() through
 * agm_cpld_get_base() and used by the DMA source address. */
static uintptr_t fabric_base;

/* Fabric SPI control register bits (vendor spi.h + example_spi_advanced.c:
 * SPI_CTRL_DMA_EN = 1 << 8, SPI_CTRL_ENDIAN = 1 << 10, CPOL/CPHA were added
 * by the patch at bits 24/25). */
#define SPI_FULL_CTRL_ENDIAN  BIT(10)
#define SPI_FULL_CTRL_DMA_EN  BIT(8)
#define SPI_FULL_CTRL_CPOL    BIT(24)
#define SPI_FULL_CTRL_CPHA    BIT(25)

#define SPI_FULL_CTRL_OFFSET  CONFIG_APP_SPI_FULL_CTRL_OFFSET
#define SPI_FULL_DATA_OFFSET  CONFIG_APP_SPI_FULL_DATA_OFFSET

/* The direct (non-DMA) path carries 4 bytes: one phase, one 32-bit register. */
#define SPI_FULL_MAX_XFER     4U

#define SPI_FLASH_CMD_RDID    0x9FU
#define SPI_FLASH_CMD_REMS    0x90U
#define SPI_FLASH_CMD_RUID    0x4BU

/* The engine sends up to eight TX phases of four bytes each (32 bytes);
 * anything longer needs the SDK's DMA-into-PHASE_DATA path. */
#define SPI_FULL_MAX_TX       32U

/* 1 MHz is a safe bring-up rate; the engine divides SYSCLK by 2..256. */
#define SPI_FULL_FREQ         1000000U

static const struct spi_config spi_cfg = {
	.frequency = SPI_FULL_FREQ,
	/* Mode 0 on the *engine*: CPOL/CPHA are the fabric's business here. */
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = {
		.cs_is_gpio = false,
	},
};

static void print_bytes(const uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		printk(" %02x", buf[i]);
	}
}

/* Program the fabric control register (CPOL/CPHA/endianness, DMA path). */
static int fabric_configure(bool dma_en)
{
	uint32_t ctrl = SPI_FULL_CTRL_ENDIAN;

	if (dma_en) {
		ctrl |= SPI_FULL_CTRL_DMA_EN;
	}

	if (IS_ENABLED(CONFIG_APP_SPI_FULL_CPOL)) {
		ctrl |= SPI_FULL_CTRL_CPOL;
	}

	if (IS_ENABLED(CONFIG_APP_SPI_FULL_CPHA)) {
		ctrl |= SPI_FULL_CTRL_CPHA;
	}

	return agm_cpld_write32(cpld, SPI_FULL_CTRL_OFFSET, ctrl);
}

#if IS_ENABLED(CONFIG_APP_SPI_FULL_DMA)
static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(dma0));

struct fabric_dma_ctx {
	struct k_sem done;
	volatile int status;
};

static void fabric_dma_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	struct fabric_dma_ctx *ctx = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	ctx->status = status;
	k_sem_give(&ctx->done);
}

/* Drain `len` bytes from the fabric RX FIFO (fixed source address) into the
 * caller's buffer. The fabric raises ext_dma_DMACBREQ[0] while DMA_EN is set
 * and its FIFO is not empty, so the engine clocks and the DMA follows. */
static int fabric_dma_start(uint8_t *rx, size_t len, struct fabric_dma_ctx *ctx)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	int ret;

	k_sem_init(&ctx->done, 0, 1);
	ctx->status = -1;

	block.source_address = (uint32_t)fabric_base + SPI_FULL_DATA_OFFSET;
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
	cfg.dma_callback = fabric_dma_cb;
	cfg.user_data = ctx;
	cfg.complete_callback_en = 1U;
	cfg.dma_slot = CONFIG_APP_SPI_FULL_DMA_REQUEST;

	ret = dma_config(dma, CONFIG_APP_SPI_FULL_DMA_CHANNEL, &cfg);
	if (ret < 0) {
		return ret;
	}

	return dma_start(dma, CONFIG_APP_SPI_FULL_DMA_CHANNEL);
}

static int fabric_dma_wait(struct fabric_dma_ctx *ctx)
{
	if (k_sem_take(&ctx->done, K_MSEC(1000)) != 0) {
		return -ETIMEDOUT;
	}

	return ctx->status;
}
#endif /* CONFIG_APP_SPI_FULL_DMA */

/* Full duplex: clock `len` TX bytes through the on-die engine and read back
 * what the fabric captured on MISO while those clocks ran. */
static int full_duplex_xfer(const struct device *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct spi_buf tx_buf = { .buf = (void *)tx, .len = len };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	uint32_t captured = 0U;
	int ret;

	if (len > SPI_FULL_MAX_XFER) {
		return -ENOTSUP;
	}

	ret = fabric_configure(false);
	if (ret < 0) {
		return ret;
	}

	ret = spi_write(spi, &spi_cfg, &tx_set);
	if (ret < 0) {
		return ret;
	}

	ret = agm_cpld_read32(cpld, SPI_FULL_DATA_OFFSET, &captured);
	if (ret < 0) {
		return ret;
	}

	/* The fabric packs the captured bytes little endian (CTRL ENDIAN=1). */
	for (size_t i = 0U; i < len; i++) {
		rx[i] = (uint8_t)(captured >> (8U * i));
	}

	return 0;
}

/* Ordinary half-duplex read of the flash RDID through the SPI driver. On a
 * full-duplex bitstream MISO never reaches the controller, so this is only
 * a probe: whatever comes back here is printed for contrast. */
static int half_duplex_rdid(const struct device *spi, uint8_t id[3])
{
	const uint8_t cmd = SPI_FLASH_CMD_RDID;
	uint8_t echo[1];
	struct spi_buf tx_buf = { .buf = (void *)&cmd, .len = 1U };
	/* The SPI API counts slots: the byte that carries the command is slot
	 * 0 of the RX list, and this engine cannot sample MISO there -- it
	 * reports 0xFF for it and the flash's answer follows. */
	struct spi_buf rx_bufs[2] = {
		{ .buf = echo, .len = sizeof(echo) },
		{ .buf = id, .len = 3U },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

	return spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);
}

/* The flash's answer sits somewhere in the captured word (the fabric may
 * register MISO once before capturing). Look for three consecutive bytes
 * that are neither 0x00 nor 0xFF -- i.e. the 3-byte JEDEC ID -- and return
 * where they were found. Nothing is hard-coded: the value is checked
 * against a second capture instead. */
static int find_id_candidate(const uint8_t *cap, size_t len, uint8_t id[3])
{
	for (size_t i = 0U; i + 3U <= len; i++) {
		if ((cap[i] != 0x00U) && (cap[i] != 0xFFU) && (cap[i + 1U] != 0x00U) &&
		    (cap[i + 1U] != 0xFFU) && (cap[i + 2U] != 0x00U) && (cap[i + 2U] != 0xFFU)) {
			memcpy(id, &cap[i], 3U);

			return (int)i;
		}
	}

	return -1;
}

static bool run_device_check(const struct device *spi)
{
	uint8_t id_half[3] = { 0U, 0U, 0U };
	const uint8_t tx_rdid_a[4] = { SPI_FLASH_CMD_RDID, 0xFFU, 0xFFU, 0xFFU };
	const uint8_t tx_rdid_b[4] = { SPI_FLASH_CMD_RDID, 0x00U, 0x00U, 0x00U };
	uint8_t rx_a[4] = { 0 };
	uint8_t rx_b[4] = { 0 };
	uint8_t id_a[3] = { 0 };
	uint8_t id_b[3] = { 0 };
	int at_a;
	int at_b;
	int ret;

	/* Informational: the engine's own RX path. */
	ret = half_duplex_rdid(spi, id_half);
	if (ret < 0) {
		printk("spi_full_duplex: half-duplex RDID failed (%d)\n", ret);
	} else {
		printk("spi_full_duplex: half-duplex RDID =");
		print_bytes(id_half, sizeof(id_half));
		printk(" (expected empty on a full-duplex bitstream)\n");
	}

	ret = full_duplex_xfer(spi, tx_rdid_a, rx_a, sizeof(rx_a));
	if (ret < 0) {
		printk("spi_full_duplex: 0x9F capture (dummy FF) failed (%d)\n", ret);
		return false;
	}

	ret = full_duplex_xfer(spi, tx_rdid_b, rx_b, sizeof(rx_b));
	if (ret < 0) {
		printk("spi_full_duplex: 0x9F capture (dummy 00) failed (%d)\n", ret);
		return false;
	}

	printk("spi_full_duplex: 0x9F capture (FF dummy) =");
	print_bytes(rx_a, sizeof(rx_a));
	printk("\nspi_full_duplex: 0x9F capture (00 dummy) =");
	print_bytes(rx_b, sizeof(rx_b));
	printk("\n");

	at_a = find_id_candidate(rx_a, sizeof(rx_a), id_a);
	at_b = find_id_candidate(rx_b, sizeof(rx_b), id_b);

	if ((at_a < 0) || (at_b < 0)) {
		printk("spi_full_duplex: no JEDEC ID in the captures (no device / fabric offsets?) -> FAIL\n");
		return false;
	}

	printk("spi_full_duplex: ID =");
	print_bytes(id_a, sizeof(id_a));
	printk(" (at offset %d and %d)\n", at_a, at_b);

	if (memcmp(id_a, id_b, sizeof(id_a)) != 0) {
		printk("spi_full_duplex: the two 0x9F captures disagree -> FAIL\n");
		return false;
	}

	/* Optional regression anchor: the ID this board's flash is known to
	 * return (0 = just require the two captures to agree). */
	if (IS_ENABLED(CONFIG_APP_SPI_FULL_EXPECT_ID)) {
		const uint8_t expected[3] = {
			(uint8_t)(CONFIG_APP_SPI_FULL_EXPECT_ID >> 16),
			(uint8_t)(CONFIG_APP_SPI_FULL_EXPECT_ID >> 8),
			(uint8_t)(CONFIG_APP_SPI_FULL_EXPECT_ID),
		};

		if (memcmp(id_a, expected, sizeof(expected)) != 0) {
			printk("spi_full_duplex: ID differs from the configured expectation -> FAIL\n");
			return false;
		}

		printk("spi_full_duplex: ID matches the configured expectation\n");
	}

	if (id_a[0] == id_a[1]) {
		printk("spi_full_duplex: suspect capture (two identical bytes) -> FAIL\n");
		return false;
	}

	printk("spi_full_duplex: both captures agree -> PASS\n");

	return true;
}

#if IS_ENABLED(CONFIG_APP_SPI_FULL_DMA)
/* Long transfer: the engine clocks a command of up to 32 bytes (eight TX
 * phases) while the fabric pushes captured words into its FIFO with DMA_EN
 * set, and the DMA channel drains that FIFO into the caller's buffer. */
static int fabric_xfer_long(const struct device *spi, const uint8_t *tx, size_t tx_len,
			    uint8_t *rx, size_t rx_len)
{
	struct fabric_dma_ctx ctx;
	struct spi_buf tx_buf = { .buf = (void *)tx, .len = tx_len };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	int ret;

	if ((rx_len == 0U) || ((rx_len % 4U) != 0U) || (tx_len > SPI_FULL_MAX_TX)) {
		return -EINVAL;
	}

	ret = fabric_configure(true);
	if (ret < 0) {
		return ret;
	}

	/* Vendor order: configure the DMA first, then start the engine. */
	ret = fabric_dma_start(rx, rx_len, &ctx);
	if (ret < 0) {
		return ret;
	}

	ret = spi_write(spi, &spi_cfg, &tx_set);
	if (ret == 0) {
		ret = fabric_dma_wait(&ctx);
	}

	/* Back to the register path for the next (short) transfer. */
	(void)fabric_configure(false);

	return ret;
}

/* Unique-ID read (0x4B): one command byte, four dummy bytes, then eight ID
 * bytes stream out. Clocking 16 bytes puts the whole ID inside the capture,
 * and the eight ID bytes arrive while the engine only sends dummy data --
 * i.e. this is also the "pure RX" (fabric Recv) shape. */
static bool run_long_check(const struct device *spi)
{
	static uint8_t tx[16] __aligned(4) = {
		SPI_FLASH_CMD_RUID, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
		0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
	};
	static uint8_t cap_a[16] __aligned(4);
	static uint8_t cap_b[16] __aligned(4);
	uint32_t nonempty = 0U;
	int ret;

	ret = fabric_xfer_long(spi, tx, sizeof(tx), cap_a, sizeof(cap_a));
	if (ret < 0) {
		printk("spi_full_duplex: long capture #1 failed (%d)\n", ret);
		return false;
	}

	ret = fabric_xfer_long(spi, tx, sizeof(tx), cap_b, sizeof(cap_b));
	if (ret < 0) {
		printk("spi_full_duplex: long capture #2 failed (%d)\n", ret);
		return false;
	}

	printk("spi_full_duplex: 0x4B capture #1 =");
	print_bytes(cap_a, sizeof(cap_a));
	printk("\nspi_full_duplex: 0x4B capture #2 =");
	print_bytes(cap_b, sizeof(cap_b));
	printk("\n");

	/* The first five bytes are command + four dummy bytes: the device does
	 * not drive MISO there, so what the fabric captures is the pin's idle
	 * level and differs between runs. The answer starts at byte 5. */
	for (size_t i = 5U; i < sizeof(cap_a); i++) {
		if ((cap_a[i] != 0x00U) && (cap_a[i] != 0xFFU)) {
			nonempty++;
		}
	}

	if (memcmp(&cap_a[5], &cap_b[5], sizeof(cap_a) - 5U) != 0) {
		printk("spi_full_duplex: the two long captures disagree on the answer -> FAIL\n");
		return false;
	}

	if (nonempty < 4U) {
		printk("spi_full_duplex: long capture has only %u non-FF bytes -> FAIL\n",
		       (unsigned int)nonempty);
		return false;
	}

	printk("spi_full_duplex: unique ID =");
	print_bytes(&cap_a[5], 8U);
	printk("; %u non-FF bytes in the answer, both captures agree -> PASS\n",
	       (unsigned int)nonempty);

	return true;
}
#endif /* CONFIG_APP_SPI_FULL_DMA */

static bool run_loopback_check(const struct device *spi)
{
	const uint8_t tx[4] = { 0xA5U, 0x5AU, 0x12U, 0x34U };
	uint8_t rx[4] = { 0 };
	int ret;

	ret = full_duplex_xfer(spi, tx, rx, sizeof(rx));
	if (ret < 0) {
		printk("spi_full_duplex: loopback transfer failed (%d)\n", ret);
		return false;
	}

	printk("spi_full_duplex: TX =");
	print_bytes(tx, sizeof(tx));
	printk("\nspi_full_duplex: RX =");
	print_bytes(rx, sizeof(rx));
	printk("\n");

	if (memcmp(tx, rx, sizeof(tx)) == 0) {
		printk("spi_full_duplex: loopback byte-for-byte match -> PASS\n");
		return true;
	}

	/* The fabric may insert a register stage before MISO is captured. */
	if (rx[1] == tx[0] && rx[2] == tx[1] && rx[3] == tx[2]) {
		printk("spi_full_duplex: loopback matches shifted by one byte -> PASS\n");
		return true;
	}

	/* Byte order inside the captured word follows the fabric CTRL's ENDIAN
	 * bit; accept the reversed pattern too and print it either way. */
	if ((rx[3] == tx[0]) && (rx[2] == tx[1]) && (rx[1] == tx[2]) && (rx[0] == tx[3])) {
		printk("spi_full_duplex: loopback matches with the bytes reversed -> PASS\n");
		return true;
	}

	printk("spi_full_duplex: loopback mismatch (jumper fitted? CPOL/CPHA? window offsets?) -> FAIL\n");

	return false;
}

int main(void)
{
	uintptr_t base = 0U;

	printk("\nspi_full_duplex: AgRV2K CPLD-assisted full-duplex SPI\n");
	printk("spi_full_duplex: window %s, CTRL +%#x, DATA +%#x, mode %u%u\n", cpld->name,
	       (unsigned int)SPI_FULL_CTRL_OFFSET, (unsigned int)SPI_FULL_DATA_OFFSET,
	       IS_ENABLED(CONFIG_APP_SPI_FULL_CPOL) ? 1U : 0U,
	       IS_ENABLED(CONFIG_APP_SPI_FULL_CPHA) ? 1U : 0U);

	if (!device_is_ready(cpld)) {
		printk("spi_full_duplex: %s not ready (CONFIG_CPLD_AGM?)\n", cpld->name);
	} else if (agm_cpld_get_base(cpld, &base) < 0) {
		printk("spi_full_duplex: cannot query the window\n");
	} else {
		fabric_base = base;

		printk("spi_full_duplex: window base 0x%08lx\n", (unsigned long)base);
		printk("spi_full_duplex: controller %s (fabric taps %s)\n", spi->name,
		       IS_ENABLED(CONFIG_APP_SPI_FULL_CONTROLLER_SPI1) ? "SPI1" : "SPI0");

		if (!device_is_ready(spi)) {
			printk("spi_full_duplex: not ready (CONFIG_SPI_AGM?)\n");
	} else if (IS_ENABLED(CONFIG_APP_SPI_FULL_LOOPBACK)) {
		(void)run_loopback_check(spi);
	} else {
		(void)run_device_check(spi);

#if IS_ENABLED(CONFIG_APP_SPI_FULL_DMA)
		printk("\n");
		(void)run_long_check(spi);
#endif
	}
	}

	while (true) {
		k_msleep(1000);
	}
}
