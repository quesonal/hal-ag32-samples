/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * gptimer_alarm - smoke test for the AgRV2K general-purpose timer
 * (GPTIMER0-4, agm,agrv2k-gptimer counter driver).
 *
 * Starts GPTIMER0's free-running counter and arms a 1 s periodic
 * alarm on compare channel 0. The callback re-arms itself; the main
 * loop samples the counter value once per second so the counter clock
 * can be cross-checked against k_uptime() (prints the measured
 * frequency every 5 s).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>

#define GPT0 DT_NODELABEL(gpt0)

#define ALARM_MS 1000U

static struct counter_alarm_cfg alarm_cfg;
static uint32_t alarm_seq;

static void alarm_cb(const struct device *dev, uint8_t chan_id,
		     uint32_t ticks, void *user_data)
{
	alarm_seq++;
	printk("gptimer_alarm: alarm %u @ %u ticks (uptime %u ms)\n",
	       alarm_seq, ticks, (uint32_t)k_uptime_get_32());

	/* Re-arm a 1 s relative alarm. */
	counter_set_channel_alarm(dev, chan_id, user_data);
}

int main(void)
{
	const struct device *counter = DEVICE_DT_GET(GPT0);
	uint32_t last_val = 0U;
	uint32_t last_up = 0U;
	int err;

	printk("gptimer_alarm: GPTIMER0 counter device test\n");

	if (!device_is_ready(counter)) {
		printk("gptimer_alarm: counter not ready\n");
		return 0;
	}

	printk("gptimer_alarm: freq=%u Hz channels=%u top=%u\n",
	       counter_get_frequency(counter),
	       counter_get_num_of_channels(counter),
	       counter_get_top_value(counter));

	err = counter_start(counter);
	if (err != 0) {
		printk("gptimer_alarm: counter_start failed: %d\n", err);
		return 0;
	}

	alarm_cfg.ticks = counter_us_to_ticks(counter, ALARM_MS * 1000U);
	alarm_cfg.callback = alarm_cb;
	alarm_cfg.user_data = &alarm_cfg;
	alarm_cfg.flags = 0;

	err = counter_set_channel_alarm(counter, 0, &alarm_cfg);
	printk("gptimer_alarm: alarm set: %u ticks (err %d)\n",
	       alarm_cfg.ticks, err);

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

			printk("gptimer_alarm: value=%u ticks (%u ms), measured clock %u MHz\n",
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
