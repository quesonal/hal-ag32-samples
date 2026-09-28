/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi_flash_rw — erase and program an SPI NOR through the AgRV2K SPI
 * driver, then read the result back and compare it.
 *
 * This is the write half of the SPI bring-up. It exercises all three
 * transmit shapes the driver supports:
 *
 *   - 4-byte frames (0x06 Write-Enable, 0x20 Sector-Erase + address):
 *     one register-fed phase;
 *   - a 4 + 64-byte page program: past 32 bytes, so phase0 comes from the
 *     data register and the DMAC feeds phase1;
 *   - a 4 + 256-byte page program: the full-frame case a page program
 *     needs, and the reason the TX DMA path exists at all.
 *
 * The page-program frame is one contiguous, 4-byte aligned buffer because
 * the DMA port moves 32-bit words out of memory.
 *
 * THIS SAMPLE DESTROYS DATA: it erases and reprograms the first sector
 * (4 KiB) of whatever SPI NOR it finds. On a board whose BOOT0 strap can
 * boot from that flash, do not run it without a backup.
 *
 * Which controller reaches the flash is a bitstream property, so the
 * sample probes every enabled agm,agrv2k-spi node and takes the first one
 * that answers RDID with something other than all-zero/all-ones.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/spi_agm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if !DT_HAS_COMPAT_STATUS_OKAY(agm_agrv2k_spi)
#error "no agm,agrv2k-spi node is enabled: build with a board overlay that sets one okay"
#endif

#define SPI_AGM_DEV_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const spi_controllers[] = {
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, SPI_AGM_DEV_ENTRY)
};

/* SPI NOR commands: 25-series, single-line, the subset a write needs. */
#define FLASH_CMD_RDID   0x9FU /* JEDEC ID */
#define FLASH_CMD_RDSR   0x05U /* status register */
#define FLASH_CMD_WREN   0x06U /* write enable */
#define FLASH_CMD_WRDI   0x04U /* write disable */
#define FLASH_CMD_READ   0x03U /* read data */
#define FLASH_CMD_PROGRAM 0x02U /* page program */
#define FLASH_CMD_ERASE  0x20U /* 4 KiB sector erase */

#define FLASH_STATUS_WIP BIT(0)
#define FLASH_STATUS_WEL BIT(1)

#define FLASH_PAGE_SIZE     256U
#define FLASH_SECTOR_SIZE   4096U
/* Erase takes up to 400 ms on a 25-series part; program up to 3 ms. The
 * limits here are the "something is wrong" guards, not the typical times. */
#define FLASH_ERASE_TIMEOUT_MS 2000U
#define FLASH_PROGRAM_TIMEOUT_MS 200U

/* The whole first sector, read back in FLASH_PAGE_SIZE chunks (the
 * driver's RX bounce buffer is smaller than a sector). */
static uint8_t sector[FLASH_SECTOR_SIZE] __aligned(4);
static uint8_t page[FLASH_PAGE_SIZE] __aligned(4);
/* cmd + address + up to one page of data, in the single buffer the DMA
 * needs: 0x02 + 24-bit address + 256 bytes. */
static uint8_t program_frame[4U + FLASH_PAGE_SIZE] __aligned(4);

static const struct spi_config spi_cfg = {
	.frequency = CONFIG_APP_FLASH_RW_FREQUENCY,
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = {
		/* No cs-gpios on this bus: the controller drives the flash's
		 * CSN pin itself for the whole phase run. */
		.cs_is_gpio = false,
	},
};

static const struct device *flash_spi;

#if defined(CONFIG_PM)
/* Count the SoC's sleep entries so the test can tell whether the idle path
 * really went through pm_state_set() (and, with CONFIG_PM_DEVICE, whether a
 * device refusing to suspend would have blocked it). */
static volatile uint32_t pm_entries;
static volatile uint32_t pm_exits;

static void pm_entry_notify(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		pm_entries++;
	}
}

static void pm_exit_notify(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		pm_exits++;
	}
}

static struct pm_notifier pm_notifier = {
	.state_entry = pm_entry_notify,
	.state_exit = pm_exit_notify,
};
#endif

#if defined(CONFIG_SPI_ASYNC)
static K_SEM_DEFINE(async_done, 0, 1);
static volatile int async_result;

static void async_cb(const struct device *dev, int result, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	async_result = result;
	k_sem_give(&async_done);
}
#endif

#if defined(CONFIG_APP_FLASH_RW_CS_GPIO)
/* The cs_is_gpio path drives a GPIO for the whole transfer. Nothing else on
 * this board is free, so borrow LED3's pin and watch it: it has to read
 * asserted while a transfer runs and released again afterwards. (The flash
 * is still selected by the engine, so the data itself must not change.) The
 * transfer runs through the async entry point, which leaves the CPU free to
 * sample the pin -- the engine's own wait loop never yields. */
static const struct gpio_dt_spec cs_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(led2), gpios);
static volatile uint32_t cs_samples;
static volatile uint32_t cs_asserted;

/* Sampled from a timer ISR: the engine's wait loop runs with interrupts
 * enabled, so this sees the chip-select window even though the calling
 * thread is busy inside the transfer. */
static void cs_timer_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (gpio_pin_get_dt(&cs_gpio) != 0) {
		cs_asserted++;
	}

	cs_samples++;
}

static K_TIMER_DEFINE(cs_timer, cs_timer_expiry, NULL);
#endif

static void print_bytes(const uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		printk(" %02x", buf[i]);
	}
}

