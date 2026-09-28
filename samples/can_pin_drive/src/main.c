/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * can_pin_drive — raw-GPIO square-wave probe for the CAN0 pin pair.
 *
 * Purpose: put an unambiguous, low-frequency (1 kHz) square wave on the
 * CAN0 pin so that the PIN_39 -> transceiver-D and transceiver-R ->
 * PIN_38 wiring can be chased point by point with a meter / scope. The
 * on-chip CAN controller is deliberately NOT involved: this tests the
 * *pins and the board net*, not the SJA1000 core.
 *
 * What the 407 bitstream actually gives us, per
 * ~/spi_full_mac_bitstream_200mhz/example_board.v (the vendor reference
 *     netlist the flashed bitstream corresponds to, lines 114-120; a locally
 *     generated <build>/logic/board.vx is NOT compile-verified -- cite the
 *     reference, not it):
 *
 *   PIN_38_iobuf (.datain 1'b0, .oe 1'b0)  -> combout PIN_38_in
 *       PIN_38 is INPUT-ONLY. There is no output driver at all, so it
 *       is physically impossible to emit a square wave on PIN_38. It is
 *       readable as GPIO7 bit 3 (gpio7_io_in[3] = PIN_38_in).
 *
 *   PIN_39_iobuf (.datain PIN_39_out_data, .oe PIN_39_out_en)
 *       PIN_39_out_data = gpio8_io_out_data[7]   (GPIO8 bit 7)
 *       PIN_39_out_en   = !gpio8_io_out_en[7]    (INVERTED vs. PIN_68)
 *       PIN_39 has no read-back (combout unconnected), so this sample
 *       uses PIN_38 as the observer. Jumper PIN_39 <-> PIN_38 and the
 *       edge count tells you whether the pin really drives.
 *
 * Because the output-enable is inverted only for this pin, the DIR bit
 * that actually drives is not obvious from the netlist alone. This
 * sample therefore sweeps the four (AFSEL, DIR) combinations and holds
 * each one long enough to measure, so a meter/scope can follow the
 * waveform downstream.
 *
 * History -- read this before trusting any PIN_38 reading taken with the
 * transceivers attached:
 *
 *   The 2026-09-11 openocd run (CPU halted, 38<->39 jumpered, continuity
 *   OK) found that GPIO8.7's data latch toggled 0x80 <-> 0x00 while
 *   PIN_38 (GPIO7.3) stayed a constant 0x08 for every (AFSEL, DIR)
 *   combination, and concluded "PIN_39 never drives PIN_38 in GPIO mode".
 *   That conclusion is **invalid**: the transceivers were wired to those
 *   pins at the time, and an SN65HVD230's R output is push-pull -- it
 *   holds the node recessive-high whenever D is high, which masks
 *   whatever the pin does. (The same dev board also had a transceiver VCC
 *   fault, found on 2026-09-12.)
 *
 *   Re-tested 2026-09-12 with a healthy 2-node link (ESP32-C3 peer, 2x
 *   SN65HVD230): CAN0_TX0 drives fine through the AF path, and clearing
 *   GPIO8's DIR bit 7 through openocd did not change the peer's frame
 *   rate at all (260 frames / 13 s before and after) -- i.e. in AF mode
 *   the pin output enable is not gated by DIR, matching the vendor SDK,
 *   which never writes DIR for CAN. The (AFSEL, DIR) sweep below is
 *   still useful, but read it only with the transceivers disconnected.
 *
 * Usage notes:
 *   - Boot prints a quick 1 kHz GPIO edge-count sweep, then runs CAN
 *     transmit bursts forever, alternating the GPIO8.7 DIR every 5 s.
 *     The two windows are deliberately distinguishable on a plain
 *     logic analyser *zoomed out to the whole capture*, without a
 *     console and without an envelope view:
 *         DIR=1 -> CAN id 0x111, 3 s of CONTINUOUS frames (one solid bar)
 *         DIR=0 -> CAN id 0x222, 3 s of 200 ms on / 800 ms off
 *                  (three clearly separated 200 ms blobs)
 *         (1 s of silence between the two windows -> 8 s full cycle)
 *     A 40 ms/40 ms envelope is useless on a logic analyser: at any zoom
 *     that still fits the 5 s window in one screen, 40 ms is 1-2 pixels
 *     wide and PulseView renders the train as a single filled band.
 *     `pin38_edges` in the burst log is crosstalk on the (floating)
 *     neighbour pin; use the LA on PIN_39 for the real answer.
 *   - Self-test mode (MOD.STM) is used so the core does not wait for
 *     bus-free on RX0; the burst restarts the core after bus-off so the
 *     train keeps going.
 *   - PIN_38 (RX0) only reaches the CAN core through the GPIO7.3 AF mux,
 *     so AFSEL must be set on it too -- without that the controller
 *     never sees bus-free (SR.TS stuck, no error counters).
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/* SYS APB clock gate: GPIO bank n is bit (4 + n). */
#define SYS_APB_CLKENABLE	0x03000060UL
#define SYS_APB_CLK_GPIO(n)	(1UL << (4U + (n)))

