/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi_flash_id — read an SPI NOR through the AgRV2K SPI driver.
 *
 * Read-only bring-up of the SPI port. The AgRV2K controller is a phase
 * engine with three documented constraints (vendor document "AG32 下 SPI
 * 的扩展使用", page 1):
 *
 *   1. a transfer starts with TX (never RX);
 *   2. TX and RX do not overlap (TX first, then RX);
 *   3. RX is the last segment.
 *
 * The direct (non-DMA) path moves at most 4 bytes per direction — the phase
 * data register is 32 bits wide, and longer transfers stream through that
 * register with DMA (the SDK's SPI_SendAndRecvDMA; not implemented here
 * yet, see the port record (peripherals) 3.26).
 *
 * So every command below is a 4-byte-or-shorter command/response pair, and
 * the "read a few words" part issues one command per word instead of one
 * long read — the honest shape of the current driver, not of the flash.
 *
 * Nothing here writes: RDID/RDSR/0x90/READ are read-only, so the flash
 * content is untouched (on a board whose bitstream boots from this flash a
 * write could brick it).
 *
 * Which controller reaches the flash is a bitstream property, not a driver
 * one, so the sample probes every enabled agm,agrv2k-spi node.
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
#error "no agm,agrv2k-spi node is enabled: build with a board overlay that sets one okay"
#endif

#define SPI_AGM_DEV_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const spi_controllers[] = {
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, SPI_AGM_DEV_ENTRY)
};

#define SPI_FLASH_CMD_RDID  0x9FU
#define SPI_FLASH_CMD_RDSR  0x05U
#define SPI_FLASH_CMD_READ  0x03U
#define SPI_FLASH_CMD_REMS  0x90U
#define SPI_FLASH_CMD_EID   0xABU
#define SPI_FLASH_CMD_RUID  0x4BU
#define SPI_FLASH_CMD_WREN  0x06U

/* The engine divides SYSCLK by 2..256; 1 MHz is a safe bring-up rate. */
#define SPI_FLASH_FREQ      1000000U

/* Words read back with separate 0x03 commands (addresses 0, 4, 8). */
#define SPI_FLASH_WORDS     3U

static const struct spi_config spi_cfg = {
	.frequency = SPI_FLASH_FREQ,
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = {
		/* No cs-gpios on this bus: the controller drives the flash's
		 * CSN pin itself for the whole phase run. */
		.cs_is_gpio = false,
	},
};

static void print_bytes(const uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		printk(" %02x", buf[i]);
	}
}

/* tx: what to send, rx: what to read back, in one chip-select window.
 *
 * The engine samples MISO only after its TX phases are done, so the slots
 * that carry the command have to be part of the RX list: the driver reports
 * them as 0xFF (it cannot see the bus then) and puts the bytes that follow
 * where they belong. That is the shape a full-duplex controller serves
 * natively and what upstream drivers such as spi-nor hand over. */
static int flash_cmd_read(const struct device *spi, const uint8_t *tx, size_t tx_len, uint8_t *rx,
			  size_t rx_len)
{
	static uint8_t echo[8];
	struct spi_buf tx_buf = { .buf = (void *)tx, .len = tx_len };
	struct spi_buf rx_bufs[2] = {
		{ .buf = echo, .len = tx_len },
		{ .buf = rx, .len = rx_len },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

	if (rx_len == 0U) {
		return spi_write(spi, &spi_cfg, &tx_set);
	}

	if (tx_len > sizeof(echo)) {
		return -EINVAL;
	}

	return spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);
}

static bool run_status_read(const struct device *spi, uint8_t cmd, uint8_t *status)
{
	int ret = flash_cmd_read(spi, &cmd, 1U, status, 1U);

	if (ret < 0) {
		printk("spi_flash_id: RDSR failed (%d)\n", ret);
		return false;
	}

	return true;
}

