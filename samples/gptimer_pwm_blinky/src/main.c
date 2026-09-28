/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * gptimer_pwm_blinky — drive the on-board 4 LEDs from gptimer_pwm's
 * CC0IF compare event.
 *
 * Why software toggle, not direct OCx output?
 *   The shipped example_board.bin does NOT route any GPTIMER OCx
 *   to a physical L100 pin. The Zephyr PWM driver programs the
 *   timer registers correctly, but the pin never toggles because
 *   the bitstream's fabric connection is missing. To get a
 *   visible "blinky" without rebuilding the bitstream (which
 *   needs Quartus on a separate workstation), we poll the CC0IF
 *   status bit in a busy loop and toggle the LED GPIO there.
 *   This proves:
 *     (a) gptimer_pwm configured the timer correctly
 *     (b) ARR + CCRx values result in a 1 Hz CC0IF tick
 *     (c) The LED GPIO toggles in lockstep with the PWM tick
 *
 * Wiring:
 *   PWM CC0   -> SR.CC0IF (every 1 sec, 50% duty)
 *   LED1      -> GPIO4 bit 1  (active-low: write 0 to light)
 *   LED2      -> GPIO4 bit 2  (active-low: write 0 to light)
 *   LED3      -> GPIO4 bit 3  (active-low: write 0 to light)
 *   LED_EXT   -> GPIO4 bit 4  (active-low: write 0 to light)
 *
 * Per CC0IF we cycle through the 4 LEDs (LED1 -> LED2 -> LED3
 * -> LED_EXT -> off -> repeat), so all 4 LEDs blink in turn at
 * 1/4 Hz each, with 1 LED on at any moment.
 *
 * See `the development notes (not published here)` GPTIMER PWM for context. Once
 * board.ve routes an OCx output to a physical pin, this same
 * sample would Just Work as a direct PWM-driven blinky (no
 * software toggle needed).
 */

#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

/* GPTIMER0 register block (see pwm_agm_gptimer.c). */
#define GPT0_BASE 0x40020000UL
#define REG_SR    0x10U
#define GPT_SR_CC0IF  BIT(1)

/* The four on-board LEDs (gpio4 pins 1..4, active-low — board dts
 * gpio-leds). LED logic level 1 = on (physical low). */
static const struct gpio_dt_spec leds[4] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};

static void leds_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) != 0) {
			printk("gptimer_pwm_blinky: led%u configure failed\n",
			       (unsigned)i);
		}
	}
}

static void led_set(int idx, bool on)
{
	if (idx >= 0 && idx < ARRAY_SIZE(leds)) {
		gpio_pin_set_dt(&leds[idx], on ? 1 : 0);
	}
}

static inline uint32_t gpt0_read(uint32_t off)
{
	return sys_read32(GPT0_BASE + off);
}

static inline void gpt0_write(uint32_t off, uint32_t v)
{
	sys_write32(v, GPT0_BASE + off);
}

int main(void)
{
	const struct device *pwm = DEVICE_DT_GET(DT_NODELABEL(gpt0_pwm));
	int rc;

	if (!device_is_ready(pwm)) {
		printk("gptimer_pwm_blinky: gpt0_pwm not ready\n");
		return 0;
	}

	/* Set up the LED pins as outputs, all off (logical 0). */
	leds_init();

	/* 1 Hz PWM on CC0: ARR=199,999,999 (period-1), CCR0=99,999,999
	 * (50% duty, pclk = 200 MHz so 200,000,000 ticks = 1 sec).
	 */
	const uint32_t period = 200000000U;
	const uint32_t pulse  = 100000000U;
	rc = pwm_set_cycles(pwm, 0, period, pulse, PWM_POLARITY_NORMAL);
	if (rc != 0) {
		printk("gptimer_pwm_blinky: pwm_set_cycles failed: %d\n", rc);
		return 0;
	}

	/* Read back to confirm. */
	uint32_t sr_init = gpt0_read(REG_SR);

	/* ARR/CCER.CC0E sanity check. */
	uint32_t arr = sys_read32(GPT0_BASE + 0x2cU);
	uint32_t ccer = sys_read32(GPT0_BASE + 0x20U);
	uint32_t ccmr0 = sys_read32(GPT0_BASE + 0x18U);
	uint32_t ocm = (ccmr0 >> 4) & 0x7U;

	printk("gptimer_pwm_blinky: PWM CC0 configured, ARR=%u CCR0=period/2=%u\n",
	       arr, pulse);
	printk("gptimer_pwm_blinky: SR=0x%x CCER=0x%x CCMR0.OC0M=%u (6=PWM1)\n",
	       sr_init, ccer, ocm);
	if (ocm != 6U) {
		printk("gptimer_pwm_blinky: WARNING OC0M != PWM1 mode\n");
	}
	if (!(ccer & BIT(0))) {
		printk("gptimer_pwm_blinky: WARNING CCER.CC0E not set\n");
	}

	/* LED cycle: 0 = LED1, 1 = LED2, 2 = LED3, 3 = LED4, 4 = off. */
	const int n_leds = ARRAY_SIZE(leds) + 1;

	uint32_t tick = 0;
	int led_idx = 0;
	int prev_idx = -1;              /* no LED lit yet */

	while (1) {
		/* Poll SR.CC0IF. Each match toggles the active LED. */
		uint32_t sr = gpt0_read(REG_SR);
		if (sr & GPT_SR_CC0IF) {
			/* rc_w0: clear by writing 0 to the bit. */
			gpt0_write(REG_SR, ~GPT_SR_CC0IF);

			/* Turn the previous LED off, light the next one. */
			led_set(prev_idx, false);
			led_set(led_idx, true);
			prev_idx = led_idx;
			led_idx = (led_idx + 1) % n_leds;

			tick++;
			if ((tick % 4) == 0) {
				/* every 4 sec print a heartbeat */
				uint32_t sr2 = gpt0_read(REG_SR);
				uint32_t cnt = sys_read32(GPT0_BASE + 0x24U);
				printk("gptimer_pwm_blinky: tick=%u led_idx=%d "
				       "SR=0x%x CNT=%u\n",
				       tick, led_idx, sr2, cnt);
			}
		}
	}
	return 0;
}