/* Command plus (optional) reply in one chip-select window.
 *
 * The engine samples MISO only once its TX phases are done, so the slots
 * that carry the command are part of the RX list too: the driver reports
 * them as 0xFF and puts the bytes that follow where they belong. That keeps
 * this sample's transfers the same shape a full-duplex controller (and
 * upstream spi-nor) uses. */
static int flash_cmd_read(uint8_t cmd, const uint8_t *addr3, uint8_t *rx, size_t rx_len)
{
	static uint8_t echo[4];
	uint8_t frame[4] = { cmd, 0U, 0U, 0U };
	size_t tx_len = (addr3 != NULL) ? 4U : 1U;
	struct spi_buf tx_buf = { .buf = frame, .len = tx_len };
	struct spi_buf rx_bufs[2] = {
		{ .buf = echo, .len = tx_len },
		{ .buf = rx, .len = rx_len },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	const struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = 2U };

	if (addr3 != NULL) {
		memcpy(&frame[1], addr3, 3U);
	}

	return spi_transceive(flash_spi, &spi_cfg, &tx_set, &rx_set);
}

static int flash_cmd_write(uint8_t cmd, const uint8_t *addr3)
{
	uint8_t frame[4] = { cmd, 0U, 0U, 0U };
	struct spi_buf tx_buf = { .buf = frame, .len = (addr3 != NULL) ? 4U : 1U };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };

	if (addr3 != NULL) {
		memcpy(&frame[1], addr3, 3U);
	}

	return spi_write(flash_spi, &spi_cfg, &tx_set);
}

static void addr_to_bytes(uint32_t addr, uint8_t out[3])
{
	out[0] = (uint8_t)(addr >> 16);
	out[1] = (uint8_t)(addr >> 8);
	out[2] = (uint8_t)addr;
}

static int flash_read_status(uint8_t *status)
{
	return flash_cmd_read(FLASH_CMD_RDSR, NULL, status, 1U);
}

/* Poll WIP until the part is idle again; returns the last status read. */
static int flash_wait_ready(uint32_t timeout_ms, uint8_t *status)
{
	uint32_t elapsed = 0U;
	int ret;

	for (;;) {
		ret = flash_read_status(status);
		if (ret < 0) {
			return ret;
		}

		if ((*status & FLASH_STATUS_WIP) == 0U) {
			return 0;
		}

		if (elapsed >= timeout_ms) {
			return -ETIMEDOUT;
		}

		k_msleep(1);
		elapsed++;
	}
}

static int flash_read(uint32_t addr, uint8_t *buf, size_t len)
{
	uint8_t a[3];

	addr_to_bytes(addr, a);

	return flash_cmd_read(FLASH_CMD_READ, a, buf, len);
}

/* Write-Enable + the 0x20 command, without waiting for the part. */
static int flash_send_sector_erase(uint32_t addr)
{
	uint8_t a[3];
	uint8_t status;
	int ret;

	addr_to_bytes(addr, a);

	ret = flash_cmd_write(FLASH_CMD_WREN, NULL);
	if (ret < 0) {
		return ret;
	}

	ret = flash_read_status(&status);
	if (ret < 0) {
		return ret;
	}

	if ((status & FLASH_STATUS_WEL) == 0U) {
		return -EIO;
	}

	ret = flash_cmd_write(FLASH_CMD_ERASE, a);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int flash_sector_erase(uint32_t addr)
{
	uint8_t status;
	int ret = flash_send_sector_erase(addr);

	if (ret < 0) {
		return ret;
	}

	return flash_wait_ready(FLASH_ERASE_TIMEOUT_MS, &status);
}

/* One 0x02 frame: command + address + len data bytes, in a single buffer so
 * that a length above 32 bytes goes through the engine's TX DMA. */
static int flash_page_program(uint32_t addr, const uint8_t *data, size_t len)
{
	uint8_t a[3];
	uint8_t status;
	struct spi_buf tx_buf = { .buf = program_frame, .len = 4U + len };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1U };
	int ret;

	if (len > FLASH_PAGE_SIZE) {
		return -EINVAL;
	}

	addr_to_bytes(addr, a);

	program_frame[0] = FLASH_CMD_PROGRAM;
	program_frame[1] = a[0];
	program_frame[2] = a[1];
	program_frame[3] = a[2];
	memcpy(&program_frame[4], data, len);

	ret = flash_cmd_write(FLASH_CMD_WREN, NULL);
	if (ret < 0) {
		return ret;
	}

	ret = flash_read_status(&status);
	if (ret < 0) {
		return ret;
	}

	if ((status & FLASH_STATUS_WEL) == 0U) {
		return -EIO;
	}

	ret = spi_write(flash_spi, &spi_cfg, &tx_set);
	if (ret < 0) {
		return ret;
	}

	return flash_wait_ready(FLASH_PROGRAM_TIMEOUT_MS, &status);
}

static void md_pattern(uint8_t *buf, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		buf[i] = (uint8_t)(0xA0U + i);
	}
}

/* How many of these bytes a blank (0xFF) read-back would not account for.
 * The pattern is 0xA0 + i, so a byte that lands exactly on 0xFF is
 * indistinguishable from erased and must not be counted. */
static size_t count_non_ff(const uint8_t *buf, size_t len)
{
	size_t n = 0U;

	for (size_t i = 0U; i < len; i++) {
		if (buf[i] != 0xFFU) {
			n++;
		}
	}

	return n;
}

/* Read `len` bytes and compare them with `expect`; the driver moves RX
 * through its bounce buffer, so the caller's buffer may be anything. */
