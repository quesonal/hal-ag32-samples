/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Payload for samples/spi_boot_loader.
 *
 * This image is built to run from SRAM (CONFIG_XIP=n, see prj.conf and
 * boards/agrv2k_407.overlay): the loader reads it out of the on-board SPI
 * NOR, copies it to its link address and jumps into it. The banner below is
 * what tells us on the console that the jump worked -- the loader prints
 * its own banner first.
 *
 * It also answers the loader's trial-boot handshake: the banner is this
 * image's self-test, so right after it the image says "this one works" with
 * agm_boot_trial_confirm(). That is what turns the trial slot into a
 * confirmed one on the next boot, and it is what stops the watchdog the
 * loader armed -- an image that never calls it (or hangs before it) gets
 * reset and, after three attempts, rolled back (see
 * CONFIG_BOOT_AGM_TRIAL_WATCHDOG).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	uint32_t tick = 0;
	int ret;

	printk("\n");
	printk("=========================================\n");
	printk(" spi_boot_app: running\n");
	printk(" main    : %p\n", (void *)main);
	printk(" This line only appears if spi_boot_loader\n");
	printk(" loaded this image -- from the external SPI NOR\n");
	printk(" into RAM, or into the on-die slot.\n");
	printk("=========================================\n");

	/* Self-test passed: answer the handshake. */
	ret = agm_boot_trial_confirm();
	if (ret == 0) {
		printk(" confirmed: this slot becomes permanent on the next boot\n");
	} else if (ret == -ENOENT) {
		printk(" not a trial boot - nothing to confirm\n");
	} else {
		printk(" confirm failed (%d) - the loader will roll back after "
		       "its attempts run out\n", ret);
	}

	while (1) {
		printk("[ram-app %u] uptime=%u ms\n", tick++,
		       (uint32_t)k_uptime_get());
		k_msleep(1000);
	}

	return 0;
}
