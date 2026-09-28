/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * slave_spi - AgRV2K + Zephyr port of
 *   AgRV_pio/platforms/AgRV/examples/spi/slave_spi
 *
 * Topology
 * --------
 *   The FPGA bitstream instantiates two slave SPIs in user logic
 *   (sspi0_dma.v + sspi1_ahb.v), exposed to the MCU at MMIO_BASE
 *   (0x60000000) as AHB slaves. The on-die hardware SPI0/SPI1
 *   controllers act as masters, so the slave pins live on the same
 *   pins as the master pins (sspi0_csn/SPI0_CSN = PIN_1, etc.) and
 *   the bitstream wires the pair together internally.
 *
 *   See dts/riscv/agm/agrv2k-pins.dtsi + the sample's
 *   boards/agrv2k_407.overlay for the pin map (now generated into
 *   <board_dir>/board.ve by tools/generate_board_ve.py), and
 *   the board.ve notes for the dts-driven board.ve pipeline.
 *
 * Driver coverage
 * ---------------
 *   - sspi0 / sspi1 control + data registers are poked directly via
 *     sys_write32 / sys_read32 (no upstream Zephyr API; logic-side
 *     registers, not a real peripheral).
 *   - DMA channel for sspi0 RX/TX uses the Zephyr dma_agm driver.
 *   - SPI0 / SPI1 master controllers are programmed via raw register
 *     writes (no spi_agm driver exists; the upstream Zephyr SPI API
 *     does not cover AgRV and the AgRV SDK helpers haven't been
 *     ported).
 *
 * Test matrix
 * -----------
 *   run_spi_reg()    - sspi0 RX/TX with direct register access, slow SCK
 *   run_sspi0_dma_rx - sspi0 RX with dma_agm, fast SCK
 *   run_sspi0_dma_tx - sspi0 TX with dma_agm, fast SCK
 *   run_sspi1_ahb()  - sspi1 RX/TX, logic drives AHB master, fast SCK
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

/* SSPI register window (logic-side AHB slave @ MMIO_BASE).
 * Layout matches slave_spi/logic/slave_spi.v in the SDK example:
 *   ADDR_SSPI0_CTRL    = SSPI_BASE + 0x000
 *   ADDR_SSPI0_DATA    = SSPI_BASE + 0x004
 *   ADDR_SSPI1_CTRL    = SSPI_BASE + 0x100
 *   ADDR_SSPI1_RX_ADDR = SSPI_BASE + 0x104
 *   ADDR_SSPI1_TX_ADDR = SSPI_BASE + 0x108
 */
#define MMIO_BASE                 0x60000000UL
#define ADDR_SSPI0_CTRL           (MMIO_BASE + 0x000UL)
#define ADDR_SSPI0_DATA           (MMIO_BASE + 0x004UL)
#define ADDR_SSPI1_CTRL           (MMIO_BASE + 0x100UL)
#define ADDR_SSPI1_RX_ADDR        (MMIO_BASE + 0x104UL)
#define ADDR_SSPI1_TX_ADDR        (MMIO_BASE + 0x108UL)

/* SSPI_CTRL bit layout (slave_spi.v line 73-78):
 *   [0]  RX_EN
 *   [1]  RX_DMA_EN
 *   [2]  RX_VALID     (RO for sspi0; sspi1 mirrors via _valid)
 *   [16] TX_EN
 *   [17] TX_DMA_EN
 *   [18] TX_READY     (RO)
 */
#define SSPI_RX_EN_BIT            (1U << 0)
#define SSPI_RX_DMA_EN_BIT        (1U << 1)
#define SSPI_RX_VALID_BIT         (1U << 2)
#define SSPI_TX_EN_BIT            (1U << 16)
#define SSPI_TX_DMA_EN_BIT        (1U << 17)
#define SSPI_TX_READY_BIT         (1U << 18)

