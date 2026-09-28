/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * fcb_hotswap — reload the FPGA fabric at runtime, without rebooting.
 *
 * Two bitstreams live in FLASH:
 *   slot A (0x800e7000, the boot slot): what the FLASH option byte
 *           "FPGA CONFIG" points at, so it is what the board runs after a
 *           reset — written with tools/flash_logic.sh;
 *   slot B (0x800cd000, the spare):   never booted, written with
 *           tools/agm_oo.sh fw <image.bin> 0x800cd000 (the real fabric
 *           update slot 2; the default was 0x800c0000 until 2026-09-24,
 *           which overlapped fabric slot 1, the bind-salt sector and slot 2
 *           -- see docs/FLASH-LAYOUT.md).
 *
 * Pressing 'x' swaps the live fabric to the other slot:
 *
 *   1. validate the target image (CRC-32/BZIP2 + IDCODE) — a rejected image
 *      must never reach ACTIVATE, because that is the point of no return;
 *   2. agrv2k_clk_switch_hsi() — move the CPU onto the on-die RC clock, the
 *      only clock that does not come out of the fabric;
 *   3. agrv2k_fcb_reload() — DEACTIVATE / stream 24986 words / ACTIVATE;
 *   4. agrv2k_clk_switch_pll() + re-assert pinctrl — hand the clock tree and
 *      the pin routes to the new fabric.
 *
 * Nothing is rebooted, so the evidence is a *pair* of observations:
 *   - the same k_uptime keeps counting and no boot banner appears, while
 *   - the SPI NOR RDID probe changes with the fabric (slot A on this dev board
 *     routes SPI0 to the flash: 68 40 15 W25Q16; slot B routes it nowhere:
 *     00 00 00) — for identical firmware.
 *
 * Constraints (the port record (peripherals) 3.8.3): both images have to be
 * the same clock class and keep the console pin route, and the swap window is
 * several ms in which the console can drop out. If the fabric never comes
 * back, SWD is gone too; the way out is BOOT0 + agrv32flash.
 *
 * Commands: x = swap, i = HSI round trip only, s = probe, q = halt, h = help.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/agm_bitstream.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdint.h>

/* soc/agm/agrv2k/{fcb,clk}.c — the same entry points soc.c uses at boot. */
extern int agrv2k_fcb_reload(uint32_t flash_addr);
extern void agrv2k_fcb_log_status(const char *tag);
extern void agrv2k_clk_switch_hsi(void);
extern void agrv2k_clk_switch_pll(uint32_t pll_hz, uint32_t flash_max_hz);

/* src/slot.c */
extern int slot_verify(uint32_t addr, uint32_t *idcode);
extern int slot_rdid(uint8_t out[3]);
extern const char *slot_rdid_name(const uint8_t rdid[3]);

/* Slot addresses come from the devicetree (boards/agrv2k_407.overlay):
 * DT_REG_ADDR() on a fixed-partitions child is the address itself on this
 * tree (see the comment there for why it is not the *_ADDR macro that adds
 * the parent's base). Both are pinned to the addresses the FCB and the FLASH
 * option byte actually use -- a partition that drifted must fail the build,
 * not write into whatever else lives there.
 */
#define SLOT_A_ADDR DT_REG_ADDR(DT_NODELABEL(hotswap_slot_a))
#define SLOT_B_ADDR DT_REG_ADDR(DT_NODELABEL(hotswap_slot_b))

BUILD_ASSERT(SLOT_A_ADDR == (uint32_t)AGM_BITSTREAM_FACTORY_ADDR,
	     "slot A has to be the factory slot the option byte points at");
BUILD_ASSERT(SLOT_B_ADDR == (uint32_t)AGM_BITSTREAM_SLOT2_ADDR,
	     "slot B has to be one of the fabric update slots (slot 2)");

#define SYS_BASE            0x03000000UL
#define SYS_RST_CNTL        0x04UL
#define SYS_APB_CLKENABLE   0x60UL
#define SYS_RST_REMOVE      BIT(24)

/* Same two devicetree values soc.c uses for its clock switch (board dts /
 * overlay), read through the sys node's phandle. */
static const uint32_t pll_hz =
	DT_PROP_BY_PHANDLE(DT_NODELABEL(sys), clocks, clock_frequency);
