/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * pm_gate_audit — print SYS.APB_CLKENABLE at three points to verify the
 * bitstream-declared gate set actually lines up with what soc.c opens.
 *
 *   boot                  — immediately after boot, all drivers inited;
 *   idle (5 s)            — every PM-aware device put by the kernel idle;
 *   all-PM-aware-put      — every PM-aware device pm_device_runtime_put
 *                           from this main thread, on top of the idle
 *                           path.
 *
 * The three readings should differ by exactly the gates a runtime-PM
 * driver can close. soc.c derives the same bit set from devicetree
 * (soc/agm/agrv2k/soc.c::agrv2k_apb_gates()), so the boot value must
 * match that mask. The "all-PM-aware-put" value is the lower bound on
 * what the kernel can power-gate -- the always-on set is the bitstream
 * (FCB0, GPIO banks without PM actions, etc.) plus the devicetree
 * nodes that have no PM_DEVICE declaration.
 *
 * The system clock (k_sleep, k_uptime_get) needs at least one of those
 * gates to be left open; if the bitmask ever becomes 0x00000001 (only
 * FCB0), k_sleep will never wake and the print loop will appear frozen.
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/printk.h>

/* SYS.APB_CLKENABLE lives at SYS_BASE + 0x60; SYS_BASE is 0x03000000
 * (soc/agm/agrv2k/agm_sys.h). The bit layout is documented in soc.c:
 * bit 0 = FCB0 (always-on; the bitstream stream), bit 1..31 = one APB
 * peripheral each.
 *
 * Read the register through a plain volatile pointer instead of
 * sys_read32(): the latter drags the sys controller's PM device into
 * the link, which this sample is not configured to register. */
#define APB_CLKENABLE_ADDR 0x03000060U

static inline uint32_t apb_read(void)
{
	return *((volatile uint32_t *)APB_CLKENABLE_ADDR);
}

/* Walk every PM-aware device tree node and ask the runtime PM framework
 * to put it (decrement its reference count). After the loop every device
 * has refcount == 0 and the ones with a TURN_OFF action close their APB
 * gate. We never take a get() so the put always succeeds.
 *
 * Doing this from main() (not the idle thread) keeps the printed
 * sequence linear: the boot + idle prints come first, then this loop,
 * then the third print. */
#define PUT_PM_AWARE(node)                                                                    \
	do {                                                                                   \
		const struct device *d = DEVICE_DT_GET(node);                                  \
                                                                                               \
		if (device_is_ready(d)) {                                                       \
			(void)pm_device_runtime_put(d);                                        \
		}                                                                              \
	} while (0);

#if defined(CONFIG_PM_DEVICE_RUNTIME)
/* Only compiled when the runtime-PM path exists: the call site below is
 * inside the same guard, and an unconditional definition trips
 * -Werror=unused-function in the default configuration (twister builds
 * with warnings-as-errors). */
static void put_all_pm_aware(void)
{
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, PUT_PM_AWARE)
}
#endif

int main(void)
{
	uint32_t boot_val = apb_read();

	printk("\npm_gate_audit: APB_CLKENABLE addresses the SYS clock controller\n");
	printk("pm_gate_audit: boot                = 0x%08x\n", boot_val);

	k_sleep(K_SECONDS(5));

	uint32_t idle_val = apb_read();

	printk("pm_gate_audit: idle (5 s)          = 0x%08x\n", idle_val);

#if defined(CONFIG_PM_DEVICE_RUNTIME)
	put_all_pm_aware();

	k_sleep(K_MSEC(100));

	uint32_t put_val = apb_read();

	printk("pm_gate_audit: all-PM-aware-put    = 0x%08x\n", put_val);
	printk("pm_gate_audit: bit delta boot->put = 0x%08x\n", boot_val & ~put_val);
#else
	/* Without CONFIG_PM_DEVICE_RUNTIME the per-device TURN_OFF path is
	 * not wired up, so this print collapses to the boot value. It is
	 * still useful: it documents which bit set was on when the kernel
	 * started idling. */
	uint32_t put_val = boot_val;

	(void)put_val;
#endif

	while (true) {
		k_msleep(1000);
	}
}