/* SPI0 / SPI1 master controller register window.
 * AltaRiscv.h: SPI0_BASE = 0x40012000, SPI1_BASE = 0x40013000.
 * Same SPI_TypeDef layout for both:
 *   CTRL      @ +0x00  (START|DONE|ERROR|INT_CLR|DMA_EN|ENDIAN|INT_EN|
 *                       PHASE_CNT[6:4]|SCLK_DIV[19:12]|RESET[31])
 *   PHASE_CTRL[0..7] @ +0x10..0x2C  (ACTION[5:4]|MODE[21:20]|
 *                                    BYTE_CNT[19:8])
 *   PHASE_DATA[0..7] @ +0x30..0x4C
 */
#define SPI0_BASE                 0x40012000UL
#define SPI1_BASE                 0x40013000UL
#define SPI_REG(base, off)        ((volatile uint32_t *)(base + (off)))
#define SPI_CTRL(spi_base)        SPI_REG(spi_base, 0x00U)
#define SPI_PHASE_CTRL(spi_base, n) SPI_REG(spi_base, 0x10U + (n) * 4U)
#define SPI_PHASE_DATA(spi_base, n)  SPI_REG(spi_base, 0x30U + (n) * 4U)

#define SPI_CTRL_START            (1U << 0)
#define SPI_CTRL_DONE             (1U << 1)
#define SPI_CTRL_ERROR            (1U << 2)
#define SPI_CTRL_INT_CLR          (1U << 3)
#define SPI_CTRL_DMA_EN           (1U << 8)
#define SPI_CTRL_ENDIAN           (1U << 10)
#define SPI_CTRL_INT_EN           (1U << 20)
#define SPI_CTRL_RESET            (1U << 31)

#define SPI_CTRL_SCLK_DIV_OFFSET  12U
#define SPI_CTRL_SCLK_DIV16       (16U << SPI_CTRL_SCLK_DIV_OFFSET)
#define SPI_CTRL_SCLK_DIV4        (4U  << SPI_CTRL_SCLK_DIV_OFFSET)
#define SPI_CTRL_PHASE_CNT1       (0U << 4)

#define SPI_PHASE_START           (1U << 0)
#define SPI_PHASE_DONE            (1U << 1)
#define SPI_PHASE_ERROR           (1U << 2)
#define SPI_PHASE_ACTION_TX       (0U << 4)
#define SPI_PHASE_ACTION_RX       (2U << 4)
#define SPI_PHASE_BYTE_CNT_OFFSET 8U
#define SPI_PHASE_BYTE_CNT_MASK   (0xfffU << SPI_PHASE_BYTE_CNT_OFFSET)

#define SPI_PHASE_MODE_SINGLE     (0U << 20)

/* AHB clock gate for SPI in SYS.AHB_CLKENABLE @ 0x03000000 + 0x70.
 * Bits per AltaRiscv.h: AHB_MASK_SPI0 = 6, AHB_MASK_SPI1 = 7.
 * soc.c gates SPI0/1 at PRE_KERNEL_1; included for completeness in
 * case this sample is run on a board with a stripped soc init.
 */
#define SYS_BASE                  0x03000000UL
#define SYS_AHB_CLKENABLE         (SYS_BASE + 0x70UL)
#define AHB_MASK_SPI0             (1U << 6)
#define AHB_MASK_SPI1             (1U << 7)

/* DMA peripheral-to-memory / memory-to-peripheral request numbers.
 * AltaRiscv.h:
 *   SPI0_RX_DMA_REQ = EXT_DMA0_REQ = 0
 *   SPI0_TX_DMA_REQ = EXT_DMA1_REQ = 1
 *   SPI1_RX_DMA_REQ = EXT_DMA2_REQ = 2
 *   SPI1_TX_DMA_REQ = EXT_DMA3_REQ = 3
 *
 * For the SSPI lines, slave_spi.v:94-95 wires:
 *   assign ext_dma_DMACBREQ[0] = sspi0_rx_dma_req;
 *   assign ext_dma_DMACBREQ[1] = sspi0_tx_dma_req;
 * i.e. SSPI0 RX/TX ride EXT_DMA0_REQ (= 0) and EXT_DMA1_REQ (= 1)
 * — the same peripheral request lines as SPI0 RX/TX, but on a
 * different DMA channel. The SDK's run_spi_dma() uses EXT_DMA0_REQ
 * / EXT_DMA1_REQ for SSPI0 too; the previous 4/5 values here were
 * wrong (no ext_dma line is wired to sspi0_rx/tx_dma_req in 4/5).
 *
 *   SSPI1 has no DMA (AHB-master mode, logic writes SRAM directly).
 */
