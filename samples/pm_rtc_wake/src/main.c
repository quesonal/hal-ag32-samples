/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * pm_rtc_wake — AG32 stop-mode RTC alarm wake demo (§3.7 / §3.20).
 *
 * The shipped example_board.bin ties every EXT_INTx wake line to GND
 * (boards/agm/agrv2k_407/board.ve), so the backup-domain RTC alarm is
 * the only usable wake source for stop mode. Flow per round:
 *
 *   1) arm a 5 s relative alarm on the rtc0 counter device;
 *   2) pm_state_force(PM_STATE_STANDBY) — the kernel idles into AG32
 *      stop mode (soc/agm/agrv2k/pm.c mirrors CRH.ALRIE onto the SYS
 *      wake-up controller WKP line 8);
 *   3) the RTC alarm wakes the SoC ~5 s later; pm_state_exit_post_ops()
 *      restores the PLL, then the RTC ISR fires the alarm callback
 *      which gives the wake semaphore;
 *   4) the thread clears the forced state and reports the deltas:
 *      the RTC CNT (backup domain, LSE clock) must have advanced by
 *      ~5 ticks while k_uptime() (MTIME in the 1.2 V domain) froze
 *      across the stop.
 *
 * A soft-off (PM_STATE_SOFT_OFF) round is available behind
 * CONFIG_PM_RTC_WAKE_TEST_STANDBY (default n): the 1.2 V domain goes
 * off and the RTC alarm brings the SoC back ~6 s later. On boards with
 * the FLASH user option byte STDBY_NO_RST set (factory default on the
 * agrv2k_407), the core resumes after the pm_state_set() WFI instead of
 * a power-on reset; soc/agm/agrv2k/pm.c then forces a software reset so
 * the image restarts from a known state. The wake is detected on the
 * next boot through a backup-domain marker (RTC BKP_DR survives the
 * standby and the software reset) — the SYS_RST_CNTL LPWR flag is NOT
 * set by this wake path, only SFT.
 */

#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/state.h>

#define RTC0 DT_NODELABEL(rtc0)

#define ALARM_MS_STOP  5000U
#define ALARM_MS_STBY  6000U
#define STOP_ROUNDS    3U

/* SYS controller reset/power registers (system.h layout). */
#define SYS_BASE        0x03000000UL
#define SYS_RST_CNTL    (*(volatile uint32_t *)(SYS_BASE + 0x04U))
#define SYS_CLK_CNTL    (*(volatile uint32_t *)(SYS_BASE + 0x0CU))
#define SYS_RSTF_LPWR   BIT(31)
#define SYS_RSTF_WDOG   BIT(30)
#define SYS_RSTF_IWDG   BIT(29)
#define SYS_RSTF_SFT    BIT(28)
#define SYS_RSTF_POR    BIT(27)
#define SYS_RSTF_PIN    BIT(26)
#define SYS_RSTF_EXT    BIT(25)

/* RTC backup data registers (0x40000040, 16-bit at 4-byte stride) — the
 * backup domain keeps its state across standby and software resets, so
 * the sample uses BKP_DR as a "standby round armed" marker. */
#define RTC_BASE        0x40000000UL
#define RTC_CRL         (*(volatile uint16_t *)(RTC_BASE + 0x04U))
#define RTC_CRL_RTOFF   BIT(5)
#define RTC_BKP_DR(m)   (*(volatile uint16_t *)(RTC_BASE + 0x40U + (m) * 4U))
#define BKP_STBY_MAGIC  0xA55AU
#define BKP_MAGIC_IDX   0U
#define BKP_CNT_LO_IDX  1U
#define BKP_CNT_HI_IDX  2U

/* On-board LED1 (gpio4 pin 1, active-low — board dts gpio-leds). */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct k_sem wake_sem;
static volatile uint32_t pm_stop_entries;
static volatile uint32_t pm_stop_exits;
static volatile uint32_t alarm_seq;

static const struct pm_state_info stop_info = {
	.state = PM_STATE_STANDBY,
	.substate_id = 1, /* SYS_STOP_MODE (PWR_CNTL 0b01) */
};
static const struct pm_state_info standby_info = {
	.state = PM_STATE_SOFT_OFF,
	.substate_id = 3, /* SYS_STANDBY_MODE (PWR_CNTL 0b11) */
};

static void pm_notify_entry(enum pm_state state)
{
	if (state == PM_STATE_STANDBY) {
		pm_stop_entries++;
	}
}

