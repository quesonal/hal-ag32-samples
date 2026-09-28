/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * gptimer_pwm_pinout — verify the gptimer_pwm driver on a path
 * that actually drives a physical L100 pin.
 *
 * The shipped example_board.ve (AgRV_pio .../example_board.ve)
 * contains exactly one GPTIMER -> PIN routing:
 *
 *   GPTIMER1_CH0 PIN_7
 *
 * so this sample uses &gpt1_pwm (GPTIMER1 PWM) and configures
 * CC0 to drive PIN_7 directly. PIN_7 is not connected to an
 * on-board LED on the 407 — to see the waveform you need a
 * scope or logic analyzer on PIN_7 (or a jumper wire from
 * PIN_7 to an LED + resistor to GND).
 *
 * What this sample verifies at the register level (which is
 * all we can do from the host without a scope):
 *   1. Driver binds, device_is_ready
 *   2. pwm_set_cycles succeeds with a 1 kHz / 50% config
 *   3. ARR + CCR0 + CCER.CC0E + CCMR0.OC0M=PWM1 are all set
 *      as expected (read back via sys_read32)
 *   4. BDTR.MOE = 1 (this is the gate that lets OCx reach the
 *      L100 fabric — without it, the pin never toggles even
 *      with CCER.CC0E set)
 *   5. CC0IF fires at the expected rate (1 kHz at 200 MHz pclk
 *      = once per 200,000 ticks). We count events in a 1-second
 *      window and print the result.
 *
 * What this sample does NOT verify (hardware scope required):
 *   - PIN_7 actually toggles between 0V and 3.3V at 1 kHz / 50%.
 *   To verify, hook a scope probe to PIN_7 on the L100
 *   footprint. If the bitstream is intact and the driver is
 *   correct, you should see a clean 1 kHz square wave.
 *
 * See `the development notes (not published here)` GPTIMER PWM for context.
 */

#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

/* GPTIMER1 register block (same layout as GPTIMER0, just at
 * 0x40021000). Offsets from pwm_agm_gptimer.c header.
 */
#define GPT1_BASE     0x40021000UL
#define REG_CR1       0x00U
#define REG_CCMR0     0x18U
#define REG_CCER      0x20U
#define REG_CNT       0x24U
#define REG_ARR       0x2cU
#define REG_CCR0      0x34U
#define REG_BDTR      0x44U
#define REG_SR        0x10U

#define GPT_SR_UIF    BIT(0)
#define GPT_SR_CC0IF  BIT(1)

/* CCR0=period-1 -> period_cycles ticks per PWM period.
 * At 200 MHz pclk, 200,000 ticks = 1 ms = 1 kHz.
 */
#define PWM_TARGET_HZ        1000U
#define PWM_PERIOD    (200000000U / PWM_TARGET_HZ)   /* 200,000 ticks = 1 kHz */
#define PWM_PULSE     (PWM_PERIOD / 2U)       /* 50% duty */

static inline uint32_t gpt1_read(uint32_t off)
{
	return sys_read32(GPT1_BASE + off);
}

static inline void gpt1_write(uint32_t off, uint32_t v)
{
	sys_write32(v, GPT1_BASE + off);
}

