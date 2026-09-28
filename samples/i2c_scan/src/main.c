/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c_scan — smoke test for the AgRV2K I2C0 controller driver.
 *
 * Scans the standard 7-bit address range and reports what ACKs.
 * The reference wiring is a BH1750 (GY-302) on PIN_35/36, which both
 * supplies the bus pull-ups and answers at 0x23.
 *
 * NOTE (measured 2026-09-10): with NOTHING attached every probe
 * returns -ETIMEDOUT, not -ENXIO -- the pins have no pull-ups, SCL is
 * never released high and the core's TIP bit never clears. So an
 * all-(-ETIMEDOUT) scan means "no slave / no pull-ups", not a broken
 * controller; -ENXIO is only reachable on a pulled-up bus.
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#define I2C0 DT_NODELABEL(i2c0)

int main(void)
{
	const struct device *i2c = DEVICE_DT_GET(I2C0);
	uint8_t dummy = 0;
	uint32_t found = 0;
	uint32_t errors = 0;

	printk("i2c_scan: I2C0 = %s\n", i2c->name);

	if (!device_is_ready(i2c)) {
		printk("i2c_scan: I2C0 not ready\n");
		return 0;
	}

	printk("i2c_scan: scanning 0x08..0x77...\n");
	for (uint16_t addr = 0x08U; addr <= 0x77U; addr++) {
		struct i2c_msg msg = {
			.buf = &dummy,
			.len = 1U,
			.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
		};
		int rc = i2c_transfer(i2c, &msg, 1U, addr);

		if (rc == 0) {
			printk("i2c_scan:   found device at 0x%02x\n", addr);
			found++;
		} else if (rc != -ENXIO) {
			/* -ENXIO == address NACK (needs a pulled-up bus);
			 * anything else is a real error. */
			printk("i2c_scan:   0x%02x -> err %d\n", addr, rc);
			errors++;
		}
	}
	printk("i2c_scan: done: %u found, %u unexpected errors\n",
	       found, errors);
	if (errors == 0U) {
		printk("i2c_scan: PASS (controller alive; empty bus as expected)\n");
	} else {
		printk("i2c_scan: FAIL (check BH1750 / pull-ups on PIN_35/36)\n");
	}

	while (1) {
		k_msleep(1000);
	}
}