static void pm_notify_exit(enum pm_state state)
{
	if (state == PM_STATE_STANDBY) {
		pm_stop_exits++;
	}
}

static struct pm_notifier notifier = {
	.state_entry = pm_notify_entry,
	.state_exit = pm_notify_exit,
};

static inline uint16_t bkp_read(uint32_t idx)
{
	return RTC_BKP_DR(idx);
}

static inline void bkp_write(uint32_t idx, uint16_t v)
{
	/* Backup-domain writes must wait for the previous write sync
	 * (SDK RTC_WaitForWrite / driver agm_rtc_wait_rtoff). */
	while ((RTC_CRL & RTC_CRL_RTOFF) == 0U) {
	}
	RTC_BKP_DR(idx) = v;
}

static void led_init(void)
{
	if (gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE) != 0) {
		printk("pm_rtc_wake: led0 configure failed\n");
	}
}

static void led_toggle(void)
{
	gpio_pin_toggle_dt(&led0);
}

static void alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks,
		     void *user_data)
{
	uint32_t cnt = 0U;

	(void)counter_get_value(dev, &cnt);
	alarm_seq++;
	printk("pm_rtc_wake: [alarm #%u] CNT=%u target=%u\n",
	       (unsigned)alarm_seq, cnt, ticks);
	k_sem_give(&wake_sem);
}

static void print_reset_flags(void)
{
	uint32_t f = SYS_RST_CNTL;

	printk("pm_rtc_wake: RST_CNTL=0x%08x (", f);
	if (f & SYS_RSTF_LPWR) {
		printk("LPWR ");
	}
	if (f & SYS_RSTF_WDOG) {
		printk("WDOG ");
	}
	if (f & SYS_RSTF_IWDG) {
		printk("IWDG ");
	}
	if (f & SYS_RSTF_SFT) {
		printk("SFT ");
	}
	if (f & SYS_RSTF_POR) {
		printk("POR ");
	}
	if (f & SYS_RSTF_PIN) {
		printk("PIN ");
	}
	if (f & SYS_RSTF_EXT) {
		printk("EXT ");
	}
	printk(")\n");
}

/* One deep-sleep round: arm an RTC alarm, force the given PM state,
 * wait for the alarm callback, then report CNT vs k_uptime deltas. */
static int deep_round(const struct device *rtc,
		      const struct pm_state_info *info,
		      const char *mode, uint32_t round, uint32_t alarm_ms)
{
	struct counter_alarm_cfg alarm_cfg;
	uint32_t cnt0, cnt1, ticks;
	int64_t up0, up1;

	if (counter_get_value(rtc, &cnt0) != 0) {
		printk("pm_rtc_wake: counter_get_value failed\n");
		return -EIO;
	}
	up0 = k_uptime_get();
	ticks = counter_us_to_ticks(rtc, alarm_ms * 1000U);
	alarm_cfg.ticks = ticks;
	alarm_cfg.callback = alarm_cb;
	alarm_cfg.user_data = NULL;
	alarm_cfg.flags = 0;
	if (counter_set_channel_alarm(rtc, 0, &alarm_cfg) != 0) {
		printk("pm_rtc_wake: set alarm failed\n");
		return -EIO;
	}

	printk("pm_rtc_wake: [%u] %s for %u ms (alarm %u ticks from "
	       "CNT=%u)\n", round, mode, alarm_ms, ticks, cnt0);
	/* Let the console drain before the clocks stop (stop mode freezes
	 * the UART mid-frame otherwise). */
	k_msleep(20);
	if (info->state == PM_STATE_SOFT_OFF) {
		/* Record the marker + counter so the post-standby boot can
		 * prove the RTC alarm woke the SoC and report how long the
		 * backup-domain RTC kept counting. Written in this order so
		 * a reset between the two writes leaves no false marker. */
		bkp_write(BKP_CNT_LO_IDX, cnt0 & 0xFFFFU);
		bkp_write(BKP_CNT_HI_IDX, cnt0 >> 16);
		bkp_write(BKP_MAGIC_IDX, BKP_STBY_MAGIC);
	}
	if (!pm_state_force(0, info)) {
		printk("pm_rtc_wake: pm_state_force(%s) failed\n", mode);
		return -EIO;
	}
	led_toggle();
	k_sem_take(&wake_sem, K_FOREVER);
	/* The kernel consumes the forced state on the idle entry it
	 * just made, so the next idle automatically falls back to
	 * suspend-to-idle (sleep) — no explicit clear needed. */
	led_toggle();

	up1 = k_uptime_get();
	if (counter_get_value(rtc, &cnt1) != 0) {
		cnt1 = 0U;
	}
	printk("pm_rtc_wake: [%u] woke: RTC +%u ticks (%u -> %u), "
	       "uptime +%lld ms, pm entries/exits=%u/%u, CLK_CNTL=0x%08x\n",
	       round, (unsigned)(cnt1 - cnt0), cnt0, cnt1,
	       (long long)(up1 - up0), (unsigned)pm_stop_entries,
	       (unsigned)pm_stop_exits, SYS_CLK_CNTL);

	return (cnt1 - cnt0) >= 1U ? 0 : -ETIMEDOUT;
}

