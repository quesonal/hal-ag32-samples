/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * timer_alarm - smoke test for the AgRV2K dual 32-bit timer
 * (TIMER0/1, agm,agrv2k-timer counter driver).
 *
 * Starts TIMER0's counter, arms a 1 s periodic alarm on channel 0
 * (sub-timer 1) and a 2 s periodic alarm on channel 1 (sub-timer 2).
 * The 1 s callback re-arms itself; the main loop samples the counter
 * value once per second so the counter clock can be cross-checked
 * against k_uptime() (prints the measured frequency every 5 s).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>

#define TIM0 DT_NODELABEL(tim0)

#define ALARM_MS_CH0 1000U
#define ALARM_MS_CH1 2000U

static struct counter_alarm_cfg ch0_cfg;
static struct counter_alarm_cfg ch1_cfg;
static uint32_t ch0_seq;

static void ch0_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks,
		   void *user_data)
{
	ch0_seq++;
	printk("timer_alarm: ch0 alarm %u @ %u ticks (uptime %u ms)\n",
	       ch0_seq, ticks, (uint32_t)k_uptime_get_32());

	/* Re-arm a 1 s relative alarm. */
	counter_set_channel_alarm(dev, chan_id, user_data);
}

static void ch1_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks,
		   void *user_data)
{
	printk("timer_alarm: ch1 alarm @ %u ticks (uptime %u ms)\n",
	       ticks, (uint32_t)k_uptime_get_32());

	/* Re-arm a 2 s relative alarm. */
	counter_set_channel_alarm(dev, chan_id, user_data);
}

int main(void)
{
	const struct device *counter = DEVICE_DT_GET(TIM0);
	uint32_t last_val = 0U;
	uint32_t last_up = 0U;
	int err;

	printk("timer_alarm: TIMER0 counter device test\n");

	if (!device_is_ready(counter)) {
		printk("timer_alarm: counter not ready\n");
		return 0;
	}

	printk("timer_alarm: freq=%u Hz channels=%u top=%u\n",
	       counter_get_frequency(counter),
	       counter_get_num_of_channels(counter),
	       counter_get_top_value(counter));

	err = counter_start(counter);
	if (err != 0) {
		printk("timer_alarm: counter_start failed: %d\n", err);
		return 0;
	}

	ch0_cfg.ticks = counter_us_to_ticks(counter, ALARM_MS_CH0 * 1000U);
	ch0_cfg.callback = ch0_cb;
	ch0_cfg.user_data = &ch0_cfg;
	ch0_cfg.flags = 0;

	err = counter_set_channel_alarm(counter, 0, &ch0_cfg);
	printk("timer_alarm: ch0 alarm set: %u ticks (err %d)\n",
	       ch0_cfg.ticks, err);

	ch1_cfg.ticks = counter_us_to_ticks(counter, ALARM_MS_CH1 * 1000U);
	ch1_cfg.callback = ch1_cb;
	ch1_cfg.user_data = &ch1_cfg;
	ch1_cfg.flags = 0;

	err = counter_set_channel_alarm(counter, 1, &ch1_cfg);
	printk("timer_alarm: ch1 alarm set: %u ticks (err %d)\n",
	       ch1_cfg.ticks, err);

	for (;;) {
		uint32_t now_val;
		uint32_t now_up;
		uint64_t dt;

		k_sleep(K_MSEC(1000));
		if (counter_get_value(counter, &now_val) != 0) {
			continue;
		}
		now_up = (uint32_t)k_uptime_get_32();
		/* uint32 subtraction keeps the delta correct across the
		 * 32-bit counter wrap (~21.5 s at 200 MHz).
		 */
		dt = (uint32_t)(now_val - last_val);

		if (now_up - last_up >= 5000U) {
			uint32_t dt_ms = now_up - last_up;

			printk("timer_alarm: value=%u ticks (%u ms), measured clock %u MHz\n",
			       now_val,
			       (uint32_t)counter_ticks_to_us(counter, now_val) / 1000U,
			       (uint32_t)((dt * 1000U) / dt_ms / 1000000U));
			last_val = now_val;
			last_up = now_up;
		} else if (last_up == 0U) {
			last_val = now_val;
			last_up = now_up;
		}
	}
	return 0;
}
