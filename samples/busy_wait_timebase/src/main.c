/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief k_busy_wait's time base, measured on the board.
 *
 * What the code says (the port record, boot-and-clock 3.31): k_busy_wait()
 * converts microseconds with CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC, which
 * soc/agm/agrv2k/Kconfig.defconfig defaults to the board devicetree's
 * /cpus/cpu@0 clock-frequency (= the board file's AGM_SYSCLK_HZ), and that
 * value is a compile-time constant: SYSTEM_CLOCK_HW_CYCLES_PER_SEC_RUNTIME_
 * UPDATE is not enabled and nothing in this module calls
 * z_sys_clock_hw_cycles_per_sec_update().
 *
 * The assumption under that constant is that k_cycle_get_32() -- the CLINT's
 * `mtime`, which the RISC-V timer driver returns with
 * RISCV_MACHINE_TIMER_SYSTEM_CLOCK_DIVIDER (0 here) -- counts at SYSCLK. This
 * sample tests exactly that, without needing a host clock:
 *
 *   k_busy_wait(N) waits until `mtime` has advanced by N * FDECLARED ticks. If
 *   `mtime` ticks at SYSCLK, the core's own cycle counter (`mcycle`, CSR
 *   0xB00) advances by the same number of ticks over the same interval, so
 *   the ratio comes out 1. If `mtime` ran at a fixed 100 MHz while the core
 *   ran at 200 MHz, one wait would advance `mcycle` twice as far as `mtime`
 *   and the ratio would read 2 -- i.e. every k_busy_wait(N) would really last
 *   2N microseconds.
 *
 * The ratio is taken over a 100 ms wait so the 1 kHz tick's own cycles (a few
 * thousand, i.e. <0.1% of 20 M) cannot move it.
 *
 * The two MARK lines bracket 2 s of *declared* time, so a host that timestamps
 * the capture can also check the absolute rate (the port record quotes the
 * board-vs-wall numbers for both bitstreams). One-shot: the sample prints its
 * table and verdict and then idles.
 *
 * The "long" row takes ~21.6 s on purpose: it asks for 100 ms past the 32-bit
 * conversion boundary, which the generic kernel loop wraps around (measured
 * 2026-09-24: served in 100 ms), while this build's arch_busy_wait()
 * (CONFIG_ARCH_HAS_CUSTOM_BUSY_WAIT, soc/agm/agrv2k/busy_wait.c) counts the
 * 64-bit `mcycle` and serves it as asked. Both numbers are in port record 3.31.
 */

#include <zephyr/arch/riscv/csr.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * The machine-mode cycle counter, CSR 0xB00. Not the unprivileged alias
 * 0xC00: this core does not implement it, and reading it is an illegal
 * instruction (measured 2026-09-24: `mtval: c0002973`, i.e. `csrr a0, cycle`).
 */
#define CSR_MCYCLE  0xB00U
#define CSR_MCYCLEH 0xB80U

/* Consistent 64-bit read, the same way the driver does it: two high-word reads
 * have to agree, or the low word may have wrapped between them. */
static uint64_t mcycle64(void)
{
	uint32_t hi, lo, hi_again;

	do {
		hi = (uint32_t)csr_read(CSR_MCYCLEH);
		lo = (uint32_t)csr_read(CSR_MCYCLE);
		hi_again = (uint32_t)csr_read(CSR_MCYCLEH);
	} while (hi != hi_again);

	return ((uint64_t)hi << 32) | lo;
}

#define DECLARED_HZ ((uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC)
/* What the kernel's 32-bit conversion multiplies microseconds by. */
#define CYCLES_PER_US (DECLARED_HZ / 1000000U)

/* The target cycle count k_busy_wait() computes, same ceil as the kernel's
 * k_us_to_cyc_ceil32(): ceil(us * hz / 1000000). */
static uint32_t want_cycles(uint32_t us)
{
	return (uint32_t)(((uint64_t)us * DECLARED_HZ + 999999ULL) / 1000000ULL);
}

/* One wait, reported in the timer's ticks and in the core's own cycles.
 * @a judge decides whether this row's verdict counts towards PASS. */
static bool probe(uint32_t us, bool judge)
{
	uint32_t t0, t1, c0, c1, want = want_cycles(us);
	uint32_t ticks, core;

	c0 = (uint32_t)csr_read(CSR_MCYCLE);
	t0 = k_cycle_get_32();
	k_busy_wait(us);
	t1 = k_cycle_get_32();
	c1 = (uint32_t)csr_read(CSR_MCYCLE);

	ticks = t1 - t0;
	core = c1 - c0;

	/* The wait is a lower bound (interrupts may extend it), so the honest
	 * test is "at least the target, and not wildly more". The fixed term
	 * is one loop iteration plus the call into k_busy_wait (the counter
	 * read and the compare), which dominates a 1 us request -- measured
	 * 2026-09-24 in the warm pass: +97..+125 cycles over a 200-cycle
	 * target. The slack below is deliberately loose enough to also accept
	 * the cold pass, whose first rows carry a few hundred extra cycles;
	 * one_pass() is what decides which pass the verdict uses. */
	{
		uint32_t slack = want / 20U + 1024U;
		bool ok = (ticks >= want) && (ticks <= want + slack);

		printk("  wait %6u us: mtime +%9u (target %9u, %+9d over, %s), "
		       "mcycle +%9u\n", us, ticks, want, (int)(ticks - want),
		       ok ? "ok" : "OFF", core);
		return judge ? ok : true;
	}
}

