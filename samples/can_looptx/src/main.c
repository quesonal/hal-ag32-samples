/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * can_looptx - AG32 CAN normal-mode burst generator for on-bus TX
 * verification (scope / logic analyzer / external CAN node RX).
 *
 * Sends bursts of 5 standard frames (id 0x123, dlc 8,
 * 55 AA 55 AA 55 AA 55 AA) at 125 kbit/s, then idles ~150 ms, forever.
 *
 * Raw SJA1000-style register flow (same as the SDK example
 * $HOME/agm_example-can.elf):
 *   wait SR.TBS -> load TX buffer -> CMR |= TR -> poll SR.TBS
 * MOD is written as 0x00 (NORMAL; STM/LOM/AFM clear): the frame only
 * completes when a peer ACKs -- a peer CAN controller in normal mode
 * drives its TX low in the ACK slot, which is exactly what AG32 RX0
 * (PIN_38) samples, so the IP releases the buffer and TXERR stays 0.
 *
 * If no peer ACKs (no peer on the bus / wiring fault), TXERR climbs and
 * the controller eventually goes bus-off; the loop then forces a
 * reset-mode recovery (RM set -> clear error counters -> RM clear) so
 * the stream keeps trying without user intervention.
 *
 * Wiring (DISCONNECT the PIN_39-PIN_38 jumper for on-bus tests):
 *   AG32 PIN_39 (CAN0_TX0) -> peer RX
 *   AG32 PIN_38 (CAN0_RX0) <- peer TX
 *   GND common
 * RX0 must see a stable recessive idle: the peer's idle-high TX output
 * (or a pull-up) provides it. A scope on PIN_39 shows ~8 us/bit bursts
 * only while a frame actually completes (SOF..ACK); without a peer there
 * is no bus activity at all (controller waits for 11 consecutive
 * recessive bits, then transmits, fails the ACK slot, retries until
 * bus-off).
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define CAN0_BASE      0x4002a000UL
#define CAN0_MOD       0x000UL
#define CAN0_CMR       0x004UL
#define CAN0_SR        0x008UL
#define CAN0_IR        0x00cUL
#define CAN0_TXERR     0x03cUL /* reg 15 (RXERR=reg14 @0x38) */
#define CAN0_TXBUF     0x040UL /* frame info (reg 16) */
#define CAN0_BTR0      0x018UL /* reg 6 */
#define CAN0_BTR1      0x01cUL /* reg 7 */

#define MOD_RM         BIT(0)
#define MOD_AFM        BIT(3)
#define CMR_TR         BIT(0)
#define CMR_RRB        BIT(2)
#define SR_TBS         BIT(2)
#define SR_TCS         BIT(3)
#define IR_RI          BIT(0)

#define CAN0_PLIC_IRQ  29U

#define BURST_FRAMES   5U
#define BURST_GAP_MS   20U
#define IDLE_MS        150U

static inline void wr(uint32_t off, uint32_t val)
{
	sys_write32(val, CAN0_BASE + off);
}

static inline uint32_t rd(uint32_t off)
{
	return sys_read32(CAN0_BASE + off);
}

static void load_sff_frame(uint32_t id, uint8_t dlc)
{
	/* SJA1000 standard frame TX buffer:
	 * reg16 = info (DLC), reg17 = id[10:3], reg18 = id[2:0] << 5,
	 * reg19.. = data bytes.  Registers live on a 4-byte stride. */
	wr(CAN0_TXBUF + 0x00, dlc);
	wr(CAN0_TXBUF + 0x04, (id >> 3) & 0xffU);
	wr(CAN0_TXBUF + 0x08, (id & 0x07U) << 5);
	for (uint32_t i = 0; i < dlc; i++) {
		wr(CAN0_TXBUF + 0x04U * (3U + i), (i & 1U) ? 0xAAU : 0x55U);
	}
}

/* Recover from a pending bus-off / stuck TX: enter reset mode, clear the
 * error counters, leave reset mode.  Note: we leave reset mode with
 * MOD=0 (NORMAL mode, no AFM bit) per the can_agm.c quirk — see the
 * comment near the bottom of main() above. */
static void can_force_recover(void)
{
	wr(CAN0_MOD, MOD_RM);
	wr(CAN0_TXERR, 0);
	wr(CAN0_TXERR - 0x04U, 0); /* RXERR (reg14) */
	wr(CAN0_MOD, 0);
}