static bool verify_bytes(uint32_t addr, const uint8_t *expect, size_t len, const char *what,
			 uint8_t *scratch)
{
	int ret = flash_read(addr, scratch, len);

	if (ret < 0) {
		printk("spi_flash_rw: %s read-back failed (%d)\n", what, ret);
		return false;
	}

	if (memcmp(scratch, expect, len) != 0) {
		printk("spi_flash_rw: %s mismatch at 0x%06x\n", what, addr);
		printk("spi_flash_rw:   wrote ");
		print_bytes(expect, MIN(len, 16U));
		printk("\nspi_flash_rw:   read  ");
		print_bytes(scratch, MIN(len, 16U));
		printk("\n");
		return false;
	}

	return true;
}

/* Read the whole sector and report how many bytes are not 0xFF. */
static int count_non_erased(uint32_t addr, size_t *non_ff)
{
	*non_ff = 0U;

	for (size_t off = 0U; off < FLASH_SECTOR_SIZE; off += FLASH_PAGE_SIZE) {
		int ret = flash_read(addr + off, &sector[off], FLASH_PAGE_SIZE);

		if (ret < 0) {
			return ret;
		}
	}

	for (size_t i = 0U; i < FLASH_SECTOR_SIZE; i++) {
		if (sector[i] != 0xFFU) {
			(*non_ff)++;
		}
	}

	return 0;
}

static bool find_flash(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(spi_controllers); i++) {
		const struct device *spi = spi_controllers[i];
		uint8_t id[3] = { 0U, 0U, 0U };
		int ret;

		if (!device_is_ready(spi)) {
			printk("spi_flash_rw: %s not ready\n", spi->name);
			continue;
		}

		flash_spi = spi;

		ret = flash_cmd_read(FLASH_CMD_RDID, NULL, id, sizeof(id));
		printk("spi_flash_rw: %s RDID =", spi->name);
		print_bytes(id, sizeof(id));
		printk(" (ret=%d)\n", ret);

		if ((ret == 0) && ((id[0] != 0x00U) || (id[1] != 0x00U) || (id[2] != 0x00U)) &&
		    ((id[0] != 0xFFU) || (id[1] != 0xFFU) || (id[2] != 0xFFU))) {
			return true;
		}
	}

	return false;
}

