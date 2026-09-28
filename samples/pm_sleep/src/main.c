/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * pm_sleep — AG32 sleep-mode (suspend-to-idle) PM demo .
 *
 * What it proves:
 *   With CONFIG_PM=y the kernel idle path goes through
 *   pm_system_suspend() -> pm_state_set(PM_STATE_SUSPEND_TO_IDLE),
 *   which arms the AG32 sleep mode (PWR_CNTL = SYS_SLEEP_MODE) and
 *   executes WFI (soc/agm/agrv2k/pm.c). The CLINT keeps running in
 *   sleep mode, so the MTIP deadline wakes the CPU exactly when the
 *   tickless kernel needs it: each k_sleep(K_SECONDS(3)) below should
 *   produce exactly one sleep entry + one exit, and k_uptime should
 *   still advance 3000 ms (no time drift).
 *
 *   A pm_notifier counts entry/exit transitions in ISR-safe globals
 *   (no console traffic from the PM path). The main thread prints the
 *   counters after each wake and blinks LED1 (GPIO4 bit 1) so the
 *   board visibly stays alive across the sleeps.
 *
 * Stop (PM_STATE_STANDBY) and standby (PM_STATE_SOFT_OFF) hooks are
 * implemented too but the states are disabled in the SoC dtsi: the
 * shipped example_board.bin ties every EXT_INTx wake line to GND, so
 * only a bitstream with a routed wake source (or an RTC alarm) can
 * bring the SoC back from those modes. Enable the corresponding DT
 * state (status = "okay" on &stop_state / &standby_state in the
 * board .dts, or pm_state_force()) only on such a board.
 */

#include <stdint.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/pm.h>

/* On-board LED1 (gpio4 pin 1, active-low — board dts gpio-leds). */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* PM-path counters (touched from idle context only). */
volatile uint32_t pm_sleep_entries;   /* suspend-to-idle entry notifications */
volatile uint32_t pm_sleep_exits;     /* suspend-to-idle exit notifications  */
volatile uint32_t pm_other_entries;   /* any other state (should stay 0)     */

static void pm_notify_entry(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		pm_sleep_entries++;
	} else {
		pm_other_entries++;
	}
}

static void pm_notify_exit(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		pm_sleep_exits++;
	} else {
		pm_other_entries++;
	}
}

static struct pm_notifier notifier = {
	.state_entry = pm_notify_entry,
	.state_exit = pm_notify_exit,
};

static void led_init(void)
{
	if (gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE) != 0) {
		printk("pm_sleep: led0 configure failed\n");
	}
}

static void led_toggle(void)
{
	gpio_pin_toggle_dt(&led0);
}

int main(void)
{
	int iter;

	led_init();
	pm_notifier_register(&notifier);

	/* Boot-time idles may have slept before main started. */
	pm_sleep_entries = 0;
	pm_sleep_exits = 0;
	pm_other_entries = 0;

	printk("pm_sleep: CONFIG_PM=%d, %u iterations of k_sleep(3 s)\n",
	       IS_ENABLED(CONFIG_PM), 5);

	for (iter = 0; iter < 5; iter++) {
		int64_t t0 = k_uptime_get();

		led_toggle();
		printk("[%d] sleep: t=%lld ms, entries=%u exits=%u\n",
		       iter, (long long)t0, (unsigned)pm_sleep_entries, (unsigned)pm_sleep_exits);

		k_sleep(K_SECONDS(3));

		led_toggle();
		printk("[%d] wake : t=%lld ms (+%lld), entries=%u exits=%u\n",
		       iter, (long long)k_uptime_get(),
		       (long long)(k_uptime_get() - t0),
		       (unsigned)pm_sleep_entries, (unsigned)pm_sleep_exits);
	}

	printk("pm_sleep: done — sleep entries/exits: %u/%u (other: %u)\n",
	       (unsigned)pm_sleep_entries, (unsigned)pm_sleep_exits,
	       (unsigned)pm_other_entries);
	return 0;
}
