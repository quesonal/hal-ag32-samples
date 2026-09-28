/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * can_txedge v2 - measure on-wire TX polarity + real bit time by
 * tight-loop sampling of the RX0 pin (GPIO7.3) while CAN transmits
 * in self-test mode with PIN_39 - PIN_38 shorted.
 *
 * min_gap is the shortest run between two edges, i.e. one bit, so
 * f_can = 2 * (BRP+1) * TQ_total / min_gap. Cross-checked against an
 * ESP32-C3 listener that measured 2.59 us/bit while this IP ran
 * BTR0=0x12/BTR1=0x1a -> f_can = 200 MHz, not SYSCLK/3.
 *
 * Setup: no external peer; PIN_39 jumpered to PIN_38.
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define CAN0_BASE       0x4002a000UL
#define CAN0_MOD        0x000UL
#define CAN0_CMR        0x004UL
#define CAN0_SR         0x008UL
#define CAN0_IR         0x00cUL
#define CAN0_IER        0x010UL
#define CAN0_TXBUF      0x040UL
#define CAN0_BTR0      0x018UL
#define CAN0_BTR1      0x01cUL

#define MOD_RM         BIT(0)
#define MOD_SELFTEST   BIT(2)
#define MOD_AFM        BIT(3)
#define CMR_TR         BIT(0)
#define CMR_RRB        BIT(2)
#define SR_TCS         BIT(3)
#define IR_RI          BIT(0)

#define GPIO7_BASE     0x4001b000UL
#define GPIO_DATA8     0x3fcUL
#define RX_BIT         BIT(3)   /* GPIO7.3 = CAN0_RX0 pin */

#define CAN0_PLIC_IRQ  29U

static inline uint32_t rd(uint32_t off)
{
	return sys_read32(CAN0_BASE + off);
}

static inline void wr(uint32_t off, uint32_t val)
{
	sys_write32(val, CAN0_BASE + off);
}

static inline uint32_t rx_pin(void)
{
	return (sys_read32(GPIO7_BASE + GPIO_DATA8) & RX_BIT) ? 1U : 0U;
}

