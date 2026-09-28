/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slot validation + the "which fabric is live" probe for fcb_hotswap.
 *
 * Why validating first matters: the FCB only checks the image *at ACTIVATE*,
 * i.e. at the exact moment the running fabric is already gone. A rejected
 * image therefore leaves the board in the state
 * the port record (peripherals) 3.8.1 describes (core stopped, console
 * dead, SWD AP stalled) with BOOT0 + agrv32flash as the only way back. The
 * bitstream carries its own checksum, so the same check can run *before*
 * anything is touched.
 *
 * Checksum format (measured 2026-09-14 against <build_dir>/zephyr/board.bin,
 * ~/spi_full_bitstream_97pad/example_board.bin and
 * ~/spi_full_bitstream_without_flash/example_board.bin):
 *
 *   image  = 24986 words (99944 bytes, FCB_AUTO_WORDS from the vendor SDK)
 *   word 0 = 0x01002040 (IDCODE), word 1 = 0xffff0000 (USERID)
 *   CRC    = CRC-32/BZIP2 (poly 0x04C11DB7, init 0xFFFFFFFF, no bit
 *            reflection, final xor 0xFFFFFFFF) over bytes 0..99939,
 *            stored big-endian in the last four bytes.
 *
 * All three images satisfy it — including the two that differ in 3137 words —
 * so this is not a constant that happens to match one file.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if !defined(CONFIG_LITTLE_ENDIAN)
#error "fcb_hotswap: the slot CRC walks image words as little-endian bytes"
#endif

#if !DT_HAS_COMPAT_STATUS_OKAY(agm_agrv2k_spi)
#error "fcb_hotswap: need agm,agrv2k-spi enabled in the build"
#endif

/* Mirrors FCB_AUTO_WORDS in soc/agm/agrv2k/fcb.c (vendor SDK
 * fcb.h:FCB_AUTO_WORDS = 99944/4). */
#define SLOT_WORDS      24986U

#define CRC32_BZIP2_POLY 0x04C11DB7U

static uint32_t crc32_table[256];
static bool crc32_table_ready;

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
	return (crc << 8) ^ crc32_table[((crc >> 24) ^ byte) & 0xFFU];
}

static void crc32_table_build(void)
{
	for (uint32_t i = 0; i < 256U; i++) {
		uint32_t c = i << 24;

		for (int bit = 0; bit < 8; bit++) {
			c = ((c & 0x80000000U) != 0U)
				    ? ((c << 1) ^ CRC32_BZIP2_POLY)
				    : (c << 1);
		}
		crc32_table[i] = c;
	}
	crc32_table_ready = true;
}

/*
 * Returns 0 if the slot at `addr` holds a bitstream the FCB would accept, and
 * leaves its IDCODE (image word 0) in *idcode when the pointer is not NULL.
 *
 *   -ENODATA  empty slot (erased 0xFF, or 0x00): nothing written there yet
 *   -EILSEQ   checksum mismatch: truncated or half-written image
 */
int slot_verify(uint32_t addr, uint32_t *idcode)
{
	const volatile uint32_t *img = (const volatile uint32_t *)addr;
	uint32_t crc;
	uint32_t stored;
	uint32_t first = img[0];

	if ((first == 0xFFFFFFFFU) && (img[1] == 0xFFFFFFFFU) &&
	    (img[2] == 0xFFFFFFFFU) && (img[3] == 0xFFFFFFFFU)) {
		return -ENODATA;
	}
	if ((first == 0U) && (img[1] == 0U) && (img[2] == 0U) &&
	    (img[3] == 0U)) {
		return -ENODATA;
	}

	if (!crc32_table_ready) {
		crc32_table_build();
	}

	crc = 0xFFFFFFFFU;
	for (uint32_t i = 0; i < (SLOT_WORDS - 1U); i++) {
		uint32_t word = img[i];

		for (int b = 0; b < 4; b++) {
			crc = crc32_update(crc, (uint8_t)(word >> (8 * b)));
		}
	}
	crc ^= 0xFFFFFFFFU;

	/* Stored big-endian: 0x9eae7287 lives at the end of example_board.bin
	 * as 9e ae 72 87. */
	stored = (img[SLOT_WORDS - 1U] & 0xFFU) << 24;
	stored |= ((img[SLOT_WORDS - 1U] >> 8) & 0xFFU) << 16;
	stored |= ((img[SLOT_WORDS - 1U] >> 16) & 0xFFU) << 8;
	stored |= (img[SLOT_WORDS - 1U] >> 24) & 0xFFU;

	if (crc != stored) {
		return -EILSEQ;
	}
	if (idcode != NULL) {
		*idcode = first;
	}
	return 0;
}

