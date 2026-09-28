/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * board_info — print what this firmware was actually built against.
 *
 * Written for the failure mode that cost this port the most time: the
 * devicetree and the bitstream disagreeing (a 100 MHz bitstream with a
 * 200 MHz board default shows up as a garbled console), or a peripheral
 * being compiled in while nothing is routed to it. Every number below is
 * read from the devicetree the build used, so it answers "which DT am I
 * running" without an openocd session.
 *
 * The marker is meant to be edited by hand: bump it, rebuild, and if the
 * console still shows the old one, the image on the board is not the one
 * you just built.
 *
 * Read-only: no flash, no buses, no pin changes.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* Edit this to prove a freshly built image reached the board. */
#define BOARD_INFO_MARKER "base"

#define CLK0_HZ       DT_PROP(DT_NODELABEL(clk0), clock_frequency)
#define CPU0_HZ       DT_PROP(DT_NODELABEL(cpu0), clock_frequency)
#define FLASH_CEIL_HZ DT_PROP(DT_NODELABEL(sys), flash_max_frequency)

/* "0 + 1 + 1 …" so an empty node list still yields a valid initialiser. */
#define COUNT_NODE(n) +1
static const unsigned uart_nodes = 0 DT_FOREACH_STATUS_OKAY(agm_agrv2k_uart, COUNT_NODE);
static const unsigned spi_nodes = 0 DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, COUNT_NODE);
static const unsigned i2c_nodes = 0 DT_FOREACH_STATUS_OKAY(agm_agrv2k_i2c, COUNT_NODE);
static const unsigned can_nodes = 0 DT_FOREACH_STATUS_OKAY(agm_agrv2k_can, COUNT_NODE);

int main(void)
{
	printk("\nboard_info: what this image was built against\n");
	printk("board_info: board      = %s\n", CONFIG_BOARD_TARGET);
	printk("board_info: built      = %s %s\n", __DATE__, __TIME__);
	printk("board_info: marker     = %s\n", BOARD_INFO_MARKER);
	printk("board_info: SYSCLK     = %u Hz (clk0, from DT)\n", (unsigned int)CLK0_HZ);
	printk("board_info: cpu0       = %u Hz (from DT)\n", (unsigned int)CPU0_HZ);
	printk("board_info: flash ceil = %u Hz (sys, from DT)\n",
	       (unsigned int)FLASH_CEIL_HZ);
	printk("board_info: enabled AGM nodes: uart %u, spi %u, i2c %u, can %u\n",
	       uart_nodes, spi_nodes, i2c_nodes, can_nodes);
	printk("board_info: SYSCLK must match the flashed bitstream "
	       "(see the sample workflow)\n");

	while (true) {
		k_msleep(1000);
	}
}