/* Stellaris-style GPIO: DATA is a masked array at base + (mask << 2),
 * DIR lives at +0x400 and AFSEL at +0x420 (see samples/can_listen). */
#define GPIO7_BASE		0x4001B000UL
#define GPIO8_BASE		0x4001C000UL
#define GPIO_DIR_OFF		0x400U
#define GPIO_AFSEL_OFF		0x420U
#define GPIO_DATA_MASK_OFF(m)	((m) << 2)

#define CAN_RX0_BIT		3U	/* GPIO7.3 == PIN_38 (read only) */
#define CAN_TX0_BIT		7U	/* GPIO8.7 == PIN_39 (drive)     */

/* CAN0 register file (SJA1000 / PeliCAN, 8-bit regs on 32-bit stride). */
#define CAN0_BASE		0x4002A000UL
#define CAN_MOD			0x000UL
#define CAN_CMR			0x004UL
#define CAN_SR			0x008UL
#define CAN_IER			0x010UL
#define CAN_IR			0x00CUL
#define CAN_BTR0		0x018UL
#define CAN_BTR1		0x01CUL
#define CAN_OCR			0x020UL
#define CAN_ECC			0x030UL
#define CAN_TXERR		0x03CUL
#define CAN_TXBUF		0x040UL

#define CAN_MOD_RM		0x01U
#define CAN_MOD_STM_AFM		0x0CU
/* Normal mode: AFM set, RM/STM clear. Unlike STM this does NOT loop TX
 * back to RX on this IP, so SR/ECC/TXERR reflect the real bus. */
#define CAN_MOD_NORMAL		0x08U
#define CAN_CMR_TR		0x01U
#define CAN_CMR_RRB		0x04U
#define CAN_SR_RBS		0x01U
#define CAN_SR_TCS		0x08U
#define CAN_SR_BS		0x80U
#define CAN_IR_DOI		0x08U

#define SYS_APB_CLK_CAN0	(1UL << 26)
/* Same GPIO8 bank, normal (non-inverted) output: PIN_67 = GPIO8.0.
 * Used as a reference square wave so a dead PIN_39 can be blamed on the
 * CAN0_TX0 pin specifically rather than on the whole GPIO8 bank.
 */
#define REF_TX_BIT		0U	/* GPIO8.0 == PIN_67 */

/* 1 kHz square wave: 500 us per half period. */
#define HALF_US			500U
#define SWEEP_HALVES		40U	/* ~20 ms per (AFSEL, DIR) trial */
#define HOLD_MS			3000U	/* dwell per DIR window            */

/* Envelope for the DIR=0 window. 200 ms of frames every 1 s leaves an
 * 800 ms hole -- wide enough (80 px on a 1000 px/10 s view) that the
 * window reads as five separate blobs even with the trace fully zoomed
 * out, where a 40 ms/40 ms envelope would just look solid.
 */
#define GAP_ON_MS		200U
#define GAP_PERIOD_MS		1000U
#define WINDOW_GAP_MS		1000U	/* silence between the two windows */

static int rx_level(void)
{
	uint32_t mask = BIT(CAN_RX0_BIT);
	uint32_t v = sys_read32(GPIO7_BASE + GPIO_DATA_MASK_OFF(mask));

	return (v & mask) != 0U ? 1 : 0;
}

static void tx_level(int level)
{
	uint32_t mask = BIT(CAN_TX0_BIT) | BIT(REF_TX_BIT);

	sys_write32(level ? mask : 0U, GPIO8_BASE + GPIO_DATA_MASK_OFF(mask));
}

