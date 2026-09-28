/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Busy-path tick regression demo (`the development notes (not published here)`).
 *
 * Scenario:
 *   main()          prio 0  prints uptime once per second (k_msleep(1000))
 *   worker thread   prio 5  busy-spins forever (never sleeps or yields)
 *
 * Scheduling: main prints, sleeps, and the worker is the only ready
 * thread — the kernel NEVER runs the idle thread between main's
 * sleeps. The worker is lower priority, so when main's 1 s deadline
 * fires it must be able to preempt the worker.
 *
 * Phase 3.4/3.12 polling timer (removed in Phase 3.15) announced
 * ticks only from arch_cpu_idle(). With the worker always ready the
 * idle path never ran, so main's deadline was never serviced: the
 * console showed exactly one line and then froze (uptime, timeouts
 * and time slicing all stalled).
 *
 * Phase 3.15 uses the upstream riscv_machine_timer (MTIP-driven,
 * tickless). sys_clock_set_timeout() arms mtimecmp and the MTIP ISR
 * announces at the deadline no matter which thread is running, so
 * main wakes every second and preempts the worker.
 *
 * Pass criteria on hardware: one console line per second for N
 * seconds, uptime tracking wall time, worker_loops growing between
 * lines, and no console garbage (MTIP preemption must not corrupt
 * the PL011 poll-out path).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define WORKER_PRIO 5
#define WORKER_STACK_SIZE 1024

/* ISR/thread-visible state (readable via openocd if CDC is down). */
volatile uint32_t worker_loops;
volatile uint32_t main_wakeups;
volatile uint32_t last_uptime_ms;

static K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);
static struct k_thread worker_thread;

static void worker_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		worker_loops++;
	}
}

int main(void)
{
	printk("\n");
	printk("=========================================\n");
	printk(" AgRV2K busy-path tick demo (Phase 3.15)\n");
	printk(" Board : %s\n", CONFIG_BOARD);
	printk(" main sleeps 1s/loop; worker busy-spins at\n");
	printk(" prio %d. Expect one line per second.\n", WORKER_PRIO);
	printk(" (pre-fix polling timer: freezes after line 1)\n");
	printk("=========================================\n");

	k_thread_create(&worker_thread, worker_stack,
			WORKER_STACK_SIZE,
			worker_entry, NULL, NULL, NULL,
			WORKER_PRIO, 0, K_NO_WAIT);

	while (1) {
		uint32_t up = (uint32_t)k_uptime_get();

		main_wakeups++;
		last_uptime_ms = up;

		printk("[wakeup %u] uptime=%u ms  worker_loops=%u\n",
		       (unsigned)main_wakeups, (unsigned)up,
		       (unsigned)worker_loops);
		k_msleep(1000);
	}

	return 0;
}
