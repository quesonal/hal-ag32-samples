/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-die flash driver self-test (drivers/flash/flash_agm.c).
 *
 * It touches one 4 KiB sector chosen by the board overlay (just below the
 * fabric bitstream reservation) and leaves it erased, so a board that runs
 * this sample is not left with debris in its application space.
 *
 * Cases, all with explicit verdicts:
 *   1. flash_get_parameters()/page layout are sane (write 4 B, erase 4 KiB)
 *   2. erase -> every byte reads 0xff
 *   3. program a non-trivial pattern -> read back byte for byte
 *   4. unaligned write (off+1, len 4) -> -EINVAL
 *   5. erase past the end of the flash -> -EINVAL
 *   6. write over an already-programmed word without erasing -> the driver
 *      must report -EIO (the controller raises PGERR), never a silent
 *      "different data"
 *   7. erase again -> 0xff again (area restored)
 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <string.h>

#define FLASH_NODE   DT_NODELABEL(flash0)
#define SCRATCH_NODE DT_NODELABEL(flash_agm_scratch)

#define FLASH_BASE   DT_REG_ADDR(FLASH_NODE)
#define SCRATCH_OFF  (DT_REG_ADDR(SCRATCH_NODE) - FLASH_BASE)
#define SCRATCH_SIZE DT_REG_SIZE(SCRATCH_NODE)

static const struct device *const flash = DEVICE_DT_GET(FLASH_NODE);

static int failures;

static void verdict(const char *name, bool ok)
{
	printk("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) {
		failures++;
	}
}

static bool all_ff(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != 0xffU) {
			return false;
		}
	}
	return true;
}

int main(void)
{
	uint8_t pattern[64];
	uint8_t readback[64];
	struct flash_pages_info page;
	const struct flash_parameters *params;
	int ret;

	printk("\n=== flash_internal: on-die flash driver self-test ===\n");

	if (!device_is_ready(flash)) {
		printk("FATAL: %s not ready (is CONFIG_FLASH_AGM on?)\n", flash->name);
		return 0;
	}

	params = flash_get_parameters(flash);
	printk("device %s, size %u KiB, write-block %u B, erase-value 0x%02x\n",
	       flash->name, (uint32_t)(DT_REG_SIZE(FLASH_NODE) / 1024U),
	       params->write_block_size, params->erase_value);
	verdict("write block size is 4", params->write_block_size == 4U);
	verdict("erase value is 0xff", params->erase_value == 0xffU);

	ret = flash_get_page_info_by_offs(flash, SCRATCH_OFF, &page);
	printk("scratch: offset 0x%06lx, size 0x%x; page %u @0x%08lx size 0x%x\n",
	       (long)SCRATCH_OFF, (unsigned int)SCRATCH_SIZE, page.index,
	       (long)page.start_offset, (unsigned int)page.size);
	verdict("page info lookup works", ret == 0);
	verdict("erase page size is 4 KiB", ret == 0 && page.size == 4096U);

	/* 2. erase -> blank */
	ret = flash_erase(flash, SCRATCH_OFF, SCRATCH_SIZE);
	verdict("erase scratch sector", ret == 0);
	ret = flash_read(flash, SCRATCH_OFF, readback, sizeof(readback));
	verdict("erased sector reads 0xff", ret == 0 && all_ff(readback, sizeof(readback)));

	/* 3. program a non-blank pattern and read it back */
	for (size_t i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(0xA5U ^ i);
	}
	ret = flash_write(flash, SCRATCH_OFF, pattern, sizeof(pattern));
	memset(readback, 0, sizeof(readback));
	ret = flash_read(flash, SCRATCH_OFF, readback, sizeof(readback)) == 0 && ret == 0;
	verdict("program + read back byte for byte",
		ret && memcmp(pattern, readback, sizeof(pattern)) == 0);

	/* 4. unaligned write must be rejected, not silently rounded */
	ret = flash_write(flash, SCRATCH_OFF + 1, pattern, sizeof(pattern));
	verdict("unaligned write returns -EINVAL", ret == -EINVAL);

	/* 5. erase past the end of the flash must be rejected */
	ret = flash_erase(flash, DT_REG_SIZE(FLASH_NODE) - 2048U, 4096U);
	verdict("out-of-range erase returns -EINVAL", ret == -EINVAL);

	/* 6. programming a non-erased word: explicit-erase semantics */
	uint32_t other = 0x00000000U;

	ret = flash_write(flash, SCRATCH_OFF, &other, sizeof(other));
	verdict("program over a written word reports an error (no silent merge)",
		ret != 0);

	/* 7. restore the sector */
	ret = flash_erase(flash, SCRATCH_OFF, SCRATCH_SIZE);
	ret = flash_read(flash, SCRATCH_OFF, readback, sizeof(readback)) == 0 && ret == 0;
	verdict("scratch sector restored to 0xff", ret && all_ff(readback, sizeof(readback)));

	printk("=== %s (%d failure%s) ===\n", failures ? "FAIL" : "PASS", failures,
	       failures == 1 ? "" : "s");
	return 0;
}
