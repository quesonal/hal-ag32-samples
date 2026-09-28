/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * RDID probe for fcb_reload. Same pattern as samples/spi_flash_id
 * but trimmed to a single command (0x9F, 3-byte response) so the
 * sample compiles without pulling the whole spi_flash_id surface.
 *
 * The shape of the call matches the AgRV2K SPI phase engine's
 * constraint: a transfer starts with TX, RX is the last segment,
 * and the driver's TX-then-RX cycle hides the command bytes as
 * 0xFF in the first RX slot.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>

#if !DT_HAS_COMPAT_STATUS_OKAY(agm_agrv2k_spi)
#error "fcb_reload: need agm,agrv2k-spi enabled in the build"
#endif

#define FCB_RELOAD_SPI_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const spi_controllers[] = {
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, FCB_RELOAD_SPI_ENTRY)
};

#define SPI_FLASH_CMD_RDID  0x9FU

static const struct spi_config spi_cfg = {
	.frequency = 1000000U, /* 1 MHz — conservative for an unknown bitstream */
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = { .cs_is_gpio = false },
};

static int rdid_once(const struct device *spi, uint8_t out[3])
{
	uint8_t cmd = SPI_FLASH_CMD_RDID;
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

/*
 * Which controller reaches the flash is a bitstream property, so probe every
 * enabled one and report the first that actually answers (a controller whose
 * MISO never arrives reads back all-0x00, and one that is wired to nothing
 * reads all-0xff -- neither is a failure worth stopping on). The first blank
 * result is kept so a bitstream that routes SPI nowhere still prints
 * something meaningful.
 */
int fcb_reload_rdid(uint8_t out[3])
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
			printk("fcb_reload: %s RDID read failed\n", spi->name);
			continue;
		}

		printk("fcb_reload: %s RDID = %02x %02x %02x\n",
		       spi->name, r[0], r[1], r[2]);

		if (r[0] != 0x00U || r[1] != 0x00U || r[2] != 0x00U) {
			if (r[0] != 0xFFU || r[1] != 0xFFU || r[2] != 0xFFU) {
				out[0] = r[0];
				out[1] = r[1];
				out[2] = r[2];
				return 0;
			}
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
