/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * dma_memcpy — SRAM-to-SRAM DMA smoke test for the AgRV2K DMAC0
 * driver (dma_agm.c).
 *
 * Uses the Zephyr v1 DMA API:
 *   1. dma_config(channel 0, MEMORY_TO_MEMORY, 8-bit)
 *   2. dma_start()
 *   3. wait for the TC callback (k_sem) with a polling fallback
 *      through dma_get_status()
 *   4. verify the destination buffer equals the source
 *
 * The buffers live in SRAM (0x20000000), which both AHB masters of
 * the DMAC can reach.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>

#define DMAC0 DT_NODELABEL(dma0)
#define CHAN 0

#define BUF_SIZE 2048U
#define DMA_TIMEOUT_MS 2000

static uint8_t src_buf[BUF_SIZE] __aligned(4);
static uint8_t dst_buf[BUF_SIZE] __aligned(4);

static struct k_sem dma_done;
static volatile int dma_status;

static void dma_cb(const struct device *dev, void *user_data,
		   uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	dma_status = status;
	k_sem_give(&dma_done);
}

int main(void)
{
	const struct device *dma = DEVICE_DT_GET(DMAC0);
	struct dma_block_config block = { 0 };
	struct dma_config dma_cfg = { 0 };
	uint32_t iter = 0;
	int rc;

	k_sem_init(&dma_done, 0, 1);

	printk("dma_memcpy: DMAC0 = %s\n", dma->name);
	if (!device_is_ready(dma)) {
		printk("dma_memcpy: DMAC0 not ready\n");
		return 0;
	}

	/* Fill a deterministic pattern. */
	for (uint32_t i = 0; i < BUF_SIZE; i++) {
		src_buf[i] = (uint8_t)(i * 7U + 1U);
	}
	memset(dst_buf, 0xaa, BUF_SIZE);

	dma_cfg.channel_direction = MEMORY_TO_MEMORY;
	dma_cfg.source_data_size = 1U;
	dma_cfg.dest_data_size = 1U;
	dma_cfg.source_burst_length = 4U;
	dma_cfg.dest_burst_length = 4U;
	dma_cfg.block_count = 1U;
	dma_cfg.head_block = &block;
	dma_cfg.dma_callback = dma_cb;
	dma_cfg.user_data = NULL;
	dma_cfg.complete_callback_en = 0U;

	block.source_address = (uint32_t)(uintptr_t)src_buf;
	block.dest_address = (uint32_t)(uintptr_t)dst_buf;
	block.block_size = BUF_SIZE;

	while (1) {
		rc = dma_config(dma, CHAN, &dma_cfg);
		if (rc != 0) {
			printk("dma_memcpy: dma_config failed: %d\n", rc);
			return 0;
		}

		dma_status = -1;
		k_sem_reset(&dma_done);

		rc = dma_start(dma, CHAN);
		if (rc != 0) {
			printk("dma_memcpy: dma_start failed: %d\n", rc);
			return 0;
		}

		/* Prefer the completion callback; fall back to polling
		 * in case the PLIC lines are not wired in the bitstream. */
		if (k_sem_take(&dma_done, K_MSEC(DMA_TIMEOUT_MS)) != 0) {
			struct dma_status st;
			uint32_t tries = 1000000U;

			do {
				dma_get_status(dma, CHAN, &st);
			} while (st.busy && --tries > 0U);
			if (st.busy) {
				printk("dma_memcpy: transfer timeout\n");
				return 0;
			}
			dma_status = 0;
		}

		if (memcmp(src_buf, dst_buf, BUF_SIZE) == 0 &&
		    dma_status >= 0) {
			printk("dma_memcpy: iter %u OK (%u B)\n", iter,
			       BUF_SIZE);
		} else {
			printk("dma_memcpy: iter %u FAIL (status %d)\n",
			       iter, dma_status);
			return 0;
		}

		/* Rewrite dst so a stale success can't mask a no-op. */
		memset(dst_buf, 0x55, BUF_SIZE);
		iter++;
		k_msleep(500);
	}
}