int main(void)
{
	const uint32_t base = CONFIG_APP_FLASH_RW_TEST_OFFSET;
	uint8_t status = 0U;
	size_t non_ff = 0U;
	size_t expected_programmed = 0U;
	bool ok = true;
	int ret;

	printk("\nspi_flash_rw: erase + program the first sector of the SPI NOR\n");
	printk("spi_flash_rw: this DESTROYS whatever is at 0x%06x\n", base);

	if (!find_flash()) {
		printk("spi_flash_rw: no controller answered RDID -> cannot continue\n");
		return 0;
	}

	ret = flash_read_status(&status);
	if (ret < 0) {
		printk("spi_flash_rw: RDSR failed (%d)\n", ret);
		return 0;
	}

	printk("spi_flash_rw: status before = 0x%02x\n", status);

	/* What the sector holds right now, before this run touches it. On a
	 * board that was left with CONFIG_APP_FLASH_RW_KEEP_PROGRAMMED data,
	 * this is the persistence check a same-boot read-back cannot give. */
	ret = count_non_erased(base, &non_ff);
	if (ret < 0) {
		printk("spi_flash_rw: read-back before erase failed (%d)\n", ret);
		return 0;
	}

	printk("spi_flash_rw: sector 0x%06x currently holds %u bytes that differ from 0xff%s\n",
	       base, (unsigned int)non_ff,
	       (non_ff == 0U) ? " (blank)" : " (previous run's data?)");

	if (CONFIG_APP_FLASH_RW_DUMP_BYTES > 0) {
		printk("spi_flash_rw: first %u bytes =",
		       (unsigned int)CONFIG_APP_FLASH_RW_DUMP_BYTES);
		print_bytes(sector, CONFIG_APP_FLASH_RW_DUMP_BYTES);
		printk("\n");
	}

	/* 1. Erase the sector and check that every byte came back 0xFF. */
	{
		uint32_t t0 = k_cycle_get_32();

		ret = flash_sector_erase(base);
		printk("spi_flash_rw: [1] erase (software WIP loop) took %u cycles\n",
		       (unsigned int)(k_cycle_get_32() - t0));
	}
	if (ret < 0) {
		printk("spi_flash_rw: sector erase failed (%d)\n", ret);
		return 0;
	}

	ret = count_non_erased(base, &non_ff);
	if (ret < 0) {
		printk("spi_flash_rw: read-back after erase failed (%d)\n", ret);
		return 0;
	}

	printk("spi_flash_rw: [1] erase 0x%06x..0x%06x: %u of %u bytes differ from 0xff -> %s\n",
	       base, base + FLASH_SECTOR_SIZE - 1U, (unsigned int)non_ff, FLASH_SECTOR_SIZE,
	       (non_ff == 0U) ? "PASS" : "FAIL");
	ok = ok && (non_ff == 0U);

	/* 2. Four bytes, the register-fed shape (4-byte command + 4-byte
	 *    data = 8 bytes of TX). */
	md_pattern(page, 4U);
	expected_programmed += count_non_ff(page, 4U);
	ret = flash_page_program(base, page, 4U);
	if (ret < 0) {
		printk("spi_flash_rw: 4-byte program failed (%d)\n", ret);
		return 0;
	}

	ok = verify_bytes(base, page, 4U, "[2] program 4 B (register path)", sector) && ok;
	printk("spi_flash_rw: [2] program 4 B at 0x%06x -> %s\n", base, ok ? "MATCH" : "mismatch");

	/* 3. 64 bytes: the short end of the TX DMA path (4 + 64 = 68 bytes). */
	md_pattern(page, 64U);
	expected_programmed += count_non_ff(page, 64U);
	ret = flash_page_program(base + FLASH_PAGE_SIZE, page, 64U);
	if (ret < 0) {
		printk("spi_flash_rw: 64-byte program failed (%d)\n", ret);
		return 0;
	}

	ok = verify_bytes(base + FLASH_PAGE_SIZE, page, 64U, "[3] program 64 B (TX DMA)",
			  sector) && ok;
	printk("spi_flash_rw: [3] program 64 B at 0x%06x -> %s\n", base + FLASH_PAGE_SIZE,
	       ok ? "MATCH" : "mismatch");

	/* 4. A whole page: 4 + 256 = 260 bytes, the shape a flash writer
	 *    actually needs. */
	md_pattern(page, FLASH_PAGE_SIZE);
	expected_programmed += count_non_ff(page, FLASH_PAGE_SIZE);
	ret = flash_page_program(base + 2U * FLASH_PAGE_SIZE, page, FLASH_PAGE_SIZE);
	if (ret < 0) {
		printk("spi_flash_rw: 256-byte program failed (%d)\n", ret);
		return 0;
	}

	ok = verify_bytes(base + 2U * FLASH_PAGE_SIZE, page, FLASH_PAGE_SIZE,
			  "[4] program 256 B (TX DMA)", sector) && ok;
	printk("spi_flash_rw: [4] program 256 B at 0x%06x -> %s\n",
	       base + 2U * FLASH_PAGE_SIZE, ok ? "MATCH" : "mismatch");

	/* 5. The programmed pages must be the only non-blank ones left. */
	ret = count_non_erased(base, &non_ff);
	if (ret < 0) {
		printk("spi_flash_rw: final read-back failed (%d)\n", ret);
		return 0;
	}

	printk("spi_flash_rw: [5] %u of %u bytes in the sector are now programmed (expected %u)\n",
	       (unsigned int)non_ff, FLASH_SECTOR_SIZE, (unsigned int)expected_programmed);
	ok = ok && (non_ff == expected_programmed);

	if (IS_ENABLED(CONFIG_APP_FLASH_RW_KEEP_PROGRAMMED)) {
		printk("spi_flash_rw: [6] skipping the final erase (CONFIG_APP_FLASH_RW_KEEP_PROGRAMMED)\n");
		printk("spi_flash_rw: %s (the sector stays programmed for the next boot)\n",
		       ok ? "PASS" : "FAIL");

		return 0;
	}

	/* 6. Erase again, so the sector ends up the way it started (blank). */
	ret = flash_sector_erase(base);
	if (ret < 0) {
		printk("spi_flash_rw: final erase failed (%d)\n", ret);
		return 0;
	}

	ret = count_non_erased(base, &non_ff);
	if (ret < 0) {
		printk("spi_flash_rw: read-back after the final erase failed (%d)\n", ret);
		return 0;
	}

	printk("spi_flash_rw: [6] erase again: %u of %u bytes differ from 0xff -> %s\n",
	       (unsigned int)non_ff, FLASH_SECTOR_SIZE, (non_ff == 0U) ? "PASS" : "FAIL");
	ok = ok && (non_ff == 0U);

	/* 7. The engine has a POLL phase (PHASE_ACTION = 3) that keeps clocking
	 *    the command and comparing the answer inside one CS window -- the
	 *    WIP loop in hardware, bounded to `limit` attempts per call.
	 *    Positive and negative control, plus the repeat-until-done shape a
	 *    long wait (a sector erase) needs. */
	{
		const uint8_t cmd_rdsr = FLASH_CMD_RDSR;
		const uint8_t cmd_wrdi = 0x04U;
		uint32_t started;
		unsigned int calls;

		/* Positive: WEL is set the moment WREN completes, so the very
		 * first attempt has to match. */
		ret = flash_cmd_write(FLASH_CMD_WREN, NULL);
		ret = (ret < 0) ? ret : spi_agm_poll_status(flash_spi, &spi_cfg, &cmd_rdsr, 1U,
							    FLASH_STATUS_WEL, FLASH_STATUS_WEL,
							    10U);
		printk("spi_flash_rw: [7a] poll WEL=1 after WREN: ret=%d -> %s\n", ret,
		       (ret == 0) ? "PASS" : "FAIL");
		ok = ok && (ret == 0);
		(void)flash_cmd_write(cmd_wrdi, NULL);

		/* Real wait: start an erase and repeat the bounded poll until WIP
		 * reads 0, then confirm with a plain RDSR that it really did. */
		ret = flash_send_sector_erase(base);
		started = k_cycle_get_32();

		/* Each call covers up to 255 attempts, so a sector erase needs a
		 * handful of them; anything that is not a timeout is the verdict. */
		for (calls = 0U; calls < 2000U; calls++) {
			ret = spi_agm_poll_status(flash_spi, &spi_cfg, &cmd_rdsr, 1U,
						  FLASH_STATUS_WIP, 0U, 255U);
			if (ret != -ETIMEDOUT) {
				break;
			}
		}

		started = k_cycle_get_32() - started;

		if (ret == 0) {
			uint8_t status = 0xFFU;

			(void)flash_read_status(&status);
			printk("spi_flash_rw: [7b] WIP=0 by poll: %u calls, %u cycles, "
			       "RDSR now 0x%02x -> %s\n",
			       calls + 1U, (unsigned int)started, status,
			       ((status & FLASH_STATUS_WIP) == 0U) ? "PASS" : "FAIL");
			ok = ok && ((status & FLASH_STATUS_WIP) == 0U);
		} else {
			printk("spi_flash_rw: [7b] WIP=0 by poll failed (%d) after %u calls\n",
			       ret, calls);
			ok = false;
		}

		/* Negative: WEL is clear (the erase self-cleared it and WRDI
		 * confirmed), so asking for WEL=1 has to run out of attempts. */
		ret = spi_agm_poll_status(flash_spi, &spi_cfg, &cmd_rdsr, 1U, FLASH_STATUS_WEL,
					  FLASH_STATUS_WEL, 10U);
		printk("spi_flash_rw: [7c] poll WEL=1 without WREN: ret=%d -> %s\n", ret,
		       (ret == -ETIMEDOUT) ? "PASS (timed out as expected)" : "FAIL");
		ok = ok && (ret == -ETIMEDOUT);
	}

#if defined(CONFIG_SPI_ASYNC)
	/* 8. The interrupt-driven entry point: submit a 256-byte read and get
	 *    the callback from the engine's IRQ, then check the bytes against
	 *    the blocking path. */
	{
		static uint8_t async_buf[FLASH_PAGE_SIZE] __aligned(4);
		uint8_t sync_buf[FLASH_PAGE_SIZE] __aligned(4);
		uint8_t cmd[4] = { FLASH_CMD_READ, 0U, 0U, 0U };
		uint8_t echo[4];
		struct spi_buf tb = { .buf = cmd, .len = sizeof(cmd) };
		struct spi_buf rb[2] = {
			{ .buf = echo, .len = sizeof(echo) },
			{ .buf = async_buf, .len = sizeof(async_buf) },
		};
		const struct spi_buf_set ts = { .buffers = &tb, .count = 1U };
		const struct spi_buf_set rs = { .buffers = rb, .count = 2U };
		int second;
		int submit;
		bool same;

		async_result = -1;
		submit = spi_transceive_cb(flash_spi, &spi_cfg, &ts, &rs, async_cb, NULL);

		/* Nothing between the two calls: the first transfer is still
		 * running (256 bytes take ~82 us at 25 MHz), so this has to be
		 * refused. */
		second = spi_transceive_cb(flash_spi, &spi_cfg, &ts, &rs, async_cb, NULL);

		if (k_sem_take(&async_done, K_MSEC(1000)) != 0) {
			printk("spi_flash_rw: [8] async read never completed\n");
			ok = false;
		} else if ((submit != 0) || (async_result != 0)) {
			printk("spi_flash_rw: [8] async submit=%d callback=%d -> FAIL\n", submit,
			       async_result);
			ok = false;
		} else {
			memset(sync_buf, 0, sizeof(sync_buf));
			ret = flash_read(base, sync_buf, sizeof(sync_buf));
			same = (ret == 0) && (memcmp(async_buf, sync_buf, sizeof(async_buf)) == 0);

			printk("spi_flash_rw: [8] async %u B read: submit=%d second=%d, "
			       "bytes %s the blocking read -> %s\n",
			       (unsigned int)sizeof(async_buf), submit, second,
			       same ? "match" : "differ", same ? "PASS" : "FAIL");
			ok = ok && same;

			if (second == 0) {
				/* It was accepted after all (the transfer finished
				 * first): drain its completion. */
				(void)k_sem_take(&async_done, K_MSEC(1000));
			}
		}
	}
#endif

#if defined(CONFIG_APP_FLASH_RW_CS_GPIO)
	/* 9. Chip select from a GPIO (cs_is_gpio), watched on the pin itself. */
	{
		static uint8_t cs_buf[FLASH_PAGE_SIZE] __aligned(4);
		static const struct spi_config cs_cfg = {
			/* Slowest divider (SYSCLK/256): the transfer has to last
			 * long enough for the CPU to sample the pin. */
			.frequency = 400000U,
			.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
				     SPI_TRANSFER_MSB,
			.slave = 0,
			.cs = {
				.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(led2), gpios),
				.cs_is_gpio = true,
			},
		};
		uint8_t cmd[4] = { FLASH_CMD_READ, 0U, 0U, 0U };
		uint8_t echo[4];
		struct spi_buf tb = { .buf = cmd, .len = sizeof(cmd) };
		struct spi_buf rb[2] = {
			{ .buf = echo, .len = sizeof(echo) },
			{ .buf = cs_buf, .len = sizeof(cs_buf) },
		};
		const struct spi_buf_set ts = { .buffers = &tb, .count = 1U };
		const struct spi_buf_set rs = { .buffers = rb, .count = 2U };
		uint32_t samples = 0U;
		uint32_t asserted = 0U;
		int idle_level;

		if (!gpio_is_ready_dt(&cs_gpio)) {
			printk("spi_flash_rw: [9] %s not ready -> FAIL\n", cs_gpio.port->name);
			ok = false;
		} else {
			cs_samples = 0U;
			cs_asserted = 0U;
			k_timer_start(&cs_timer, K_MSEC(1), K_MSEC(1));

			ret = spi_transceive(flash_spi, &cs_cfg, &ts, &rs);

			k_timer_stop(&cs_timer);
			samples = cs_samples;
			asserted = cs_asserted;
			idle_level = gpio_pin_get_dt(&cs_gpio);

			printk("spi_flash_rw: [9] cs_is_gpio: ret=%d, %u of %u timer samples "
			       "asserted, idle now %d, first byte 0x%02x -> %s\n",
			       ret, (unsigned int)asserted, (unsigned int)samples, idle_level,
			       cs_buf[0],
			       ((ret == 0) && (asserted > 0U) && (idle_level == 0) &&
				(cs_buf[0] == 0xFFU))
				       ? "PASS"
				       : "FAIL");
			ok = ok && (ret == 0) && (asserted > 0U) && (idle_level == 0) &&
			     (cs_buf[0] == 0xFFU);
		}
	}