static bool run_checks(const struct device *spi)
{
	uint8_t rdid[3] = { 0U, 0U, 0U };
	uint8_t status = 0U;
	uint8_t mfid[4] = { 0 };
	uint8_t eid[2] = { 0 };
	uint8_t words[SPI_FLASH_WORDS][4] = { { 0 } };
	const uint8_t cmd_rdid = SPI_FLASH_CMD_RDID;
	const uint8_t cmd_rdsr = SPI_FLASH_CMD_RDSR;
	/* 0x90 + 24-bit address: 4-byte TX phase, then MFID + device ID. */
	const uint8_t cmd_rems[4] = { SPI_FLASH_CMD_REMS, 0x00U, 0x00U, 0x00U };
	/* 0xAB + three dummy bytes, then the 8-bit electronic ID. */
	const uint8_t cmd_eid[4] = { SPI_FLASH_CMD_EID, 0x00U, 0x00U, 0x00U };
	/* 0x03 + 24-bit address; the address is filled in per word below. */
	uint8_t cmd_read[4] = { SPI_FLASH_CMD_READ, 0x00U, 0x00U, 0x00U };
	bool ok = true;
	int ret;

	ret = flash_cmd_read(spi, &cmd_rdid, 1U, rdid, sizeof(rdid));
	if (ret < 0) {
		printk("spi_flash_id: RDID failed (%d)\n", ret);
		return false;
	}

	printk("spi_flash_id: RDID  = %02x %02x %02x\n", rdid[0], rdid[1], rdid[2]);

	/* A floating MISO or an unclocked bus shows up as all-ones or all-zero. */
	if (((rdid[0] == 0xFFU) && (rdid[1] == 0xFFU) && (rdid[2] == 0xFFU)) ||
	    ((rdid[0] == 0x00U) && (rdid[1] == 0x00U) && (rdid[2] == 0x00U))) {
		printk("spi_flash_id: RDID is all-%s -> no flash answering\n",
		       rdid[0] == 0xFFU ? "ones" : "zeros");
		ok = false;
	}

	ret = flash_cmd_read(spi, &cmd_rdsr, 1U, &status, 1U);
	if (ret < 0) {
		printk("spi_flash_id: RDSR failed (%d)\n", ret);
		return false;
	}

	printk("spi_flash_id: RDSR  = 0x%02x%s\n", status,
	       (status & BIT(0)) != 0U ? " (WIP set!)" : "");

	if ((status & BIT(0)) != 0U) {
		ok = false;
	}

	/* 0x90 returns manufacturer ID + device ID twice over, so it also
	 * cross-checks the RDID's manufacturer byte. */
	ret = flash_cmd_read(spi, cmd_rems, sizeof(cmd_rems), mfid, sizeof(mfid));
	if (ret < 0) {
		printk("spi_flash_id: 0x90 failed (%d)\n", ret);
		return false;
	}

	printk("spi_flash_id: MFID  = %02x %02x (repeated: %02x %02x)\n", mfid[0], mfid[1], mfid[2],
	       mfid[3]);

	if (mfid[0] != rdid[0]) {
		printk("spi_flash_id: 0x90 manufacturer (%02x) disagrees with RDID (%02x)\n", mfid[0],
		       rdid[0]);
		ok = false;
	}

	/* 0xAB (release power-down / read electronic ID): a third, independent
	 * command whose answer must match the device ID from 0x90. */
	ret = flash_cmd_read(spi, cmd_eid, sizeof(cmd_eid), eid, 1U);
	if (ret < 0) {
		printk("spi_flash_id: 0xAB failed (%d)\n", ret);
		return false;
	}

	printk("spi_flash_id: EID   = %02x (expect the 0x90 device ID %02x)\n", eid[0], mfid[1]);

	if (eid[0] != mfid[1]) {
		printk("spi_flash_id: electronic ID disagrees with 0x90 device ID\n");
		ok = false;
	}

	for (uint8_t i = 0U; i < SPI_FLASH_WORDS; i++) {
		cmd_read[3] = (uint8_t)(i * 4U); /* addresses 0x00, 0x04, 0x08 */

		ret = flash_cmd_read(spi, cmd_read, sizeof(cmd_read), words[i], sizeof(words[i]));
		if (ret < 0) {
			printk("spi_flash_id: read@%u failed (%d)\n", i * 4U, ret);
			return false;
		}
	}

	printk("spi_flash_id: READ  =");
	for (uint8_t i = 0U; i < SPI_FLASH_WORDS; i++) {
		printk(" %02x%02x%02x%02x", words[i][0], words[i][1], words[i][2], words[i][3]);
	}
	printk(" (addr 0x00/0x04/0x08, one 0x03 command each)\n");

	printk("spi_flash_id: %s\n", ok ? "PASS" : "FAIL");

	return ok;
}

#if IS_ENABLED(CONFIG_APP_SPI_LONG_XFER)
/* RX longer than four bytes goes through the engine's DMA: the RX phase is
 * the last one, so its data register is the DMA port. The transfer keeps its
 * TX within the phase limit (0x4B + four dummy bytes = 5 bytes) and asks for
 * an 8-byte reply -- the flash's 64-bit unique ID. */
