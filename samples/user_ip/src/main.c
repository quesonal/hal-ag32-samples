/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * user_ip — reach a piece of user logic in the fabric from Zephyr.
 *
 * The split this sample demonstrates (docs/CUSTOM-IP.md §1.1 and §2.1):
 *
 *   devicetree: the *window* (agm,agrv2k-cpld -- an SoC address range the
 *               driver owns and range-checks), and, when there is one, the pin
 *               the fabric drives as an interrupt;
 *   macros:     the register map and the block's offset/size inside that
 *               window (src/user_ip_regs.h), because those belong to the
 *               bitstream and to the RTL rather than to the board: the same
 *               board with another bitstream has different registers, and a
 *               header is something the RTL can share or generate.
 *
 * What still has to be enforced is that the description *fits*: the block
 * inside the window (BUILD_ASSERT against DT_REG_SIZE(cpld0)) and every
 * register inside the block. On this bus a mismatch stalls the CPU instead of
 * faulting, so it has to fail the build.
 *
 * If the design ever grows several instances of the block, or a driver that
 * wants to bind and be enumerated (DEVICE_DT_GET / device_is_ready), that is
 * when a devicetree node for the *instance* starts paying for itself -- add
 * the binding then and keep the register map here.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "user_ip_regs.h"

#define WINDOW_NODE DT_NODELABEL(cpld0)

#if !DT_NODE_HAS_STATUS(WINDOW_NODE, okay)
#error "cpld0 is disabled: build with a board overlay that enables the window"
#endif

#define WINDOW_SIZE DT_REG_SIZE(WINDOW_NODE)

/* What can be checked without the RTL, checked where a mistake is cheap. */
BUILD_ASSERT(USER_IP_OFFSET % 4U == 0U,
	     "the block has to start on a 32-bit register boundary");
BUILD_ASSERT(USER_IP_SIZE % 4U == 0U,
	     "the block has to be a whole number of 32-bit registers");
BUILD_ASSERT(USER_IP_OFFSET + USER_IP_SIZE <= WINDOW_SIZE,
	     "the user IP block does not fit the fabric window: widen the "
	     "window's reg, or move the block in the RTL and in user_ip_regs.h "
	     "-- an access outside the window stalls the CPU");
BUILD_ASSERT(USER_IP_REG_OK(USER_IP_REG_ID) &&
		     USER_IP_REG_OK(USER_IP_REG_CTRL) &&
		     USER_IP_REG_OK(USER_IP_REG_STATUS) &&
		     USER_IP_REG_OK(USER_IP_REG_COUNTER),
	     "a register in user_ip_regs.h is past the end of the block");

#if defined(CONFIG_APP_USER_IP_TOUCH_BUS)
/* The application's half of the guard: the driver checks the *window*, this
 * checks the *block*, so a bad offset never reaches the bus at all. */
static int user_ip_read(const struct device *window, uint32_t reg, uint32_t *val)
{
	if (!USER_IP_REG_OK(reg)) {
		printk("user_ip: +0x%02x is outside the block (size 0x%x)\n", reg,
		       USER_IP_SIZE);
		return -EINVAL;
	}

	return agm_cpld_read32(window, USER_IP_OFFSET + reg, val);
}
#endif

int main(void)
{
	const struct device *window = DEVICE_DT_GET(WINDOW_NODE);
	uintptr_t base = 0U;
	size_t size = 0U;

	printk("=== user_ip: user logic in the fabric window ===\n");
	printk("window   : %s @0x%08x+0x%x\n", window->name,
	       (uint32_t)DT_REG_ADDR(WINDOW_NODE), WINDOW_SIZE);
	printk("block    : window+0x%x, 0x%x bytes, %u register(s)\n",
	       USER_IP_OFFSET, USER_IP_SIZE, USER_IP_REG_COUNT);
	printk("  +0x%02x  ID      (expect 0x%08x)\n", USER_IP_REG_ID,
	       (uint32_t)USER_IP_ID_VALUE);
	printk("  +0x%02x  CTRL    (ENABLE|IRQEN|CLR)\n", USER_IP_REG_CTRL);
	printk("  +0x%02x  STATUS  (READY|IRQ)\n", USER_IP_REG_STATUS);
	printk("  +0x%02x  COUNTER\n", USER_IP_REG_COUNTER);

	if (!device_is_ready(window)) {
		printk("user_ip: RESULT: FAIL -- the window driver is not ready\n");
		return 0;
	}

	if (agm_cpld_get_base(window, &base) == 0 &&
	    agm_cpld_get_size(window, &size) == 0) {
		printk("driver   : base 0x%08x size 0x%x\n", (uint32_t)base,
		       (uint32_t)size);
	}

#if defined(CONFIG_APP_USER_IP_TOUCH_BUS)
	/* Only on a board whose bitstream really decodes this block: the bus
	 * has no timeout, so an unmapped offset hangs rather than faulting. */
	uint32_t id = 0U;

	if (user_ip_read(window, USER_IP_REG_ID, &id) != 0) {
		printk("user_ip: RESULT: FAIL -- reading the ID register\n");
		return 0;
	}
	printk("read     : ID = 0x%08x%s\n", id,
	       (id == (uint32_t)USER_IP_ID_VALUE) ? "" : "  (unexpected)");

	if (user_ip_read(window, USER_IP_SIZE, &id) == 0) {
		printk("user_ip: RESULT: FAIL -- the block guard let a read past "
		       "the block through\n");
		return 0;
	}
#else
	printk("bus      : not touched (CONFIG_APP_USER_IP_TOUCH_BUS=n); the "
	       "declaration above is the whole test\n");
#endif

	printk("user_ip: RESULT: OK -- window+0x%x, block 0x%x\n", USER_IP_OFFSET,
	       USER_IP_SIZE);
	return 0;
}
