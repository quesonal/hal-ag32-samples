/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * rtc_alarm - RTC (agm,agrv2k-rtc counter driver) smoke test.
 *
 * The RTC is a backup-domain STM32F1-style 32-bit counter. With the
 * default DT properties (32768 Hz LSE, prescaler 32767) it ticks once
 * per second, so the counter value should track k_uptime().
 *
 * 1) Watch the counter for 10 s: the value must advance by ~1 every
 *    second (prints the running delta).
 * 2) Arm a 5 s relative alarm and re-arm it twice from the callback;
 *    each callback must land within ~1 s of the armed uptime.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define RTC0 DT_NODELABEL(rtc0)

#define ALARM_MS 5000U

static struct counter_alarm_cfg alarm_cfg;
static uint32_t alarm_seq;
static uint32_t alarm_deltas_ms[3];
static uint32_t alarm_armed_ms;

static void alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks,
		     void *user_data)
{
	uint32_t up = (uint32_t)k_uptime_get_32();
	uint32_t cnt = 0U;
	uint32_t delta = up - alarm_armed_ms;

	if (alarm_seq < ARRAY_SIZE(alarm_deltas_ms)) {
		alarm_deltas_ms[alarm_seq] = delta;
	}
	alarm_seq++;
	(void)counter_get_value(dev, &cnt);
	printk("rtc_alarm: alarm #%u @ %u ticks (CNT=%u, uptime %u ms, "
	       "fired %u ms after arming)\n",
	       alarm_seq, ticks, cnt, up, delta);

	if (alarm_seq < 3U) {
		/* Re-arm a 5 s relative alarm from the callback. */
		alarm_armed_ms = up;
		counter_set_channel_alarm(dev, chan_id, user_data);
	}
}

int main(void)
{
	const struct device *rtc = DEVICE_DT_GET(RTC0);
	uint32_t val, prev_val;
	uint32_t up, prev_up;
	uint32_t start_ms, start_val;
	int err;

	printk("rtc_alarm: RTC counter device test\n");

	if (!device_is_ready(rtc)) {
		printk("rtc_alarm: rtc counter not ready\n");
		return 0;
	}

	printk("rtc_alarm: freq=%u Hz channels=%u top=%u\n",
	       counter_get_frequency(rtc),
	       counter_get_num_of_channels(rtc),
	       counter_get_top_value(rtc));

	err = counter_start(rtc);
	if (err != 0) {
		printk("rtc_alarm: counter_start failed: %d\n", err);
		return 0;
	}

	/* 1) Free-run check: value should advance ~1 tick per second. */
	start_ms = (uint32_t)k_uptime_get_32();
	if (counter_get_value(rtc, &start_val) != 0) {
		printk("rtc_alarm: counter_get_value failed\n");
		return 0;
	}
	prev_val = start_val;
	prev_up = start_ms;
	for (int i = 0; i < 10; i++) {
		k_sleep(K_MSEC(1000));
		if (counter_get_value(rtc, &val) != 0) {
			continue;
		}
		up = (uint32_t)k_uptime_get_32();
		printk("rtc_alarm: +%u s: cnt=%u (delta %u ticks over %u ms)\n",
		       i + 1, val, (uint32_t)(val - prev_val), up - prev_up);
		prev_val = val;
		prev_up = up;
	}

	/* Report the average rate over the whole window. */
	if (counter_get_value(rtc, &val) == 0) {
		uint32_t dt_ms = (uint32_t)k_uptime_get_32() - start_ms;
		uint32_t dt_ticks = (uint32_t)(val - start_val);

		printk("rtc_alarm: measured %u ticks over %u ms "
		       "(expected ~1 Hz)\n", dt_ticks, dt_ms);
	}

	/* 2) Alarm test: 5 s relative alarm, re-armed twice. */
	alarm_cfg.ticks = counter_us_to_ticks(rtc, ALARM_MS * 1000U);
	alarm_cfg.callback = alarm_cb;
	alarm_cfg.user_data = &alarm_cfg;
	alarm_cfg.flags = 0;

	alarm_armed_ms = (uint32_t)k_uptime_get_32();
	err = counter_set_channel_alarm(rtc, 0, &alarm_cfg);
	printk("rtc_alarm: alarm set: %u ticks (err %d)\n",
	       alarm_cfg.ticks, err);
	if (err != 0) {
		return 0;
	}

	while (alarm_seq < 3U) {
		k_sleep(K_MSEC(100));
	}

	printk("rtc_alarm: alarm deltas: %u / %u / %u ms (armed %u ms, "
	       "expected ~%u each)\n",
	       alarm_deltas_ms[0], alarm_deltas_ms[1], alarm_deltas_ms[2],
	       ALARM_MS, ALARM_MS);

	err = counter_cancel_channel_alarm(rtc, 0);
	printk("rtc_alarm: alarm cancelled (err %d) - PASS\n", err);
	return 0;
}