static void set_dir(uint32_t bit, uint32_t dir)
{
	if (dir != 0U) {
		sys_set_bits(GPIO8_BASE + GPIO_DIR_OFF, BIT(bit));
	} else {
		sys_clear_bits(GPIO8_BASE + GPIO_DIR_OFF, BIT(bit));
	}
}

static void set_afsel(uint32_t bit, uint32_t afsel)
{
	if (afsel != 0U) {
		sys_set_bits(GPIO8_BASE + GPIO_AFSEL_OFF, BIT(bit));
	} else {
		sys_clear_bits(GPIO8_BASE + GPIO_AFSEL_OFF, BIT(bit));
	}
}

static inline uint32_t can_rd(uint32_t off)
{
	return sys_read32(CAN0_BASE + off) & 0xFFU;
}

static inline void can_wr(uint32_t off, uint32_t val)
{
	sys_write32(val & 0xFFU, CAN0_BASE + off);
}

/*
 * Drain the receive buffer and report what the core actually saw on
 * CAN0_RX0. On this bitstream MOD.STM does *not* appear to loop TX back
 * into RX (transmissions go un-ACKed and TXERR climbs), so anything that
 * shows up here came off the real bus -- the only way to tell "the AG32
 * cannot receive" apart from "the AG32 cannot transmit".
 */
static uint32_t rx_drain(void)
{
	uint32_t n = 0U;

	for (int i = 0; i < 16; i++) {
		uint32_t sr = can_rd(CAN_SR);
		uint32_t info;
		uint32_t id1;
		uint32_t id2;
		uint32_t id;
		uint32_t dlc;
		uint32_t d0;
		uint32_t ir;

		if ((sr & CAN_SR_RBS) == 0U) {
			break;
		}
		info = can_rd(CAN_TXBUF + 0x00U);
		id1 = can_rd(CAN_TXBUF + 0x04U);
		id2 = can_rd(CAN_TXBUF + 0x08U);
		id = (((id1 << 3) | (id2 >> 5)) & 0x7FFU);
		dlc = info & 0x0FU;
		d0 = (dlc > 0U) ? can_rd(CAN_TXBUF + 0x0CU) : 0U;
		ir = can_rd(CAN_IR);
		if (n < 6U) {
			printk("pin: RX id=0x%03x dlc=%u d0=0x%02x%s%s\n",
			       id, dlc, d0,
			       (ir & CAN_IR_DOI) ? " OVERRUN" : "",
			       (info & 0x40U) ? " rtr" : "");
		}
		can_wr(CAN_CMR, CAN_CMR_RRB);
		n++;
	}
	return n;
}

/*
 * Transmit CAN frames back-to-back in self-test mode (MOD.STM): the CAN
 * core loops TX->RX internally, so it does NOT wait for bus-free on RX0
 * and starts immediately. This is the only way to get a continuous,
 * scope/LA-visible burst on PIN_39 regardless of what the bus is doing.
 *
 * DIR is the variable under test: the bitstream inverts this pin's OE
 * (`PIN_39_out_en = !gpio8_io_out_en[7]`), so exactly one DIR setting
 * drives. The LA on PIN_39 shows which one produces frames.
 */
/*
 * gapped == 0: back-to-back frames (continuous train).
 * gapped != 0: GAP_ON_MS of frames every GAP_PERIOD_MS, so even a fully
 *              zoomed-out trace shows separate blobs instead of a solid
 *              train (PulseView has no envelope rendering).
 * `id` is the standard frame ID: use different IDs per DIR so the LA's
 * CAN decoder names the window directly.
 */