static const uint32_t flash_max_hz =
	DT_PROP(DT_NODELABEL(sys), flash_max_frequency);

/*
 * Pin routes to re-assert after the fabric changes. The new bitstream owns
 * AFSEL/DIR at ACTIVATE, and both the console and the SPI probe pins have to
 * come back before the sample can report anything. Each device's pinctrl
 * state is defined here (the drivers' own configs are static to their
 * translation units), so this still goes through pinctrl_configure_pins() —
 * nothing in this sample writes AFSEL itself.
 *
 * The nodes are named explicitly rather than derived with
 * DT_FOREACH_STATUS_OKAY: that macro wraps each expansion in COND_CODE_1,
 * which cannot emit declarations (it only works in expression position).
 */
PINCTRL_DT_DEFINE(DT_NODELABEL(uart0));
PINCTRL_DT_DEFINE(DT_NODELABEL(spi0));

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi1), okay)
PINCTRL_DT_DEFINE(DT_NODELABEL(spi1));
#endif

static const struct pinctrl_dev_config *const pinned[] = {
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(uart0)),
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(spi0)),
#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi1), okay)
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(spi1)),
#endif
};

static uint32_t live_slot = SLOT_A_ADDR;
static uint32_t swap_count;

static const char *slot_desc(int rc)
{
	switch (rc) {
	case 0:
		return "ok";
	case -ENODATA:
		return "empty (erased)";
	case -EILSEQ:
		return "checksum mismatch";
	default:
		return "unreadable";
	}
}

static void report_slots(void)
{
	uint32_t idcode = 0U;
	int rc;

	rc = slot_verify(SLOT_A_ADDR, &idcode);
	printk("hotswap: slot A @0x%08x boot slot  : %s", SLOT_A_ADDR,
	       slot_desc(rc));
	if (rc == 0) {
		printk(" (idcode=0x%08x)", idcode);
	}
	printk("\n");

	rc = slot_verify(SLOT_B_ADDR, &idcode);
	printk("hotswap: slot B @0x%08x spare      : %s", SLOT_B_ADDR,
	       slot_desc(rc));
	if (rc == 0) {
		printk(" (idcode=0x%08x)", idcode);
	}
	printk("\n");
}

/*
 * One line of "what is live right now": FCB verdict, uptime, SYS state and
 * the SPI probe. Called before and after a swap so the two can be diffed.
 */
static void fabric_state(const char *tag)
{
	uint8_t rdid[3] = { 0U, 0U, 0U };
	int rc = slot_rdid(rdid);

	agrv2k_fcb_log_status(tag);
	printk("hotswap [%s]: uptime=%u ms rst=0x%08x apb=0x%08x "
	       "live=0x%08x rdid=%02x %02x %02x (%s)\n",
	       tag, (unsigned int)k_uptime_get_32(),
	       sys_read32(SYS_BASE + SYS_RST_CNTL),
	       sys_read32(SYS_BASE + SYS_APB_CLKENABLE),
	       live_slot, rdid[0], rdid[1], rdid[2],
	       (rc == 0) ? slot_rdid_name(rdid) : "probe failed");
}

static void repin(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(pinned); i++) {
		int rc = pinctrl_apply_state(pinned[i], PINCTRL_STATE_DEFAULT);

		if (rc != 0) {
			printk("hotswap: pinctrl state %u re-apply failed (%d)\n",
			       (unsigned int)i, rc);
		}
	}
}