/*
 * SPI NOR RDID probe (0x9F, 3-byte response). Same shape as
 * samples/fcb_reload's spi_id.c: a transfer starts with TX, RX is the last
 * segment, and the driver's TX-then-RX cycle hides the command byte as 0xFF
 * in the first RX slot. Which controller reaches the flash is a property of
 * the live bitstream, so probe every enabled one and report the first that
 * answers.
 */
#define HOTSWAP_SPI_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const spi_controllers[] = {
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, HOTSWAP_SPI_ENTRY)
};

static const struct spi_config spi_cfg = {
	.frequency = 1000000U, /* 1 MHz — conservative, the fabric is unknown */
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = { .cs_is_gpio = false },
};

static int rdid_once(const struct device *spi, uint8_t out[3])
{
	uint8_t cmd = 0x9FU; /* RDID */
	uint8_t echo;
	uint8_t rx[3] = { 0 };
	struct spi_buf tx_buf = { .buf = &cmd, .len = 1U };
	struct spi_buf rx_bufs[2] = {
		{ .buf = &echo, .len = 1U },
		{ .buf = rx, .len = 3U },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

	int ret = spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);

	if (ret < 0) {
		return ret;
	}
	out[0] = rx[0];
	out[1] = rx[1];
	out[2] = rx[2];
	return 0;
}

int slot_rdid(uint8_t out[3])
{
	bool have_blank = false;
	uint8_t blank[3] = { 0 };

	for (size_t i = 0; i < ARRAY_SIZE(spi_controllers); i++) {
		const struct device *spi = spi_controllers[i];
		uint8_t r[3] = { 0 };

		if (!device_is_ready(spi)) {
			continue;
		}
		if (rdid_once(spi, r) < 0) {
			printk("hotswap: %s RDID read failed\n", spi->name);
			continue;
		}
		printk("hotswap: %s RDID = %02x %02x %02x\n",
		       spi->name, r[0], r[1], r[2]);

		if (!((r[0] == 0U && r[1] == 0U && r[2] == 0U) ||
		      (r[0] == 0xFFU && r[1] == 0xFFU && r[2] == 0xFFU))) {
			out[0] = r[0];
			out[1] = r[1];
			out[2] = r[2];
			return 0;
		}
		if (!have_blank) {
			blank[0] = r[0];
			blank[1] = r[1];
			blank[2] = r[2];
			have_blank = true;
		}
	}

	if (!have_blank) {
		return -ENODEV;
	}
	out[0] = blank[0];
	out[1] = blank[1];
	out[2] = blank[2];
	return 0;
}

const char *slot_rdid_name(const uint8_t rdid[3])
{
	if (rdid[0] == 0xC8U && rdid[1] == 0x40U && rdid[2] == 0x16U) {
		return "GD25Qxx";
	}
	if (rdid[0] == 0x68U && rdid[1] == 0x40U && rdid[2] == 0x15U) {
		return "W25Q16";
	}
	if (rdid[0] == 0x00U && rdid[1] == 0x00U && rdid[2] == 0x00U) {
		return "nothing answering (MISO held low)";
	}
	if (rdid[0] == 0xFFU && rdid[1] == 0xFFU && rdid[2] == 0xFFU) {
		return "nothing answering (MISO floating high)";
	}
	return "unexpected";
}