#define SPI0_RX_DMA_REQ           0U
#define SPI0_TX_DMA_REQ           1U
#define SPI1_RX_DMA_REQ           2U
#define SPI1_TX_DMA_REQ           3U
#define SSPI0_RX_DMA_REQ          0U
#define SSPI0_TX_DMA_REQ          1U

/* DMA channels. Channels 0-5 are wired to the hardware DMA request
 * lines (one channel per EXT_DMA0..5_REQ). Channels 6-7 are
 * software-triggered; the SDK uses them for the master-side SPI
 * Send/SendAndReceive. The Zephyr dma_agm driver configures them
 * with the peripheral request index passed in dma_slot, so the
 * channel number and the request number are independent.
 *
 * SDK defines (AltaRiscv.h + slave_spi/src/main.c):
 *   SSPI_DMAC_RX_CHANNEL = DMAC_CHANNEL0
 *   SSPI_DMAC_TX_CHANNEL = DMAC_CHANNEL1
 *   MSPI_DMAC_RX_CHANNEL = DMAC_CHANNEL6
 *   MSPI_DMAC_TX_CHANNEL = DMAC_CHANNEL7
 */
#define SSPI_DMAC_RX_CHANNEL      0U
#define SSPI_DMAC_TX_CHANNEL      1U
#define MSPI_DMAC_RX_CHANNEL      6U
#define MSPI_DMAC_TX_CHANNEL      7U

#define SPI_WORDS                 16U
#define DMA_TIMEOUT_MS            2000U

#define DMA0 DT_NODELABEL(dma0)

/* ---------- helpers ---------------------------------------------------- */

static inline void sspi_wr(uint32_t addr, uint32_t val)
{
	sys_write32(val, addr);
}

static inline uint32_t sspi_rd(uint32_t addr)
{
	return sys_read32(addr);
}

static inline void spi_reset(uint32_t spi_base)
{
	*SPI_CTRL(spi_base) |= SPI_CTRL_RESET;
	*SPI_CTRL(spi_base) &= ~SPI_CTRL_RESET;
}

static inline void spi_init(uint32_t spi_base, uint32_t sclk_div)
{
	spi_reset(spi_base);
	/* Preserve SCLK divider, clear all status bits. */
	*SPI_CTRL(spi_base) = sclk_div;
}

static inline bool spi_is_done(uint32_t spi_base)
{
	return (*SPI_CTRL(spi_base) & SPI_CTRL_DONE) != 0U;
}

static inline void spi_wait_done(uint32_t spi_base)
{
	while (!spi_is_done(spi_base)) {
	}
}

/* Wait until SSPI has a fresh RX word. Reads CTRL until RX_VALID
 * latches, then the next RD_REG(SSPI0_DATA) returns the new word.
 */
static inline void sspi0_wait_rx_valid(void)
{
	while ((sspi_rd(ADDR_SSPI0_CTRL) & SSPI_RX_VALID_BIT) == 0U) {
	}
}

/* Wait until SSPI's TX FIFO has room for one more 32-bit word.
 * The TX_READY bit clears when the controller reads out the last
 * word we wrote; we can write the next one only after it re-asserts.
 */
static inline void sspi0_wait_tx_ready(void)
{
	while ((sspi_rd(ADDR_SSPI0_CTRL) & SSPI_TX_READY_BIT) == 0U) {
	}
}