int main(void)
{
	const struct device *rtc;
	uint32_t cnt, cnt_entry;
	int err;

	printk("pm_rtc_wake: RTC-alarm stop/standby wake demo\n");
	print_reset_flags();

	/* The reset-cause flags are sticky (cleared only by writing the
	 * REMOVE bit); clear them now so the next boot's print_reset_flags
	 * shows the fresh cause. */
	SYS_RST_CNTL |= BIT(24); /* SYS_RST_REMOVE */

	if (bkp_read(BKP_MAGIC_IDX) == BKP_STBY_MAGIC) {
		/* Boot right after the standby round: pm.c's standby wake
		 * (RTC alarm after ~6 s) forced a software reset because
		 * this board's option byte makes the core resume after WFI
		 * instead of a power-on reset. The backup-domain marker and
		 * RTC CNT prove the soft-off wake and its duration. */
		cnt_entry = ((uint32_t)bkp_read(BKP_CNT_HI_IDX) << 16) |
			    bkp_read(BKP_CNT_LO_IDX);
		bkp_write(BKP_MAGIC_IDX, 0U);
		printk("pm_rtc_wake: standby wake confirmed (backup marker)\n");
		rtc = DEVICE_DT_GET(RTC0);
		if (device_is_ready(rtc) &&
		    counter_get_value(rtc, &cnt) == 0) {
			uint32_t delta = cnt - cnt_entry;

			printk("pm_rtc_wake: RTC CNT continued %u -> %u "
			       "(+%u, expect ~6)\n",
			       cnt_entry, cnt, delta);
			/* A real 6 s soft-off must advance the backup-domain
			 * counter by ~6; a near-zero delta means the entry
			 * fell back to a shallow state and reset at once. */
			printk("pm_rtc_wake: %s\n",
			       delta >= 3U ? "standby 6 s RTC wake - PASS" :
			       "standby duration too short - FAIL");
		} else {
			printk("pm_rtc_wake: rtc0 read failed\n");
		}
		return 0;
	}

	rtc = DEVICE_DT_GET(RTC0);
	if (!device_is_ready(rtc)) {
		printk("pm_rtc_wake: rtc0 not ready\n");
		return 0;
	}
	printk("pm_rtc_wake: rtc freq=%u Hz\n", counter_get_frequency(rtc));

	err = counter_start(rtc);
	if (err != 0) {
		printk("pm_rtc_wake: counter_start failed: %d\n", err);
		return 0;
	}

	led_init();
	k_sem_init(&wake_sem, 0, 1);
	pm_notifier_register(&notifier);
	pm_stop_entries = 0;
	pm_stop_exits = 0;
	alarm_seq = 0;

	for (uint32_t i = 0; i < STOP_ROUNDS; i++) {
		if (deep_round(rtc, &stop_info, "stop", i + 1U,
			       ALARM_MS_STOP) != 0) {
			printk("pm_rtc_wake: stop round %u FAILED\n", i + 1U);
			return 0;
		}
	}

	printk("pm_rtc_wake: %u stop-mode RTC alarm wakes OK - PASS\n",
	       STOP_ROUNDS);

	if (IS_ENABLED(CONFIG_PM_RTC_WAKE_TEST_STANDBY)) {
		/* Final round: soft-off (standby). The RTC alarm wakes the
		 * SoC ~6 s later; on this board pm.c turns the wake into a
		 * software reset and the image reboots, where the marker
		 * path above reports the wake. */
		printk("pm_rtc_wake: entering standby (soft-off) for %u ms "
		       "- expect RTC-alarm wake\n", ALARM_MS_STBY);
		err = deep_round(rtc, &standby_info, "standby", 99U,
				 ALARM_MS_STBY);
		if (err != 0) {
			printk("pm_rtc_wake: standby round returned %d "
			       "(unexpected for POR wake)\n", err);
		}
	}
	return 0;
}
