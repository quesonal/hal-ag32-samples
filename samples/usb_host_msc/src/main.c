/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K USB host Mass Storage sample.
 *
 * This Zephyr revision ships no host MSC class (subsys/usb/host/class has
 * UVC only), so the class lives here. It uses public APIs only:
 * uhc_xfer_alloc_with_buf()/uhc_ep_enqueue()/uhc_xfer_return() for the
 * transfers, the class API of subsys/usb/host for the device lifecycle, and
 * the endpoint descriptors the host stack filled in while parsing the
 * configuration.
 *
 * Protocol: Bulk-Only Transport (usbmassbulk_10). Every command is a 31-byte
 * Command Block Wrapper on bulk OUT, an optional data stage, and a 13-byte
 * Command Status Wrapper on bulk IN. Three commands prove the card is present
 * and readable:
 *
 *   INQUIRY          -> vendor / product / revision strings
 *   READ CAPACITY(10)-> last LBA + block length, i.e. the card's size
 *   READ(10) LBA 0   -> the first sector (MBR boot sector, 0x55AA at the end)
 *
 * This is also the first sample that drives *bulk* transfers on this driver;
 * usb_host_enum only ever used control transfers.
 *
 * Unplugging while a transfer is in flight: the driver reports that transfer
 * with -ESHUTDOWN, the completion callback frees it and the state machine
 * stops. removed() and the completion callback run on different host-stack
 * threads (either order is legal), so removed() only marks the device gone and
 * leaves the state the completion path still needs intact.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/usb/usb_ch9.h>

#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

USBH_CONTROLLER_DEFINE(uhs_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));

/* BOT wrapper sizes and signatures (little-endian on the wire). */
#define BOT_CBW_SIZE		31U
#define BOT_CSW_SIZE		13U
#define CBW_SIGNATURE		0x43425355U	/* "USBC" */
#define CSW_SIGNATURE		0x53425355U	/* "USBS" */

/* SCSI opcodes and the data each one returns. */
#define SCSI_INQUIRY		0x12U
#define SCSI_READ_CAPACITY_10	0x25U
#define SCSI_READ_10		0x28U
#define SCSI_WRITE_10		0x2AU

#define INQUIRY_LEN		36U
#define CAPACITY_LEN		8U
#define SECTOR_LEN		512U

/*
 * Performance test shape. One SCSI command moves PERF_BLOCKS_PER_CMD blocks in
 * a single bulk transfer: the driver's qTD "total bytes" field is 15 bits, so
 * one transfer cannot exceed 32767 bytes, and 32 blocks = 16 KiB sits
 * comfortably inside that.
 */
#define PERF_BLOCK_SIZE		512U
#define PERF_BLOCKS_PER_CMD	32U
#define PERF_READ_BURSTS	256U	/* 4 MiB read in total */
#define PERF_WRITE_BURSTS	16U	/* 256 KiB, the same region rewritten */
#define PERF_WRITE_LBA		2048U

/* MSC interface class/subclass/protocol. */
#define MSC_INTERFACE_CLASS	0x08U	/* Mass Storage */
#define MSC_INTERFACE_PROTOCOL	0x50U	/* Bulk-Only Transport */

enum msc_phase {
	MSC_PHASE_CBW,
	MSC_PHASE_DATA,
	MSC_PHASE_CSW,
};

/* The three commands, in order; the CDB is built from the step index. */
static const struct {
	uint8_t opcode;
	uint8_t cdb_len;
	uint16_t data_len;
} msc_step[] = {
	{ SCSI_INQUIRY, 6U, INQUIRY_LEN },
	{ SCSI_READ_CAPACITY_10, 10U, CAPACITY_LEN },
	{ SCSI_READ_10, 10U, SECTOR_LEN },
};

struct msc_ctx {
	const struct device *uhc;
	struct usb_device *udev;
	uint8_t ep_in;
	uint8_t ep_out;
	uint16_t bulk_in_mps;
	uint8_t phase;
	uint8_t step;
	uint32_t tag;
	bool failed;
	/* Set by the removal callback; a transfer that was in flight when the
	 * device went away completes afterwards and stops the state machine. */
	bool stopped;
	/* The command in flight (built by the identification steps or by the
	 * performance phase; the CBW is assembled from these). */
	uint8_t cdb[16];
	uint8_t cdb_len;
	uint16_t data_len;
	bool data_in;
	uint8_t inquiry[INQUIRY_LEN];
	uint8_t capacity[CAPACITY_LEN];
	uint8_t sector[SECTOR_LEN];
};

