/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * cpld_reg — access the AgRV2K CPLD (FPGA fabric) AHB window.
 *
 * The MCU reaches the fabric with plain loads/stores in
 * 0x60000000-0x7FFFFFFF: the SoC decoder forwards them to the bitstream's
 * `mem_ahb_*` slave port, and the fabric answers with the AHB hreadyout
 * handshake (vendor docs "MCU + CPLD 交互编程" part 1/2). Devicetree
 * exposes that range as `cpld0` (compatible "agm,agrv2k-cpld"), which binds
 * the module's CPLD driver, so applications use agm_cpld_read32()/write32()
 * instead of hard-coding a pointer or an MMIO type.
 *
 * What is *not* described anywhere: the register map inside the window.
 * It belongs to the user's Verilog, so this sample only does what works
 * on any bitstream -- a read-only dump plus a syscon bounds check -- and
 * puts the vendor echo bitstream's write/read-back pair behind
 * CONFIG_APP_CPLD_ECHO_TEST (default n).
 *
 * WARNING: an access the fabric does not answer never completes. If the
 * flashed bitstream has no slave at the offsets used here, the CPU stalls
 * in the first syscon_read_reg() with no fault and no console output.
 * Check the bitstream before running this.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define CPLD_NODE DT_NODELABEL(cpld0)

/* The window is board-level (like can0) and the board overlays turn it on;
 * a build without one would dump a disabled node's absent registers. */
#if !DT_NODE_HAS_STATUS(CPLD_NODE, okay)
#error "cpld0 is disabled: build with a board overlay that sets it okay"
#endif

/* Register pair of the vendor echo bitstream
 * (example_cpldAhbTxRxReg/logic/analog_ip.v): a write to +0x00 latches
 * hwdata_reg, a read from +0x04 returns that same word. */
#define CPLD_OFF_ECHO_TX 0x00U
#define CPLD_OFF_ECHO_RX 0x04U

#define CPLD_DUMP_WORDS 8U
#define CPLD_ECHO_ROUNDS 3U

static const struct device *const cpld = DEVICE_DT_GET(CPLD_NODE);

static void dump_words(uint32_t words)
{
	for (uint32_t off = 0U; off < words * sizeof(uint32_t); off += sizeof(uint32_t)) {
		uint32_t val = 0U;
		int ret = agm_cpld_read32(cpld, off, &val);

		if (ret < 0) {
			printk("cpld_reg:  +0x%04x: <error %d>\n", off, ret);
		} else {
			printk("cpld_reg:  +0x%04x: 0x%08x\n", off, val);
		}
	}
}

/* The driver refuses offsets outside `reg` before touching the bus;
 * exercise that so a mis-sized window shows up here rather than as a stall
 * inside the fabric. */
static bool check_bounds(size_t size)
{
	uint32_t val = 0U;
	int ret = agm_cpld_read32(cpld, (uint32_t)size, &val);

	printk("cpld_reg: bounds: read(+0x%zx) -> %d %s\n", size, ret,
	       ret == -EINVAL ? "(PASS: rejected)" : "(FAIL: expected -EINVAL)");

	return ret == -EINVAL;
}

static bool echo_test(void)
{
	bool ok = true;

	for (uint32_t round = 0U; round < CPLD_ECHO_ROUNDS; round++) {
		uint32_t tx = 0xa5a50000U + round;
		uint32_t rx = 0U;
		int ret;

		ret = agm_cpld_write32(cpld, CPLD_OFF_ECHO_TX, tx);
		if (ret < 0) {
			printk("cpld_reg: echo %u: write failed (%d)\n", round, ret);
			ok = false;
			continue;
		}

		/* The fabric latches on sys_clock and drives hreadyout off its
		 * own clock domain; give the handshake a moment before the
		 * read. (Two reads in a row also cover the read path: the
		 * vendor RTL only fills hrdata_reg on the first NONSEQ.) */
		(void)agm_cpld_read32(cpld, CPLD_OFF_ECHO_RX, &rx);
		k_busy_wait(10);
		ret = agm_cpld_read32(cpld, CPLD_OFF_ECHO_RX, &rx);
		if (ret < 0) {
			printk("cpld_reg: echo %u: read failed (%d)\n", round, ret);
			ok = false;
			continue;
		}

		printk("cpld_reg: echo %u: wrote 0x%08x, read 0x%08x -> %s\n", round, tx, rx,
		       rx == tx ? "PASS" : "FAIL");
		ok = ok && (rx == tx);
	}

	return ok;
}

static bool run_checks(void)
{
	uintptr_t base = 0U;
	size_t size = 0U;
	uint32_t words;
	bool ok = true;
	int ret;

	if (!device_is_ready(cpld)) {
		printk("cpld_reg: %s not ready (is CONFIG_CPLD_AGM=y?)\n", cpld->name);
		return false;
	}

	ret = agm_cpld_get_base(cpld, &base);
	if (ret < 0) {
		printk("cpld_reg: agm_cpld_get_base failed (%d)\n", ret);
		return false;
	}

	ret = agm_cpld_get_size(cpld, &size);
	if (ret < 0) {
		printk("cpld_reg: agm_cpld_get_size failed (%d)\n", ret);
		return false;
	}

	printk("cpld_reg: %s: base 0x%08lx, size 0x%zx\n", cpld->name, (unsigned long)base, size);

	words = MIN(size / sizeof(uint32_t), (size_t)CPLD_DUMP_WORDS);
	printk("cpld_reg: read-only dump (%u words, window-relative offsets)\n", words);
	dump_words(words);

	ok = check_bounds(size);

	if (IS_ENABLED(CONFIG_APP_CPLD_ECHO_TEST)) {
		printk("cpld_reg: echo self test (vendor example_cpldAhbTxRxReg map)\n");
		ok = echo_test() && ok;
	} else {
		printk("cpld_reg: echo self test skipped (CONFIG_APP_CPLD_ECHO_TEST=n)\n");
	}

	printk("cpld_reg: %s\n", ok ? "PASS" : "FAIL");

	return ok;
}

int main(void)
{
	printk("\ncpld_reg: AgRV2K CPLD fabric window\n");
	(void)run_checks();

	while (true) {
		k_msleep(1000);
	}
}
