/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * hello_world — AgRV2K 407 UART0 console smoke test.
 *
 * Sequence:
 *   1. Light the 4 on-board LEDs (active-low sink driver, GPIO4;
 *      level driven explicitly after GPIO_OUTPUT — see leds_on())
 *   2. Print a banner once, then heartbeat every 500 ms.
 *
 * The LEDs are plain Zephyr `gpio-leds` DT nodes (boards/agm/agrv2k_407/
 * agrv2k_407.dts: led0..led3 -> gpio4 pins 1..4, GPIO_ACTIVE_LOW), so
 * this sample uses only the generic GPIO API — no raw MMIO.
 *
 * UART setup is entirely DTS-driven (no manual PL011 register pokes):
 *   * AFSEL for GPIO6 bit 1 (RX) + GPIO7 bit 6 (TX) is set at PRE_KERNEL_1
 *     by soc.c::uart_route_pins_to_af(). See
 *     zephyrhal-ag32/the port status §3.10 and
 *     memory/agmv2k-uart-afsel-missing.md. Without this, printk emits
 *     no bytes because the pins stay in software-GPIO mode.
 *   * The PL011 baud divisor is computed by the native
 *     drivers/serial/uart_agm.c driver from uart0::clock-frequency +
 *     uart0::current-speed (both set in
 *     boards/agm/agrv2k_407/agrv2k_407.dts). At 200 MHz /
 *     115200 this gives IBRD=108, FBRD=32 (115207 Hz actual,
 *     0.006% error).
 *   * The driver also sets 8N1 from dts (parity, stop-bits,
 *     data-bits), so no manual LCRH poke is needed here either.
 *
 * Board: agrv2k_407 (SYSCLK 200 MHz from FCB; BITSTREAM_ADDR 0x800e7000).
 */

#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/* The four on-board LEDs (gpio4 pins 1..4, active-low). led0..led2
 * heartbeat together; led3 stays on as a power indicator. */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);

static const struct gpio_dt_spec *hb_leds[] = { &led0, &led1, &led2 };

static int leds_on(void)
{
	const struct gpio_dt_spec *leds[] = { &led0, &led1, &led2, &led3 };

	/* Drive the level AFTER GPIO_OUTPUT, never via the GPIO_OUTPUT_ACTIVE/
	 * GPIO_OUTPUT_INACTIVE configure flags: the vanilla ti,stellaris-gpio
	 * driver writes the INIT_* level to the GPIO DATA register *before*
	 * setting DIR=output, and the AgRV2K GPIO only latches DATA writes on
	 * output-enabled pins — the configured initial level is dropped (on
	 * this board led3/LED4 stayed dark). Zephyr's GPIO_ACTIVE_LOW
	 * inversion (from the gpio-leds DT nodes) still applies, so
	 * gpio_pin_set_dt(led, 1) = LED on. See the port status
	 * §3.5 "AgRV2K GPIO quirk" note. */
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		int ret = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT);

		if (ret != 0) {
			printk("led%u configure failed: %d\n",
			       (unsigned)i, ret);
			return ret;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		int ret = gpio_pin_set_dt(leds[i], 1);

		if (ret != 0) {
			printk("led%u set failed: %d\n", (unsigned)i, ret);
			return ret;
		}
	}
	return 0;
}

static void led_hb_toggle(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(hb_leds); i++) {
		gpio_pin_toggle_dt(hb_leds[i]);
	}
}

/* Physical levels of the heartbeat LEDs (bit N = gpio4 pin N), the
 * same 0x0e (all off) / 0x00 (all on) readback the raw-MMIO version
 * reported — lets host-side checks keep watching the DATA register. */
static uint32_t led_hb_raw(void)
{
	uint32_t raw = 0U;

	for (size_t i = 0; i < ARRAY_SIZE(hb_leds); i++) {
		if (gpio_pin_get_raw(hb_leds[i]->port, hb_leds[i]->pin)) {
			raw |= BIT(i + 1);
		}
	}
	return raw;
}

int main(void)
{
	if (leds_on() != 0) {
		printk("hello_world: LED init failed\n");
		return 0;
	}

	/* Banner — should appear on the CDC-ACM bridge (/dev/ttyACM0)
	 * at 115200 8N1. The host reads correctly because:
	 *   * soc.c has routed GPIO6/7 to UART0 RX/TX via AFSEL.
	 *   * drivers/serial/uart_agm.c has set 8N1 + IBRD=108/FBRD=32
	 *     from DTS (115207 Hz actual, well within UART tolerance).
	 *   * clk0::clock-frequency matches the bitstream's actual SYSCLK.
	 *     The line below prints it (and cpu0) from the devicetree
	 *     instead of a literal: a board overlay that sets &clk0 -- the
	 *     vendor bitstream examples are 100 MHz, the 407's own is
	 *     200 MHz -- has to show up here, or the number misleads
	 *     whoever reads the console next.
	 */
	printk("\n");
	printk("=========================================\n");
	printk(" AgRV2K Zephyr hello_world\n");
	printk(" Board : %s\n", CONFIG_BOARD);
	printk(" SYSCLK: %u MHz (clk0), cpu0 %u MHz\n",
	       (unsigned int)(DT_PROP(DT_NODELABEL(clk0), clock_frequency) / 1000000U),
	       (unsigned int)(DT_PROP(DT_NODELABEL(cpu0), clock_frequency) / 1000000U));
	printk(" UART0 : 115200 8N1  (DTS-driven, no MMIO poke)\n");
	printk(" LEDs  : gpio-leds (gpio4_1..4, GPIO_ACTIVE_LOW)\n");
	printk("=========================================\n");
	printk("\n");

	uint32_t tick = 0;
	while (1) {
		/* If you ever see "[....." with no closing bracket, the
		 * UART AFSEL trap has fired — see §3.10. */
		printk("[%07u] tick=%u led=", (unsigned)tick, (unsigned)tick);
		led_hb_toggle();
		/* Read back the physical LED levels so the host can see
		 * the current GPIO state without scope access. */
		printk("0x%x\n", led_hb_raw());
		tick++;
		k_msleep(500);
	}
	return 0;
}
