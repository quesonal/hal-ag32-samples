/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * usb_msc_dfu_boot -- UF2-style USB MSC DFU channel for spi_boot_loader.
 *
 * Modelled on a UF2 bootloader (RP2040 BOOTSEL, Adafruit nRF52): the device
 * stores no file system. It publishes a *fake* FAT16 volume so a host mounts
 * it and copies a .uf2 onto it, and it turns the 512-byte MSC writes into a
 * UF2 block stream:
 *
 *   sector 0, FAT sector 0   the static metadata from src/uf2_volume.h
 *   every other sector       reads back as zero (an empty FAT16 volume);
 *                            writes into the metadata region are kept in a
 *                            small RAM overlay so the volume stays coherent
 *   a write carrying a UF2 block
 *                            the payload is streamed straight into the store
 *                            the boot record does not point at
 *
 * The last block ends the update. The sample then closes the MSC interface
 * (the host sees the drive disappear), publishes through the boot driver
 * (agm_boot_upload_finish) and reboots, so the loader boots the new image.
 *
 * The image is never held in SRAM as a whole: only the 512-byte block being
 * parsed, plus whatever the boot driver keeps while it programs the store.
 *
 * Why the volume is a lie: the whole point of a UF2 bootloader is that the
 * file is not stored -- the block stream *is* the transfer, so the volume
 * can claim far more space than the device has (64 MiB here) and the host
 * never runs out. Nothing but the host's own FAT bookkeeping is ever kept.
 *
 * This is the USB MSC upload channel next to the loader's console / AN3155 /
 * mcumgr paths (docs/BOOT-DFU-STATUS.md 0.1, the update section). It runs in
 * an application rather than in the loader itself because the loader's
 * 96 KiB region is sized for the console and the two protocol servers.
 */

#include <sample_usbd.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/usbd.h>

#include <string.h>

#include "uf2_volume.h"

#define BOOT_DEV DEVICE_DT_GET(DT_NODELABEL(boot))

/* disk_access()/MSC name of the volume; uf2_disk_ops owns every access. */
#define MSC_DISK_NAME "UF2"

/* ---- UF2 block layout (Microsoft USB Flashing Format) ---------------- */

#define UF2_MAGIC0       0x0A324655u
#define UF2_MAGIC1       0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u

/* Our own family id; tools/bin_to_uf2.py stamps the same value. targetAddr
 * is a *store* offset, not an absolute flash address (see the tool). */
#define UF2_FAMILY       0x41474D55u

/* tools/bin_to_uf2.py emits 256-byte payloads, so every targetAddr is a
 * multiple of 256 and the stream is trivially forward-ordered. */
#define UF2_UNIT         256u

#define UF2_OFF_MAGIC0   0u
#define UF2_OFF_MAGIC1   4u
#define UF2_OFF_FLAGS    8u
#define UF2_OFF_ADDR     12u
#define UF2_OFF_SIZE     16u
#define UF2_OFF_BLOCK    20u
#define UF2_OFF_NUMBLK   24u
#define UF2_OFF_FAMILY   28u
#define UF2_OFF_DATA     32u
#define UF2_OFF_END      508u
#define UF2_PAYLOAD_MAX  476u

/* ---- volume geometry (uf2_volume.h carries the generated numbers) ----- */

#define VOL_FAT1_START   1u
#define VOL_FAT2_START   (VOL_FAT1_START + UF2_VOL_FAT_SECTORS)
#define VOL_META_END     UF2_VOL_DATA_START

/* How many metadata sectors the host's own bookkeeping can touch before the
 * overlay gives up. One file needs its directory entry plus a FAT chain; a
 * 200 KiB image (the layout's app-size) is ~100 clusters, i.e. one FAT
 * sector per copy, so this is generous. */
#define META_SLOTS 16

struct meta_slot {
	uint32_t lba;
	bool used;
	uint8_t data[UF2_VOL_SECTOR_SIZE];
};

