/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO bank-interrupt end-to-end demo (`the development notes (not published here)`).
 *
 * Path under test:
 *   GPIO6 pin edge (button/switch)
 *     -> Stellaris GPIO MIS latch (IS=0/IBE=1 set by the vanilla
 *        ti,stellaris-gpio driver's pin_interrupt_configure; IE bit
 *        re-asserted by this sample -- see AgRV quirk note below)
 *     -> PLIC line 13 asserts MEIP (mie.MEIE = 1, set by intc_plic)
 *     -> trap: _isr_wrapper -> sw_isr_table[11] -> plic_irq_handler
 *        claims line 13 -> sw_isr_table[32+13=45]
 *     -> gpio_stellaris_isr -> gpio_fire_callbacks -> handlers below
 *
 * The interrupt path is 100% generic Zephyr: gpio6 is a plain
 * "ti,stellaris-gpio" DT node (dts/riscv/agm/agrv2k.dtsi) with
 * status="okay" in boards/agm/agrv2k_407/agrv2k_407.dts, and the
 * driver's IRQ_CONNECT uses DT_INST_IRQN (multi-level aware, unlike
 * the pl011 driver which needed the DT_IRQN_BY_IDX fix in
 * commit 07bb3bd). The LED blink uses the gpio-leds nodes on gpio4
 * (now a regular ti,stellaris-gpio DT device); the only remaining raw
 * GPIO MMIO is the AgRV IE re-assert below, which exists because the
 * vanilla driver cannot express this silicon's IE polarity.
 *
 * Wiring (2026-09-10): the dev board board has a single user key on PIN_5
 * which the bitstream assigns to SPI0_CSN. To exercise the ISR path
 * without external pulse generators, this build drives PIN_24
 * (GPIO6_4) from the firmware and the user wires PIN_24 <-> PIN_23
 * (GPIO6_2) with a jumper. The main loop toggles PIN_24 at 1 Hz to
 * inject edges into PIN_23; the ISR fires on every edge and prints
 * the count.
 *
 * Pins under test (board.ve):
 *   GPIO6_2 PIN_23  INPUT + edge IRQ
 *   GPIO6_4 PIN_24  OUTPUT, toggled by main loop (jumper to PIN_23)
 *
 * AgRV silicon quirk worked around below (NOT a zephyr change):
 * GPIO reg 0x410 is GpioIE in the SDK (gpio.h: GPIO_EnableInt = IE |=
 * bits, i.e. write 1 = ENABLE), but the vanilla ti,stellaris-gpio
 * driver names it IM (interrupt MASK) and clears it when enabling --
 * on AgRV that DISABLES the interrupt. The driver still sets IS/IBE/
 * IEV correctly, so after gpio_pin_interrupt_configure() we re-assert
 * IE. Verified on hardware: with IE=0 no event ever fires (PLIC
 * ENABLE bit13 set, but GPIO output never asserts); with IE bits set,
 * 26 events from PIN_23 reached the callback in 13 s (1 edge/500 ms,
 * 100% capture). SDK also confirms GPIO6_IRQn = 13 (AltaRiscv.h).
 * AgRV has no DEN reg (0x51C writes are no-ops) and the input path
 * needs no digital enable.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#define GPIO6_DEV DEVICE_DT_GET(DT_NODELABEL(gpio6))

/* GPIO6 register map (SDK framework-agrv_sdk/src/gpio.h). */
#define GPIO6_BASE     0x4001A000UL
#define GPIO_IE_OFF    0x410U   /* GpioIE: write 1 = enable (AgRV) */

#define PIN_INPUT   2U   /* GPIO6_2 / PIN_23 — INPUT + edge IRQ */
#define PIN_OUTPUT  4U   /* GPIO6_4 / PIN_24 — OUTPUT, driven by main loop */

volatile uint32_t isr_seq;
volatile uint32_t evt_count;
volatile uint32_t last_level;

static struct gpio_callback pin_cb;

static void pin_isr(const struct device *dev, struct gpio_callback *cb,
		    gpio_port_pins_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	isr_seq++;
	evt_count++;
	last_level = (uint32_t)gpio_pin_get_raw(GPIO6_DEV, PIN_INPUT);
}

static int setup_input_pin(const struct device *dev)
{
	int ret;

	ret = gpio_pin_configure(dev, PIN_INPUT, GPIO_INPUT);
	if (ret != 0) {
		printk("cfg INPUT pin %u: %d\n", PIN_INPUT, ret);
		return ret;
	}
	gpio_init_callback(&pin_cb, pin_isr, BIT(PIN_INPUT));
	ret = gpio_add_callback(dev, &pin_cb);
	if (ret != 0) {
		printk("add cb: %d\n", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure(dev, PIN_INPUT, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		printk("int cfg: %d\n", ret);
		return ret;
	}
	/* AgRV quirk: re-assert GpioIE (write 1 = enable on AgRV). */
	*((volatile uint32_t *)(GPIO6_BASE + GPIO_IE_OFF)) |= BIT(PIN_INPUT);
	return 0;
}

static int setup_output_pin(const struct device *dev)
{
	int ret;

	ret = gpio_pin_configure(dev, PIN_OUTPUT, GPIO_OUTPUT);
	if (ret != 0) {
		printk("cfg OUTPUT pin %u: %d\n", PIN_OUTPUT, ret);
		return ret;
	}
	gpio_pin_set(dev, PIN_OUTPUT, 0);
	return 0;
}

int main(void)
{
	int ret;

	ret = setup_output_pin(GPIO6_DEV);
	if (ret != 0) {
		return ret;
	}
	ret = setup_input_pin(GPIO6_DEV);
	if (ret != 0) {
		return ret;
	}

	printk("\n=========================================\n");
	printk(" AgRV2K GPIO6 PLIC IRQ demo (self-driven)\n");
	printk(" Wire PIN_23 <-> PIN_24 with a jumper.\n");
	printk(" PIN_24 toggles at 1 Hz to inject edges into PIN_23.\n");
	printk(" Expect ~1 EVT per 500 ms once warmed up.\n");
	printk("=========================================\n\n");

	bool out_level = false;
	while (1) {
		/* Toggle output -> edge on PIN_23 via jumper */
		out_level = !out_level;
		gpio_pin_set(GPIO6_DEV, PIN_OUTPUT, out_level ? 1 : 0);
		k_msleep(500);

		int lvl = gpio_pin_get_raw(GPIO6_DEV, PIN_INPUT);
		printk("[t=%u ms] isr_seq=%u evt=%u last_level=%u input_raw=%d\n",
		       (unsigned)k_uptime_get_32(),
		       (unsigned)isr_seq,
		       (unsigned)evt_count,
		       (unsigned)last_level,
		       lvl);
	}

	return 0;
}
