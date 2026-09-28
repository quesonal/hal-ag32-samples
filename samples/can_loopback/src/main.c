/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * can_loopback - exercise can_agm via the Zephyr CAN API in
 * MOD_SELFTEST (loopback) mode. No second CAN controller is
 * required -- but the bus must read idle, see below.
 *
 * Sends one frame, waits for the TX complete callback, then checks
 * that the self-test echo arrives on RX with id/dlc/payload intact.
 * Prints a PASS/FAIL summary at the end.
 *
 * NEEDS AN IDLE BUS. MOD_SELFTEST removes the need for a second CAN
 * *controller* (no ACK, the frame loops back in the IP), but not the
 * need for a bus that reads recessive: with nothing on the CAN pins
 * RX0 floats dominant, the transmitter never starts and this sample
 * FAILs with tx_done=0 (measured 2026-09-25 on the unloaded 407;
 * same symptom samples/can_pin_drive documents as "SR.TS stuck, no
 * error counters"). Run it with the CAN bench attached (2x SN65HVD230
 * + a peer, as on 2026-09-12) or with nothing else driving the pins.
 * Confirmed both ways on the 407: FAIL without the bench (2026-09-25),
 * 3/3 PASS with it (2026-09-26).
 *
 * See `the development notes (not published here)` CAN for status.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

#define TX_ID        0x123U
#define TX_DLC       8U
#define TX_TIMEOUT_MS 1000U
#define RX_TIMEOUT_MS 1000U

static const uint8_t TX_PAYLOAD[TX_DLC] = {
	0xDE, 0xAD, 0xBE, 0xEF, 0x55, 0x77, 0x99, 0xBB
};

struct rx_log {
	bool     arrived;
	uint32_t id;
	uint8_t  dlc;
	uint8_t  data[CAN_MAX_DLEN];
};

static volatile bool tx_done;
static volatile int  tx_status;
static struct rx_log rx_log;

static void on_tx_done(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	tx_status = error;
	tx_done = true;
}

static void on_rx(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	rx_log.arrived = true;
	rx_log.id  = frame->id;
	rx_log.dlc = frame->dlc;
	memcpy(rx_log.data, frame->data, frame->dlc);
	/* Echo to console so we can see what the bus delivered, even
	 * if it doesn't match our TX (e.g. ESP32 is sending its own
	 * 0x456 frames and we want to see them too). */
	printk("RX id=0x%x dlc=%u data=", (uint32_t)frame->id, frame->dlc);
	for (int i = 0; i < frame->dlc; i++) {
		printk("%02x", frame->data[i]);
	}
	printk("\n");
}