static void hot_swap(void)
{
	uint32_t dst = (live_slot == SLOT_A_ADDR) ? SLOT_B_ADDR : SLOT_A_ADDR;
	const char *dst_name = (dst == SLOT_B_ADDR) ? "B" : "A";
	uint32_t idcode = 0U;
	uint32_t t0;
	uint32_t n = swap_count + 1U;
	int rc;

	printk("\nhotswap: swap #%u: 0x%08x -> 0x%08x (slot %s)\n",
	       (unsigned int)n, live_slot, dst, dst_name);

	/* Gate first: this is the same check the silicon does at ACTIVATE,
	 * but it costs nothing to fail here. */
	rc = slot_verify(dst, &idcode);
	if (rc != 0) {
		printk("hotswap: slot %s rejected (%s) — fabric untouched, "
		       "still on 0x%08x\n", dst_name, slot_desc(rc), live_slot);
		return;
	}
	printk("hotswap: slot %s verified (idcode=0x%08x, crc ok)\n",
	       dst_name, idcode);

	/* Everything up to the clock switch back runs with the console
	 * expected to drop out: the pins it prints through belong to the
	 * fabric being replaced. No printk in the window. */
	printk("hotswap: sys_clk -> HSI (CPU off the fabric clock)\n");
	t0 = k_uptime_get_32();
	agrv2k_clk_switch_hsi();

	rc = agrv2k_fcb_reload(dst);

	agrv2k_clk_switch_pll(pll_hz, flash_max_hz);
	repin();

	if (rc == 0) {
		live_slot = dst;
		swap_count = n;
	}
	printk("\nhotswap: FCB reload %s (rc=%d), window %u ms\n",
	       (rc == 0) ? "completed" : "FAILED", rc,
	       (unsigned int)(k_uptime_get_32() - t0));

	fabric_state("post");
	if (rc == 0) {
		printk("hotswap: slot %s is live; uptime kept counting and no "
		       "boot banner appeared, so the core was never reset — "
		       "that is the hot swap\n", dst_name);
	}
}

/*
 * HSI round trip with no fabric change: isolates "can this core run off a
 * clock that does not come from the fabric?" from everything else. If the
 * console stays alive here but dies in 'x', the DEACTIVATE is the culprit.
 */
static void hsi_roundtrip(void)
{
	uint32_t t0 = k_uptime_get_32();

	printk("\nhotswap: HSI round trip (no fabric change)\n");
	agrv2k_clk_switch_hsi();
	printk("hotswap: on HSI, still executing (uptime=%u ms, %u ms since "
	       "the request)\n", (unsigned int)k_uptime_get_32(),
	       (unsigned int)(k_uptime_get_32() - t0));
	k_msleep(10);
	printk("hotswap: alive after a 10 ms sleep on HSI (uptime=%u ms)\n",
	       (unsigned int)k_uptime_get_32());
	agrv2k_clk_switch_pll(pll_hz, flash_max_hz);
	printk("hotswap: back on the fabric PLL (uptime=%u ms)\n",
	       (unsigned int)k_uptime_get_32());
}

static void cmd_help(void)
{
	printk("hotswap commands:\n");
	printk("  x  hot swap to the other slot (validated first)\n");
	printk("  i  HSI round trip only — no fabric change\n");
	printk("  s  probe: FCB STAT + SPI RDID + both slot verdicts\n");
	printk("  q  halt here — SWD attach point\n");
	printk("  h  this menu\n");
}

static void handle_cmd(char c)
{
	switch (c) {
	case 'x':
	case 'X':
		hot_swap();
		break;
	case 'i':
	case 'I':
		hsi_roundtrip();
		break;
	case 's':
	case 'S':
		report_slots();
		fabric_state("s");
		break;
	case 'q':
	case 'Q':
		printk("hotswap: halted — SWD may attach\n");
		for (;;) {
			k_msleep(1000);
		}
	case 'h':
	case 'H':
	case '?':
		cmd_help();
		break;
	case '\r':
	case '\n':
		break;
	default:
		printk("? '%c' (press h for help)\n", c);
		break;
	}
}

int main(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t rst;
	char c;

	if (!device_is_ready(uart)) {
		for (;;) {
			k_msleep(1000);
		}
	}

	rst = sys_read32(SYS_BASE + SYS_RST_CNTL);
	printk("\n=== fcb_hotswap: runtime fabric reload (no reboot) ===\n");
	printk("hotswap: sys pll=%u Hz flash_max=%u Hz rst=0x%08x apb=0x%08x\n",
	       (unsigned int)pll_hz, (unsigned int)flash_max_hz, rst,
	       sys_read32(SYS_BASE + SYS_APB_CLKENABLE));
	/* Clear the sticky reset flags so the next run starts clean. */
	sys_write32(rst | SYS_RST_REMOVE, SYS_BASE + SYS_RST_CNTL);

	report_slots();
	fabric_state("boot");
	cmd_help();
	printk("hotswap> ");

	for (;;) {
		if (uart_poll_in(uart, &c) == 0) {
			handle_cmd(c);
			printk("hotswap> ");
		}
		k_msleep(10);
	}
}