static int send_one(void)
{
	uint32_t t0 = k_cycle_get_32();

	/* wait for transmit buffer status (free), bounded */
	while ((rd(CAN0_SR) & SR_TBS) == 0U) {
		if ((k_cycle_get_32() - t0) > 1000000U) { /* ~5 ms */
			return -1;
		}
	}

	load_sff_frame(0x123, 8);
	wr(CAN0_CMR, CMR_TR);

	/* Poll SR.TBS rather than SR.TCS.
	 *
	 * Re-measured 2026-09-12 on a working 125 kbit/s link (ESP32-C3 +
	 * 2x SN65HVD230): SR.TCS *is* set after a successful frame -- the
	 * register reads 0x0c (TCS=1, TBS=1) while idle between frames and
	 * 0x2c right after this loop returns. The old comment here ("AgRV's
	 * IP silently drops TCS") was observed while the dev board transceiver
	 * was faulty and the driver was 3x off in bit rate, and it is not
	 * reproducible now.
	 *
	 * TBS is still the better signal for this sample: it drops when TR
	 * latches and rises when the IP releases the buffer, for success and
	 * for error alike, so one poll covers both. Kept for that reason,
	 * not because TCS is broken.
	 */
	t0 = k_cycle_get_32();
	while ((rd(CAN0_SR) & SR_TBS) != 0U) {
		/* Wait for TR to be latched (TBS -> 0). */
		if ((k_cycle_get_32() - t0) > 1000000U) { /* ~5 ms */
			return -1;
		}
	}
	while ((rd(CAN0_SR) & SR_TBS) == 0U) {
		/* Wait for the IP to release the buffer (TBS -> 1). */
		if ((k_cycle_get_32() - t0) > 4000000U) { /* ~20 ms */
			return -2;
		}
	}

	if ((rd(CAN0_IR) & IR_RI) != 0U) {
		wr(CAN0_CMR, CMR_RRB);
	}
	return 0;
}

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_NODELABEL(can0));
	uint32_t sent = 0;
	uint32_t fails = 0;

	printk("can_looptx: NORMAL-mode bursts on CAN0_TX0 (PIN_39)\n");

	if (!device_is_ready(can)) {
		printk("looptx: FAIL can0 not ready\n");
		return 0;
	}
	if (can_set_mode(can, CAN_MODE_NORMAL) != 0 || can_start(can) != 0) {
		printk("looptx: FAIL can start\n");
		return 0;
	}

	/* Driver init done (clock gate, AFSEL, BTR0=0x12/BTR1=0x1a =
	 * 125.3 kbit/s). Take over raw: normal mode, poll instead of the
	 * interrupt-driven can_send() path.
	 *
	 * MOD is written as 0x00. The old comment here claimed MOD.AFM=1
	 * "also disables the receiver and transmitter on AgRV" (MOD=0x08 →
	 * SR.RS=0, SR.TS=0). That reading is wrong: SR.RS/SR.TS are
	 * *transient* status bits, only set while a frame is actually being
	 * received/transmitted, so sampling them at idle always yields 0 --
	 * with or without AFM.
	 *
	 * Re-measured 2026-09-12 by flipping MOD to 0x08 through openocd on
	 * the live link: the peer's frame rate was unchanged (260 frames /
	 * 13 s at MOD=0x08 vs 260 / 13 s at MOD=0x00). AFM only selects the
	 * single vs dual acceptance-filter layout, so it is a don't-care for
	 * TX/RX here; MOD=0 is written simply because it is the plain
	 * NORMAL-mode value. */
	irq_disable(CAN0_PLIC_IRQ);
	wr(CAN0_MOD, 0);

	/* SJA1000 encodes TSEG1/TSEG2 as (value - 1), so decode with the +1:
	 * TQ_total = 1 (sync) + (TSEG1 + 1) + (TSEG2 + 1). For BTR1=0x1c
	 * that is 1 + 13 + 2 = 16, not 14. */
	printk("looptx: BTR0=0x%02x BTR1=0x%02x (BRP+1=%u TQ=%u)\n",
	       rd(CAN0_BTR0) & 0xffU, rd(CAN0_BTR1) & 0xffU,
	       (rd(CAN0_BTR0) & 0x3fU) + 1U,
	       3U + (rd(CAN0_BTR1) & 0x0fU) + ((rd(CAN0_BTR1) >> 4) & 0x07U));

	printk("looptx: started (AG32 39->peer RX, AG32 38<-peer TX, GND common)\n");

	for (;;) {
		for (uint32_t i = 0; i < BURST_FRAMES; i++) {
			int rc = send_one();

			if (rc != 0) {
				fails++;
				if (fails >= 20U ||
				    (rd(CAN0_TXERR) & 0xffU) > 200U) {
					printk("looptx: no peer ACK, recovering "
					       "(fails=%u TXERR=%u)\n", fails,
					       rd(CAN0_TXERR) & 0xffU);
					can_force_recover();
					fails = 0;
				}
			} else {
				fails = 0;
			}

			if (sent % 25U == 0U) {
				printk("looptx sent=%u rc=%d SR=0x%02x TXERR=%u uptime=%lld\n",
				       sent, rc, rd(CAN0_SR) & 0xffU,
				       rd(CAN0_TXERR) & 0xffU,
				       (long long)k_uptime_get());
			}
			sent++;
			k_sleep(K_MSEC(BURST_GAP_MS));
		}
		k_sleep(K_MSEC(IDLE_MS));
	}
}
