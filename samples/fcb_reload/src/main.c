/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * fcb_reload — reload the bitstream *from FLASH*, the boot way, on agrv2k_407.
 *
 * The reload this sample demonstrates is: change the image at
 * FCB_BITSTREAM_ADDR, reboot, watch the fabric change. It is the *boot* path
 * on purpose — on AgRV2K the CPU's clock tree is produced by the fabric
 * itself, so a DEACTIVATE/AutoConfig/ACTIVATE sequence run as-is freezes the
 * core mid-instruction, takes the console with it and leaves the SWD AP
 * stalled. The 2026-09-14 dev board ended exactly there; see the block comment in
 * soc/agm/agrv2k/fcb.c and the port record (peripherals) 3.8.1.
 *
 * The runtime version now exists: samples/fcb_hotswap parks the CPU on HSI
 * (a clock the fabric does not own) for the window, validates the target
 * image first, and re-hands the clock tree + pin routes to the new fabric.
 * Measured both directions 2026-09-14 — the port record (peripherals) 3.8.3.
 *
 * So this sample is a probe: it prints the reset reason, the FCB STAT and a
 * SPI NOR RDID, which is all the dev board needs to tell which bitstream is live.
 *
 * Bench (both bitstreams are 100 MHz, hence the 100 MHz overlay):
 *
 *   bash tools/flash_logic.sh $HOME/spi_full_bitstream/example_board.bin
 *   bash tools/test_uart_capture.sh -n -t 8 <this sample's zephyr.bin>
 *     -> FCB boot: STAT=0x... (ACTIVE), RDID = C8 40 16 (flash on SPI0)
 *
 *   bash tools/flash_logic.sh \
 *        $HOME/spi_full_bitstream_without_flash/example_board.bin
 *   bash tools/test_uart_capture.sh -n -t 8        # same firmware!
 *     -> FCB boot: STAT=0x... (ACTIVE), RDID = FF FF FF (SPI0 goes nowhere)
 *
 * Pass: both boots report FCB ACTIVE, and the RDID follows the *bitstream*
 * while the firmware stays byte-identical. That is the whole point — the
 * fabric content follows FLASH, and the CPU came up normally on both.
 *
 * Commands: s = re-probe, q = halt here (SWD attach point), h = help.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <stdint.h>

/* soc/agm/agrv2k/fcb.c — the same entry point soc.c uses at boot. */
extern void agrv2k_fcb_log_status(const char *tag);

/* Trimmed copy of samples/spi_flash_id — see src/spi_id.c. */
extern int fcb_reload_rdid(uint8_t out[3]);

/* SYS controller reset-flag register (RST_CNTL, bit 24 = write-1-to-clear). */
#define SYS_RST_CNTL   0x03000004UL
#define SYS_RST_REMOVE BIT(24)

/*
 * Which part answers depends on the board and on which controller the
 * bitstream routes to it, so the probe reports what it reads instead of
 * asserting one part number:
 *   C8 40 16  GD25Qxx (the flash this dev board normally has on SPI0)
 *   68 40 15  W25Q16 (what samples/spi_flash_id sees on the other bitstream)
 *   00 00 00  nothing answering with the line held low (measured on
 *             spi_full_bitstream_without_flash)
 *   FF FF FF  nothing answering, line floating high
 */
static const char *rdid_part(uint8_t a, uint8_t b, uint8_t c)
{
	if (a == 0xC8U && b == 0x40U && c == 0x16U) {
		return "GD25Qxx";
	}
	if (a == 0x68U && b == 0x40U && c == 0x15U) {
		return "W25Q16";
	}
	if (a == 0x00U && b == 0x00U && c == 0x00U) {
		return "nothing answering (MISO held low)";
	}
	if (a == 0xFFU && b == 0xFFU && c == 0xFFU) {
		return "nothing answering (MISO floating high)";
	}
	return "unexpected";
}

static const struct device *console_dev(void)
{
	return DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
}

static void probe(const char *tag)
{
	uint8_t rdid[3] = { 0U, 0U, 0U };
	int rc = fcb_reload_rdid(rdid);

	/* FCB STAT first: it is the one line that says whether the
	 * boot-time fabric load succeeded.
	 */
	agrv2k_fcb_log_status(tag);

	if (rc == 0) {
		printk("fcb_reload [%s]: RDID = %02x %02x %02x (%s)\n",
		       tag, rdid[0], rdid[1], rdid[2],
		       rdid_part(rdid[0], rdid[1], rdid[2]));
	} else {
		/* Legitimate when the bitstream routes SPI0 nowhere: the TX
		 * side still completes, the RX side reports -EIO.
		 */
		printk("fcb_reload [%s]: RDID read failed (%d) — SPI0 not driven\n",
		       tag, rc);
	}
}

static void cmd_help(void)
{
	printk("fcb_reload commands:\n");
	printk("  s  re-probe: FCB STAT + SPI0 RDID\n");
	printk("  q  halt here — SWD attach point\n");
	printk("  h  this menu\n");
	printk("note: reloading a bitstream = flash_logic.sh + reboot;\n");
	printk("      there is no runtime hot-swap (see README.md)\n");
}

static void handle_cmd(char c)
{
	switch (c) {
	case 's':
	case 'S':
		probe("s");
		break;
	case 'q':
	case 'Q':
		printk("fcb_reload: halted — SWD may attach\n");
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
	const struct device *uart = console_dev();
	uint32_t rst;
	char c;

	if (!device_is_ready(uart)) {
		for (;;) {
			k_msleep(1000);
		}
	}

	rst = sys_read32(SYS_RST_CNTL);
	printk("\nfcb_reload: bitstream-from-FLASH probe (boot path)\n");
	printk("fcb_reload: RST_CNTL=0x%08x\n", rst);
	/* Clear the sticky reset flags so the next run starts clean. */
	sys_write32(rst | SYS_RST_REMOVE, SYS_RST_CNTL);

	probe("boot");
	cmd_help();
	printk("fcb_reload> ");

	for (;;) {
		if (uart_poll_in(uart, &c) == 0) {
			handle_cmd(c);
			printk("fcb_reload> ");
		}
		k_msleep(10);
	}
}