static void hexprint(const uint8_t *p, uint8_t n)
{
	for (int i = 0; i < n; i++) {
		printk("%02x ", p[i]);
	}
}

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_NODELABEL(can0));
	struct can_frame tx = {
		.id    = TX_ID,
		.dlc   = TX_DLC,
		.flags = 0U,
	};
	/* Accept only our own ID. In MOD.STM the IP still monitors the real
	 * bus (measured 2026-09-12: it received the ESP32's 0x0A2 frame while
	 * self-testing), so an accept-all filter lets a peer frame overwrite
	 * the single rx_log slot before the checks run and the test becomes
	 * flaky. */
	struct can_filter filter = { .id = TX_ID, .mask = CAN_STD_ID_MASK, .flags = 0 };
	bool started = false;
	bool tx_ok = false;
	bool passed = true;
	int filter_id = -1;
	uint32_t t0;
	int rc;

	printk("can_loopback: AgRV2K CAN MOD_SELFTEST loopback test\n");

	if (!device_is_ready(can)) {
		printk("can_loopback: FAIL - can0 not ready\n");
		return 0;
	}

	/* Real MOD_SELFTEST: no second CAN controller is needed (no ACK, the
	 * frame loops back in the IP) -- but the bus still has to read idle,
	 * see the file header. Must be set before can_start() -- the shared
	 * SJA1000 driver returns -EBUSY once the controller is started.
	 */
	rc = can_set_mode(can, CAN_MODE_LOOPBACK);
	if (rc != 0) {
		/* -EBUSY is fine if the driver already had a mode set */
		if (rc != -EBUSY) {
			printk("can_loopback: FAIL - can_set_mode(LOOPBACK): %d\n", rc);
			return 0;
		}
	}

	rc = can_start(can);
	if (rc != 0) {
		printk("can_loopback: FAIL - can_start: %d\n", rc);
		return 0;
	}
	started = true;

	filter_id = can_add_rx_filter(can, on_rx, NULL, &filter);
	if (filter_id < 0) {
		printk("can_loopback: FAIL - can_add_rx_filter: %d\n", filter_id);
		goto out;
	}

	memcpy(tx.data, TX_PAYLOAD, TX_DLC);

	/* Send up to 5 frames so a single transient failure (e.g. bus-off
	 * from the first attempt with no peer) doesn't immediately abort
	 * the test. */
	for (int attempt = 0; attempt < 5; attempt++) {
		tx_done = false;
		tx_status = 0;
		t0 = k_uptime_get_32();
		rc = can_send(can, &tx, K_MSEC(TX_TIMEOUT_MS), on_tx_done, NULL);
		if (rc != 0) {
			printk("can_loopback: WARN - can_send attempt %d: %d\n",
			       attempt, rc);
			k_msleep(200);
			continue;
		}
		while (!tx_done && (k_uptime_get_32() - t0) < TX_TIMEOUT_MS) {
			k_msleep(1);
		}
		if (tx_done && tx_status == 0) {
			break;
		}
		printk("can_loopback: WARN - attempt %d: tx_done=%d tx_status=%d\n",
		       attempt, (int)tx_done, tx_status);
		k_msleep(200);
	}
	if (!tx_done || tx_status != 0) {
		if (tx_status == -114) {
			passed = false;
			goto out;
		}
		passed = false;
		goto out;
	}
	while (!tx_done && (k_uptime_get_32() - t0) < TX_TIMEOUT_MS) {
		k_msleep(1);
	}

	if (!tx_done) {
		printk("can_loopback: FAIL - TX ack never fired (waited %u ms)\n",
		       TX_TIMEOUT_MS);
		passed = false;
	} else if (tx_status != 0) {
		printk("can_loopback: FAIL - TX ack error %d\n", tx_status);
		passed = false;
	} else {
		printk("can_loopback: TX ack OK\n");
		tx_ok = true;
	}

	if (!tx_ok) {
		printk("can_loopback: RX check skipped (no TX)\n");
		goto out;
	}

	t0 = k_uptime_get_32();
	while (!rx_log.arrived && (k_uptime_get_32() - t0) < RX_TIMEOUT_MS) {
		k_msleep(1);
	}

	if (!rx_log.arrived) {
		printk("can_loopback: FAIL - RX never arrived (waited %u ms)\n",
		       RX_TIMEOUT_MS);
		passed = false;
	} else {
		bool id_ok   = (rx_log.id == TX_ID);
		bool dlc_ok  = (rx_log.dlc == TX_DLC);
		bool data_ok = (memcmp(rx_log.data, TX_PAYLOAD, TX_DLC) == 0);

		printk("can_loopback: RX id=0x%x dlc=%u data=",
		       (uint32_t)rx_log.id, rx_log.dlc);
		hexprint(rx_log.data, rx_log.dlc);
		printk("\n");
		if (!id_ok) {
			printk("can_loopback: FAIL - RX id mismatch (got 0x%x)\n",
			       (uint32_t)rx_log.id);
			passed = false;
		}
		if (!dlc_ok) {
			printk("can_loopback: FAIL - RX dlc mismatch (got %u)\n",
			       rx_log.dlc);
			passed = false;
		}
		if (!data_ok) {
			printk("can_loopback: FAIL - RX data mismatch\n");
			passed = false;
		}
	}

out:
	if (filter_id >= 0) {
		(void)can_remove_rx_filter(can, filter_id);
	}
	if (started) {
		(void)can_stop(can);
	}

	printk("can_loopback: %s\n", passed ? "PASS" : "FAIL");
	while (1) {
		k_msleep(1000);
	}
}
