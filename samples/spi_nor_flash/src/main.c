/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi_nor_flash — the Zephyr SPI NOR flash driver on top of the AgRV2K SPI
 * controller.
 *
 * This is the pay-off of the SPI port: `jedec,spi-nor` talking through
 * spi_agm, whose phase engine does command/response transfers (TX phases,
 * then one RX phase) and streams the RX phase out of its data register with
 * DMA. The part on this board does not implement SFDP, so the devicetree
 * carries `jedec-id` and `size` and the driver runs in its
 * CONFIG_SPI_NOR_SFDP_MINIMAL configuration -- which also means the driver
 * checks the JEDEC ID at init: a wrong ID or a broken RX path fails
 * device_is_ready() instead of silently reading garbage.
 *
 * The read side of the sample is always read-only. With
 * CONFIG_APP_SPI_NOR_WRITE_TEST it also runs flash_erase() + flash_write()
 * over one 4 KiB sector and reads every byte back -- that is the API path
 * whose page program hands the SPI driver a two-buffer TX list, which only
 * works through the engine's TX DMA and the driver's TX bounce buffer.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define FLASH_NODE DT_NODELABEL(spi_nor)

#if !DT_NODE_HAS_STATUS(FLASH_NODE, okay)
#error "spi_nor is disabled: build with a board overlay that sets it okay"
#endif

#define READ_LEN 256U
#define WRITE_LEN 4096U

static const struct device *const flash = DEVICE_DT_GET(FLASH_NODE);

#if IS_ENABLED(CONFIG_APP_SPI_NOR_WRITE_TEST)
static uint8_t write_buf[WRITE_LEN] __aligned(4);
#endif

static void print_bytes(const uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		printk(" %02x", buf[i]);
	}
}

