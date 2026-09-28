/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * PLIC end-to-end external-interrupt demo (`the development notes (not published here)`).
 *
 * Path under test:
 *   uart0 RX byte (PL011 @0x40025000)
 *     -> PLIC line 24 asserts MEIP (mie.MEIE = 1, set by intc_plic)
 *     -> trap: _isr_wrapper -> __soc_handle_irq(11) -> sw_isr_table[11]
 *     -> plic_irq_handler claims line 24
 *     -> sw_isr_table[24] pl011_isr -> uart_irq_callback_set() callback
 *
 * Before P0-1 (commit c853c1a1d0c) soc.c wiped `mie` after the PLIC
 * driver's PRE_KERNEL_1 irq_enable(11), so MEIP was never set and this
 * whole path was inert. This sample is the first real-ISR exercise of
 * the dispatch.
 *
 * Host verification:
 *   send bytes on /dev/ttyACM0 @115200 8N1 -> each byte is echoed and
 *   counted; LED1-3 (gpio-leds led0..led2) toggle per byte; LED4
 *   steady. `irq_count` / `rx_count` live in SRAM for openocd readback
 *   when the CDC bridge is unavailable.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#define UART_DEV DEVICE_DT_GET(DT_NODELABEL(uart0))

/* On-board LEDs (gpio4 pins 1..4, active-low — board dts gpio-leds). */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);

static const struct gpio_dt_spec *rx_leds[] = { &led0, &led1, &led2 };

static void leds_init(void)
{
	const struct gpio_dt_spec *all[] = { &led0, &led1, &led2, &led3 };

	/* Same AgRV GPIO quirk as hello_world (docs §3.5): GPIO_OUTPUT_ACTIVE
	 * would have its INIT_* level dropped, so configure plain GPIO_OUTPUT
	 * first, then drive the levels explicitly. */
	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		if (gpio_pin_configure_dt(all[i], GPIO_OUTPUT) != 0) {
			printk("led%u configure failed\n", (unsigned)i);
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		/* LED1-3 start off and toggle per byte; led3/LED4 is the
		 * steady power indicator. */
		gpio_pin_set_dt(all[i], i == 3 ? 1 : 0);
	}
}

static void led_ext_toggle(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(rx_leds); i++) {
		gpio_pin_toggle_dt(rx_leds[i]);
	}
}

/* --- ISR-visible state (symbols readable via openocd) ------------- */
volatile uint32_t irq_count;   /* PLIC ISR invocations (pl011_isr callbacks) */
volatile uint32_t rx_count;    /* bytes pulled from the RX FIFO */
volatile uint32_t rx_ring[64];
volatile uint32_t rx_head;

static void uart_rx_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_rx_ready(dev)) {
		unsigned char c;

		if (uart_fifo_read(dev, &c, 1) == 1) {
			irq_count++;
			rx_count++;
			rx_ring[rx_head++ & 63U] = c;
			led_ext_toggle(); /* visible per-byte blink */
		}
	}
}

int main(void)
{
	uint32_t last = 0;

	leds_init();

	printk("\n");
	printk("=========================================\n");
	printk(" AgRV2K PLIC UART RX IRQ demo\n");
	printk(" Board : %s\n", CONFIG_BOARD);
	printk(" Path  : uart0 RX -> PLIC line 24 -> plic_irq_handler\n");
	printk("         -> sw_isr_table[56] -> agm_uart_isr -> callback\n");
	printk(" Send chars on the console; each is echoed + counted.\n");
	printk("=========================================\n");
	printk("\n");

	uart_irq_callback_set(UART_DEV, uart_rx_cb);
	uart_irq_rx_enable(UART_DEV);

	printk("RX IRQ enabled. waiting...\n");

	while (1) {
		while (last != rx_head) {
			unsigned char c = (unsigned char)rx_ring[last++ & 63U];

			uart_poll_out(UART_DEV, c); /* echo */
			printk("[irq #%u] ch='%c' (0x%02x) total=%u\n",
			       (unsigned)irq_count, c, c, (unsigned)rx_count);
		}
		k_msleep(100);
	}

	return 0;
}
