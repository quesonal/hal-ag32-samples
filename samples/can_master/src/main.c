/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * can_master - AgRV2K CAN master paired with an external CAN 2.0B slave
 * node (any controller/driver, normal mode, 125 kbit/s) running the
 * standard master/slave PING/START/DATA/STOP handshake below.
 *
 * Protocol (all standard 11-bit frames, bitrate must match on both ends):
 *   master -> slave : PING (0x0A2) / START (0x0A1) / STOP (0x0A0)
 *   slave  -> master: PING_RESP (0x0B2) / DATA (0x0B1, dlc 4, 50 ms
 *                      period) / STOP_RESP (0x0B0)
 *
 * The slave repeats the handshake NO_OF_ITERS (3) times then uninstalls
 * its driver, so this master runs the same number of iterations and then
 * stops talking. Wiring: AgRV CAN0_TX0 (PIN_39) / CAN0_RX0 (PIN_38) via
 * an SN65HVD230/TJA1050 transceiver onto a shared CAN bus, 120 ohm
 * termination at both ends.
 *
 * Diagnostic build (2026-09-08): heartbeat thread prints k_uptime() +
 * raw CAN SR/IR every 500 ms so a hang can be attributed to the system
 * clock, the CAN IP, or the SJA1000 driver state machine.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define ID_PING        0x0A2U
#define ID_START       0x0A1U
#define ID_STOP        0x0A0U
#define ID_PING_RESP   0x0B2U
#define ID_DATA        0x0B1U
#define ID_STOP_RESP   0x0B0U

/* Peer counts down ~3 s at boot before installing its CAN driver. */
#define BOOT_WAIT_MS       5000U
/* Slave pauses ITER_DELAY_MS (1 s) with the driver stopped between iters. */
#define ITER_GAP_MS        1600U
/* How long to collect DATA frames after START before sending STOP. */
#define DATA_WINDOW_MS     1500U
#define NUM_ITERS          3U

#define TX_TIMEOUT_MS      1000U
#define RESP_TIMEOUT_MS    3000U

/* AgRV CAN0 register window (byte reg N at 32-bit word N*4). */
#define CAN0_BASE          0x4002a000UL
#define CAN0_SR_OFF        0x008UL   /* reg 2  */
#define CAN0_IR_OFF        0x00cUL   /* reg 3  */
#define CAN0_RXERR_OFF     0x038UL   /* reg 14 */
#define CAN0_TXERR_OFF     0x03cUL   /* reg 15 */

static struct k_sem ping_resp_sem;
static struct k_sem stop_resp_sem;

static volatile uint32_t data_count;
static volatile uint8_t  last_data[4];
static volatile bool     got_data;

static void on_rx(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (frame->id) {
	case ID_PING_RESP:
		k_sem_give(&ping_resp_sem);
		break;
	case ID_STOP_RESP:
		k_sem_give(&stop_resp_sem);
		break;
	case ID_DATA:
		if (frame->dlc >= 4U) {
			for (int i = 0; i < 4; i++) {
				last_data[i] = frame->data[i];
			}
			got_data = true;
		}
		data_count++;
		break;
	default:
		break;
	}
}

static int send_std(const struct device *can, uint32_t id, uint8_t dlc,
		    const uint8_t *data)
{
	struct can_frame tx = {
		.id  = id,
		.dlc = dlc,
	};

	if (dlc > 0U && data != NULL) {
		memcpy(tx.data, data, dlc);
	}
	return can_send(can, &tx, K_MSEC(TX_TIMEOUT_MS), NULL, NULL);
}

static bool wait_sem(struct k_sem *sem, uint32_t timeout_ms)
{
	return k_sem_take(sem, K_MSEC(timeout_ms)) == 0;
}

static void dump_can(const char *tag)
{
	uint32_t sr  = sys_read32(CAN0_BASE + CAN0_SR_OFF) & 0xFFU;
	uint32_t ir  = sys_read32(CAN0_BASE + CAN0_IR_OFF) & 0xFFU;
	uint32_t rxe = sys_read32(CAN0_BASE + CAN0_RXERR_OFF) & 0xFFU;
	uint32_t txe = sys_read32(CAN0_BASE + CAN0_TXERR_OFF) & 0xFFU;

	printk("can[%s] t=%lld SR=0x%02x IR=0x%02x RXERR=%u TXERR=%u\n",
	       tag, k_uptime_get(), sr, ir, rxe, txe);
}

