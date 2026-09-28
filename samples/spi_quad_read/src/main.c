/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi_quad_read — exercise the generic SPI API with SPI_LINES_DUAL/QUAD.
 *
 * The AgRV2K phase engine has three documented constraints (vendor
 * "AG32 下 SPI 的扩展使用", page 1):
 *
 *   1. a transfer starts with TX (never RX);
 *   2. TX and RX do not overlap (TX first, then RX);
 *   3. RX is the last phase.
 *
 * The driver-level multiline wrapper
 * (spi_agm_transceive_multiline(), inside drivers/spi/spi_agm.c) turns a
 * generic transceive() request that asked for SPI_LINES_DUAL/QUAD into a
 * phase list with one single-line TX phase for the command+address, an
 * optional single-line DUMMY phase for the wait states, and one RX phase
 * on the requested line mode. This sample verifies three of those shapes
 * against the on-board SPI NOR flash:
 *
 *   - RDID (0x9F) on single line: the baseline. No dummy phase, 3 data
 *     bytes back. The same path used by samples/spi_flash_id, which
 *     proves the controller and bitstream are wired up.
 *   - 0x3B on dual lines: 1-byte command + 3-byte address + 8 dummy
 *     clocks (the wrapper always inserts one byte of filler between the
 *     command+address phase and the data phase, which is the 8-clock
 *     wait state the W25Q16 family of 0x3B/0x6B/0xEB fast reads wants)
 *     + N data bytes on MOSI+MISO. Reads at 0x000000 by default.
 *   - 0x6B on quad lines: same shape, four data lines. Reads at 0x000000
 *     by default.
 *
 * All three are read-only and target the same flash region, so they
 * leave the flash content untouched and cross-check each other: the
 * three patterns must read the same first three bytes (JEDEC ID).
 *
 * Two things have to be in place for the multi-line half to run at all:
 * the bitstream must route IO2/IO3 to the flash's WP#/HOLD# pins and the
 * node must declare `agm,spi-multiline` (see the board overlay), and
 * CONFIG_SPI_EXTENDED_MODES must be set (see prj.conf) -- without it
 * spi_operation_t is 16 bits wide, the SPI_LINES_* bits are truncated at
 * compile time and the request silently stays on the single-line path.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if !DT_HAS_COMPAT_STATUS_OKAY(agm_agrv2k_spi)
#error "no agm,agrv2k-spi node is enabled: enable one in the sample overlay"
#endif

#define SPI_AGM_DEV_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const spi_controllers[] = {
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, SPI_AGM_DEV_ENTRY)
};

/* JEDEC commands the multiline path accepts (single-line cmd+addr on the
 * first phase, one dummy byte = 8 clocks of wait state, then data on the
 * requested line mode). */
#define SPI_FLASH_CMD_RDID_SINGLE 0x9FU
#define SPI_FLASH_CMD_READ_SINGLE 0x03U
#define SPI_FLASH_CMD_READ_DUAL   0x3BU
#define SPI_FLASH_CMD_READ_QUAD   0x6BU

/* 1 MHz is a safe bring-up rate for an unknown board. */
#define SPI_FLASH_FREQ            1000000U

#define SPI_FLASH_READ_LEN        16U

/* The sample picks its own address: it walks the page starts of the first
 * sector and uses the first 16-byte window that is not all 0xff. A blank
 * window makes "the reads agree" meaningless -- the multiline bring-up
 * once reported a false pass on an erased page
 * (the port record (peripherals) 3.27.9) -- so a fully erased sector is
 * reported as INCONCLUSIVE rather than as a pass.
 *
 * The dev board's usual content is what samples/spi_flash_rw leaves behind
 * (its page pattern at base + 3 * 256), which this scan finds on its own.
 */
#define SPI_FLASH_SCAN_BYTES      0x1000U
#define SPI_FLASH_SCAN_STEP       0x100U

static void print_bytes(const char *tag, const uint8_t *buf, size_t len)
{
	printk("%s =", tag);
	for (size_t i = 0U; i < len; i++) {
		printk(" %02x", buf[i]);
	}
	printk("\n");
}

/* One single-line RDID read -- this is the shape spi_flash_id exercises
 * and is the baseline we expect to keep working after the multiline path
 * is added. The driver returns -ENOTSUP for SPI_LINES_QUAD when the
 * bitstream's agm,spi-multiline property is 0 (the shipped
 * example_board.bin), so this sample asserts the property is set: see
 * boards/agrv2k_407.overlay.
 */