static struct meta_slot meta[META_SLOTS];

/* ---- update session -------------------------------------------------- */

static struct usbd_context *sample_usbd;

static struct {
	enum agm_boot_target target;
	bool active;
	uint32_t nblocks;
	uint32_t block;
	uint32_t written;
	uint32_t total;
	atomic_t ready;
	atomic_t failed;
} upd;

static void upd_reset(void)
{
	upd.active = false;
	upd.block = 0;
	upd.written = 0;
	upd.total = 0;
	upd.nblocks = 0;
	atomic_clear(&upd.ready);
	atomic_clear(&upd.failed);
}

/* The store the record does not point at; the running image stays intact. */
static enum agm_boot_target inactive_store(void)
{
	struct agm_boot_info info;

	if (agm_boot_info_get(BOOT_DEV, &info) == 0 && info.active == 0U) {
		return AGM_BOOT_TARGET_STORE_B;
	}

	return AGM_BOOT_TARGET_STORE_A;
}

static void update_fail(const char *what, uint32_t block, int err)
{
	printk("FAIL: %s at UF2 block %u (%d)\n", what, block, err);
	atomic_set(&upd.failed, 1);
}

static int update_begin(uint32_t nblocks, uint32_t addr0)
{
	uint32_t max;
	int err;

	if (addr0 != 0U) {
		update_fail("first UF2 block is not at offset 0", 0, -EINVAL);
		return -EINVAL;
	}

	upd.target = inactive_store();
	max = agm_boot_upload_max(BOOT_DEV, upd.target);
	if (nblocks * UF2_UNIT > max) {
		printk("FAIL: UF2 holds up to %u B, store %s takes %u\n",
		       nblocks * UF2_UNIT, agm_boot_target_name(upd.target), max);
		atomic_set(&upd.failed, 1);
		return -EINVAL;
	}

	err = agm_boot_upload_begin(BOOT_DEV, upd.target);
	if (err) {
		printk("FAIL: upload_begin(%s): %d\n",
		       agm_boot_target_name(upd.target), err);
		atomic_set(&upd.failed, 1);
		return err;
	}

	printk("receiving UF2: %u blocks into store %s\n", nblocks,
	       agm_boot_target_name(upd.target));
	upd.active = true;
	upd.nblocks = nblocks;
	upd.block = 0;
	upd.written = 0;

	return 0;
}

static int uf2_block_handle(const uint8_t *s)
{
	uint32_t size = sys_get_le32(s + UF2_OFF_SIZE);
	uint32_t block = sys_get_le32(s + UF2_OFF_BLOCK);
	uint32_t nblocks = sys_get_le32(s + UF2_OFF_NUMBLK);
	uint32_t family = sys_get_le32(s + UF2_OFF_FAMILY);
	uint32_t addr = sys_get_le32(s + UF2_OFF_ADDR);
	int err;

	if (family != UF2_FAMILY) {
		update_fail("UF2 family mismatch", block, -EINVAL);
		return -EIO;
	}

	if (size > UF2_PAYLOAD_MAX || nblocks == 0U || block >= nblocks) {
		update_fail("malformed UF2 block", block, -EINVAL);
		return -EIO;
	}

	if (!upd.active && update_begin(nblocks, addr) != 0) {
		return -EIO;
	}

	if (block != upd.block || addr != upd.written) {
		update_fail("UF2 block out of order", block, -EINVAL);
		return -EIO;
	}

	err = agm_boot_upload_write(BOOT_DEV, upd.target, addr,
				    s + UF2_OFF_DATA, size);
	if (err) {
		update_fail("upload_write", block, err);
		return -EIO;
	}

	upd.written += size;
	upd.block++;

	if (block + 1U == nblocks) {
		upd.total = upd.written;
		atomic_set(&upd.ready, 1);
	}

	return 0;
}

/* ---- the fake FAT16 volume ------------------------------------------- */

