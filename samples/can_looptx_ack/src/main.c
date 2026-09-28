/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * can_looptx_ack - AG32 CAN TX + software ACK injection via PIN_23.
 *
 * STATUS 2026-09-12: this is now a *fallback* for a single-board dev board.
 * The dev board does have a second CAN node (ESP32-C3 + 2x SN65HVD230, see
 * the port status §3.18), so a real ACK is available and the
 * injection trick is no longer needed for validation.
 *
 * Background (why this sample exists): with only AgRV on the bus, the
 * standard `can_looptx` sample shows TX leaving PIN_39 but TXERR still
 * climbs, because no second node drives PIN_38 dominant during the ACK
 * slot. The IP never sees an ACK, the frame errors out, and from that
 * test alone you cannot tell whether the IP's RX path works at all.
 *
 * NOTE on the ACK-slot timing: the ~890 us below assumes a real
 * 125 kbit/s bit time. That only became true on 2026-09-12 -- before the
 * CAN_AGM_CAN_CLOCK_HZ fix the "125k" configuration actually ran at
 * ~376 kbit/s (2.66 us/bit), so a pulse placed at 890 us landed nowhere
 * near the ACK slot. Any single-board result from before that date is
 * therefore inconclusive, not evidence of a broken IP.
 *
 * Strategy: jumper PIN_23 <-> PIN_38 (so PIN_23 GPIO drives RX0), and
 *   when this sample detects that the IP has just kicked off a transmit,
 *   schedule a one-shot GPIO pulse on PIN_23 about ~890 us later — that
 *   is the ACK-slot position in a 11-bit-ID + 8-byte-data frame at
 *   125 kbit/s (~111 bits incl. CRC and CRC delimiter). The pulse
 *   duration is ~16 us (2 bit times) so we straddle the actual ACK slot
 *   regardless of small jitter in the bit-banding overhead.
 *
 *   If IR.RI fires (IP reports it received the self-transmitted frame)
 *   and SR.RBS goes high (RX buffer holds the echoed frame), the IP RX
 *   path is verified end-to-end on the chip itself.
 *
 * Wiring:
 *   PIN_38 (CAN0_RX0) <-> PIN_23 (GPIO6_2)       (jumper — this sample
 *                                                  drives PIN_23 from
 *                                                  software)
 *   PIN_39 (CAN0_TX0) -> goes out on its own (no jumper for this test;
 *                          the ACK slot is fed back via PIN_23, not via
 *                          the IP's own TX echo).
 *   GND common (only matters if you also wire a peer).
 *
 * Compared to can_looptx the key new code is:
 *   - PIN_23 GPIO6 bit 2 init as output, idle high (recessive)
 *   - busy-wait ~890 us after TR, then drive PIN_23 low for ~16 us
 *   - poll IR.RI + SR.RBS and report
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

/* CAN0 (SJA1000 stride=4 layout) */
#define CAN0_BASE      0x4002a000UL
#define CAN0_MOD       0x000UL
#define CAN0_CMR       0x004UL
#define CAN0_SR        0x008UL
#define CAN0_IR        0x00cUL
#define CAN0_TXERR     0x03cUL /* reg 15 (RXERR=reg14 @0x38) */
#define CAN0_TXBUF     0x040UL /* frame info (reg 16) */

#define MOD_AFM        BIT(3)
#define CMR_TR         BIT(0)
#define CMR_RRB        BIT(2)
#define SR_TBS         BIT(2)
#define SR_TCS         BIT(3)
#define SR_RBS         BIT(0)
#define IR_RI          BIT(0)

#define CAN0_PLIC_IRQ  29U

/* GPIO6 base + bit 2 = PIN_23 (IO_Button1 on the 407 board).
 * See $HOME/zephyrproject/modules/hal_ag32/boards/agm/agrv2k_407/agrv2k_407.dts
 * line 200 ("GPIO6_2 -> PIN_23 IO_Button1").
 */
#define GPIO6_BASE     0x4001A000UL
#define GPIO6_DIR      0x400UL
#define PIN23_BIT      BIT(2)
/* Bit-banded alias for GPIO6 bit 2 = GPIO6_BASE + 2*4 = 0x4001A008.
 * Writing any non-zero value drives the pin high; writing 0 drives it low.
 * (Stellaris-style bit-banding — also documented in the AgRV SDK header
 * framework-agrv_sdk/src/gpio.h: gpio->GpioDATA[bits] = val.)
 */
#define PIN23_BITBAND  (GPIO6_BASE + 2U * 4U)

/* 125 kbit/s = 8 us / bit.
 * 11-bit ID + RTR + IDE + r0 (14) + DLC (4) + 8 byte data (64) +
 * CRC (15) + CRC delim (1) + ACK slot (1) + ACK delim (1) + EOF (7)
 * + IFS (3) = ~111 bits non-stuffed.
 * Stuff bits depend on data — for the 0x55/0xAA alternating payload
 * there are zero stuff bits in the data segment and ~2-3 in the ID/CRC
 * regions, so the ACK slot lands somewhere in [108, 115] bit indices.
 * Center our pulse at bit 110 (880 us) and stretch it to 16 us (2 bits)
 * so it straddles the actual slot regardless of jitter.
 */
#define BIT_TIME_US    8U
#define ACK_BIT_IDX    110U
#define ACK_PULSE_US   16U
#define ACK_OFFSET_US  (ACK_BIT_IDX * BIT_TIME_US - ACK_PULSE_US / 2U)

static inline void wr(uint32_t off, uint32_t val)
{
	sys_write32(val, CAN0_BASE + off);
}

static inline uint32_t rd(uint32_t off)
{
	return sys_read32(CAN0_BASE + off);
}

/* Drive PIN_23 high (recessive — bus idle, IP RX sees recessive).
 * Bit-banded DATA writes: any non-zero value drives the pin high. */
static inline void pin23_high(void)
{
	sys_write32(0xff, PIN23_BITBAND);
}

static inline void pin23_low(void)
{
	sys_write32(0x00, PIN23_BITBAND);
}

static void pin23_init(void)
{
	uint32_t dir = sys_read32(GPIO6_BASE + GPIO6_DIR);

	/* GPIO6 bit 2 = output (DIR=1), AFSEL stays 0 (software mode). */
	sys_write32(dir | PIN23_BIT, GPIO6_BASE + GPIO6_DIR);
	/* Default the wire to recessive (high). */
	pin23_high();
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

/* Force-reset: clear error counters so a stranded error-passive state
 * does not block the next try. */
static void can_force_recover(void)
{
	wr(CAN0_MOD, MOD_AFM | BIT(0)); /* AFM | RM */
	wr(CAN0_TXERR, 0);
	wr(CAN0_TXERR - 0x04U, 0);      /* RXERR */
	wr(CAN0_MOD, MOD_AFM);          /* back to NORMAL */
}

static int send_one_with_ack(void)
{
	uint32_t t0;

	/* 1) Wait for the TX buffer to be free. */
	t0 = k_cycle_get_32();
	while ((rd(CAN0_SR) & SR_TBS) == 0U) {
		if ((k_cycle_get_32() - t0) > 1000000U) { /* ~5 ms @ 200 MHz */
			return -1;
		}
	}

	/* Clear stale IR (read-only — clears on read of CMR.RRB, which we
	 * skip here because we want the RI bit to actually mean "received
	 * during this TX"). We rely on RRB at the end of a successful
	 * send to release any prior frame from the RX buffer. */

	load_sff_frame(0x123, 8);
	wr(CAN0_CMR, CMR_TR);

	/* 2) Busy-wait until ~ACK slot, then drive PIN_23 dominant.
	 * k_busy_wait spins the CPU, so the time-base is deterministic.
	 * The PIN_23->PIN_38 jumper routes the GPIO bit into the CAN IP's
	 * RX0 input; during the ACK slot the IP expects RX to be dominant
	 * (any peer ACKing), and the IP itself drives TX recessive. */
	k_busy_wait(ACK_OFFSET_US);
	pin23_low();
	k_busy_wait(ACK_PULSE_US);
	pin23_high();

	/* 3) Wait for frame-complete (TCS). */
	t0 = k_cycle_get_32();
	while ((rd(CAN0_SR) & SR_TCS) == 0U) {
		if ((k_cycle_get_32() - t0) > 4000000U) { /* ~20 ms */
			return -2;
		}
	}

	/* 4) Did the IP loop the frame back into its own RX buffer?
	 * IR.RI fires on successful reception; SR.RBS indicates the RX
	 * buffer holds the frame. If both, the IP RX path works and the
	 * ACK injection succeeded. */
	uint8_t ir = rd(CAN0_IR) & 0xffU;
	uint8_t sr = rd(CAN0_SR) & 0xffU;
	bool ack_ok = ((ir & IR_RI) != 0U) && ((sr & SR_RBS) != 0U);

	if (ack_ok) {
		wr(CAN0_CMR, CMR_RRB); /* release the RX buffer for next round */
		return 0;
	}
	return -3;
}

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_NODELABEL(can0));
	uint32_t sent = 0;
	uint32_t acks = 0;
	uint32_t fails = 0;

	pin23_init();

	if (!device_is_ready(can)) {
		printk("can_looptx_ack: FAIL can0 not ready\n");
		return 0;
	}
	if (can_set_mode(can, CAN_MODE_NORMAL) != 0 || can_start(can) != 0) {
		printk("can_looptx_ack: FAIL can start\n");
		return 0;
	}

	irq_disable(CAN0_PLIC_IRQ);
	wr(CAN0_MOD, MOD_AFM);

	printk("can_looptx_ack: 125k NORMAL + ACK injection via PIN_23\n");
	printk("  JUMPER REQUIRED: PIN_38 <-> PIN_23 (GPIO6_2)\n");
	printk("  ACK slot pulse: offset=%u us, width=%u us (bit=%u)\n",
	       ACK_OFFSET_US, ACK_PULSE_US, BIT_TIME_US);
	for (;;) {
		int rc = send_one_with_ack();

		if (rc == 0) {
			acks++;
			fails = 0;
		} else {
			fails++;
			if (fails >= 20U ||
			    (rd(CAN0_TXERR) & 0xffU) > 200U) {
				printk("ack: stuck (rc=%d TXERR=%u) — recovering\n",
				       rc, rd(CAN0_TXERR) & 0xffU);
				can_force_recover();
				fails = 0;
			}
		}

		if (sent % 25U == 0U || acks > 0) {
			uint8_t sr = rd(CAN0_SR) & 0xffU;
			uint8_t ir = rd(CAN0_IR) & 0xffU;
			uint8_t te = rd(CAN0_TXERR) & 0xffU;

			printk("ack sent=%u acks=%u rc=%d SR=0x%02x IR=0x%02x TXERR=%u\n",
			       sent, acks, rc, sr, ir, te);
		}
		sent++;
		k_sleep(K_MSEC(20));
	}
}