static int run_single_rdid(const struct device *spi, uint8_t *rdid)
{
	const uint8_t cmd = SPI_FLASH_CMD_RDID_SINGLE;
	struct spi_buf tx_buf = { .buf = (void *)&cmd, .len = 1U };
	struct spi_buf rx_bufs[2] = {
		{ .buf = NULL, .len = 1U },
		{ .buf = rdid, .len = 3U },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };
	const struct spi_config cfg = {
		.frequency = SPI_FLASH_FREQ,
		.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
			     SPI_TRANSFER_MSB,
		.slave = 0,
		.cs = { .cs_is_gpio = false },
	};

	return spi_transceive(spi, &cfg, &tx_set, &rx_set);
}

/* Single-line 0x03 read of `len` bytes at `addr` — the reference the
 * multiline reads have to reproduce.
 *
 * This is the shape the *single-line* half of the driver implements: the
 * RX buffer set mirrors the TX buffer set, with a NULL slot standing in
 * for the command/address bytes (spi_agm_collect() reports those slots as
 * 0xff, which is what the SPI API's buffer-set convention asks for).
 */
static int run_single_read(const struct device *spi, uint32_t addr, uint8_t *rx, size_t len)
{
	const uint8_t cmd[4] = {
		SPI_FLASH_CMD_READ_SINGLE,
		(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
	};
	struct spi_buf tx_buf = { .buf = (void *)cmd, .len = sizeof(cmd) };
	struct spi_buf rx_bufs[2] = {
		{ .buf = NULL, .len = sizeof(cmd) },
		{ .buf = rx, .len = len },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };
	const struct spi_config cfg = {
		.frequency = SPI_FLASH_FREQ,
		.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
			     SPI_TRANSFER_MSB,
		.slave = 0,
		.cs = { .cs_is_gpio = false },
	};

	return spi_transceive(spi, &cfg, &tx_set, &rx_set);
}

/* Dual- or quad-line read of `len` bytes at `addr`: 1 command byte + 3
 * address bytes on a single line, 8 dummy clocks, then `len` data bytes on
 * the requested number of lines.
 *
 * The multiline wrapper (spi_agm_transceive_multiline()) is stricter than
 * the single-line path: it takes **exactly one** TX buffer (the
 * command+address frame) and **exactly one** RX buffer (the data phase),
 * and answers -ENOTSUP for anything else. So do not mirror tx_bufs here
 * with a NULL slot — that shape is silent about the command phase in the
 * single-line path, but the wrapper has no cmd/addr slots in its RX
 * phase to fill and rejects the request outright.
 *
 * SPI_LINES_QUAD is also refused with -ENOTSUP when the bitstream does not
 * declare `agm,spi-multiline = 2`, so a positive return here is the
 * bitstream-routing verdict.
 */
static int run_multiline_read(const struct device *spi, uint8_t command, uint32_t addr,
			      uint8_t *rx, size_t len, uint32_t lines)
{
	const uint8_t frame[4] = {
		command,
		(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
	};
	struct spi_buf tx_buf = { .buf = (void *)frame, .len = sizeof(frame) };
	struct spi_buf rx_buf = { .buf = rx, .len = len };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1U };
	const struct spi_config cfg = {
		.frequency = SPI_FLASH_FREQ,
		.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
			     SPI_TRANSFER_MSB | lines,
		.slave = 0,
		.cs = { .cs_is_gpio = false },
	};

	return spi_transceive(spi, &cfg, &tx_set, &rx_set);
}

/* First page-aligned 16-byte window inside the first sector that is not
 * all 0xff, or -ENOENT when the whole sector is blank. */
static int find_pattern_addr(const struct device *spi, uint32_t *addr)
{
	for (uint32_t a = 0U; a < SPI_FLASH_SCAN_BYTES; a += SPI_FLASH_SCAN_STEP) {
		uint8_t probe[SPI_FLASH_READ_LEN];
		int ret = run_single_read(spi, a, probe, sizeof(probe));

		if (ret < 0) {
			return ret;
		}

		for (size_t i = 0U; i < sizeof(probe); i++) {
			if (probe[i] != 0xFFU) {
				*addr = a;
				return 0;
			}
		}
	}

	return -ENOENT;
}

int main(void)
{
	bool any_pass = false;

	printk("\nspi_quad_read: generic-API SPI_LINES_DUAL/QUAD over the "
	       "phase engine\n");

	for (size_t i = 0U; i < ARRAY_SIZE(spi_controllers); i++) {
		const struct device *spi = spi_controllers[i];
		uint8_t rdid[3] = { 0U, 0U, 0U };
		uint8_t ref_buf[SPI_FLASH_READ_LEN] = { 0 };
		uint8_t dual_buf[SPI_FLASH_READ_LEN] = { 0 };
		uint8_t quad_buf[SPI_FLASH_READ_LEN] = { 0 };
		uint32_t addr = 0U;
		int ret;

		printk("\nspi_quad_read: %s\n", spi->name);

		if (!device_is_ready(spi)) {
			printk("spi_quad_read: not ready\n");
			continue;
		}

		ret = run_single_rdid(spi, rdid);
		if (ret < 0) {
			printk("spi_quad_read: single RDID failed (%d)\n", ret);
			continue;
		}

		print_bytes("spi_quad_read: RDID  ", rdid, sizeof(rdid));

		ret = find_pattern_addr(spi, &addr);
		if (ret == -ENOENT) {
			printk("spi_quad_read: %s -> INCONCLUSIVE (the first %u bytes "
			       "are blank; program a pattern first, e.g. with "
			       "samples/spi_flash_rw)\n", spi->name,
			       (unsigned int)SPI_FLASH_SCAN_BYTES);
			continue;
		}
		if (ret < 0) {
			printk("spi_quad_read: scan read failed (%d)\n", ret);
			continue;
		}

		printk("spi_quad_read: pattern at %#06x\n", (unsigned int)addr);

		/* Single-line 0x03 reference: the two multiline reads must
		 * reproduce exactly these bytes, not merely agree with each
		 * other. */
		ret = run_single_read(spi, addr, ref_buf, sizeof(ref_buf));
		if (ret < 0) {
			printk("spi_quad_read: 0x03 reference read failed (%d)\n", ret);
			continue;
		}

		print_bytes("spi_quad_read: 0x03  ", ref_buf, sizeof(ref_buf));

		ret = run_multiline_read(spi, SPI_FLASH_CMD_READ_DUAL, addr,
					 dual_buf, sizeof(dual_buf), SPI_LINES_DUAL);
		if (ret < 0) {
			printk("spi_quad_read: dual read failed (%d)\n", ret);
		} else {
			print_bytes("spi_quad_read: DUAL  ", dual_buf, sizeof(dual_buf));
		}

		ret = run_multiline_read(spi, SPI_FLASH_CMD_READ_QUAD, addr,
					 quad_buf, sizeof(quad_buf), SPI_LINES_QUAD);
		if (ret < 0) {
			printk("spi_quad_read: quad read failed (%d)\n", ret);
		} else {
			print_bytes("spi_quad_read: QUAD  ", quad_buf, sizeof(quad_buf));
		}

		/* Verdict. -ENOTSUP from either multiline call means the
		 * bitstream does not declare agm,spi-multiline = 2, or the
		 * request never left the single-line path because
		 * CONFIG_SPI_EXTENDED_MODES is off (spi_operation_t is then
		 * 16 bits wide and the SPI_LINES_* bits are truncated away --
		 * see prj.conf). */
		if (ret < 0) {
			printk("spi_quad_read: %s -> FAIL (multiline unsupported or "
			       "errored)\n", spi->name);
		} else if ((memcmp(dual_buf, ref_buf, sizeof(ref_buf)) == 0) &&
			   (memcmp(quad_buf, ref_buf, sizeof(ref_buf)) == 0)) {
			printk("spi_quad_read: %s -> PASS (0x03, 0x3B and 0x6B agree "
			       "byte for byte)\n", spi->name);
			any_pass = true;
		} else {
			printk("spi_quad_read: %s -> FAIL (multiline data differs from "
			       "0x03)\n", spi->name);
		}
	}

	printk("\nspi_quad_read: %s\n", any_pass
	       ? "PASS" : "FAIL (no controller produced a readable multiline page)");

	while (true) {
		k_msleep(1000);
	}
}
