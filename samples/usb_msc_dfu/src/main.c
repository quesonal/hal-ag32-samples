/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K composite USB MSC + DFU sample.
 *
 * The device enumerates full-speed with two interfaces and serves a single
 * in-RAM disk (the "RAM" zephyr,ram-disk from boards/agrv2k_407.overlay)
 * over both of them:
 *
 *   MSC (Bulk-Only Transport)  the host sees a removable raw block device
 *                              and can read/write its 128 x 512 B sectors;
 *   DFU (runtime -> DFU mode)  dfu-util can upload/download the same disk
 *                              as the DFU image "ramdisk0".
 *
 * Because both paths go through disk_access() on the same ramdisk, a block
 * written over MSC reads back over a DFU upload and vice versa.
 *
 * The runtime -> DFU-mode transition needs a second usbd context: the
 * device-next stack registers class instances before usbd_init() and cannot
 * re-register a class after usbd_shutdown(), so on USBD_MSG_DFU_APP_DETACH
 * this sample shuts the runtime context (MSC + DFU runtime) down and brings
 * up a DFU-mode context with only the DFU class. This mirrors the upstream
 * samples/subsys/usb/dfu flow.
 *
 * Scope: FS-only, matching the udc_agm controller (64 B bulk MPS). The DFU
 * transfer size is pinned to the ramdisk sector size so each DFU block maps
 * onto exactly one sector.
 */

#include <sample_usbd.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_dfu.h>
#include <zephyr/usb/class/usbd_msc.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* disk_access() name of the ramdisk0 node in boards/agrv2k_407.overlay. */
#define DISK_NAME "RAM"

/* Each DFU block is one disk sector; see the BUILD_ASSERT below. */
BUILD_ASSERT(CONFIG_USBD_DFU_TRANSFER_SIZE == 512,
	     "the DFU transfer size must equal the ramdisk sector size");

/* MSC LUN: the host sees this as a removable disk. */
USBD_DEFINE_MSC_LUN(ram, DISK_NAME, "AgRV", "MSC+DFU RAMDisk", "0.01");

/*
 * The DFU image backend. It is the same disk as the MSC LUN; the block
 * cursor enforces the "start at block 0, then consecutive blocks" rule the
 * upstream DFU sample uses, so a malformed transfer sequence is rejected.
 */
struct dfu_image {
	const char *disk;
	uint32_t sector_size;
	uint32_t sector_count;
	uint32_t last_block;
	uint32_t transferred;
};

static struct dfu_image image0 = {
	.disk = DISK_NAME,
};

static int image_query(struct dfu_image *const img)
{
	int err;

	err = disk_access_init(img->disk);
	if (err) {
		LOG_ERR("disk_access_init(%s): %d", img->disk, err);
		return err;
	}

	err = disk_access_status(img->disk);
	if (err) {
		LOG_ERR("disk_access_status(%s): %d", img->disk, err);
		return err;
	}

	err = disk_access_ioctl(img->disk, DISK_IOCTL_GET_SECTOR_COUNT,
				&img->sector_count);
	if (err) {
		LOG_ERR("get sector count: %d", err);
		return err;
	}

	err = disk_access_ioctl(img->disk, DISK_IOCTL_GET_SECTOR_SIZE,
				&img->sector_size);
	if (err) {
		LOG_ERR("get sector size: %d", err);
		return err;
	}

	if (img->sector_size != CONFIG_USBD_DFU_TRANSFER_SIZE) {
		LOG_ERR("disk \"%s\": sector size %u != DFU transfer size %u",
			img->disk, img->sector_size,
			CONFIG_USBD_DFU_TRANSFER_SIZE);
		return -EINVAL;
	}

	return 0;
}

/*
 * A transfer always starts at block 0 (which also re-reads the disk geometry)
 * and then walks consecutive blocks; anything else is a protocol error.
 * Returns 0 when the block cursor is valid or when the transfer has no more
 * data, and the number of bytes still to move when it does not.
 */
static int image_block_begin(struct dfu_image *const img, const uint32_t block,
			     const uint16_t size)
{
	int err;

	if (block == 0) {
		err = image_query(img);
		if (err) {
			return err;
		}

		img->last_block = 0;
		img->transferred = 0;
	} else if (img->last_block + 1U != block) {
		LOG_ERR("DFU block out of sequence: %u after %u", block,
			img->last_block);
		return -EINVAL;
	}

	if (size == 0 || block >= img->sector_count) {
		/* Nothing left to move. */
		return 0;
	}

	if (size > img->sector_size) {
		LOG_ERR("DFU block %u: %u B exceeds the %u B sector", block, size,
			img->sector_size);
		return -EINVAL;
	}

	/* Positive return means "there is work for this block". */
	return (int)size;
}

/* DFU upload: device -> host, i.e. read the disk into the transfer buffer. */
static int image_read(void *const priv, const uint32_t block,
		      const uint16_t size,
		      uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	struct dfu_image *const img = priv;
	int err;

	err = image_block_begin(img, block, size);
	if (err <= 0) {
		return err;
	}

	err = disk_access_read(img->disk, buf, block, 1);
	if (err) {
		LOG_ERR("disk_access_read(%s, %u): %d", img->disk, block, err);
		return -EIO;
	}

	img->last_block = block;
	img->transferred += size;

	return (int)size;
}