static struct msc_ctx msc;

/*
 * Read/write performance phase. The write test always saves the region it is
 * about to touch, puts it back afterwards and reads it back once more to prove
 * the restore landed, so the card's contents survive the measurement (the only
 * exposure is a power cut between the write and the restore).
 */
enum perf_mode {
	PERF_OFF = 0,
	PERF_READ,
	PERF_WRITE_SAVE,
	PERF_WRITE,
	PERF_WRITE_VERIFY,
	PERF_RESTORE,
	PERF_RESTORE_VERIFY,
	PERF_DONE,
};

static struct {
	uint8_t mode;
	uint32_t lba;
	uint32_t bursts;
	uint32_t t0;
	size_t bytes;
	uint8_t pattern;
	bool ok;
} perf;

/* The card region the write test rewrites, saved so it can be put back. */
static uint8_t perf_save[PERF_BLOCKS_PER_CMD * PERF_BLOCK_SIZE];

static int msc_transfer_done(struct usb_device *udev, struct uhc_transfer *xfer);

/* ---------- BOT transfers ---------- */

static void msc_fail(const char *what, int err)
{
	LOG_ERR("FAIL: %s (%d)", what, err);
	msc.failed = true;
}

static struct uhc_transfer *msc_alloc(uint8_t ep, uint16_t size)
{
	struct uhc_transfer *xfer;

	xfer = uhc_xfer_alloc_with_buf(msc.uhc, ep, msc.udev, msc_transfer_done,
				       NULL, size);
	if (xfer == NULL) {
		msc_fail("transfer alloc", -ENOMEM);
	}

	return xfer;
}

/* Load msc.cdb with the identification command for the current step. */
static void msc_build_ident_cmd(void)
{
	memset(msc.cdb, 0, sizeof(msc.cdb));
	msc.cdb[0] = msc_step[msc.step].opcode;
	msc.cdb_len = msc_step[msc.step].cdb_len;
	msc.data_len = msc_step[msc.step].data_len;
	msc.data_in = true;

	switch (msc.cdb[0]) {
	case SCSI_INQUIRY:
		msc.cdb[4] = INQUIRY_LEN;	/* allocation length */
		break;
	case SCSI_READ_CAPACITY_10:
		break;				/* LBA 0 is implied */
	case SCSI_READ_10:
		/* LBA 0, one block. */
		msc.cdb[7] = 0U;
		msc.cdb[8] = 1U;
		break;
	default:
		break;
	}
}

static void msc_send_cbw(void)
{
	struct uhc_transfer *xfer = msc_alloc(msc.ep_out, BOT_CBW_SIZE);
	uint8_t *cbw;

	if (xfer == NULL) {
		return;
	}

	cbw = xfer->buf->data;
	sys_put_le32(CBW_SIGNATURE, cbw);
	sys_put_le32(++msc.tag, cbw + 4);
	sys_put_le32(msc.data_len, cbw + 8);
	cbw[12] = msc.data_in ? 0x80U : 0x00U;
	cbw[13] = 0U;				/* LUN 0 */
	cbw[14] = msc.cdb_len;
	memcpy(cbw + 15, msc.cdb, msc.cdb_len);
	net_buf_add(xfer->buf, BOT_CBW_SIZE);

	LOG_DBG("CBW opcode 0x%02x len %u", msc.cdb[0], msc.data_len);

	if (uhc_ep_enqueue(msc.uhc, xfer) != 0) {
		msc_fail("CBW enqueue", -EIO);
	}
}

/* Write a burst-dependent pattern so a read-back cannot be confused with
 * whatever the card had there before. */
static void perf_fill(uint8_t *buf, size_t len, uint32_t lba)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(i + (lba & 0xFFU));
	}
}

static bool perf_check(const uint8_t *buf, size_t len, uint32_t lba)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != (uint8_t)(i + (lba & 0xFFU))) {
			return false;
		}
	}

	return true;
}

