/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * dual_ip — the AHB RAM half of the two-IP test.
 *
 * ip/dual_ip.v gives 0x60000000 + 0x000..0x7FF to the vendor custom_ip RAM
 * block and everything above that to the SPI IP. The block is instantiated
 * with RAM_SIZE = 2048 rather than the vendor example's 4096: the user-logic
 * LogicLock region holds 4 M9K blocks, and the SPI IP's RX FIFO already uses
 * one (4 KiB of RAM would be 4 more -- one too many for Quartus; see
 * ip/dual_ip.v).
 *
 * Two functions, and main.c runs them on either side of the SPI half:
 *   dual_ip_ram_test()   writes/reads six words of the block;
 *   dual_ip_alias_test() writes one block higher (the SPI IP's side) and
 *                        checks the RAM did not change -- that is the address
 *                        decode; without it both IPs would answer everything.
 *
 * WARNING: an access the fabric does not answer never completes (no
 * hreadyout, no fault), so a wrong bitstream hangs instead of printing a
 * failure. main.c prints a banner before each half, which is what makes the
 * point of the hang identifiable from the capture.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/sys/printk.h>

#include "ram_window.h"

#define CPLD_NODE DT_NODELABEL(cpld0)

/* The RAM block: 2 KiB, decoded by the wrapper as haddr[15:11] == 0. */
#define RAM_BLOCK_SIZE 0x0800U
/* One word past it -- the SPI IP's side of the window. */
#define RAM_ALIAS_OFF  0x0800U

static const struct device *const cpld = DEVICE_DT_GET(CPLD_NODE);

/* Spread over the block, including its last word (the vendor example checks
 * every word; six of them catch an address decode that wraps or drops bits,
 * which is what actually goes wrong here). */
static const uint32_t offsets[] = {
	0x000U, 0x004U, 0x100U, 0x400U, 0x600U, 0x7fcU,
};

static uint32_t pattern_for(size_t i)
{
	return 0xa5a50000U ^ ((uint32_t)i * 0x01010101U);
}

int dual_ip_ram_test(void)
{
	uint32_t written[ARRAY_SIZE(offsets)];
	uint32_t val;
	int ret;

	printk("dual_ip: RAM window 0x%08x+0x%03x.. (custom_ip, "
	       "ip/dual_ip.v)\n", (uint32_t)DT_REG_ADDR(CPLD_NODE),
	       RAM_BLOCK_SIZE - 1U);

	if (!device_is_ready(cpld)) {
		printk("dual_ip: RAM: cpld0 not ready\n");
		return -ENODEV;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(offsets); i++) {
		written[i] = pattern_for(i);
		ret = agm_cpld_write32(cpld, offsets[i], written[i]);
		if (ret < 0) {
			printk("dual_ip: RAM: write +0x%03x failed (%d)\n",
			       offsets[i], ret);
			return ret;
		}
	}

	for (size_t i = 0U; i < ARRAY_SIZE(offsets); i++) {
		ret = agm_cpld_read32(cpld, offsets[i], &val);
		if (ret < 0) {
			printk("dual_ip: RAM: read +0x%03x failed (%d)\n",
			       offsets[i], ret);
			return ret;
		}
		if (val != written[i]) {
			printk("dual_ip: RAM: +0x%03x read 0x%08x, "
			       "0x%08x expected\n", offsets[i], val, written[i]);
			return -EIO;
		}
	}

	printk("dual_ip: RAM: %u word(s) read back -> PASS\n",
	       (unsigned int)ARRAY_SIZE(offsets));
	return 0;
}

int dual_ip_alias_test(void)
{
	uint32_t val;
	int ret;

	/* The decode: a write above the block must not reach the RAM. This one
	 * goes to the SPI IP's mem_ahb port, which answers (the same access
	 * completes on the single-IP bitstream too), it just must not alias. */
	ret = agm_cpld_write32(cpld, RAM_ALIAS_OFF, ~pattern_for(0));
	if (ret < 0) {
		printk("dual_ip: RAM: write +0x%03x failed (%d)\n",
		       RAM_ALIAS_OFF, ret);
		return ret;
	}
	ret = agm_cpld_read32(cpld, offsets[0], &val);
	if (ret < 0 || val != pattern_for(0)) {
		printk("dual_ip: RAM: +0x%03x changed to 0x%08x after a write "
		       "to +0x%03x -- the two IP windows are not decoded\n",
		       offsets[0], val, RAM_ALIAS_OFF);
		return -EIO;
	}

	printk("dual_ip: RAM: +0x%03x does not alias into the block -> PASS\n",
	       RAM_ALIAS_OFF);
	return 0;
}