/* Master-side: send `tx_bytes` of `tx_data` (padded to 4-byte words)
 * through `spi_base` using a single TX phase. Used to drive the
 * internal SPI master so it generates SCK/MOSI/CSN for the slave.
 *
 * `tx_words` is the word count (= tx_bytes / 4). We put tx_data into
 * PHASE_DATA[0] once and let the master repeat it via... no, the
 * controller does NOT repeat — each PHASE_DATA[n] is one word per
 * transfer. For SPI_WORDS words we must use DMA on the master side,
 * or set up multiple phases.
 *
 * Easiest: write one phase per word, START it, wait DONE. Slow but
 * matches what the SDK example does with DMA at DIV16. We use DMA
 * for fast cases (DIV4).
 */
static void spi_send_blocking(uint32_t spi_base,
			      const uint32_t *tx_data, uint32_t tx_words)
{
	for (uint32_t i = 0; i < tx_words; i++) {
		*SPI_PHASE_CTRL(spi_base, 0) =
			SPI_PHASE_ACTION_TX | SPI_PHASE_MODE_SINGLE |
			((4U << SPI_PHASE_BYTE_CNT_OFFSET) &
			 SPI_PHASE_BYTE_CNT_MASK);
		*SPI_PHASE_DATA(spi_base, 0) = tx_data[i];
		*SPI_CTRL(spi_base) = SPI_CTRL_SCLK_DIV16 |
			SPI_CTRL_START | SPI_CTRL_PHASE_CNT1;
		spi_wait_done(spi_base);
		/* Clear DONE for the next iteration. */
		*SPI_CTRL(spi_base) = SPI_CTRL_SCLK_DIV16;
	}
}

/* ---------- DMA plumbing for the master SPI TX ------------------------- */

struct dma_ctx {
	struct k_sem done;
	volatile int status;
};

static void dma_cb(const struct device *dev, void *user_data,
		   uint32_t channel, int status)
{
	struct dma_ctx *ctx = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	ctx->status = status;
	k_sem_give(&ctx->done);
}

/* Configure dma_agm for a single MEM_TO_PERIPH transfer on `channel`
 * from `src` (SRAM, address-increment) to `dst` (peripheral MMIO,
 * address-fixed). 32-bit wide on both sides, burst 1.
 */
static int dma_xfer_mem_to_periph(const struct device *dma,
				  uint32_t channel,
				  uint32_t src, uint32_t dst, uint32_t bytes,
				  uint32_t periph_req)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	struct dma_ctx ctx = { 0 };
	int rc;

	k_sem_init(&ctx.done, 0, 1);

	block.source_address = src;
	block.dest_address = dst;
	block.block_size = bytes;

	cfg.channel_direction = MEMORY_TO_PERIPHERAL;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.source_burst_length = 1U;
	cfg.dest_burst_length = 1U;
	cfg.block_count = 1U;
	cfg.head_block = &block;
	cfg.dma_callback = dma_cb;
	cfg.user_data = &ctx;
	cfg.complete_callback_en = 1U;
	/* The dma_agm driver needs the peripheral request line in
	 * dma_slot. It maps to the EXT_DMA<n>_REQ mux that the SPI
	 * controllers are wired to.
	 */
	cfg.dma_slot = periph_req;

	rc = dma_config(dma, channel, &cfg);
	if (rc != 0) {
		return rc;
	}

	ctx.status = -1;
	k_sem_reset(&ctx.done);
	rc = dma_start(dma, channel);
	if (rc != 0) {
		return rc;
	}

	if (k_sem_take(&ctx.done, K_MSEC(DMA_TIMEOUT_MS)) != 0) {
		return -ETIMEDOUT;
	}

	return ctx.status;
}