static void msc_data_stage(void)
{
	struct uhc_transfer *xfer;

	xfer = msc_alloc(msc.data_in ? msc.ep_in : msc.ep_out, msc.data_len);
	if (xfer == NULL) {
		return;
	}

	if (!msc.data_in) {
		if (perf.mode == PERF_RESTORE) {
			memcpy(xfer->buf->data, perf_save, msc.data_len);
		} else {
			perf_fill(xfer->buf->data, msc.data_len, perf.lba);
		}
		net_buf_add(xfer->buf, msc.data_len);
	}

	if (uhc_ep_enqueue(msc.uhc, xfer) != 0) {
		msc_fail("data enqueue", -EIO);
	}
}

static void msc_recv_csw(void)
{
	struct uhc_transfer *xfer = msc_alloc(msc.ep_in, BOT_CSW_SIZE);

	if (xfer == NULL) {
		return;
	}

	if (uhc_ep_enqueue(msc.uhc, xfer) != 0) {
		msc_fail("CSW enqueue", -EIO);
	}
}

static void msc_store_data(const uint8_t *data, uint16_t len)
{
	if (msc.step >= ARRAY_SIZE(msc_step)) {
		return;
	}

	switch (msc_step[msc.step].opcode) {
	case SCSI_INQUIRY:
		memcpy(msc.inquiry, data, MIN(len, INQUIRY_LEN));
		break;
	case SCSI_READ_CAPACITY_10:
		memcpy(msc.capacity, data, MIN(len, CAPACITY_LEN));
		break;
	case SCSI_READ_10:
		memcpy(msc.sector, data, MIN(len, SECTOR_LEN));
		break;
	default:
		break;
	}
}

static bool msc_check_csw(const uint8_t *csw, uint16_t len)
{
	if (len < BOT_CSW_SIZE) {
		LOG_ERR("short CSW: %u bytes", len);
		return false;
	}
	if (sys_get_le32(csw) != CSW_SIGNATURE) {
		LOG_ERR("bad CSW signature 0x%08x", sys_get_le32(csw));
		return false;
	}
	if (sys_get_le32(csw + 8) != 0U) {
		LOG_ERR("CSW residue %u", sys_get_le32(csw + 8));
	}
	if (csw[12] != 0U) {
		LOG_ERR("CSW status 0x%02x", csw[12]);
		return false;
	}

	return true;
}

/* ---------- command sequence ---------- */

static void msc_report(void)
{
	uint32_t last_lba = sys_get_be32(msc.capacity);
	uint32_t block_len = sys_get_be32(msc.capacity + 4);
	uint64_t bytes = ((uint64_t)last_lba + 1U) * (uint64_t)block_len;

	LOG_INF("  vendor      %.8s", (char *)msc.inquiry + 8);
	LOG_INF("  product     %.16s", (char *)msc.inquiry + 16);
	LOG_INF("  revision    %.4s", (char *)msc.inquiry + 32);
	LOG_INF("  block size  %u bytes, last LBA %u", block_len, last_lba);
	LOG_INF("  capacity    %llu bytes (%llu MiB)",
		(unsigned long long)bytes,
		(unsigned long long)(bytes / (1024U * 1024U)));
	LOG_HEXDUMP_INF(msc.sector, 16U, "  sector 0");

	/* MBR partition table: four 16-byte entries at offset 446. */
	for (uint8_t i = 0U; i < 4U; i++) {
		const uint8_t *p = msc.sector + 446U + ((size_t)i * 16U);
		uint32_t start = sys_get_le32(p + 8);
		uint32_t count = sys_get_le32(p + 12);

		if (p[4] == 0U || count == 0U) {
			continue;
		}

		LOG_INF("  partition %u: type 0x%02x, LBA %u + %u blocks (%u MiB)",
			i, p[4], start, count,
			(unsigned int)(((uint64_t)count * PERF_BLOCK_SIZE) / (1024U * 1024U)));
	}

	if (msc.sector[510] != 0x55U || msc.sector[511] != 0xAAU) {
		LOG_ERR("FAIL: sector 0 has no 0x55AA signature");
		return;
	}

	LOG_INF("PASS: read TF card through the USB card reader");
}