static void do_iteration(const struct device *can, int iter)
{
	uint32_t frames;
	bool ok = true;
	int rc;

	printk("can_master: t=%lld iter %d - PING\n", k_uptime_get(), iter);
	dump_can("pre-ping");
	rc = send_std(can, ID_PING, 0, NULL);
	printk("can_master: t=%lld send PING rc=%d\n", k_uptime_get(), rc);
	if (rc != 0) {
		printk("can_master: FAIL - PING send failed\n");
		return;
	}
	if (!wait_sem(&ping_resp_sem, RESP_TIMEOUT_MS)) {
		printk("can_master: t=%lld FAIL - no PING_RESP (check wiring / "
		       "bitrate / peer running?)\n", k_uptime_get());
		dump_can("no-ping-resp");
		return;
	}
	printk("can_master: PING_RESP OK\n");

	rc = send_std(can, ID_START, 0, NULL);
	printk("can_master: t=%lld send START rc=%d\n", k_uptime_get(), rc);
	if (rc != 0) {
		printk("can_master: FAIL - START send failed\n");
		return;
	}

	data_count = 0U;
	got_data = false;
	k_sleep(K_MSEC(DATA_WINDOW_MS));
	frames = data_count;
	printk("can_master: t=%lld collected %u DATA frames", k_uptime_get(),
	       frames);
	if (got_data) {
		printk(", last payload %02x %02x %02x %02x\n",
		       last_data[0], last_data[1], last_data[2], last_data[3]);
	} else {
		printk("\n");
	}

	rc = send_std(can, ID_STOP, 0, NULL);
	printk("can_master: t=%lld send STOP rc=%d\n", k_uptime_get(), rc);
	if (rc != 0) {
		printk("can_master: FAIL - STOP send failed\n");
		return;
	}
	if (!wait_sem(&stop_resp_sem, RESP_TIMEOUT_MS)) {
		printk("can_master: t=%lld FAIL - no STOP_RESP\n", k_uptime_get());
		ok = false;
	}

	if (frames == 0U) {
		printk("can_master: FAIL - no DATA frames received\n");
		ok = false;
	}

	printk("can_master: iter %d %s\n", iter, ok ? "PASS" : "FAIL");
}

static struct k_sem hb_sem;

static void hb_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Heartbeat: exercises k_sem_take() with a 500 ms timeout while
	 * dumping raw CAN SR/IR. (Historic note: added 2026-09-08 when a
	 * tick-freeze made k_sleep() look broken; that was the §3.15 bug,
	 * fixed by restoring the MTIP tick driver -- the probe has
	 * printed normally ever since.) */
	k_sem_init(&hb_sem, 0, 1);
	for (;;) {
		if (k_sem_take(&hb_sem, K_MSEC(500)) != 0) {
			printk("hb: sem take TIMED OUT\n");
		}
		dump_can("hb");
	}
}

K_THREAD_DEFINE(hb_tid, 1024, hb_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_NODELABEL(can0));
	struct can_filter filter = { .id = 0, .mask = 0, .flags = 0 };
	bool started = false;
	int filter_id;

	printk("can_master: AgRV2K CAN master (external slave counterpart)\n");

	if (!device_is_ready(can)) {
		printk("can_master: FAIL - can0 not ready\n");
		goto out;
	}

	/* Normal mode: our TX needs the slave's ACK and vice versa. */
	if (can_set_mode(can, CAN_MODE_NORMAL) != 0) {
		printk("can_master: FAIL - can_set_mode(NORMAL)\n");
		goto out;
	}
	if (can_start(can) != 0) {
		printk("can_master: FAIL - can_start\n");
		goto out;
	}
	started = true;

	filter_id = can_add_rx_filter(can, on_rx, NULL, &filter);
	if (filter_id < 0) {
		printk("can_master: FAIL - can_add_rx_filter: %d\n", filter_id);
		goto out;
	}

	k_sem_init(&ping_resp_sem, 0, 1);
	k_sem_init(&stop_resp_sem, 0, 1);

	/* Let the peer finish its boot countdown + driver install. */
	printk("can_master: waiting %u ms for CAN slave...\n",
	       BOOT_WAIT_MS);
	k_sleep(K_MSEC(BOOT_WAIT_MS));
	printk("can_master: t=%lld boot wait done\n", k_uptime_get());

	for (int i = 0; i < NUM_ITERS; i++) {
		do_iteration(can, i);
		if (i + 1 < NUM_ITERS) {
			/* Slave stops the driver for ITER_DELAY_MS between iters. */
			k_sleep(K_MSEC(ITER_GAP_MS));
		}
	}

	printk("can_master: done (slave uninstalls driver after %u iterations)\n",
	       NUM_ITERS);

out:
	if (started) {
		can_stop(can);
	}
	printk("can_master: t=%lld exiting main\n", k_uptime_get());
	return 0;
}