static bool uf2_is_block(const uint8_t *s)
{
	return sys_get_le32(s + UF2_OFF_MAGIC0) == UF2_MAGIC0 &&
	       sys_get_le32(s + UF2_OFF_MAGIC1) == UF2_MAGIC1 &&
	       sys_get_le32(s + UF2_OFF_END) == UF2_MAGIC_END;
}

static const uint8_t *meta_lookup(uint32_t lba)
{
	for (int i = 0; i < META_SLOTS; i++) {
		if (meta[i].used && meta[i].lba == lba) {
			return meta[i].data;
		}
	}

	return NULL;
}

static void meta_store(uint32_t lba, const uint8_t *data)
{
	for (int i = 0; i < META_SLOTS; i++) {
		if (meta[i].used && meta[i].lba == lba) {
			memcpy(meta[i].data, data, UF2_VOL_SECTOR_SIZE);
			return;
		}
	}

	for (int i = 0; i < META_SLOTS; i++) {
		if (!meta[i].used) {
			meta[i].used = true;
			meta[i].lba = lba;
			memcpy(meta[i].data, data, UF2_VOL_SECTOR_SIZE);
			return;
		}
	}

	/* The metadata region is tiny; running out means something is writing
	 * much more than one file's bookkeeping. Drop it (the host still sees
	 * a successful write) rather than fail the copy. */
	printk("WARN: metadata overlay full, dropping LBA %u\n", lba);
}

static int uf2_disk_status(struct disk_info *disk)
{
	ARG_UNUSED(disk);

	return DISK_STATUS_OK;
}

static int uf2_disk_init(struct disk_info *disk)
{
	ARG_UNUSED(disk);

	return 0;
}

static int uf2_disk_read(struct disk_info *disk, uint8_t *buf,
			 uint32_t lba, uint32_t count)
{
	ARG_UNUSED(disk);

	for (uint32_t i = 0; i < count; i++, lba++, buf += UF2_VOL_SECTOR_SIZE) {
		const uint8_t *src = meta_lookup(lba);

		if (src != NULL) {
			memcpy(buf, src, UF2_VOL_SECTOR_SIZE);
		} else if (lba == 0U) {
			memcpy(buf, uf2_boot_sector, UF2_VOL_SECTOR_SIZE);
		} else if (lba == VOL_FAT1_START || lba == VOL_FAT2_START) {
			memcpy(buf, uf2_fat0, UF2_VOL_SECTOR_SIZE);
		} else {
			/* Empty FAT, empty root directory, empty data area. */
			memset(buf, 0, UF2_VOL_SECTOR_SIZE);
		}
	}

	return 0;
}

static int uf2_disk_write(struct disk_info *disk, const uint8_t *buf,
			  uint32_t lba, uint32_t count)
{
	ARG_UNUSED(disk);

	for (uint32_t i = 0; i < count; i++, lba++, buf += UF2_VOL_SECTOR_SIZE) {
		if (uf2_is_block(buf)) {
			int err = uf2_block_handle(buf);

			if (err) {
				/* Fail the SCSI WRITE so the copy shows an
				 * error instead of "succeeding" silently. */
				return err;
			}

			continue;
		}

		/* The host's own FAT/directory bookkeeping: keep it so the
		 * volume stays coherent if the host reads it back. Writes into
		 * the data area that are not UF2 blocks (another file the host
		 * left behind) are not stored -- nothing reads them back. */
		if (lba < VOL_META_END) {
			meta_store(lba, buf);
		}
	}

	return 0;
}