static void can_stm_burst(uint32_t dir, uint32_t id, uint32_t ms, int gapped)
{
	uint32_t frames = 0U;
	uint32_t rx_frames = 0U;
	uint32_t edges = 0U;
	uint32_t t0;
	uint32_t rx0;
	int last;

	/* AFSEL on both pins; RX0 is an input, TX0 DIR is the variable. */
	sys_set_bits(SYS_APB_CLKENABLE, SYS_APB_CLK_CAN0 |
		     SYS_APB_CLK_GPIO(7) | SYS_APB_CLK_GPIO(8));
	sys_set_bits(GPIO7_BASE + GPIO_AFSEL_OFF, BIT(CAN_RX0_BIT));
	sys_clear_bits(GPIO7_BASE + GPIO_DIR_OFF, BIT(CAN_RX0_BIT));
	set_afsel(CAN_TX0_BIT, 1U);
	set_dir(CAN_TX0_BIT, dir);

	can_wr(CAN_MOD, CAN_MOD_RM);
	can_wr(CAN_BTR0, 0x12U);
	can_wr(CAN_BTR1, 0x1AU);
	can_wr(CAN_OCR, 0x02U);
	can_wr(CAN_IER, 0x00U);
	can_wr(CAN_MOD, CAN_MOD_NORMAL);

	last = rx_level();
	rx0 = (uint32_t)last;
	/*
	 * The SJA1000 will not leave "waiting for bus free" until RX0 has
	 * seen 11 recessive bits. PIN_38 is an input-only pin with no
	 * keeper: left floating it reads 0 (dominant) and the controller
	 * sits on a pending CMR.TR forever -- SR=0x30, ECC=0, TXERR=0 and
	 * *nothing* on PIN_39. That looks exactly like "the pin is dead".
	 */
	if (rx0 == 0U) {
		static int warned;

		if (warned == 0) {
			warned = 1;
			printk("pin: WARN RX0 (PIN_38) reads LOW/dominant -- "
			       "CAN never sees bus-free, TX will not start. "
			       "Floating input? Tie it high (jumper to PIN_39 "
			       "or the transceiver's R output).\n");
		}
	}
	t0 = k_uptime_get_32();
	while ((k_uptime_get_32() - t0) < ms) {
		if (gapped &&
		    (((k_uptime_get_32() - t0) % GAP_PERIOD_MS) >= GAP_ON_MS)) {
			/* silent part of the on/off envelope */
			k_msleep(5);
			continue;
		}
		can_wr(CAN_TXBUF + 0x00U, 0x08U);	/* SFF, DLC=8 */
		can_wr(CAN_TXBUF + 0x04U, (id >> 3) & 0xFFU);
		can_wr(CAN_TXBUF + 0x08U, (id << 5) & 0xE0U);
		for (int i = 0; i < 8; i++) {
			/* SFF data starts at reg 19 (driver: SFF_DATA), i.e.
			 * TXBUF + 4*3; reg 16/17/18 are info/ID1/ID2. */
			can_wr(CAN_TXBUF + (0x04U * (3U + (uint32_t)i)),
			       (i & 1) ? 0xAAU : 0x55U);
		}
		can_wr(CAN_CMR, CAN_CMR_TR);
		/*
		 * Sample RX0 (PIN_38, the transceiver's R output) for a
		 * fixed ~1 ms window instead of breaking out on SR.TCS.
		 * TCS latches and the loop exits after a handful of
		 * samples, which made pin38_edges look like 0 even when
		 * the bus was moving. The point of this loop is to count
		 * real edges on the pin, so it must run for at least one
		 * full 125 kbit/s frame (8 us/bit * ~90 bits).
		 */
		for (int n = 0; n < 3000; n++) {
			int v = rx_level();

			if (v != last) {
				edges++;
				last = v;
			}
		}
		/* Nothing is driving RX0, so the core errors out and may go
		 * bus-off; restart it so the LA sees a continuous burst.
		 */
		if ((can_rd(CAN_SR) & CAN_SR_BS) != 0U) {
			can_wr(CAN_MOD, CAN_MOD_RM);
			can_wr(CAN_MOD, CAN_MOD_NORMAL);
		}
		rx_frames += rx_drain();
		frames++;
	}

	printk("pin: CAN STM burst DIR=%u id=0x%03x: frames=%u pin38_edges=%u "
	       "SR=0x%02x ECC=0x%02x TXERR=%u RX0=%u rx=%u\n",
	       dir, id, frames, edges, can_rd(CAN_SR), can_rd(CAN_ECC),
	       can_rd(CAN_TXERR), rx0, rx_frames);
}

/*
 * Hold one half period of `usec`, sampling PIN_38 continuously and
 * counting any transition. The pin has no read-back, so PIN_38 (jumpered
 * to PIN_39) is the only way to confirm the driver is really active.
 */
static void drive_half(int level, uint32_t usec, uint32_t *edges, int *last)
{
	uint32_t left = usec;

	tx_level(level);
	while (left > 0U) {
		uint32_t step = MIN(left, 10U);

		for (uint32_t i = 0U; i < 8U; i++) {
			int v = rx_level();

			if (v != *last) {
				(*edges)++;
				*last = v;
			}
		}
		k_busy_wait(step);
		left -= step;
	}
}

