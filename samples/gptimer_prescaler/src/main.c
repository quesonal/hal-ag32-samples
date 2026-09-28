/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * gptimer_prescaler - report the GPTIMER0 PSC prescaler width.
 *
 * The register is 32 bits wide in the SDK map (framework-agrv_sdk/src/
 * gptimer.h declares `__IO uint32_t PSC`) but the implemented field is
 * PSC[15:0], matching the SDK's GPTIMER_InitTypeDef.Prescaler being
 * uint16_t. This sample measures the two independently, because a field
 * could store 32 bits and still divide by only 16 of them:
 *
 *   - readback of a value with bits set above bit 15, and
 *   - the rate CNT actually advances at, against both hypotheses:
 *         16-bit:  pclk / (psc & 0xffff) + 1
 *         32-bit:  pclk / psc + 1
 *
 * The probe values differ by up to 65536x between the two readings, so
 * the 300 ms window is enough to tell them apart. 0xffffffff is not
 * probed: at a true 32-bit divide it would need ~21 s for a single tick.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define GPT0      DT_NODELABEL(gpt0)
#define GPT0_BASE DT_REG_ADDR(GPT0)

/* Register offsets (SDK gptimer.h). */
#define R_EGR 0x14U
#define R_PSC 0x28U
#define EGR_UG BIT(0)

/* pclk, taken from the node's `clocks` phandle the same way the driver does. */
#define PCLK_HZ DT_PROP_BY_PHANDLE(GPT0, clocks, clock_frequency)

/* Sampling window. Long enough that the slowest probe still accumulates a
 * useful number of counts, short enough to keep the sample quick.
 */
#define WINDOW_MS 300U

struct probe {
	uint32_t psc;
	const char *label;
};

/* 0x000f0000 and 0x80000000 are the decisive ones: at a 32-bit divide their
 * tick rates are ~203 Hz and ~0.09 Hz, so a 300 ms window collects tens and
 * zero counts respectively, against ten million for the 16-bit reading.
 */
static const struct probe probes[] = {
	{ 0x00000000U, "0          (control, /1)" },
	{ 0x00000003U, "3          (control, /4)" },
	{ 0x0000ffffU, "0xffff     (16-bit max)" },
	{ 0x00010000U, "0x10000    (bit 16)" },
	{ 0x000f0000U, "0xf0000    (bits 16-19)" },
	{ 0x80000000U, "0x80000000 (bit 31)" },
};

/* Program PSC, then latch it with an update event: PSC only takes effect
 * once EGR.UG follows it. Returns the value that read back.
 */
static uint32_t program_psc(uint32_t psc)
{
	sys_write32(psc, GPT0_BASE + R_PSC);
	sys_write32(EGR_UG, GPT0_BASE + R_EGR);
	return sys_read32(GPT0_BASE + R_PSC);
}

/* Measure the tick rate by sampling CNT across a wall-clock window.
 *
 * CNT wraps every 2^32 ticks, which at the /1 rate is ~21 s, so a short
 * window is handled by plain uint32 subtraction. The arithmetic below is
 * 64-bit: ten million counts over 300 ms overflows 32 bits once scaled
 * to per-second.
 */
static uint64_t measure_hz(const struct device *dev)
{
	uint32_t t0, t1, u0, u1, dt;
	uint64_t hz;

	if (counter_get_value(dev, &t0) != 0) {
		return 0U;
	}
	u0 = (uint32_t)k_uptime_get_32();
	k_busy_wait(WINDOW_MS * 1000U);
	if (counter_get_value(dev, &t1) != 0) {
		return 0U;
	}
	u1 = (uint32_t)k_uptime_get_32();

	dt = (uint32_t)(t1 - t0);

	/* Guard the divide: k_uptime_get_32() can read the same value twice if
	 * the window was shorter than its resolution, which would blow up.
	 */
	if (u1 == u0) {
		return 0U;
	}

	hz = (uint64_t)dt * 1000ULL / (uint64_t)(u1 - u0);
	return hz;
}

int main(void)
{
	const struct device *counter = DEVICE_DT_GET(GPT0);
	uint64_t expected16, expected32;
	int err;

	printk("\n");
	printk("gptimer_prescaler: GPTIMER0 PSC width characterisation\n");
	printk("  pclk from DT: %u Hz\n", (unsigned int)PCLK_HZ);

	if (!device_is_ready(counter)) {
		printk("gptimer_prescaler: counter not ready\n");
		return 0;
	}

	err = counter_start(counter);
	if (err != 0) {
		printk("gptimer_prescaler: counter_start failed: %d\n", err);
		return 0;
	}

	printk("\n");
	printk("  %-28s %-12s %-14s %-14s %s\n", "programmed", "readback",
	       "measured", "expect /1<<16", "verdict");
	printk("  %-28s %-12s %-14s %-14s %s\n", "----------------------------",
	       "------------", "--------------", "--------------", "-------");

	for (size_t i = 0U; i < ARRAY_SIZE(probes); i++) {
		uint32_t readback;
		uint64_t hz;

		readback = program_psc(probes[i].psc);

		/* Let the new divisor take effect before sampling; the first
		 * counts after EGR.UG can still be at the old rate.
		 */
		k_msleep(20);

		hz = measure_hz(counter);

		/* Both hypotheses, from the *programmed* value: the readback is
		 * reported separately rather than folded in, so a register that
		 * stores 32 bits but divides by 16 shows up as a disagreement
		 * here instead of being masked by a masked readback.
		 */
		expected16 = (uint64_t)PCLK_HZ / (((uint64_t)probes[i].psc & 0xffffU) + 1U);
		expected32 = (uint64_t)PCLK_HZ / ((uint64_t)probes[i].psc + 1U);

		printk("  %-28s 0x%08x   ", probes[i].label, (unsigned int)readback);
		if (hz == 0U) {
			printk("%-14s %-14llu %s\n", "(no window)", (unsigned long long)expected16,
			       "?");
			continue;
		}
		printk("%-14llu %-14llu ", (unsigned long long)hz,
		       (unsigned long long)expected16);

		/* Match against whichever hypothesis the measurement supports.
		 * 20% slack is generous for a 300 ms window but far tighter
		 * than the 65536x gap the probes are chosen to create.
		 */
		if (hz * 5U >= expected16 * 4U && hz * 5U <= expected16 * 6U) {
			printk("16-bit\n");
		} else if (hz * 5U >= expected32 * 4U && hz * 5U <= expected32 * 6U) {
			printk("32-bit\n");
		} else {
			printk("neither\n");
		}
	}

	/* Leave the timer the way the node asked for it. */
	program_psc(0U);

	printk("\n");
	printk("gptimer_prescaler: if every row above reads '16-bit', the 0..65535\n");
	printk("gptimer_prescaler: range in the bindings is the implemented width.\n");

	return 0;
}
