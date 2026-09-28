/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * can_rxmon - AgRV2K CAN RX-only monitor.
 *
 * Diagnostic sample: puts CAN0 in NORMAL mode with an accept-all filter
 * and only listens. A peer CAN 2.0B node (125 kbit/s) should be driving
 * the bus; if the AgRV CAN0_RX0 input / transceiver wiring / bitstream
 * pin bonding is healthy, frames appear and RXERR drops to 0.
 *
 * No TX is ever requested, so this cannot hang on missing ACK.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define CAN0_BASE          0x4002a000UL
#define CAN0_SR_OFF        0x008UL
#define CAN0_IR_OFF        0x00cUL
#define CAN0_IER_OFF       0x010UL
#define CAN0_RXERR_OFF     0x038UL   /* reg 14 */
#define CAN0_TXERR_OFF     0x03cUL   /* reg 15 */

static volatile uint32_t rx_count;
static volatile uint32_t last_id;
static volatile uint8_t  last_dlc;
static volatile bool     any_frame;

static void on_rx(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	last_id = frame->id;
	last_dlc = frame->dlc;
	any_frame = true;
	rx_count++;
}

static void hb_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint32_t sr  = sys_read32(CAN0_BASE + CAN0_SR_OFF) & 0xFFU;
		uint32_t ir  = sys_read32(CAN0_BASE + CAN0_IR_OFF) & 0xFFU;
		uint32_t rxe = sys_read32(CAN0_BASE + CAN0_RXERR_OFF) & 0xFFU;
		uint32_t txe = sys_read32(CAN0_BASE + CAN0_TXERR_OFF) & 0xFFU;

		printk("rxmon t=%lld rx=%u%s SR=0x%02x IR=0x%02x RXERR=%u TXERR=%u\n",
		       k_uptime_get(), rx_count,
		       any_frame ? " FRAME!" : "", sr, ir, rxe, txe);
		k_sleep(K_MSEC(500));
	}
}

K_THREAD_DEFINE(hb_tid, 1024, hb_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_NODELABEL(can0));
	struct can_filter filter = { .id = 0, .mask = 0, .flags = 0 };
	int filter_id;

	printk("can_rxmon: AgRV2K CAN RX monitor (accept-all, no TX)\n");

	if (!device_is_ready(can)) {
		printk("can_rxmon: FAIL - can0 not ready\n");
		return 0;
	}

	if (can_set_mode(can, CAN_MODE_NORMAL) != 0) {
		printk("can_rxmon: FAIL - can_set_mode(NORMAL)\n");
		return 0;
	}
	if (can_start(can) != 0) {
		printk("can_rxmon: FAIL - can_start\n");
		return 0;
	}

	filter_id = can_add_rx_filter(can, on_rx, NULL, &filter);
	if (filter_id < 0) {
		printk("can_rxmon: FAIL - can_add_rx_filter: %d\n", filter_id);
		return 0;
	}

	printk("can_rxmon: listening (expect external CAN master at 125 kbit/s)\n");

	for (;;) {
		k_sleep(K_MSEC(5000));
	}
}