/* DFU download: host -> device, i.e. write the transfer buffer to the disk. */
static int image_write(void *const priv, const uint32_t block,
		       const uint16_t size,
		       const uint8_t buf[static CONFIG_USBD_DFU_TRANSFER_SIZE])
{
	struct dfu_image *const img = priv;
	int err;

	err = image_block_begin(img, block, size);
	if (err <= 0) {
		return err;
	}

	err = disk_access_write(img->disk, buf, block, 1);
	if (err) {
		LOG_ERR("disk_access_write(%s, %u): %d", img->disk, block, err);
		return -EIO;
	}

	img->last_block = block;
	img->transferred += size;

	return 0;
}

USBD_DFU_DEFINE_IMG(ramdisk0, "ramdisk0", &image0, image_read, image_write,
		    NULL);

/*
 * DFU mode lives in its own context: sample_usbd (runtime: MSC + DFU
 * runtime) is shut down and this context is built up with only the DFU
 * class. The VID/PID are reused so the host sees the same product before and
 * after the detach.
 */
USBD_DEVICE_DEFINE(dfu_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_SAMPLE_USBD_VID, CONFIG_SAMPLE_USBD_PID);

USBD_DESC_LANG_DEFINE(dfu_lang);
USBD_DESC_CONFIG_DEFINE(dfu_fs_cfg_desc, "DFU FS Configuration");

static const uint8_t dfu_attributes =
	(IS_ENABLED(CONFIG_SAMPLE_USBD_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0) |
	(IS_ENABLED(CONFIG_SAMPLE_USBD_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0);

USBD_CONFIGURATION_DEFINE(dfu_fs_config, dfu_attributes,
			  CONFIG_SAMPLE_USBD_MAX_POWER, &dfu_fs_cfg_desc);

static void switch_to_dfu_mode(struct usbd_context *const ctx);

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (msg->type == USBD_MSG_CONFIGURATION) {
		LOG_INF("  configuration value %d", msg->status);
	}

	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable device support");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable device support");
			}
		}
	}

	if (msg->type == USBD_MSG_DFU_APP_DETACH) {
		switch_to_dfu_mode(ctx);
	}

	if (msg->type == USBD_MSG_DFU_DOWNLOAD_COMPLETED) {
		/*
		 * The verdict a bench run reads off the console: a download
		 * only completes after the manifest phase, so the byte count
		 * is the number of image bytes the host actually pushed into
		 * the "RAM" disk. Zero bytes means nothing was written.
		 */
		if (image0.transferred == 0U) {
			LOG_ERR("FAIL: DFU download completed with 0 bytes");
		} else {
			LOG_INF("PASS: DFU download completed, %u bytes in disk \"%s\"",
				image0.transferred, DISK_NAME);
		}
	}
}

static void switch_to_dfu_mode(struct usbd_context *const ctx)
{
	int err;

	LOG_INF("Detach: switching to DFU mode");

	usbd_disable(ctx);
	usbd_shutdown(ctx);

	err = usbd_add_descriptor(&dfu_usbd, &dfu_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return;
	}

	err = usbd_add_configuration(&dfu_usbd, USBD_SPEED_FS, &dfu_fs_config);
	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration (%d)", err);
		return;
	}

	err = usbd_register_class(&dfu_usbd, "dfu_dfu", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register DFU class (%d)", err);
		return;
	}

	usbd_device_set_code_triple(&dfu_usbd, USBD_SPEED_FS, 0, 0, 0);

	err = usbd_init(&dfu_usbd);
	if (err) {
		LOG_ERR("Failed to initialize DFU context (%d)", err);
		return;
	}

	err = usbd_msg_register_cb(&dfu_usbd, msg_cb);
	if (err) {
		LOG_ERR("Failed to register message callback (%d)", err);
		return;
	}

	err = usbd_enable(&dfu_usbd);
	if (err) {
		LOG_ERR("Failed to enable DFU context (%d)", err);
		return;
	}

	LOG_INF("DFU mode: `dfu-util -a 0 -D <image>` to download, -U to upload");
}

int main(void)
{
	struct usbd_context *sample_usbd;
	int err;

	if (image_query(&image0)) {
		LOG_ERR("Disk \"%s\" is not ready", DISK_NAME);
		return 0;
	}

	LOG_INF("AgRV2K USB composite: MSC + DFU");
	LOG_INF("  MSC  LUN 0 -> disk \"%s\" (%u x %u B)", DISK_NAME,
		image0.sector_count, image0.sector_size);
	LOG_INF("  DFU  image \"ramdisk0\" -> the same disk");

	sample_usbd = sample_usbd_init_device(msg_cb);
	if (sample_usbd == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return 0;
	}

	if (!usbd_can_detect_vbus(sample_usbd)) {
		err = usbd_enable(sample_usbd);
		if (err) {
			LOG_ERR("Failed to enable device support (%d)", err);
			return 0;
		}
	}

	LOG_INF("Runtime mode up: MSC disk + DFU runtime interface");
	LOG_INF("Run `dfu-util --detach` to switch USB0 into DFU mode");

	return 0;
}