static int dma_xfer_periph_to_mem(const struct device *dma,
				  uint32_t channel,
				  uint32_t src, uint32_t dst, uint32_t bytes,
				  uint32_t periph_req)
{
	struct dma_block_config block = { 0 };
	struct dma_config cfg = { 0 };
	struct dma_ctx ctx = { 0 };
	int rc;

	k_sem_init(&ctx.done, 0, 1);

	block.source_address = src;
	block.dest_address = dst;
	block.block_size = bytes;

	cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	cfg.source_data_size = 4U;
	cfg.dest_data_size = 4U;
	cfg.source_burst_length = 1U;
	cfg.dest_burst_length = 1U;
	cfg.block_count = 1U;
	cfg.head_block = &block;
	cfg.dma_callback = dma_cb;
	cfg.user_data = &ctx;
	cfg.complete_callback_en = 1U;
	cfg.dma_slot = periph_req;

	rc = dma_config(dma, channel, &cfg);
	if (rc != 0) {
		return rc;
	}

	ctx.status = -1;
	k_sem_reset(&ctx.done);
	rc = dma_start(dma, channel);
	if (rc != 0) {
		return rc;
	}

	if (k_sem_take(&ctx.done, K_MSEC(DMA_TIMEOUT_MS)) != 0) {
		return -ETIMEDOUT;
	}

	return ctx.status;
}

/* DMA-driven master TX: program one TX phase with BYTE_CNT = bytes,
 * START the controller, and let dma_agm feed it from `tx_data`. The
 * controller does byte-by-byte DMA handshakes on TX_DMA_REQ.
 */
static int spi_send_dma(uint32_t spi_base,
			const uint32_t *tx_data, uint32_t bytes,
			const struct device *dma, uint32_t channel)
{
	uint32_t spi_tx_fifo;
	int rc;

	/* SPI0 TX FIFO lives at PHASE_DATA[0] (write-only TX FIFO when
	 * the phase is configured as ACTION_TX). The SDK uses
	 * SPI0_TX_DMA_REQ = 1.
	 */
	spi_tx_fifo = spi_base + 0x30U; /* offsetof PHASE_DATA[0] */

	/* Program a single TX phase covering all bytes. */
	*SPI_PHASE_CTRL(spi_base, 0) =
		SPI_PHASE_ACTION_TX | SPI_PHASE_MODE_SINGLE |
		((bytes << SPI_PHASE_BYTE_CNT_OFFSET) &
		 SPI_PHASE_BYTE_CNT_MASK);
	*SPI_PHASE_DATA(spi_base, 0) = tx_data[0];

	*SPI_CTRL(spi_base) = SPI_CTRL_SCLK_DIV4 |
		SPI_CTRL_DMA_EN | SPI_CTRL_START | SPI_CTRL_PHASE_CNT1;

	rc = dma_xfer_mem_to_periph(dma, channel,
				    (uint32_t)(uintptr_t)tx_data,
				    spi_tx_fifo, bytes,
				    (spi_base == SPI0_BASE)
				    ? SPI0_TX_DMA_REQ
				    : SPI1_TX_DMA_REQ);
	if (rc != 0) {
		return rc;
	}
	spi_wait_done(spi_base);
	*SPI_CTRL(spi_base) = SPI_CTRL_SCLK_DIV4;
	return 0;
}