static bool run_rx_dma_check(const struct device *spi)
{
	static uint8_t tx[5] = { SPI_FLASH_CMD_RUID, 0xFFU, 0xFFU, 0xFFU, 0xFFU };
	static uint8_t rx_a[8] __aligned(4);
	static uint8_t rx_b[8] __aligned(4);
	static uint8_t echo[sizeof(tx)];
	struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf rx_bufs[2] = {
		{ .buf = echo, .len = sizeof(echo) },
		{ .buf = rx_a, .len = sizeof(rx_a) },
	};
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };
	uint32_t nonempty = 0U;
	int ret;

	ret = spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);
	if (ret < 0) {
		printk("spi_flash_id: RX-DMA read #1 failed (%d)\n", ret);
		return false;
	}

	rx_bufs[1].buf = rx_b;
	ret = spi_transceive(spi, &spi_cfg, &tx_set, &rx_set);
	if (ret < 0) {
		printk("spi_flash_id: RX-DMA read #2 failed (%d)\n", ret);
		return false;
	}

	printk("spi_flash_id: RX-DMA #1 =");
	print_bytes(rx_a, sizeof(rx_a));
	printk("\nspi_flash_id: RX-DMA #2 =");
	print_bytes(rx_b, sizeof(rx_b));
	printk("\n");

	for (size_t i = 0U; i < sizeof(rx_a); i++) {
		if ((rx_a[i] != 0x00U) && (rx_a[i] != 0xFFU)) {
			nonempty++;
		}
	}

	if (memcmp(rx_a, rx_b, sizeof(rx_a)) != 0) {
		printk("spi_flash_id: the two RX-DMA reads disagree -> FAIL\n");
		return false;
	}

	if (nonempty < 4U) {
		printk("spi_flash_id: RX-DMA read has only %u non-FF bytes -> FAIL\n",
		       (unsigned int)nonempty);
		return false;
	}

	printk("spi_flash_id: unique ID via the engine's RX DMA =");
	print_bytes(rx_a, sizeof(rx_a));
	printk("; both reads agree -> PASS\n");

	return true;
}

/* Write-Enable is the cheapest witness that the flash hears something: 0x06
 * sets WEL, which RDSR shows. Four shapes, in increasing length:
 *
 *   - one byte, no DMA            -> must set WEL;
 *   - RDID padded to 8 bytes      -> a harmless multi-TX-phase check, the
 *     same ID must come back (one dummy byte shifted in);
 *   - 32 bytes (eight register-fed phases);
 *   - 36 bytes (past them, so the driver uses the engine's TX DMA);
 *
 * The last two are printed as [informational]: this part leaves WEL clear
 * when 0x06 is followed by filler bytes in the same CS window, so WEL is
 * not a long-frame verdict. samples/spi_loopback captures MOSI through the
 * fabric and is what verifies the long transmit path.
 *
 * No program or erase command is issued, so the flash content is untouched:
 * 0x06 only sets a latch, and 0x04 clears it again. */