int main(void)
{
	const struct device *pwm = DEVICE_DT_GET(DT_NODELABEL(gpt1_pwm));
	int rc;

	if (!device_is_ready(pwm)) {
		printk("gptimer_pwm_pinout: gpt1_pwm not ready\n");
		return 0;
	}
	printk("gptimer_pwm_pinout: gpt1_pwm ready (drives PIN_7 via bitstream)\n");

	/* Configure CC0: 1 kHz, 50% duty, normal polarity. */
	rc = pwm_set_cycles(pwm, 0, PWM_PERIOD, PWM_PULSE, PWM_POLARITY_NORMAL);
	if (rc != 0) {
		printk("gptimer_pwm_pinout: pwm_set_cycles failed: %d\n", rc);
		return 0;
	}

	/* Read back and confirm every register bit that the scope
	 * viewer will be looking for.
	 */
	uint32_t cr1   = gpt1_read(REG_CR1);
	uint32_t ccmr0 = gpt1_read(REG_CCMR0);
	uint32_t ccer  = gpt1_read(REG_CCER);
	uint32_t arr   = gpt1_read(REG_ARR);
	uint32_t ccr0  = gpt1_read(REG_CCR0);
	uint32_t bdtr  = gpt1_read(REG_BDTR);
	uint32_t oc0m  = (ccmr0 >> 4) & 0x7U;
	uint32_t cen   = (cr1 >> 0) & 0x1U;
	uint32_t moe   = (bdtr >> 15) & 0x1U;

	printk("gptimer_pwm_pinout: 1 kHz / 50%% configured\n");
	printk("  CR1.CEN=%u (counter running)\n", cen);
	printk("  CCMR0.OC0M=%u (6=PWM1 mode)\n", oc0m);
	printk("  CCER=0x%x (bit 0 = CC0E, bit 1 = CC0P polarity)\n", ccer);
	printk("  ARR=%u (period-1, want %u)\n", arr, PWM_PERIOD - 1U);
	printk("  CCR0=%u (pulse, want %u)\n", ccr0, PWM_PULSE);
	printk("  BDTR.MOE=%u (1 = OCx drives the L100 fabric)\n", moe);

	if (cen != 1U) {
		printk("  WARNING: counter not enabled\n");
	}
	if (oc0m != 6U) {
		printk("  WARNING: OC0M != PWM1 mode\n");
	}
	if (!(ccer & BIT(0))) {
		printk("  WARNING: CCER.CC0E not set\n");
	}
	if (moe != 1U) {
		printk("  WARNING: BDTR.MOE not set -> PIN_7 will NOT toggle\n");
	}

	/* Measure CC0IF rate over a 1-second window. Should be ~1000
	 * (one CC0IF per period, period = 1 ms, window = 1000 ms).
	 * If it's wildly off, the driver config is wrong. If it's
	 * exactly 1000, the timer is ticking at the configured rate
	 * and the OCx output is on schedule.
	 */
	/* Clear any pre-existing CC0IF (rc_w0) so we count fresh. */
	gpt1_write(REG_SR, ~GPT_SR_CC0IF);

	uint32_t start = k_uptime_get_32();
	uint32_t cc0if_count = 0;
	while ((k_uptime_get_32() - start) < 1000U) {
		uint32_t sr = gpt1_read(REG_SR);
		if (sr & GPT_SR_CC0IF) {
			gpt1_write(REG_SR, ~GPT_SR_CC0IF);
			cc0if_count++;
		}
	}

	uint32_t elapsed_ms = k_uptime_get_32() - start;
	uint32_t measured_hz = (cc0if_count * 1000U) / elapsed_ms;

	printk("gptimer_pwm_pinout: CC0IF count in %u ms = %u (expect ~%u)\n",
	       elapsed_ms, cc0if_count, PWM_TARGET_HZ);
	printk("  -> measured PWM frequency ~%u Hz (target %u Hz)\n",
	       measured_hz, PWM_TARGET_HZ);

	if (measured_hz == PWM_TARGET_HZ) {
		printk("gptimer_pwm_pinout: PASS — timer is on schedule\n");
	} else if (measured_hz > (PWM_TARGET_HZ * 9) / 10 &&
		   measured_hz < (PWM_TARGET_HZ * 11) / 10) {
		printk("gptimer_pwm_pinout: PASS — within +/- 10%% of target\n");
	} else {
		printk("gptimer_pwm_pinout: FAIL — frequency off by >10%%\n");
	}

	/* Leave the PWM running. If a scope is on PIN_7, the user
	 * sees a continuous 1 kHz / 50% square wave forever.
	 */
	while (1) {
		k_msleep(1000);
	}
	return 0;
}
