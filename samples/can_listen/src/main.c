/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * can_listen — AgRV2K CAN0 RX monitor for a single-peer dev board bus.
 *
 * Target setup:  AgRV PIN_38/39 <-> SN65HVD230 <-> SN65HVD230 <-> ESP32-C3
 * Peer:          $HOME/esp32c3-can (sends id=0x456 every 500 ms)
 *
 * Three findings shape this sample (the port status §3.18,
 * the pinout reference §11.2;CAN controller block diagrams,filter
 * bank layout and bit fields are documented inline in the AgRV2K
 * vendor reference manual (chapter 20) and in the SDK's `can.h`):
 *
 *  1. PIN_38/39 ARE the CAN0 pins AND they are reachable from the GPIO
 *     banks. The bitstream in use wires
 *         gpio7_io_in[3]        = PIN_38_in     (CAN0_RX0, read-only)
 *         gpio8_io_out_data[7]  = PIN_39_out    (CAN0_TX0, OE inverted)
 *     per ~/spi_full_mac_bitstream_200mhz/example_board.v (the vendor
 *     reference netlist the flashed bitstream corresponds to, lines 114-120;
 *     a locally generated <build>/logic/board.vx is NOT compile-verified --
 *     cite the reference, not it)
 *     and the CAN core is routed onto them through the pinctrl state
 *     can0_default (agm,pins = AGM_PINCTRL(7, 3, INPUT),
 *     AGM_PINCTRL(8, 7, OUTPUT)).
 *     An earlier revision of this sample claimed the pins were "NOT on a
 *     GPIO bank", that board.ve had no GPIO7/8 pin entry, and that
 *     GPIO7.3 could therefore never observe PIN_38. All of that came
 *     from reading board.ve instead of board.vx and is **wrong** -- see
 *     samples/can_pin_drive, which reads PIN_38 back through GPIO7.3.
 *
 *  2. LISTEN-ONLY *does* receive. A LOM node cannot ACK, so a lone
 *     transmitter keeps erroring (ACK error -> retries -> eventual
 *     bus-off) -- but the frames still land in the receiver's RX FIFO.
 *     Measured 2026-09-12: an ESP32-C3 TWAI in LISTEN_ONLY mode decoded
 *     2615 consecutive id=0x123 frames from an AG32 that never received
 *     an ACK, so the older claim (AG32_CAN_notes.md §2) that LOM
 *     "cannot receive from a lone transmitter" is wrong.
 *     This sample still defaults to NORMAL + accept-all: the controller
 *     ACKs in hardware (so the peer's TX completes) while the
 *     application never requests a transmission. USE_LISTEN_ONLY=1
 *     overrides.
 *
 *  3. RXERR is the bit-timing tell-tale. In NORMAL mode a baud mismatch
 *     shows up as climbing RXERR/ECC with little or no rx_count, while
 *     "RX0 sees nothing at all" keeps RXERR at 0 and SR unchanged.
 *
 * Wiring (transceiver on the AgRV side):
 *   AgRV PIN_38 (CAN0_RX0) <- transceiver R  (pin 4)
 *   AgRV PIN_39 (CAN0_TX0) -> transceiver D  (pin 1)
 *   CAN_H / CAN_L <-> peer, common GND required.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define RX_FRAMES	32U

/* Set to 1 only when a second ACKing node is already on the bus. */
#define USE_LISTEN_ONLY	0

/*
 * GPIO7 bank: only used to *document* that RX0 is not routable here.
 * Stellaris-style GPIO: DATA is a 256-entry masked array addressed by
 * base | (mask << 2). Offset 0 == mask 0 always reads 0, so a full 8-bit
 * port read must use mask 0xff. DIR/AFSEL are plain registers.
 */
#define AGM_SYS_APB_CLKENABLE	0x03000060UL
#define AGM_SYS_APB_CLK_GPIO(n)	(1UL << (4U + (n)))
#define AGM_GPIO7_BASE		0x4001B000UL
#define AGM_GPIO8_BASE		0x4001C000UL
#define GPIO_DATA_MASK_OFF(mask)	((mask) << 2)
#define GPIO_DIR_OFF		0x400U
#define GPIO_AFSEL_OFF		0x420U
#define CAN_RX0_PIN_BIT		3U

/* Full 8-bit port read: DATA is addressed by base | (mask << 2), and
 * offset 0 (mask 0) always reads 0. */
#define AGM_GPIO7_DATA_MASK	(AGM_GPIO7_BASE + GPIO_DATA_MASK_OFF(0xFFU))

/* SJA1000 register file, 8-bit registers on a 32-bit word stride. */
#define AGRV_CAN0_BASE		0x4002A000UL
#define CAN_REG_MOD		0x00U
#define CAN_REG_CMR		0x04U
#define CAN_REG_SR		0x08U
#define CAN_REG_IR		0x0CU
#define CAN_REG_IER		0x10U
#define CAN_REG_BTR0		0x18U
#define CAN_REG_BTR1		0x1CU
#define CAN_REG_ECC		0x30U
#define CAN_REG_RXERR		0x38U
#define CAN_REG_TXERR		0x3CU

static const struct device *const can = DEVICE_DT_GET(DT_NODELABEL(can0));

CAN_MSGQ_DEFINE(rx_msgq, RX_FRAMES);
static volatile uint32_t rx_count;

/*
 * Pin probe: while AFSEL bit3 is cleared GPIO DATA would reflect the pin
 * if PIN_38 were routed to GPIO7_3. The counter is the answer to "does
 * PIN_38 actually toggle?", independent of the CAN engine/bit timing.
 * It is only meaningful in the GPIO phase (see main()).
 */
static volatile uint32_t pin_edges;
static volatile uint32_t pin_rises;
static volatile uint32_t pin_level;
static volatile bool pin_probe_active;

static void pin_probe_thread(void *p1, void *p2, void *p3)
{
	uint32_t prev = (sys_read32(AGM_GPIO7_DATA_MASK) >> CAN_RX0_PIN_BIT) & 1U;
	uint32_t iter = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint32_t now = (sys_read32(AGM_GPIO7_DATA_MASK) >> CAN_RX0_PIN_BIT) & 1U;

		pin_level = now;
		if (pin_probe_active && now != prev) {
			pin_edges++;
			if (now != 0U) {
				pin_rises++;
			}
		}
		prev = now;

		if ((++iter & 0x3FFU) == 0U) {
			k_yield();
		}
	}
}

K_THREAD_DEFINE(pin_tid, 1024, pin_probe_thread, NULL, NULL, NULL, 7, 0, 0);

static inline uint32_t can_reg(uint32_t off)
{
	return sys_read32(AGRV_CAN0_BASE + off) & 0xFFU;
}

/* Enable the GPIO7/8 APB clocks (the legacy AFSEL writes need them) and
 * clear AFSEL so GPIO DATA would show the pin if one were routed. */
static void prepare_pin_readback(void)
{
	sys_set_bits(AGM_SYS_APB_CLKENABLE,
		     AGM_SYS_APB_CLK_GPIO(7) | AGM_SYS_APB_CLK_GPIO(8));
	sys_write32(sys_read32(AGM_GPIO7_BASE + GPIO_AFSEL_OFF) & ~BIT(CAN_RX0_PIN_BIT),
		    AGM_GPIO7_BASE + GPIO_AFSEL_OFF);
	sys_write32(sys_read32(AGM_GPIO8_BASE + GPIO_AFSEL_OFF) & ~BIT(7),
		    AGM_GPIO8_BASE + GPIO_AFSEL_OFF);
}

static void report_pins(void)
{
	uint32_t gpio7_data = sys_read32(AGM_GPIO7_BASE + GPIO_DATA_MASK_OFF(0xFF));
	uint32_t gpio7_dir = sys_read32(AGM_GPIO7_BASE + GPIO_DIR_OFF);
	uint32_t gpio7_af = sys_read32(AGM_GPIO7_BASE + GPIO_AFSEL_OFF);
	uint32_t apb = sys_read32(AGM_SYS_APB_CLKENABLE);

	printk("can_listen: gpio7 DATA(mask=0xff)=0x%08x DIR=0x%08x AFSEL=0x%08x\n",
	       gpio7_data, gpio7_dir, gpio7_af);
	printk("can_listen: apb=0x%08x ; CAN0_RX0 is fabric-routed to PIN_38 "
	       "(no GPIO7_3 pin in board.ve) -> GPIO bit3 is NOT observable\n", apb);
}

static void dump_state(const char *tag)
{
	enum can_state state = CAN_STATE_STOPPED;
	struct can_bus_err_cnt err = { 0U, 0U };
	const char *name = "?";

	if (can_get_state(can, &state, &err) != 0) {
		err.tx_err_cnt = 0U;
		err.rx_err_cnt = 0U;
	}

	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		name = "ACTIVE";
		break;
	case CAN_STATE_ERROR_WARNING:
		name = "WARN";
		break;
	case CAN_STATE_ERROR_PASSIVE:
		name = "PASSIVE";
		break;
	case CAN_STATE_BUS_OFF:
		name = "BUS_OFF";
		break;
	case CAN_STATE_STOPPED:
		name = "STOPPED";
		break;
	default:
		break;
	}

	printk("can_listen[%s] rx=%u pin_e=%u pin_r=%u pin=%u state=%s ercnt=%u/%u | "
	       "MOD=%02x CMR=%02x SR=%02x IR=%02x IER=%02x "
	       "BTR0=%02x BTR1=%02x ECC=%02x RXERR=%02x TXERR=%02x\n",
	       tag, rx_count, pin_edges, pin_rises, pin_level, name,
	       err.tx_err_cnt, err.rx_err_cnt,
	       can_reg(CAN_REG_MOD), can_reg(CAN_REG_CMR), can_reg(CAN_REG_SR),
	       can_reg(CAN_REG_IR), can_reg(CAN_REG_IER), can_reg(CAN_REG_BTR0),
	       can_reg(CAN_REG_BTR1), can_reg(CAN_REG_ECC),
	       can_reg(CAN_REG_RXERR), can_reg(CAN_REG_TXERR));
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	struct can_frame frame;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		if (k_msgq_get(&rx_msgq, &frame, K_FOREVER) != 0) {
			continue;
		}

		rx_count++;
		printk("RX[%u] id=0x%03x dlc=%u ext=%d rtr=%d data=",
		       rx_count, frame.id, frame.dlc,
		       (frame.flags & CAN_FRAME_IDE) ? 1 : 0,
		       (frame.flags & CAN_FRAME_RTR) ? 1 : 0);
		for (uint8_t i = 0U; i < frame.dlc; i++) {
			printk("%02x", frame.data[i]);
		}
		printk("\n");
	}
}