static int spi_xfer_dma(uint32_t spi_base,
			const uint32_t *tx_data, uint32_t tx_bytes,
			uint32_t *rx_data, uint32_t rx_bytes,
			const struct device *dma,
			uint32_t tx_channel, uint32_t rx_channel)
{
	uint32_t spi_tx_fifo = spi_base + 0x30U;
	uint32_t spi_rx_fifo = spi_base + 0x30U; /* same offset, read mode */
	int rc;

	*SPI_PHASE_CTRL(spi_base, 0) =
		SPI_PHASE_ACTION_TX | SPI_PHASE_MODE_SINGLE |
		((tx_bytes << SPI_PHASE_BYTE_CNT_OFFSET) &
		 SPI_PHASE_BYTE_CNT_MASK);
	*SPI_PHASE_DATA(spi_base, 0) = tx_data[0];

	*SPI_CTRL(spi_base) = SPI_CTRL_SCLK_DIV4 |
		SPI_CTRL_DMA_EN | SPI_CTRL_START | SPI_CTRL_PHASE_CNT1;

	rc = dma_xfer_mem_to_periph(dma, tx_channel,
				    (uint32_t)(uintptr_t)tx_data,
				    spi_tx_fifo, tx_bytes,
				    (spi_base == SPI0_BASE)
				    ? SPI0_TX_DMA_REQ
				    : SPI1_TX_DMA_REQ);
	if (rc != 0) {
		return rc;
	}

	rc = dma_xfer_periph_to_mem(dma, rx_channel,
				    spi_rx_fifo,
				    (uint32_t)(uintptr_t)rx_data,
				    rx_bytes,
				    (spi_base == SPI0_BASE)
				    ? SPI0_RX_DMA_REQ
				    : SPI1_RX_DMA_REQ);
	if (rc != 0) {
		return rc;
	}
	spi_wait_done(spi_base);
	*SPI_CTRL(spi_base) = SPI_CTRL_SCLK_DIV4;
	return 0;
}

/* ---------- test buffers --------------------------------------------- */

static uint32_t mspi_tx_data[SPI_WORDS];
static uint32_t mspi_rx_data[SPI_WORDS];
static uint32_t sspi_rx_data[SPI_WORDS];
static uint32_t sspi_tx_data[SPI_WORDS + 1];

static uint32_t xorshift32(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x ? x : 1U;
	return x;
}

static void init_data(void)
{
	uint32_t rng = 0x12345U;

	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		mspi_tx_data[i] = xorshift32(&rng);
		mspi_rx_data[i] = xorshift32(&rng);
		sspi_tx_data[i] = xorshift32(&rng);
		sspi_rx_data[i] = xorshift32(&rng);
	}
	sspi_tx_data[SPI_WORDS] = xorshift32(&rng);
}

/* ---------- tests ----------------------------------------------------- */

/* Run sspi0 in pure-register mode with the master driven by DMA at
 * DIV16. Mirrors run_spi_reg() in the SDK example.
 */
static int run_spi_reg(void)
{
	int rc;

	printk("slave_spi: start testing sspi0 RX (reg mode)\n");
	init_data();

	sspi_wr(ADDR_SSPI0_CTRL, SSPI_RX_EN_BIT);
	spi_send_blocking(SPI0_BASE, mspi_tx_data, SPI_WORDS);
	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		sspi0_wait_rx_valid();
		sspi_rx_data[i] = sspi_rd(ADDR_SSPI0_DATA);
		if (sspi_rx_data[i] != mspi_tx_data[i]) {
			printk("slave_spi: RX mismatch @ %u: got 0x%08x, "
			       "expected 0x%08x\n",
			       i, sspi_rx_data[i], mspi_tx_data[i]);
			return -EIO;
		}
	}
	sspi_wr(ADDR_SSPI0_CTRL, 0);

	printk("slave_spi: start testing sspi0 TX (reg mode)\n");
	init_data();
	sspi_wr(ADDR_SSPI0_CTRL, SSPI_TX_EN_BIT);
	/* Master limitation: a dummy word is sent before actual TX
	 * data — write 0 first so the SSPI's TX path sees a known
	 * starting state.
	 */
	sspi_wr(ADDR_SSPI0_DATA, 0);
	/* Master does SendAndReceive(4, ..., SPI_WORDS*4, ...). We
	 * emulate by TX'ing one dummy word first (blocking, DIV16) and
	 * then DMAing the rest.
	 */
	spi_send_blocking(SPI0_BASE, mspi_tx_data, 1);
	rc = spi_xfer_dma(SPI0_BASE, mspi_tx_data + 1, 4,
			  mspi_rx_data, SPI_WORDS * 4,
			  DEVICE_DT_GET(DMA0),
			  MSPI_DMAC_TX_CHANNEL, MSPI_DMAC_RX_CHANNEL);
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		sspi0_wait_tx_ready();
		sspi_wr(ADDR_SSPI0_DATA, sspi_tx_data[i]);
	}
	while ((sspi_rd(ADDR_SSPI0_CTRL) & SSPI_TX_READY_BIT) != 0U) {
		/* last word draining */
	}
	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		if (sspi_tx_data[i] != mspi_rx_data[i]) {
			printk("slave_spi: TX mismatch @ %u: got 0x%08x, "
			       "expected 0x%08x\n",
			       i, mspi_rx_data[i], sspi_tx_data[i]);
			return -EIO;
		}
	}
	sspi_wr(ADDR_SSPI0_CTRL, 0);

	return 0;
}