static void msc_issue(void)
{
	msc.phase = MSC_PHASE_CBW;
	msc_send_cbw();
}

/* ---------- read/write performance ---------- */

static void msc_perf_issue(void)
{
	memset(msc.cdb, 0, sizeof(msc.cdb));
	msc.cdb_len = 10U;
	msc.data_len = PERF_BLOCKS_PER_CMD * PERF_BLOCK_SIZE;
	msc.data_in = (perf.mode != PERF_WRITE) && (perf.mode != PERF_RESTORE);
	msc.cdb[0] = msc.data_in ? SCSI_READ_10 : SCSI_WRITE_10;
	sys_put_be32(perf.lba, msc.cdb + 2);
	sys_put_be16(PERF_BLOCKS_PER_CMD, msc.cdb + 7);
	msc_issue();
}

static uint32_t msc_perf_kib_per_s(void)
{
	uint32_t ms = (uint32_t)(k_uptime_get() - perf.t0);

	if (ms == 0U) {
		return 0U;
	}

	return (uint32_t)(((uint64_t)perf.bytes * 1000U) / 1024U / ms);
}

static void msc_perf_begin(void)
{
	uint32_t block_len = sys_get_be32(msc.capacity + 4);

	if (block_len != PERF_BLOCK_SIZE) {
		LOG_INF("performance test skipped (block size %u)", block_len);
		return;
	}

	LOG_INF("--- performance: %u blocks (%u B) per SCSI command ---",
		PERF_BLOCKS_PER_CMD, PERF_BLOCKS_PER_CMD * PERF_BLOCK_SIZE);

	perf.mode = PERF_READ;
	perf.lba = 0U;
	perf.bursts = PERF_READ_BURSTS;
	perf.bytes = 0U;
	perf.t0 = k_uptime_get();
	msc_perf_issue();
}

static void msc_perf_continue(void)
{
	int ms;

	switch (perf.mode) {
	case PERF_READ:
		perf.bytes += msc.data_len;
		perf.lba += PERF_BLOCKS_PER_CMD;
		if (--perf.bursts != 0U) {
			msc_perf_issue();
			break;
		}

		ms = (int)(k_uptime_get() - perf.t0);
		LOG_INF("read  : %u KiB in %d ms -> %u KiB/s",
			(unsigned int)(perf.bytes / 1024U), ms, msc_perf_kib_per_s());

		/*
		 * Write test: save the region first, rewrite it, read it back
		 * and compare, then put the original content back. Nothing on
		 * the card is left changed.
		 */
		perf.mode = PERF_WRITE_SAVE;
		perf.lba = PERF_WRITE_LBA;
		LOG_INF("write : saving LBA %u..%u before rewriting them",
			PERF_WRITE_LBA, PERF_WRITE_LBA + PERF_BLOCKS_PER_CMD - 1U);
		msc_perf_issue();
		break;

	case PERF_WRITE_SAVE:
		perf.mode = PERF_WRITE;
		perf.bursts = PERF_WRITE_BURSTS;
		perf.bytes = 0U;
		perf.t0 = k_uptime_get();
		msc_perf_issue();
		break;

	case PERF_WRITE:
		perf.bytes += msc.data_len;
		if (--perf.bursts != 0U) {
			msc_perf_issue();
			break;
		}

		ms = (int)(k_uptime_get() - perf.t0);
		LOG_INF("write : %u KiB in %d ms -> %u KiB/s",
			(unsigned int)(perf.bytes / 1024U), ms, msc_perf_kib_per_s());
		perf.ok = true;
		perf.mode = PERF_WRITE_VERIFY;
		msc_perf_issue();
		break;

	case PERF_WRITE_VERIFY:
		if (!perf.ok) {
			msc_fail("write verify", -EIO);
			break;
		}
		LOG_INF("verify: read-back matches the pattern that was written");
		perf.mode = PERF_RESTORE;
		msc_perf_issue();
		break;

	case PERF_RESTORE:
		LOG_INF("restore: original LBA %u..%u written back",
			PERF_WRITE_LBA, PERF_WRITE_LBA + PERF_BLOCKS_PER_CMD - 1U);
		perf.ok = false;
		perf.mode = PERF_RESTORE_VERIFY;
		msc_perf_issue();
		break;

	case PERF_RESTORE_VERIFY:
		if (!perf.ok) {
			msc_fail("restore verify", -EIO);
			break;
		}
		LOG_INF("verify: LBA %u..%u read back as the original content",
			PERF_WRITE_LBA, PERF_WRITE_LBA + PERF_BLOCKS_PER_CMD - 1U);
		perf.mode = PERF_DONE;
		LOG_INF("PASS: read/write performance measured, card content unchanged");
		break;

	default:
		break;
	}
}

