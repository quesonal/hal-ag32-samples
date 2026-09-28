/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * iwdg_basic - IWDG (agm,agrv2k-iwdg) backup-domain watchdog test.
 *
 * The IWDG has no PLIC IRQ: a missed feed can only reset the SoC, it
 * cannot raise an interrupt. This sample therefore mirrors the
 * wdt_feed Phase B path (no callback, RESET_SOC), with the addition
 * that the reset source is reported via SYS_RSTF_IWDG instead of
 * SYS_RSTF_WDOG (see SYS_RST_CNTL bit 29 in SDK system.h).
 *
 * Phase A: arm a 2 s RESET_SOC timeout, feed once a second for 5 s,
 * then stop. The SoC resets ~2 s later.
 *
 * Phase B: on the next boot, read SYS_RST_CNTL, report
 * "IWDG reset confirmed", and exit.
 *
 * The dev board script is tools/test_uart_capture.sh. Two runs are needed
 * to see the full cycle (see the wdt_feed README for the same
 * caveat).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define IWDG0 DT_NODELABEL(iwdg0)

/* SYS controller reset-flag register (vendor SDK system.h). */
#define SYS_RST_CNTL		0x03000004UL
#define SYS_RSTF_IWDG		BIT(29)
#define SYS_RST_REMOVE		BIT(24)

#define PHASE_A_TIMEOUT_MS	2000U
#define PHASE_A_FEEDS		5U

int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(IWDG0);
	struct wdt_timeout_cfg cfg = { 0 };
	uint32_t rst = sys_read32(SYS_RST_CNTL);
	int err;

	printk("iwdg_basic: IWDG watchdog test\n");

	if (!device_is_ready(wdt)) {
		/* With agm,prescaler = <64> in dtsi this should always
		 * be ready on agrv2k_407. */
		printk("iwdg_basic: iwdg0 not ready (overlay missing?)\n");
		return 0;
	}

	/* Report and clear the reset flag of this boot. */
	printk("iwdg_basic: RST_CNTL=0x%08x (IWDG=%u)\n", rst,
	       (rst & SYS_RSTF_IWDG) != 0U);
	sys_write32(rst | SYS_RST_REMOVE, SYS_RST_CNTL);

	if (rst & SYS_RSTF_IWDG) {
		/* Run #2: the previous run was reset by IWDG. PASS. */
		uint16_t iwdg_reg = *(volatile uint16_t *)0x40000034UL;
		uint16_t crl = *(volatile uint16_t *)0x40000008UL;
		printk("iwdg_basic: IWDG reset confirmed (run #2 PASS)\n");
		printk("iwdg_basic: post-reset IWDG reg=0x%04x crl=0x%04x "
		       "(EN=%u PR=%u)\n", iwdg_reg, crl,
		       (iwdg_reg & 0x100) ? 1U : 0U, iwdg_reg & 0x7);
		return 0;
	}

	/* Phase A: RESET_SOC timeout, feed from the main loop. */
	cfg.window.min = 0U;
	cfg.window.max = PHASE_A_TIMEOUT_MS;
	cfg.callback = NULL;
	cfg.flags = WDT_FLAG_RESET_SOC;

	err = wdt_install_timeout(wdt, &cfg);
	if (err != 0) {
		printk("iwdg_basic: install failed: %d\n", err);
		return 0;
	}

	/* WDT_OPT_PAUSE_HALTED_BY_DBG would return -ENOTSUP on the
	 * IWDG (no debug-halt clock stop in the backup domain), so
	 * pass no options. */
	err = wdt_setup(wdt, 0);
	if (err != 0) {
		/* The most likely cause is a missing LSE crystal on
		 * the board when agm,clk-source = "lse". The dtsi
		 * default is "lsi". */
		printk("iwdg_basic: setup failed: %d\n", err);
		return 0;
	}
	printk("iwdg_basic: armed (%u ms, RESET_SOC) - feeding %u times\n",
	       PHASE_A_TIMEOUT_MS, PHASE_A_FEEDS);

	for (int i = 0; i < PHASE_A_FEEDS; i++) {
		k_sleep(K_MSEC(1000));
		err = wdt_feed(wdt, 0);
		printk("iwdg_basic: feed #%d (err %d) @ %u ms\n", i + 1, err,
		       (uint32_t)k_uptime_get_32());
	}

	/* Stop feeding. The next IWDG timeout resets the SoC. After
	 * the reset, RST_CNTL bit 29 is set and run #2 takes the
	 * "IWDG reset confirmed" branch above. */
	printk("iwdg_basic: stopped feeding - expect IWDG reset in "
	       "~%u ms\n", PHASE_A_TIMEOUT_MS);
	for (;;) {
		k_sleep(K_MSEC(1000));
		printk("iwdg_basic: still alive @ %u ms (IWDG did not reset?)"
		       "\n", (uint32_t)k_uptime_get_32());
	}
	return 0;
}
