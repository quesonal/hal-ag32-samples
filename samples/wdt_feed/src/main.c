/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * wdt_feed - WDOG0 (agm,agrv2k-wdt) watchdog smoke test.
 *
 * Phase A: install a 2 s interrupt-only timeout with a callback and
 * let the driver ISR service it three times (the ISR clears the
 * interrupt, which reloads the counter - the SDK WDOG_Feed model), then
 * disable the watchdog. Proves the WDOG0 -> PLIC IRQ 4 -> wdt_agm ISR
 * path end to end.
 *
 * Phase B: reinstall a 3 s SoC-reset timeout (no callback), feed it
 * from the main loop every 1 s five times, then stop feeding. The SoC
 * resets ~3 s later. On the next boot the sample reads back the SYS
 * RST_CNTL watchdog flag and reports "WDOG reset confirmed".
 *
 * Run twice (or just watch run #2) to see the full cycle.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define WDT0 DT_NODELABEL(wdt0)

/* SYS controller reset-flag register (SDK system.h). */
#define SYS_RST_CNTL		0x03000004UL
#define SYS_RSTF_WDOG		BIT(30)
#define SYS_RSTF_IWDG		BIT(29)
#define SYS_RSTF_SFT		BIT(28)
#define SYS_RST_REMOVE		BIT(24)

#define PHASE_A_TIMEOUT_MS	2000U
#define PHASE_B_TIMEOUT_MS	3000U

static uint32_t phase_a_fires;

static void phase_a_cb(const struct device *dev, int channel_id)
{
	phase_a_fires++;
	printk("wdt_feed: phase A timeout #%u @ %u ms (ISR serviced)\n",
	       phase_a_fires, (uint32_t)k_uptime_get_32());

	/* SDK model: the timeout ISR clears/feeds the watchdog to keep
	 * the system alive. Feed from the callback (wdt_feed clears the
	 * pending interrupt and re-enables the source). */
	wdt_feed(dev, 0);
}

int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(WDT0);
	struct wdt_timeout_cfg cfg = { 0 };
	uint32_t rst = sys_read32(SYS_RST_CNTL);
	int err;

	printk("wdt_feed: WDOG0 watchdog test\n");

	if (!device_is_ready(wdt)) {
		printk("wdt_feed: watchdog not ready\n");
		return 0;
	}

	/* Report the reset source of this boot. */
	printk("wdt_feed: RST_CNTL=0x%08x (WDOG=%u IWDG=%u SFT=%u)\n", rst,
	       (rst & SYS_RSTF_WDOG) != 0U, (rst & SYS_RSTF_IWDG) != 0U,
	       (rst & SYS_RSTF_SFT) != 0U);
	sys_write32(rst | SYS_RST_REMOVE, SYS_RST_CNTL);

	/* After a watchdog reset the flag is set: report the PASS and
	 * do not arm the watchdog again. */
	if (rst & SYS_RSTF_WDOG) {
		printk("wdt_feed: WDOG reset confirmed (run #2 PASS)\n");
		return 0;
	}

	/* Phase A: interrupt-only timeout serviced by the driver ISR. */
	cfg.window.min = 0U;
	cfg.window.max = PHASE_A_TIMEOUT_MS;
	cfg.callback = phase_a_cb;
	cfg.flags = WDT_FLAG_RESET_NONE;

	err = wdt_install_timeout(wdt, &cfg);
	if (err != 0) {
		printk("wdt_feed: phase A install failed: %d\n", err);
		return 0;
	}
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err != 0) {
		printk("wdt_feed: phase A setup failed: %d\n", err);
		return 0;
	}
	printk("wdt_feed: phase A armed (%u ms, INT only)\n",
	       PHASE_A_TIMEOUT_MS);

	while (phase_a_fires < 3U) {
		k_sleep(K_MSEC(100));
	}
	printk("wdt_feed: phase A done - %u ISR timeouts without reset\n",
	       phase_a_fires);

	err = wdt_disable(wdt);
	if (err != 0) {
		printk("wdt_feed: phase A disable failed: %d\n", err);
	}
	k_sleep(K_MSEC(500));

	/* Phase B: SoC-reset timeout, fed from the main loop. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.window.min = 0U;
	cfg.window.max = PHASE_B_TIMEOUT_MS;
	cfg.callback = NULL;
	cfg.flags = WDT_FLAG_RESET_SOC;

	err = wdt_install_timeout(wdt, &cfg);
	if (err != 0) {
		printk("wdt_feed: phase B install failed: %d\n", err);
		return 0;
	}
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err != 0) {
		printk("wdt_feed: phase B setup failed: %d\n", err);
		return 0;
	}
	printk("wdt_feed: phase B armed (%u ms, RESET) - feeding 1 s x5\n",
	       PHASE_B_TIMEOUT_MS);

	for (int i = 0; i < 5; i++) {
		k_sleep(K_MSEC(1000));
		err = wdt_feed(wdt, 0);
		printk("wdt_feed: feed #%d (err %d) @ %u ms\n", i + 1, err,
		       (uint32_t)k_uptime_get_32());
	}

	printk("wdt_feed: stopped feeding - expect WDOG reset on the next\n");
	printk("wdt_feed: unserviced timeout (hw needs a 2nd zero-crossing)\n");
	for (;;) {
		k_sleep(K_MSEC(1000));
		printk("wdt_feed: still alive @ %u ms (watchdog did not "
		       "reset?)\n", (uint32_t)k_uptime_get_32());
	}
	return 0;
}