#endif

#if defined(CONFIG_PM_DEVICE_RUNTIME)
	/* 10. Runtime PM: idle means the controller's APB clock is gated (its
	 *     registers read back 0), and a transfer turns it back on. */
	{
		uintptr_t base = (uintptr_t)DT_REG_ADDR(DT_NODELABEL(spi1));
		static uint8_t pm_buf[FLASH_PAGE_SIZE] __aligned(4);
		uint32_t gated;
		uint32_t clk_gated;
		uint32_t clk_resumed;
		int put_ret;

		/* A balanced get/put pair is what suspends. */
		ret = pm_device_runtime_get(flash_spi);
		put_ret = pm_device_runtime_put(flash_spi);
		gated = sys_read32(base);
		clk_gated = sys_read32(0x03000060U);

		/* No get() on purpose here: the driver has to resume itself. */
		memset(pm_buf, 0, sizeof(pm_buf));
		ret = flash_read(base + FLASH_PAGE_SIZE, pm_buf, sizeof(pm_buf));

		/* Hold a reference while looking at the register again. */
		(void)pm_device_runtime_get(flash_spi);

		uint32_t resumed = sys_read32(base);

		clk_resumed = sys_read32(0x03000060U);

		(void)pm_device_runtime_put(flash_spi);

		printk("spi_flash_rw: [10] runtime PM: put=%d, APB_CLKENABLE 0x%08x -> 0x%08x, "
		       "CTRL 0x%08x -> 0x%08x, read=%d (first byte 0x%02x) -> %s\n",
		       put_ret, clk_gated, clk_resumed, gated, resumed, ret, pm_buf[0],
		       ((put_ret == 0) && (clk_gated != clk_resumed) && (ret == 0) &&
			(pm_buf[0] == 0xFFU) && (resumed != 0U))
			       ? "PASS"
			       : "FAIL");
		ok = ok && (put_ret == 0) && (clk_gated != clk_resumed) && (ret == 0) &&
		     (pm_buf[0] == 0xFFU) && (resumed != 0U);
	}