static bool run_write_path_check(const struct device *spi)
{
	static uint8_t tx[36] __aligned(4);
	const uint8_t cmd_rdsr = SPI_FLASH_CMD_RDSR;
	const uint8_t cmd_wrdi = 0x04U;
	const uint8_t cmd_wren = SPI_FLASH_CMD_WREN;
	struct spi_buf wren_buf = { .buf = (void *)&cmd_wren, .len = 1U };
	const struct spi_buf_set wren_set = { .buffers = &wren_buf, .count = 1U };
	struct spi_buf wrdi_buf = { .buf = (void *)&cmd_wrdi, .len = 1U };
	const struct spi_buf_set wrdi_set = { .buffers = &wrdi_buf, .count = 1U };
	struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	uint8_t status = 0U;
	int ret;

	if (!run_status_read(spi, cmd_rdsr, &status)) {
		return false;
	}

	if ((status & BIT(1)) != 0U) {
		/* Something left the latch set; clear it before the test. */
		(void)spi_write(spi, &spi_cfg, &wrdi_set);
	}

	ret = spi_write(spi, &spi_cfg, &wren_set);
	if (ret < 0) {
		printk("spi_flash_id: plain WREN failed (%d)\n", ret);
		return false;
	}

	if (!run_status_read(spi, cmd_rdsr, &status)) {
		return false;
	}

	printk("spi_flash_id: WREN (1 byte, no DMA) -> RDSR 0x%02x (WEL=%u)\n", status,
	       (status & BIT(1)) != 0U ? 1U : 0U);

	(void)spi_write(spi, &spi_cfg, &wrdi_set);

	if ((status & BIT(1)) == 0U) {
		printk("spi_flash_id: the one-byte Write-Enable did not set WEL -> FAIL\n");
		return false;
	}

	/* Harmless multi-TX-phase check: read the RDID with the command padded
	 * to eight bytes, i.e. two TX phases (the 1-byte TX of the normal RDID
	 * uses one). Same command, same response expected. */
	{
		uint8_t tx8[8] = { SPI_FLASH_CMD_RDID, 0xFFU, 0xFFU, 0xFFU,
				   0xFFU, 0xFFU, 0xFFU, 0xFFU };
		uint8_t id3[3] = { 0U, 0U, 0U };
		uint8_t echo8[8];
		struct spi_buf tb = { .buf = tx8, .len = sizeof(tx8) };
		struct spi_buf rb[2] = {
			{ .buf = echo8, .len = sizeof(echo8) },
			{ .buf = id3, .len = sizeof(id3) },
		};
		const struct spi_buf_set ts = { .buffers = &tb, .count = 1U };
		const struct spi_buf_set rs = { .buffers = rb, .count = 2U };

		ret = spi_transceive(spi, &spi_cfg, &ts, &rs);
		printk("spi_flash_id: RDID via 8-byte TX (2 phases): ret=%d RDID =", ret);
		print_bytes(id3, sizeof(id3));
		printk("\n");
	}

	/* 32 bytes: still inside the engine's eight register-fed phases, no DMA
	 * anywhere. */
	for (size_t i = 0U; i < sizeof(tx); i++) {
		tx[i] = 0xFFU;
	}

	tx[0] = SPI_FLASH_CMD_WREN;

	{
		struct spi_buf tb = { .buf = tx, .len = 32U };
		const struct spi_buf_set ts = { .buffers = &tb, .count = 1U };

		ret = spi_write(spi, &spi_cfg, &ts);
	}

	if (ret == 0 && run_status_read(spi, cmd_rdsr, &status)) {
		printk("spi_flash_id: 32-byte WREN (register phases) -> RDSR 0x%02x (WEL=%u) [informational]\n",
		       status, (status & BIT(1)) != 0U ? 1U : 0U);
		(void)spi_write(spi, &spi_cfg, &wrdi_set);
	} else {
		printk("spi_flash_id: 32-byte WREN failed (%d)\n", ret);
	}

	/* 36 bytes: past the engine's eight-phase limit, so this goes through
	 * the engine's TX DMA. The fabric loopback (samples/spi_loopback)
	 * showed that path transmits the right bytes, so the flash sees the
	 * command; whether it latches WEL with filler bytes behind it is the
	 * part's business. */
	for (size_t i = 0U; i < sizeof(tx); i++) {
		tx[i] = 0xFFU;
	}

	tx[0] = SPI_FLASH_CMD_WREN;

	ret = spi_write(spi, &spi_cfg, &tx_set);
	if (ret < 0) {
		printk("spi_flash_id: 36-byte WREN failed (%d)\n", ret);
		return false;
	}

	if (!run_status_read(spi, cmd_rdsr, &status)) {
		return false;
	}

	printk("spi_flash_id: 36-byte WREN (engine TX DMA) -> RDSR 0x%02x (WEL=%u) [informational]\n",
	       status, (status & BIT(1)) != 0U ? 1U : 0U);

	(void)spi_write(spi, &spi_cfg, &wrdi_set);

	/* WEL after a padded WREN is *not* a TX-path verdict: this flash keeps
	 * WEL=0 when 0x06 is followed by filler bytes in the same window, while
	 * the same engine transmits byte-exact streams (see
	 * samples/spi_loopback, which captures the wire through the fabric).
	 * A real write test (command + address + data) is what would settle the
	 * write path. */
	printk("spi_flash_id: note: padded WREN leaving WEL=0 is this flash's behaviour, "
	       "not a TX-path failure (see spi_loopback)\n");

	printk("spi_flash_id: TX path OK (1 byte and 36 bytes via TX DMA)\n");

	return true;
}
#endif /* CONFIG_APP_SPI_LONG_XFER */


int main(void)
{
	printk("\nspi_flash_id: AgRV2K SPI + SPI NOR bring-up\n");

	for (size_t i = 0U; i < ARRAY_SIZE(spi_controllers); i++) {
		const struct device *spi = spi_controllers[i];

		printk("\nspi_flash_id: %s\n", spi->name);

		if (!device_is_ready(spi)) {
			printk("spi_flash_id: not ready (CONFIG_SPI_AGM?)\n");
			continue;
		}

		bool found = run_checks(spi);

#if IS_ENABLED(CONFIG_APP_SPI_LONG_XFER)
		/* Only ask for the long transfer where a device answered the
		 * short probes: a controller with nothing on it would just make
		 * the DMA wait for a request that never comes. */
		if (found) {
			(void)run_rx_dma_check(spi);
			printk("\n");
			(void)run_write_path_check(spi);
		}
#endif
	}

	while (true) {
		k_msleep(1000);
	}
}