/* sspi0 RX via DMA. The slave pushes data into ADDR_SSPI0_DATA; the
 * MCU programs dma_agm to drain it into SRAM as EXT_DMA0_REQ (==
 * SSPI0_RX_DMA_REQ) fires (slave_spi.v:94 wires sspi0_rx_dma_req
 * to ext_dma_DMACBREQ[0]).
 */
static int run_sspi0_dma_rx(const struct device *dma)
{
	int rc;

	printk("slave_spi: start testing sspi0 RX with DMA\n");
	init_data();

	sspi_wr(ADDR_SSPI0_CTRL, SSPI_RX_DMA_EN_BIT | SSPI_RX_EN_BIT);

	rc = dma_xfer_periph_to_mem(dma, SSPI_DMAC_RX_CHANNEL,
				    ADDR_SSPI0_DATA,
				    (uint32_t)(uintptr_t)sspi_rx_data,
				    SPI_WORDS * 4,
				    SSPI0_RX_DMA_REQ);
	if (rc != 0) {
		return rc;
	}

	rc = spi_send_dma(SPI0_BASE, mspi_tx_data, SPI_WORDS * 4,
			  dma, MSPI_DMAC_TX_CHANNEL);
	if (rc != 0) {
		return rc;
	}

	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		if (sspi_rx_data[i] != mspi_tx_data[i]) {
			printk("slave_spi: DMA RX mismatch @ %u: "
			       "got 0x%08x, expected 0x%08x\n",
			       i, sspi_rx_data[i], mspi_tx_data[i]);
			return -EIO;
		}
	}
	sspi_wr(ADDR_SSPI0_CTRL, 0);
	return 0;
}

/* sspi0 TX via DMA. Master reads RX while SSPI0 DMA pushes from SRAM
 * into ADDR_SSPI0_DATA via EXT_DMA1_REQ (== SSPI0_TX_DMA_REQ).
 */
static int run_sspi0_dma_tx(const struct device *dma)
{
	int rc;

	printk("slave_spi: start testing sspi0 TX with DMA\n");
	init_data();

	sspi_wr(ADDR_SSPI0_CTRL, SSPI_TX_DMA_EN_BIT | SSPI_TX_EN_BIT);

	rc = dma_xfer_mem_to_periph(dma, SSPI_DMAC_TX_CHANNEL,
				    (uint32_t)(uintptr_t)sspi_tx_data,
				    ADDR_SSPI0_DATA,
				    (SPI_WORDS + 1) * 4,
				    SSPI0_TX_DMA_REQ);
	if (rc != 0) {
		return rc;
	}

	rc = spi_xfer_dma(SPI0_BASE, mspi_tx_data, 4,
			  mspi_rx_data, SPI_WORDS * 4,
			  dma, MSPI_DMAC_TX_CHANNEL, MSPI_DMAC_RX_CHANNEL);
	if (rc != 0) {
		return rc;
	}

	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		/* Skip the first dummy word in the master RX buffer. */
		if (mspi_rx_data[i] != sspi_tx_data[i + 1]) {
			printk("slave_spi: DMA TX mismatch @ %u: "
			       "got 0x%08x, expected 0x%08x\n",
			       i, mspi_rx_data[i], sspi_tx_data[i + 1]);
			return -EIO;
		}
	}
	sspi_wr(ADDR_SSPI0_CTRL, 0);
	return 0;
}