#endif

#if defined(CONFIG_PM)
	/* 11. The controller's PM across a real SoC sleep entry/exit. With
	 *     runtime PM on the device is already suspended while idle and the
	 *     system path skips it; with runtime PM off (CONFIG_PM_DEVICE only)
	 *     the system path is what suspends it. The two configurations are
	 *     told apart by the APB gate bit and by CTRL afterwards. */
	{
		uintptr_t base = (uintptr_t)DT_REG_ADDR(DT_NODELABEL(spi1));
		static uint8_t sleep_buf[FLASH_PAGE_SIZE] __aligned(4);
		enum pm_device_state dev_state = PM_DEVICE_STATE_ACTIVE;
		uint32_t clk_before;
		uint32_t clk_after;
		uint32_t ctrl_before;
		uint32_t ctrl_after;

		/* Let the driver go idle so runtime PM (if enabled) suspends. */
		pm_notifier_register(&pm_notifier);

		k_sleep(K_MSEC(20));

		clk_before = sys_read32(0x03000060U);
		ctrl_before = sys_read32(base);

		(void)pm_state_force(0U, &(struct pm_state_info){ PM_STATE_SUSPEND_TO_IDLE, 0,
								  0 });
		k_sleep(K_MSEC(200));

		clk_after = sys_read32(0x03000060U);
		ctrl_after = sys_read32(base);
		(void)pm_device_state_get(flash_spi, &dev_state);

		/* No get() on purpose: the driver has to bring it back itself. */
		memset(sleep_buf, 0, sizeof(sleep_buf));
		ret = flash_read(base + FLASH_PAGE_SIZE, sleep_buf, sizeof(sleep_buf));

		printk("spi_flash_rw: [11] across sleep: entries=%u exits=%u, "
		       "APB_CLKENABLE 0x%08x -> 0x%08x, CTRL 0x%08x -> 0x%08x, device %s, "
		       "read=%d (first byte 0x%02x) -> %s\n",
		       (unsigned int)pm_entries, (unsigned int)pm_exits,
		       clk_before, clk_after, ctrl_before, ctrl_after,
		       pm_device_state_str(dev_state), ret, sleep_buf[0],
		       ((pm_entries > 0U) && (ret == 0) && (sleep_buf[0] == 0xFFU)) ? "PASS"
									     : "FAIL");
		ok = ok && (pm_entries > 0U) && (ret == 0) && (sleep_buf[0] == 0xFFU);
	}
#endif