static void msc_advance(void)
{
	if (msc.failed) {
		return;
	}

	if (msc.step < ARRAY_SIZE(msc_step)) {
		msc.step++;
		if (msc.step >= ARRAY_SIZE(msc_step)) {
			msc_report();
			msc_perf_begin();
			return;
		}

		msc_build_ident_cmd();
		msc_issue();
		return;
	}

	msc_perf_continue();
}

/* Route the payload of a completed data stage: identification steps keep it,
 * the performance phase either saves it, checks it, or ignores it. */
static void msc_data_received(const uint8_t *data, uint16_t len)
{
	switch (perf.mode) {
	case PERF_WRITE_SAVE:
		memcpy(perf_save, data, MIN((size_t)len, sizeof(perf_save)));
		break;
	case PERF_WRITE_VERIFY:
		perf.ok = (len == msc.data_len) && perf_check(data, len, perf.lba);
		if (!perf.ok) {
			LOG_ERR("write verify mismatch at LBA %u (%u bytes)",
				perf.lba, len);
		}
		break;
	case PERF_RESTORE_VERIFY:
		/* The region was put back from the snapshot; read it once more so
		 * "card content unchanged" is an assertion and not just the
		 * assumption that the restore write landed. */
		perf.ok = (len == msc.data_len) &&
			  (memcmp(data, perf_save, len) == 0);
		if (!perf.ok) {
			LOG_ERR("FAIL: restore verify mismatch at LBA %u (%u bytes) "
				"-- the card still holds the test pattern",
				perf.lba, len);
		}
		break;
	case PERF_OFF:
		msc_store_data(data, len);
		break;
	default:
		break;		/* read bursts: the bytes are not kept */
	}
}

static int msc_transfer_done(struct usb_device *udev, struct uhc_transfer *xfer)
{
	const uint8_t phase = msc.phase;
	struct net_buf *buf = xfer->buf;
	const uint16_t len = buf != NULL ? buf->len : 0U;
	const uint8_t *data = buf != NULL ? buf->data : NULL;
	const int err = xfer->err;
	bool csw_ok = true;

	ARG_UNUSED(udev);

	/*
	 * The device went away while this transfer was in flight: the driver
	 * reports it with -ESHUTDOWN, and that report may arrive before or
	 * after the removal callback. Free the transfer and stop here -- the
	 * state machine must not issue the next command against a device that
	 * is gone. `msc` is deliberately left intact by msc_removed() for
	 * exactly this path.
	 */
	if (msc.stopped) {
		if (buf != NULL) {
			uhc_xfer_buf_free(msc.uhc, buf);
		}
		(void)uhc_xfer_free(msc.uhc, xfer);

		return 0;
	}

	if (err == 0 && data != NULL) {
		if (phase == MSC_PHASE_DATA) {
			msc_data_received(data, len);
		} else if (phase == MSC_PHASE_CSW) {
			csw_ok = msc_check_csw(data, len);
		}
	}

	if (buf != NULL) {
		uhc_xfer_buf_free(msc.uhc, buf);
	}
	(void)uhc_xfer_free(msc.uhc, xfer);

	if (err != 0) {
		msc_fail("transfer", err);
		return 0;
	}
	if (!csw_ok) {
		msc_fail("command", -EIO);
		return 0;
	}

	switch (phase) {
	case MSC_PHASE_CBW:
		msc.phase = MSC_PHASE_DATA;
		msc_data_stage();
		break;
	case MSC_PHASE_DATA:
		msc.phase = MSC_PHASE_CSW;
		msc_recv_csw();
		break;
	case MSC_PHASE_CSW:
		msc_advance();
		break;
	default:
		break;
	}

	return 0;
}

/* ---------- host class ---------- */