/*
 * One pass over the table. The sample runs it twice because the *first* waits
 * after boot are dearer than the settled ones -- measured 2026-09-24, the same
 * 1 us row read +685 cycles over in the cold pass and +97 in the warm pass of
 * one boot, and the cold number moved between boots (+433/+685/+780 for the
 * same binary). Whatever warms up
 * (instruction fetch from XIP, the fabric's clock domain), a single cold
 * sample is not a statement about the implementation, so the verdict comes
 * from the warm pass and both are printed.
 */
static bool one_pass(bool judge)
{
	bool ok = true;

	ok &= probe(1U, judge);
	ok &= probe(10U, judge);
	ok &= probe(100U, judge);
	ok &= probe(1000U, judge);
	ok &= probe(10000U, judge);
	ok &= probe(100000U, judge);
	return ok;
}

int main(void)
{
	uint32_t t0, t1, c0, c1;
	bool ok = true;

	printk("\nbusy_wait_timebase: k_busy_wait on agrv2k_407\n");
	printk("  declared: CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC = %u (%u cycle/us)\n",
	       (unsigned int)DECLARED_HZ, (unsigned int)(DECLARED_HZ / 1000000U));
	printk("  timer   : RISCV_MACHINE_TIMER_SYSTEM_CLOCK_DIVIDER = %d\n",
	       CONFIG_RISCV_MACHINE_TIMER_SYSTEM_CLOCK_DIVIDER);

	/* EXPERIMENT: the fixed floor -- k_busy_wait() returns before touching
	 * any counter, so this is the call/syscall path alone. */
	{
		uint32_t z0 = k_cycle_get_32(), z1, c;
		uint64_t m0 = mcycle64();

		k_busy_wait(0U);
		z1 = k_cycle_get_32();
		c = (uint32_t)(mcycle64() - m0);
		printk("  zero    : k_busy_wait(0) = %u mtime / %u mcycle\n", z1 - z0, c);
	}

	/* mtime ticks per requested microsecond -- the constant's own claim. Two
	 * passes: see one_pass() for why the first one is not a judgement. */
	printk("  -- cold pass (the first waits after boot) --\n");
	(void)one_pass(false);
	printk("  -- warm pass (what the verdict uses) --\n");
	ok &= one_pass(true);

	/* Core cycles vs timer ticks over the same 100 ms: 1.000 means mtime
	 * counts at the core's clock, i.e. at SYSCLK. */
	c0 = (uint32_t)csr_read(CSR_MCYCLE);
	t0 = k_cycle_get_32();
	k_busy_wait(100000U);
	t1 = k_cycle_get_32();
	c1 = (uint32_t)csr_read(CSR_MCYCLE);

	{
		uint32_t ticks = t1 - t0;
		uint32_t core = c1 - c0;
		uint32_t milli = (uint32_t)((uint64_t)core * 1000U / ticks);

		printk("  ratio   : mcycle +%u / mtime +%u = %u.%03u  (1.000 = mtime "
		       "runs at SYSCLK)\n", core, ticks, milli / 1000U, milli % 1000U);
	}

	/*
	 * The long wait: 100 ms past the 32-bit conversion boundary (2^32 / 200
	 * cycles-per-us = 21 474 836 us at 200 MHz, 42 949 672 us at 100 MHz).
	 * The generic kernel loop multiplies in 32 bits, so a request past that
	 * boundary used to wrap and be served in ~100 ms; arch_busy_wait()
	 * counts the 64-bit mcycle and serves it as asked. Both measurements
	 * are in port record 3.31 -- and this row is why the sample takes 21.6 s
	 * at 200 MHz (10.8 s at 100).
	 */
	{
		uint32_t asked_us = (uint32_t)(0xFFFFFFFFULL / CYCLES_PER_US) + 100000U;
		uint64_t asked = (uint64_t)asked_us * CYCLES_PER_US;
		uint32_t wrapped = (uint32_t)asked;
		int64_t ms0 = k_uptime_get();
		uint64_t m0 = mcycle64();
		uint32_t served_ms;

		k_busy_wait(asked_us);
		served_ms = (uint32_t)(k_uptime_get() - ms0);

		printk("  long    : asked %u us (%.1f s, %llu cycles; a 32-bit target "
		       "would wrap to %u = %.0f ms)\n", asked_us,
		       (double)asked_us / 1000000.0, (unsigned long long)asked, wrapped,
		       (double)wrapped / (double)CYCLES_PER_US / 1000.0);
		printk("            served %u ms of uptime, %llu mcycle\n", served_ms,
		       (unsigned long long)(mcycle64() - m0));
		/* "At least": the tick granularity is 1 ms, so allow that much. */
		ok &= (served_ms + 1U) >= (asked_us / 1000U);
	}

	/* Absolute-time anchor for the host: 2 s of declared time between the
	 * two marks (a mismatched bitstream/DT pair shows up here as 4 s). */
	printk("busy_wait_timebase: MARK-A\n");
	k_busy_wait(2000000U);
	printk("busy_wait_timebase: MARK-B\n");

	printk("busy_wait_timebase: %s\n", ok ? "PASS" : "FAIL (a row is off)");
	return 0;
}