static int uf2_disk_ioctl(struct disk_info *disk, uint8_t cmd, void *buff)
{
	ARG_UNUSED(disk);

	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT:
		*(uint32_t *)buff = UF2_VOL_SECTORS;
		break;
	case DISK_IOCTL_GET_SECTOR_SIZE:
		*(uint32_t *)buff = UF2_VOL_SECTOR_SIZE;
		break;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
		*(uint32_t *)buff = UF2_VOL_SECTOR_SIZE;
		break;
	case DISK_IOCTL_CTRL_SYNC:
	case DISK_IOCTL_CTRL_INIT:
	case DISK_IOCTL_CTRL_DEINIT:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* The MSC class may issue an erase (WRITE SAME/UNMAP); the volume has no
 * real storage behind it, so accept and ignore it. */
static int uf2_disk_erase(struct disk_info *disk, uint32_t lba, uint32_t count)
{
	ARG_UNUSED(disk);
	ARG_UNUSED(lba);
	ARG_UNUSED(count);

	return 0;
}

static const struct disk_operations uf2_disk_ops = {
	.init = uf2_disk_init,
	.status = uf2_disk_status,
	.read = uf2_disk_read,
	.write = uf2_disk_write,
	.erase = uf2_disk_erase,
	.ioctl = uf2_disk_ioctl,
};

static struct disk_info uf2_disk = {
	.name = MSC_DISK_NAME,
	.ops = &uf2_disk_ops,
};

/* MSC LUN: the host sees the fake volume as a removable drive. */
USBD_DEFINE_MSC_LUN(uf2, MSC_DISK_NAME, "AgRV", "UF2 DFU volume", "0.01");

int main(void)
{
	int err;

	printk("\n");
	printk("=========================================\n");
	printk(" usb_msc_dfu_boot: UF2-style MSC DFU\n");
	printk(" volume  : %u MiB FAT16 (fake, %u sectors)\n",
	       UF2_VOL_SECTORS * UF2_VOL_SECTOR_SIZE / (1024U * 1024U),
	       UF2_VOL_SECTORS);
	printk(" protocol: copy a .uf2 onto the drive\n");
	printk("=========================================\n");

	/* Answer the loader's trial-boot handshake, like spi_boot_app: this
	 * image's banner is its self-test. */
	err = agm_boot_trial_confirm();
	if (err == 0) {
		printk(" confirmed: this slot becomes permanent on the next boot\n");
	}

	err = disk_access_register(&uf2_disk);
	if (err) {
		printk("FAIL: disk_access_register: %d\n", err);
		return 0;
	}

	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		printk("FAIL: USB device init\n");
		return 0;
	}

	if (!usbd_can_detect_vbus(sample_usbd)) {
		err = usbd_enable(sample_usbd);
		if (err) {
			printk("FAIL: usbd_enable: %d\n", err);
			return 0;
		}
	}

	printk("drive is up -- copy a .uf2 onto it\n");

	while (true) {
		if (atomic_get(&upd.failed)) {
			/* Leave the record untouched and let the host retry with
			 * a correct file; the target store may be half written
			 * (same as the loader's other upload paths). */
			agm_boot_upload_abort(BOOT_DEV, upd.target);
			upd_reset();
			printk("ready for another .uf2\n");
		}

		if (atomic_get(&upd.ready)) {
			/* "Write finished" -> close the MSC interface first,
			 * exactly like a UF2 bootloader: the host sees the
			 * drive disappear, and only then does the device
			 * publish and reboot. */
			printk("UF2 complete, closing the MSC interface\n");
			usbd_disable(sample_usbd);

			err = agm_boot_upload_finish(BOOT_DEV, upd.target,
						     upd.total);
			if (err) {
				printk("FAIL: upload_finish(%u): %d\n",
				       upd.total, err);
				agm_boot_upload_abort(BOOT_DEV, upd.target);
				upd_reset();
				/* The drive was closed before publishing; bring
				 * it back so the host can retry without a reset. */
				if (usbd_enable(sample_usbd)) {
					printk("FAIL: usbd_enable\n");
				}
			} else {
				printk("PASS: %u B in store %s, rebooting\n",
				       upd.total,
				       agm_boot_target_name(upd.target));
				k_msleep(50);
				sys_reboot(SYS_REBOOT_COLD);
			}
		}

		k_msleep(50);
	}

	return 0;
}