#if defined(CONFIG_APP_FLASH_RW_QUAD)
	/* 13. Whole-part read benchmark: the same 2 MiB through 0x03, 0x6B and
	 *     0xEB, each checksummed so the three have to agree on every byte. */
	{
		static uint8_t fast_buf[2048] __aligned(4);
		const uint32_t total = 2U * 1024U * 1024U;
		const uint32_t chunks = total / sizeof(fast_buf);
		const struct {
			uint8_t opcode;
			uint8_t lines; /* data phase line mode */
			bool quad_io;  /* address and mode on four lines too */
			const char *name;
		} modes[] = {
			{ 0x03U, SPI_AGM_LINES_SINGLE, false, "0x03 (1-1-1-1)" },
			{ 0x6BU, SPI_AGM_LINES_QUAD, false, "0x6B (1-1-1-4)" },
			{ 0xEBU, SPI_AGM_LINES_QUAD, true, "0xEB (1-4-4-4)" },
		};
		uint32_t sums[ARRAY_SIZE(modes)] = { 0U };
		uint32_t times[ARRAY_SIZE(modes)] = { 0U };
		bool all_ok = true;

		for (size_t m = 0U; m < ARRAY_SIZE(modes); m++) {
			uint32_t sum = 0U;
			uint32_t t0 = k_uptime_get_32();
			bool failed = false;

			for (uint32_t i = 0U; i < chunks; i++) {
				uint32_t addr = i * sizeof(fast_buf);

				if (modes[m].opcode == 0x03U) {
					ret = flash_read(addr, fast_buf, sizeof(fast_buf));
				} else {
					uint8_t cmd[4] = { modes[m].opcode, (uint8_t)(addr >> 16),
							   (uint8_t)(addr >> 8), (uint8_t)addr };
					struct spi_agm_phase ph[4];
					size_t n;

					if (modes[m].quad_io) {
						/* 0xEB: command, then 4 bytes on four
						 * lines (24-bit address + mode). */
						ph[0] = (struct spi_agm_phase){
							.lines = SPI_AGM_LINES_SINGLE, .len = 1U,
							.tx = &cmd[0] };
						ph[1] = (struct spi_agm_phase){
							.lines = SPI_AGM_LINES_QUAD, .len = 4U,
							.tx = &cmd[1] };
						ph[2] = (struct spi_agm_phase){
							.lines = SPI_AGM_LINES_QUAD,
							.dummy = true, .len = 2U };
						n = 4U;
					} else {
						/* 0x6B: command and address single. */
						ph[0] = (struct spi_agm_phase){
							.lines = SPI_AGM_LINES_SINGLE, .len = 1U,
							.tx = &cmd[0] };
						ph[1] = (struct spi_agm_phase){
							.lines = SPI_AGM_LINES_SINGLE, .len = 3U,
							.tx = &cmd[1] };
						ph[2] = (struct spi_agm_phase){
							.lines = SPI_AGM_LINES_SINGLE,
							.dummy = true, .len = 1U };
						n = 4U;
					}

					ph[3] = (struct spi_agm_phase){
						.lines = modes[m].lines, .len = sizeof(fast_buf),
						.rx = fast_buf };
					ret = spi_agm_transceive_phases(flash_spi, &spi_cfg, ph, n);
				}

				if (ret < 0) {
					printk("spi_flash_rw: [13] %s read #%u failed (%d)\n",
					       modes[m].name, (unsigned int)i, ret);
					failed = true;
					break;
				}

				for (size_t j = 0U; j < sizeof(fast_buf); j++) {
					sum = (sum << 1) ^ (sum >> 31) ^ fast_buf[j];
				}
			}

			times[m] = k_uptime_get_32() - t0;
			sums[m] = sum;
			all_ok = all_ok && !failed;
		}

		for (size_t m = 0U; m < ARRAY_SIZE(modes); m++) {
			printk("spi_flash_rw: [13] %s: %u ms, %u KiB/s, checksum 0x%08x\n",
			       modes[m].name, (unsigned int)times[m],
			       (unsigned int)((uint64_t)total /
					      (times[m] ? times[m] : 1U) * 1000U / 1024U),
			       sums[m]);
		}

		printk("spi_flash_rw: [13] three read shapes agree on the whole part -> %s\n",
		       ((sums[0] == sums[1]) && (sums[1] == sums[2]) && all_ok) ? "PASS"
									      : "FAIL");
		ok = ok && (sums[0] == sums[1]) && (sums[1] == sums[2]) && all_ok;
	}

	/* 12. Quad read: 0x6B puts the command, address and dummy byte on one
	 *     line and the data on four, and the bytes have to equal what 0x03
	 *     reads from the same address. */
	{
		static uint8_t quad_buf[16] __aligned(4);
		static uint8_t single_buf[16] __aligned(4);
		uint8_t cmd[4];
		uint8_t sr2_tx[2] = { 0x31U, 0x02U }; /* Write Status Register-2, QE=1 */
		uint8_t sr2_rx[2] = { 0U, 0U };
		uint8_t echo[4];
		struct spi_buf sr2_tb = { .buf = sr2_tx, .len = sizeof(sr2_tx) };
		const struct spi_buf_set sr2_ts = { .buffers = &sr2_tb, .count = 1U };
		struct spi_buf sr2_rb[2] = {
			{ .buf = echo, .len = 1U },
			{ .buf = sr2_rx, .len = 1U },
		};
		const struct spi_buf_set sr2_rs = { .buffers = sr2_rb, .count = 2U };
		uint8_t rd_sr2[2] = { 0x35U, 0xFFU };
		struct spi_buf rd_tb = { .buf = rd_sr2, .len = 1U };
		const struct spi_buf_set rd_ts = { .buffers = &rd_tb, .count = 1U };
		struct spi_agm_phase phases[4];
		const uint32_t qaddr = base + 3U * FLASH_PAGE_SIZE;
		bool same;
		uint8_t sr1_before = 0U;
		uint8_t sr2_before = 0U;
		uint8_t sr1_after = 0U;
		uint8_t quad_before[8] __aligned(4);

		/* Known content: program one page of the pattern. */
		md_pattern(page, FLASH_PAGE_SIZE);
		ret = flash_page_program(qaddr, page, FLASH_PAGE_SIZE);
		if (ret < 0) {
			printk("spi_flash_rw: [12] program for the quad read failed (%d)\n", ret);
			ok = false;
		} else {
			/* QE state and a quad read *before* touching the status
			 * register: does this part need QE for 0x6B at all? */
			(void)flash_read_status(&sr1_before);
			rd_sr2[0] = 0x35U;
			rd_tb.len = 1U;
			(void)spi_transceive(flash_spi, &spi_cfg, &rd_ts, &sr2_rs);
			sr2_before = sr2_rx[1];

			cmd[0] = 0x6BU;
			cmd[1] = (uint8_t)(qaddr >> 16);
			cmd[2] = (uint8_t)(qaddr >> 8);
			cmd[3] = (uint8_t)qaddr;
			phases[0] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .len = 1U, .tx = &cmd[0] };
			phases[1] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .len = 3U, .tx = &cmd[1] };
			phases[2] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .dummy = true, .len = 1U };
			phases[3] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_QUAD,
							    .len = sizeof(quad_before),
							    .rx = quad_before };
			memset(quad_before, 0, sizeof(quad_before));
			(void)spi_agm_transceive_phases(flash_spi, &spi_cfg, phases,
							ARRAY_SIZE(phases));

			/* QE has to be set before the part answers 0x6B. */
			(void)flash_cmd_write(FLASH_CMD_WREN, NULL);
			ret = spi_write(flash_spi, &spi_cfg, &sr2_ts);

			memset(sr2_rx, 0, sizeof(sr2_rx));
			ret = ret < 0 ? ret : spi_transceive(flash_spi, &spi_cfg, &rd_ts,
							     &sr2_rs);
			(void)flash_read_status(&sr1_after);

			printk("spi_flash_rw: [12] SR1/SR2 before = 0x%02x/0x%02x, "
			       "QE write ret=%d, after = 0x%02x/0x%02x (this part answers "
			       "0x6B without QE)\n",
			       sr1_before, sr2_before, ret, sr1_after, sr2_rx[1]);
			printk("spi_flash_rw: [12] 0x6B before the QE write =");
			print_bytes(quad_before, 8U);
			printk("\n");

			/* Reference: the plain 0x03 read. */
			ret = flash_read(qaddr, single_buf, sizeof(single_buf));

			/* 0x6B: 1-1-1-4. */
			cmd[0] = 0x6BU;
			cmd[1] = (uint8_t)(qaddr >> 16);
			cmd[2] = (uint8_t)(qaddr >> 8);
			cmd[3] = (uint8_t)qaddr;

			phases[0] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .len = 1U, .tx = &cmd[0] };
			phases[1] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .len = 3U, .tx = &cmd[1] };
			phases[2] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .dummy = true, .len = 1U };
			phases[3] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_QUAD,
							    .len = sizeof(quad_buf),
							    .rx = quad_buf };

			memset(quad_buf, 0, sizeof(quad_buf));
			ret = ret < 0 ? ret : spi_agm_transceive_phases(flash_spi, &spi_cfg,
								       phases,
								       ARRAY_SIZE(phases));
			same = (ret == 0) && (memcmp(quad_buf, single_buf, sizeof(quad_buf)) == 0);

			printk("spi_flash_rw: [12] quad 0x6B read: ret=%d\n", ret);
			printk("spi_flash_rw: [12]   0x03 =");
			print_bytes(single_buf, 8U);
			printk("\nspi_flash_rw: [12]   0x6B =");
			print_bytes(quad_buf, 8U);
			printk(" -> %s\n", same ? "MATCH" : "mismatch");
			ok = ok && same;

			/* 0x3B: the same shape on two data lines. */
			cmd[0] = 0x3BU;
			phases[3].lines = SPI_AGM_LINES_DUAL;
			memset(quad_buf, 0, sizeof(quad_buf));
			ret = spi_agm_transceive_phases(flash_spi, &spi_cfg, phases,
							ARRAY_SIZE(phases));
			same = (ret == 0) &&
			       (memcmp(quad_buf, single_buf, sizeof(quad_buf)) == 0);
			printk("spi_flash_rw: [12] dual 0x3B read: ret=%d, bytes =", ret);
			print_bytes(quad_buf, 8U);
			printk(" -> %s\n", same ? "MATCH" : "mismatch");
			ok = ok && same;

			/* 0xEB: the address and mode byte go out on four lines as
			 * well (that needs the IO1 drive path, not just its input). */
			uint8_t eb_tx[4] = { (uint8_t)(qaddr >> 16), (uint8_t)(qaddr >> 8),
					     (uint8_t)qaddr, 0x00U /* M[7:0] = normal read */ };

			cmd[0] = 0xEBU;
			phases[0] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_SINGLE,
							    .len = 1U, .tx = &cmd[0] };
			phases[1] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_QUAD,
							    .len = 4U, .tx = eb_tx };
			phases[2] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_QUAD,
							    .dummy = true, .len = 2U };
			phases[3] = (struct spi_agm_phase){ .lines = SPI_AGM_LINES_QUAD,
							    .len = sizeof(quad_buf),
							    .rx = quad_buf };
			memset(quad_buf, 0, sizeof(quad_buf));
			ret = spi_agm_transceive_phases(flash_spi, &spi_cfg, phases,
							ARRAY_SIZE(phases));
			same = (ret == 0) &&
			       (memcmp(quad_buf, single_buf, sizeof(quad_buf)) == 0);
			printk("spi_flash_rw: [12] quad-IO 0xEB read: ret=%d, bytes =", ret);
			print_bytes(quad_buf, 8U);
			printk(" -> %s\n", same ? "MATCH" : "mismatch");
			ok = ok && same;
		}
	}
#endif

	printk("spi_flash_rw: %s\n", ok ? "PASS" : "FAIL");

	return 0;
}