int main(void)
{
	static uint8_t page_a[READ_LEN] __aligned(4);
	static uint8_t page_b[READ_LEN] __aligned(4);
	const struct flash_parameters *params;
	struct flash_pages_info page;
	uint32_t erase_value;
	uint32_t non_erase = 0U;
	int ret;

	printk("\nspi_nor_flash: Zephyr spi-nor over AgRV2K SPI\n");

	/* Do not gate on device_is_ready() here: the spi-nor driver runs its
	 * bring-up (including the JEDEC ID check) in the runtime-PM resume
	 * action, which the first API call triggers, so the device may still
	 * report "not ready" right after boot. flash_read() below is that
	 * first call; a wrong bounce buffer, a bad RX path or an ID mismatch
	 * shows up as its error code. */
	params = flash_get_parameters(flash);
	erase_value = params->erase_value;

	printk("spi_nor_flash: %s before first use: %s\n", flash->name,
	       device_is_ready(flash) ? "ready" : "not ready (PM resume pending)");
	printk("spi_nor_flash: write-block-size=%u, erase value=0x%02x\n", params->write_block_size,
	       (unsigned int)erase_value);
	/* devicetree `size` is in bits (jedec,jesd216.yaml). */
	printk("spi_nor_flash: DT size = %u bits = 0x%x bytes\n",
	       (unsigned int)DT_PROP(FLASH_NODE, size), (unsigned int)(DT_PROP(FLASH_NODE, size) / 8));

	ret = flash_get_page_info_by_offs(flash, 0U, &page);
	if (ret < 0) {
		printk("spi_nor_flash: page info failed (%d)\n", ret);
	} else {
		printk("spi_nor_flash: first page: start 0x%08lx, size %u\n",
		       (unsigned long)page.start_offset, page.size);
	}

	/* A 256-byte page read: TX the 0x03 command plus address, then one RX
	 * phase of 256 bytes, which the driver hands to the DMA. */
	ret = flash_read(flash, 0U, page_a, sizeof(page_a));
	if (ret < 0) {
		printk("spi_nor_flash: flash_read(0, %u) failed (%d) -- JEDEC ID mismatch or bus problem\n",
		       READ_LEN, ret);
		goto idle;
	}

	printk("spi_nor_flash: after the first read the device is %s\n",
	       device_is_ready(flash) ? "ready" : "still not ready");

	printk("spi_nor_flash: page@0x0000 =");
	print_bytes(page_a, 8U);
	printk(" ... (%u bytes)\n", READ_LEN);

	for (size_t i = 0U; i < sizeof(page_a); i++) {
		if (page_a[i] != (uint8_t)erase_value) {
			non_erase++;
		}
	}

	printk("spi_nor_flash: %u of %u bytes differ from the erase value%s\n", non_erase, READ_LEN,
	       non_erase == 0U ? " (blank region, as expected)" : "");

	/* Read the same page again and compare: the long RX path has to be
	 * repeatable, and a half-drained DMA would show up here. */
	ret = flash_read(flash, 0U, page_b, sizeof(page_b));
	if (ret < 0) {
		printk("spi_nor_flash: second flash_read failed (%d)\n", ret);
		goto idle;
	}

	if (memcmp(page_a, page_b, sizeof(page_a)) != 0) {
		printk("spi_nor_flash: the two page reads differ -> FAIL\n");
		goto idle;
	}

	printk("spi_nor_flash: two 256-byte page reads agree -> PASS\n");

#if IS_ENABLED(CONFIG_APP_SPI_NOR_WRITE_TEST)
	/* Erase + rewrite through the flash API. flash_write() splits this into
	 * one page program per 256-byte page, each of which reaches spi_agm as
	 * a two-buffer TX list (0x02 + address, then 256 bytes of data). */
	{
		const off_t base = (off_t)CONFIG_APP_SPI_NOR_WRITE_OFFSET;
		size_t mismatches = 0U;
		size_t first_bad = 0U;
		bool have_bad = false;

		printk("spi_nor_flash: write test on 0x%06lx..0x%06lx (DESTRUCTIVE)\n",
		       (unsigned long)base, (unsigned long)(base + WRITE_LEN - 1U));

		ret = flash_erase(flash, base, WRITE_LEN);
		if (ret < 0) {
			printk("spi_nor_flash: flash_erase failed (%d)\n", ret);
			goto idle;
		}

		/* Read back in READ_LEN chunks: one SPI transfer has to fit the
		 * driver's RX bounce buffer (CONFIG_SPI_AGM_RX_BOUNCE_BYTES,
		 * 512 by default), and a single 4 KiB flash_read() would not. */
		for (size_t off = 0U; off < WRITE_LEN; off += READ_LEN) {
			ret = flash_read(flash, base + off, page_a, READ_LEN);
			if (ret < 0) {
				printk("spi_nor_flash: read after erase failed (%d)\n", ret);
				goto idle;
			}

			for (size_t i = 0U; i < READ_LEN; i++) {
				if (page_a[i] != (uint8_t)erase_value) {
					mismatches++;
				}
			}
		}

		printk("spi_nor_flash: erase -> %u of %u bytes differ from 0x%02x -> %s\n",
		       (unsigned int)mismatches, WRITE_LEN, (unsigned int)erase_value,
		       (mismatches == 0U) ? "PASS" : "FAIL");

		if (mismatches != 0U) {
			goto idle;
		}

		for (size_t i = 0U; i < WRITE_LEN; i++) {
			write_buf[i] = (uint8_t)(0xA0U + i);
		}

		ret = flash_write(flash, base, write_buf, WRITE_LEN);
		if (ret < 0) {
			printk("spi_nor_flash: flash_write failed (%d)\n", ret);
			goto idle;
		}

		memset(write_buf, 0, WRITE_LEN);

		mismatches = 0U;
		for (size_t off = 0U; off < WRITE_LEN; off += READ_LEN) {
			ret = flash_read(flash, base + off, page_a, READ_LEN);
			if (ret < 0) {
				printk("spi_nor_flash: read after write failed (%d)\n", ret);
				goto idle;
			}

			for (size_t i = 0U; i < READ_LEN; i++) {
				if (page_a[i] != (uint8_t)(0xA0U + off + i)) {
					if (!have_bad) {
						first_bad = off + i;
						have_bad = true;
					}
					mismatches++;
				}
			}
		}

		if (have_bad) {
			printk("spi_nor_flash: first mismatch at 0x%06lx: wrote 0x%02x read 0x%02x\n",
			       (unsigned long)(base + first_bad), (uint8_t)(0xA0U + first_bad),
			       page_a[first_bad % READ_LEN]);
		}

		printk("spi_nor_flash: write %u bytes, %u bytes read back wrong -> %s\n", WRITE_LEN,
		       (unsigned int)mismatches, (mismatches == 0U) ? "PASS" : "FAIL");

		/* Put the sector back the way the test found it: blank. */
		if (IS_ENABLED(CONFIG_APP_SPI_NOR_KEEP_PROGRAMMED)) {
			printk("spi_nor_flash: keeping the data (CONFIG_APP_SPI_NOR_KEEP_PROGRAMMED)\n");
		} else {
			ret = flash_erase(flash, base, WRITE_LEN);
			printk("spi_nor_flash: final erase -> %s\n",
			       (ret < 0) ? "FAIL" : "done (sector blank again)");
		}
	}
#endif

#if IS_ENABLED(CONFIG_APP_SPI_NOR_SCAN)
	/* Whole-flash scan. The chip's BOOT0/PIN_94 selects between its
	 * internal 1 MB flash and this on-board SPI flash, so the part can
	 * carry a boot image -- the write test above is opt-in for that
	 * reason. Scanning also gives the read path 8192 page reads of work,
	 * and if the part does hold an image, real data instead of the erase
	 * pattern. */
	{
		uint32_t flash_size = DT_PROP(FLASH_NODE, size) / 8U;
		uint32_t non_erase_total = 0U;
		uint32_t first_non_erase = flash_size;
		uint32_t checksum = 0U;
		uint32_t pages = flash_size / READ_LEN;
		uint32_t done = 0U;
		uint32_t scan_start = k_uptime_get_32();

		for (uint32_t i = 0U; i < pages; i++) {
			uint32_t offs = i * READ_LEN;

			ret = flash_read(flash, offs, page_a, READ_LEN);
			if (ret < 0) {
				printk("spi_nor_flash: scan read at 0x%06x failed (%d)\n", offs, ret);
				break;
			}

			done++;

			for (size_t j = 0U; j < READ_LEN; j++) {
				uint8_t v = page_a[j];

				checksum = (checksum << 1) ^ (checksum >> 31) ^ v;

				if (v != (uint8_t)erase_value) {
					non_erase_total++;

					if (first_non_erase == flash_size) {
						first_non_erase = offs + j;
					}
				}
			}
		}

		scan_start = k_uptime_get_32() - scan_start;
		printk("spi_nor_flash: scanned %u of %u pages (%u bytes) at %u Hz in %u ms "
		       "(%u KiB/s)\n",
		       done, pages, flash_size,
		       (unsigned int)DT_PROP(FLASH_NODE, spi_max_frequency),
		       (unsigned int)scan_start,
		       (unsigned int)(((uint64_t)done * READ_LEN) /
				      (scan_start ? scan_start : 1U) * 1000U / 1024U));

		if (done != pages) {
			printk("spi_nor_flash: scan incomplete -> FAIL\n");
			goto idle;
		}
		printk("spi_nor_flash: %u bytes differ from 0x%02x; checksum 0x%08x\n",
		       non_erase_total, (unsigned int)erase_value, checksum);

		if (first_non_erase == flash_size) {
			printk("spi_nor_flash: the whole part is blank\n");
		} else {
			printk("spi_nor_flash: first non-blank byte at 0x%06x\n", first_non_erase);
		}
	}
#endif

idle:
	while (true) {
		k_msleep(1000);
	}
}