/* sspi1 in AHB-master mode: the slave logic writes directly to SRAM
 * (no DMA on the slave side). MCU just sets RX_ADDR / TX_ADDR and
 * the slave engine streams bytes to/from SRAM as the master clocks
 * data in/out.
 */
static int run_sspi1_ahb(void)
{
	int rc;

	printk("slave_spi: start testing sspi1 RX (AHB mode)\n");
	init_data();

	sspi_wr(ADDR_SSPI1_RX_ADDR, (uint32_t)(uintptr_t)sspi_rx_data);
	sspi_wr(ADDR_SSPI1_CTRL, SSPI_RX_EN_BIT);
	rc = spi_send_dma(SPI1_BASE, mspi_tx_data, SPI_WORDS * 4,
			  DEVICE_DT_GET(DMA0), MSPI_DMAC_TX_CHANNEL);
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		if (sspi_rx_data[i] != mspi_tx_data[i]) {
			printk("slave_spi: sspi1 RX mismatch @ %u: "
			       "got 0x%08x, expected 0x%08x\n",
			       i, sspi_rx_data[i], mspi_tx_data[i]);
			return -EIO;
		}
	}
	sspi_wr(ADDR_SSPI1_CTRL, 0);

	printk("slave_spi: start testing sspi1 TX (AHB mode)\n");
	init_data();
	sspi_wr(ADDR_SSPI1_TX_ADDR, (uint32_t)(uintptr_t)sspi_tx_data);
	sspi_wr(ADDR_SSPI1_CTRL, SSPI_TX_EN_BIT);
	rc = spi_xfer_dma(SPI1_BASE, mspi_tx_data, 4,
			  mspi_rx_data, SPI_WORDS * 4,
			  DEVICE_DT_GET(DMA0),
			  MSPI_DMAC_TX_CHANNEL, MSPI_DMAC_RX_CHANNEL);
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < SPI_WORDS; i++) {
		if (mspi_rx_data[i] != sspi_tx_data[i + 1]) {
			/* Skip the first dummy word. */
			printk("slave_spi: sspi1 TX mismatch @ %u: "
			       "got 0x%08x, expected 0x%08x\n",
			       i, mspi_rx_data[i], sspi_tx_data[i + 1]);
			return -EIO;
		}
	}
	sspi_wr(ADDR_SSPI1_CTRL, 0);
	return 0;
}

int main(void)
{
	const struct device *dma = DEVICE_DT_GET(DMA0);
	int rc = 0;

	if (!device_is_ready(dma)) {
		printk("slave_spi: dma0 not ready\n");
		return 0;
	}

	/* Make sure SPI0/SPI1 AHB clocks are on (soc.c does this too,
	 * but a stripped or hand-rolled soc init may skip it).
	 */
	uint32_t ahb = sys_read32(SYS_AHB_CLKENABLE);

	sys_write32(ahb | AHB_MASK_SPI0 | AHB_MASK_SPI1, SYS_AHB_CLKENABLE);

	spi_init(SPI0_BASE, SPI_CTRL_SCLK_DIV16);
	spi_init(SPI1_BASE, SPI_CTRL_SCLK_DIV16);

	rc = run_spi_reg();
	if (rc != 0) {
		goto done;
	}

	rc = run_sspi0_dma_rx(dma);
	if (rc != 0) {
		goto done;
	}

	rc = run_sspi0_dma_tx(dma);
	if (rc != 0) {
		goto done;
	}

	rc = run_sspi1_ahb();

done:
	if (rc == 0) {
		printk("slave_spi: tests passed\n");
	} else {
		printk("slave_spi: tests failed (%d)\n", rc);
	}
	while (1) {
		k_msleep(1000);
	}
}