static uint32_t probe_combo(uint32_t afsel, uint32_t dir, uint32_t halves)
{
	uint32_t edges = 0U;
	int last;

	set_afsel(CAN_TX0_BIT, afsel);
	set_dir(CAN_TX0_BIT, dir);
	last = rx_level();

	for (uint32_t i = 0U; i < halves; i++) {
		drive_half((int)(i & 1U), HALF_US, &edges, &last);
	}
	return edges;
}

struct pin_combo {
	uint32_t afsel;
	uint32_t dir;
};

static const struct pin_combo combos[] = {
	{ 0U, 0U },	/* GPIO mode, DIR=input  (inverted-OE candidate) */
	{ 0U, 1U },	/* GPIO mode, DIR=output (normal convention)    */
	{ 1U, 1U },	/* alternate fn, DIR=output                     */
	{ 1U, 0U },	/* alternate fn, DIR=input                      */
};

int main(void)
{
	int best = -1;
	uint32_t best_edges = 0U;

	printk("can_pin_drive: PIN_39 square wave (PIN_38 is input-only)\n");

	/* GPIO7/8 must be clocked before any of their registers respond. */
	sys_set_bits(SYS_APB_CLKENABLE,
		     SYS_APB_CLK_GPIO(7) | SYS_APB_CLK_GPIO(8));

	/* Reference output: GPIO8.0 (PIN_67) as a plain GPIO. Its pin OE is
	 * NOT inverted, so DIR=1 + toggling DATA must move the pin.
	 */
	set_afsel(REF_TX_BIT, 0U);
	sys_set_bits(GPIO8_BASE + GPIO_DIR_OFF, BIT(REF_TX_BIT));

	printk("pin: reference square wave on PIN_67 (GPIO8.%u, DIR=1)\n",
	       REF_TX_BIT);

	printk("pin: sweep AFSEL/DIR on GPIO8.%u, watching PIN_38 "
	       "(GPIO7.%u)\n", CAN_TX0_BIT, CAN_RX0_BIT);

	for (size_t i = 0U; i < ARRAY_SIZE(combos); i++) {
		uint32_t edges = probe_combo(combos[i].afsel, combos[i].dir,
					     SWEEP_HALVES);

		printk("pin: AF=%u DIR=%u -> pin38 edges=%u%s\n",
		       combos[i].afsel, combos[i].dir, edges,
		       edges > 0U ? "  <- drives" : "");
		if (edges > best_edges) {
			best_edges = edges;
			best = (int)i;
		}
	}

	if (best < 0) {
		printk("pin: WARN no combo moved PIN_38 -- jumper 39<->38? "
		       "defaulting to AF=0 DIR=0\n");
		best = 0;
	} else {
		printk("pin: using AF=%u DIR=%u (edges=%u)\n",
		       combos[best].afsel, combos[best].dir, best_edges);
	}

	/*
	 * Slow, announced square wave: 3 s low + 3 s high per (AFSEL, DIR)
	 * config, cycling through all four. Lift the 38<->39 jumper and a
	 * DMM on PIN_39 should follow the "PIN_39=LOW/HIGH" lines below.
	 */
	/*
	 * Definitive pin test: continuous CAN frames on PIN_39 in self-test
	 * mode, alternating the pin DIR every HOLD_MS. Put the LA on PIN_39
	 * and note which burst produces the 125 kbit/s frame train.
	 */
	printk("pin: CAN burst mode -- LA on PIN_39\n");
	printk("pin:   DIR=1 -> id 0x111, 3 s CONTINUOUS  (one solid bar)\n");
	printk("pin:   DIR=0 -> id 0x222, 3 s of 200 ms on / 800 ms off "
	       "(three blobs)\n");
	printk("pin:   zoom out to a >=10 s span, or use the CAN decoder at "
	       "125000 bit/s\n");
	printk("pin:   GPIO mode cannot drive this pin; AF/CAN only\n");
	for (;;) {
		printk("pin: ==== CAN DIR=1 (id 0x111, continuous) ====\n");
		can_stm_burst(1U, 0x111U, HOLD_MS, 0);
		k_msleep(WINDOW_GAP_MS);
		printk("pin: ==== CAN DIR=0 (id 0x222, 200ms/800ms blobs) ====\n");
		can_stm_burst(0U, 0x222U, HOLD_MS, 1);
		k_msleep(WINDOW_GAP_MS);
	}
}