static void load_tx_frame(void)
{
	/* standard id 0x123, dlc 8, data 55 AA 55 AA ... */
	wr(CAN0_TXBUF + 0x00, 0x08U);
	wr(CAN0_TXBUF + 0x04, 0x24U);
	wr(CAN0_TXBUF + 0x08, 0x60U);
	for (int i = 0; i < 8; i++) {
		wr(CAN0_TXBUF + 0x04U * (5U + i), (i & 1U) ? 0xAAU : 0x55U);
	}
}

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_NODELABEL(can0));

	printk("can_txedge v2: polarity + bit-time measure (39-38 shorted)\n");
	printk("txedge: sys ticks/sec = %u\n", CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);

	if (!device_is_ready(can)) {
		printk("txedge: FAIL can0 not ready\n");
		return 0;
	}
	if (can_set_mode(can, CAN_MODE_NORMAL) != 0 || can_start(can) != 0) {
		printk("txedge: FAIL can init\n");
		return 0;
	}
	irq_disable(CAN0_PLIC_IRQ);
	k_sleep(K_MSEC(50));

	/* Idle level: majority of RX0 pin samples over ~2 ms, no TX. */
	{
		uint32_t hi = 0, lo = 0;

		for (int i = 0; i < 20000; i++) {
			if (rx_pin()) {
				hi++;
			} else {
				lo++;
			}
		}
		printk("txedge idle: RX0 hi=%u lo=%u -> %s\n", hi, lo,
		       hi > lo ? "HIGH (recessive)" : "LOW (dominant?)");
	}

	/* Enter reset mode; IER off; SELFTEST + AFM. */
	wr(CAN0_MOD, MOD_RM | MOD_SELFTEST | MOD_AFM);
	wr(CAN0_IER, 0);
	wr(CAN0_MOD, MOD_SELFTEST | MOD_AFM);

	/* (div, BTR1) table. TQ total = (TSEG1f) + (TSEG2f) + 3. */
	static const uint32_t cfgs[][2] = {
		{ 50, 0x1c }, /* 16 TQ (TSEG1=12, TSEG2=1) - 8.0 us/bit = 125k @200M */
		{ 25, 0x1c },
		{ 33, 0x1c },
		{ 17, 0x1c },
		{ 19, 0x1a }, /* 14 TQ - 2.66 us/bit = 376k @ f_can=200M */
		{ 16, 0x1c },
		{ 10, 0x1c },
	};
	uint64_t fcan_sum = 0;
	uint32_t fcan_rows = 0;

	for (size_t c = 0; c < ARRAY_SIZE(cfgs); c++) {
		uint32_t div = cfgs[c][0];
		uint32_t btr1 = cfgs[c][1];
		uint32_t tq = (btr1 & 0x0FU) + ((btr1 >> 4) & 0x07U) + 3U;
		uint32_t fcan_row = 0;

		/* reprogram in reset mode */
		wr(CAN0_MOD, MOD_RM | MOD_SELFTEST | MOD_AFM);
		wr(CAN0_BTR1, btr1);
		wr(CAN0_BTR0, div - 1U);
		wr(CAN0_MOD, MOD_SELFTEST | MOD_AFM);

		for (int f = 0; f < 2; f++) {
			uint32_t hi = 0, lo = 0, edges = 0, min_delta = 0xFFFFFFFFU;
			uint32_t prev_t = 0, last = rx_pin();
			uint32_t t0, now;
			uint32_t sr = 0;

			load_tx_frame();
			wr(CAN0_CMR, CMR_TR);

			t0 = k_cycle_get_32();
			for (uint32_t i = 0; i < 60000U; i++) {
				uint32_t cur = rx_pin();

				now = k_cycle_get_32();
				if (cur) {
					hi++;
				} else {
					lo++;
				}

				if (cur != last) {
					uint32_t dt = now - prev_t;

					/* prev_t must advance on EVERY edge,
					 * otherwise dt is not a pulse width but
					 * the distance from the first edge. With
					 * prev_t only latched while edges == 0
					 * this sample reported three bit times
					 * for an id-0x123 frame (SOF plus two
					 * dominant ID bits), which is where the
					 * old "f_can = SYSCLK/3" conclusion
					 * came from.
					 */
					prev_t = now;
					if (edges != 0U && dt > 100U && dt < min_delta) {
						min_delta = dt;
					}
					last = cur;
					edges++;
				}

				if ((i & 2047U) == 0U) {
					sr = rd(CAN0_SR) & 0xFFU;
					if ((sr & SR_TCS) != 0U) {
						break;
					}
				}
				if ((now - t0) > 1600000U) { /* 8 ms */
					break;
				}
			}

			printk("txedge div=%2u tq=%2u f%d: min_gap=%u ticks",
			       div, tq, f, min_delta);
			if (min_delta != 0xFFFFFFFFU && min_delta != 0U) {
				/* f_can = 2 * div * tq / bit_time */
				uint64_t fc = (uint64_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
					     * 2U * div * tq / min_delta;

				printk(" (~%llu ns) => f_can ~ %llu.%02llu MHz",
				       (unsigned long long)min_delta * 5U,
				       (unsigned long long)(fc / 1000000U),
				       (unsigned long long)((fc / 10000U) %
							    100U));
				fcan_row = (uint32_t)(fc / 1000U);
			} else {
				printk(" (no edges)");
			}
			printk("\n");
			if ((rd(CAN0_IR) & IR_RI) != 0U) {
				wr(CAN0_CMR, CMR_RRB);
			}
			k_sleep(K_MSEC(5));
		}

		if (fcan_row != 0U) {
			fcan_sum += fcan_row;
			fcan_rows++;
		}
	}

	if (fcan_rows != 0U) {
		uint64_t favg = fcan_sum / fcan_rows;

		printk("txedge: avg f_can ~ %llu.%03llu MHz "
		       "(ratio %.3f vs 200M)\n",
		       (unsigned long long)(favg / 1000U),
		       (unsigned long long)(favg % 1000U),
		       (double)favg / 1000.0 / 200.0);
	}

	printk("txedge: done\n");
	for (;;) {
		k_sleep(K_MSEC(60000));
	}
}