K_THREAD_DEFINE(rx_tid, 1024, rx_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	struct can_filter filter = {
		.id    = 0U,
		.mask  = 0U,
		.flags = 0U,
	};
	int rc;

	prepare_pin_readback();
	report_pins();

	/*
	 * Phase 1 (GPIO): AFSEL bit3 was cleared by prepare_pin_readback(),
	 * so the pin probe counts real PIN_38 edges if the bitstream routes
	 * CAN0_RX0 to GPIO7_3. A peer transmitting now (or a PIN_39<->PIN_38
	 * jumper) gives pin_e > 0; it stays 0 otherwise.
	 */
	pin_probe_active = true;
	printk("can_listen: PAD PROBE 6 s (AFSEL bit3=0): count PIN_38 edges now\n");
	k_sleep(K_SECONDS(6));
	pin_probe_active = false;
	printk("can_listen: PAD PROBE done: pin_e=%u pin_r=%u pin=%u "
	       "(0 => PIN_38 not visible on GPIO7_3, or no traffic)\n",
	       pin_edges, pin_rises, pin_level);

	/* Phase 2 (engine): hand the pin back to CAN0_RX0. */
	sys_set_bits(AGM_GPIO7_BASE + GPIO_AFSEL_OFF, BIT(CAN_RX0_PIN_BIT));
	sys_set_bits(AGM_GPIO8_BASE + GPIO_AFSEL_OFF, BIT(7));

	if (!device_is_ready(can)) {
		printk("can_listen: FAIL can0 not ready\n");
		return 0;
	}

#if USE_LISTEN_ONLY
	rc = can_set_mode(can, CAN_MODE_LISTENONLY);
#else
	rc = can_set_mode(can, CAN_MODE_NORMAL);
#endif
	if (rc != 0 && rc != -EBUSY) {
		printk("can_listen: FAIL can_set_mode rc=%d\n", rc);
		return 0;
	}

	rc = can_start(can);
	if (rc != 0 && rc != -EAGAIN) {
		printk("can_listen: FAIL can_start rc=%d\n", rc);
		return 0;
	}

	rc = can_add_rx_filter_msgq(can, &rx_msgq, &filter);
	if (rc < 0) {
		printk("can_listen: FAIL add std filter rc=%d\n", rc);
		return 0;
	}

	filter.flags = CAN_FILTER_IDE;
	rc = can_add_rx_filter_msgq(can, &rx_msgq, &filter);
	if (rc < 0) {
		printk("can_listen: warn add ext filter rc=%d\n", rc);
	}

#if USE_LISTEN_ONLY
	printk("can_listen: LISTEN-ONLY, accept-all; waiting for bus traffic\n");
#else
	printk("can_listen: NORMAL (ACK on, no app TX), accept-all; waiting for bus traffic\n");
#endif
	dump_state("up");

	for (;;) {
		k_sleep(K_SECONDS(1));
		dump_state("tick");
	}
}