/*
 * Required: usbh_class_init() reports -ENOTSUP when a class has no init
 * callback, and usbh_class_init_all() treats any non-zero return as "this
 * class failed to initialise" and never offers it a device.
 */
static int msc_init(struct usbh_class_data *const c_data)
{
	LOG_INF("host class '%s' ready", c_data->name);

	return 0;
}

static bool msc_find_endpoints(struct usb_device *udev)
{
	for (uint8_t i = 1U; i < 16U; i++) {
		const struct usb_ep_descriptor *ep;

		ep = udev->ep_in[i].desc;
		if (ep != NULL &&
		    (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) == USB_EP_TYPE_BULK) {
			msc.ep_in = ep->bEndpointAddress;
			msc.bulk_in_mps = ep->wMaxPacketSize;
		}

		ep = udev->ep_out[i].desc;
		if (ep != NULL &&
		    (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) == USB_EP_TYPE_BULK) {
			msc.ep_out = ep->bEndpointAddress;
		}
	}

	return (msc.ep_in != 0U) && (msc.ep_out != 0U);
}

static int msc_probe(struct usbh_class_data *const c_data,
		     struct usb_device *const udev, const uint8_t iface)
{
	const struct usb_desc_header *dhp;
	const struct usb_if_descriptor *if_desc;

	ARG_UNUSED(c_data);

	/*
	 * The class registers an empty filter table, so the stack offers it
	 * every function -- and once the whole device, with iface set to the
	 * device pseudo-interface. Screen for Mass Storage / Bulk-Only here and
	 * hand non-matching functions back to the stack.
	 */
	if (iface > UHC_INTERFACES_MAX) {
		return -ENOTSUP;
	}

	dhp = udev->ifaces[iface].dhp;
	if (dhp == NULL || dhp->bDescriptorType != USB_DESC_INTERFACE) {
		return -ENOTSUP;
	}

	if_desc = (const struct usb_if_descriptor *)dhp;
	if (if_desc->bInterfaceClass != MSC_INTERFACE_CLASS ||
	    if_desc->bInterfaceProtocol != MSC_INTERFACE_PROTOCOL) {
		return -ENOTSUP;
	}

	memset(&msc, 0, sizeof(msc));
	msc.udev = udev;
	msc.uhc = ((const struct usbh_context *)udev->ctx)->dev;

	if (!msc_find_endpoints(udev)) {
		LOG_ERR("FAIL: mass storage interface without bulk IN+OUT");
		return -ENOTSUP;
	}

	LOG_INF("mass storage device: VID:PID %04x:%04x, bulk IN 0x%02x (mps %u) OUT 0x%02x",
		udev->dev_desc.idVendor, udev->dev_desc.idProduct,
		msc.ep_in, msc.bulk_in_mps, msc.ep_out);

	msc.phase = MSC_PHASE_CBW;
	msc.step = 0U;
	msc_build_ident_cmd();
	msc_issue();

	return 0;
}

static int msc_removed(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	LOG_INF("mass storage device removed");

	/*
	 * Only mark the device gone. The driver hands back every in-flight
	 * transfer with -ESHUTDOWN, and that completion runs on a different
	 * host-stack thread than this callback -- clearing `uhc` here would
	 * make it fault instead of freeing its transfer. A re-plug re-probes,
	 * and msc_probe() resets the state.
	 */
	msc.stopped = true;
	msc.udev = NULL;

	return 0;
}

static struct usbh_class_api msc_api = {
	.init = msc_init,
	.probe = msc_probe,
	.removed = msc_removed,
};

/* Empty filter table: the probe above does the match itself. */
USBH_DEFINE_CLASS(usb_host_msc, &msc_api, &msc, NULL);

int main(void)
{
	int ret;

	LOG_INF("AgRV2K USB host Mass Storage sample");

	ret = usbh_init(&uhs_ctx);
	if (ret != 0) {
		LOG_ERR("usbh_init failed: %d", ret);
		return 0;
	}

	ret = usbh_enable(&uhs_ctx);
	if (ret != 0) {
		LOG_ERR("usbh_enable failed: %d", ret);
		return 0;
	}

	LOG_INF("USB0 host is up -- plug a card reader or flash drive into USB0");

	while (true) {
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
