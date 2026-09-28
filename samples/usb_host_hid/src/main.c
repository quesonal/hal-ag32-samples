/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K USB host HID sample.
 *
 * The Zephyr host stack's class API hands this sample every enumerated
 * function; it screens for a HID interface (bInterfaceClass 0x03), takes the
 * interrupt IN endpoint out of the descriptors the stack parsed, and then
 * keeps a transfer armed on it forever, printing each report it receives.
 *
 * Why it exists: usb_host_enum only uses control transfers and usb_host_msc
 * only bulk ones. Nothing exercised the driver's *periodic* schedule (the
 * frame list, the period heads parsed out of the endpoint's bInterval, and
 * the per-endpoint queue head living in that list) -- that is what this
 * sample covers.
 *
 * An interrupt IN endpoint NAKs while its device has nothing to report, which
 * is the normal idle state: the transfer simply stays on the wire until the
 * mouse moves (or a key is pressed) and the device answers.
 *
 * Unplugging while that transfer is armed: the driver reports it with
 * -ESHUTDOWN, the completion callback frees it and stops re-arming. removed()
 * and the completion callback run on different host-stack threads (either
 * order is legal), so removed() only marks the device gone and leaves the
 * state the completion path still needs intact.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/usb/usb_ch9.h>

#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

USBH_CONTROLLER_DEFINE(uhs_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));

#define HID_INTERFACE_CLASS	0x03U
#define REPORTS_FOR_PASS	4U
#define REPORT_DUMP_MAX		16U

struct hid_ctx {
	const struct device *uhc;
	struct usb_device *udev;
	uint8_t ep_in;
	uint16_t mps;
	uint8_t interval;
	uint32_t reports;
	/* Set by the removal callback; the completion path reads it and stops
	 * instead of arming another transfer on a device that is gone. */
	bool stopped;
};

static struct hid_ctx hid;

static void hid_poll(void);
static int hid_poll_done(struct usb_device *udev, struct uhc_transfer *xfer);

static void hid_poll(void)
{
	struct uhc_transfer *xfer;

	if (hid.stopped || hid.udev == NULL) {
		return;
	}

	xfer = uhc_xfer_alloc_with_buf(hid.uhc, hid.ep_in, hid.udev, hid_poll_done,
				       NULL, hid.mps);
	if (xfer == NULL) {
		LOG_ERR("FAIL: interrupt transfer alloc");
		return;
	}

	if (uhc_ep_enqueue(hid.uhc, xfer) != 0) {
		LOG_ERR("FAIL: interrupt enqueue");
		uhc_xfer_buf_free(hid.uhc, xfer->buf);
		(void)uhc_xfer_free(hid.uhc, xfer);
	}
}

static int hid_poll_done(struct usb_device *udev, struct uhc_transfer *xfer)
{
	struct net_buf *buf = xfer->buf;
	const int err = xfer->err;

	ARG_UNUSED(udev);

	if (err == 0 && buf != NULL && buf->len > 0U) {
		hid.reports++;
		LOG_INF("report %u (%u bytes):", hid.reports, buf->len);
		LOG_HEXDUMP_INF(buf->data, MIN(buf->len, REPORT_DUMP_MAX), "  ");

		if (hid.reports == REPORTS_FOR_PASS) {
			LOG_INF("PASS: %u interrupt IN reports received (ep 0x%02x, mps %u, bInterval %u)",
				hid.reports, hid.ep_in, hid.mps, hid.interval);
		}
	} else if (err == -ESHUTDOWN) {
		/* The device went away mid-poll. The driver reports every
		 * transfer it still held, and that report may arrive before or
		 * after the removal callback. */
		LOG_INF("interrupt IN stopped: device removed");
	} else if (err != 0) {
		LOG_ERR("interrupt transfer failed: %d", err);
	}

	if (buf != NULL) {
		uhc_xfer_buf_free(hid.uhc, buf);
	}
	(void)uhc_xfer_free(hid.uhc, xfer);

	/* Keep exactly one transfer armed on the endpoint -- unless the device
	 * is gone, in which case a re-plug re-probes and starts a fresh poll. */
	if (err == -ESHUTDOWN || hid.stopped) {
		return 0;
	}

	hid_poll();

	return 0;
}

/* ---------- host class ---------- */

/*
 * Required: usbh_class_init() reports -ENOTSUP when a class has no init
 * callback, and usbh_class_init_all() then treats the class as failed and
 * never offers it a device.
 */
static int hid_init(struct usbh_class_data *const c_data)
{
	LOG_INF("host class '%s' ready", c_data->name);

	return 0;
}

static bool hid_find_interrupt_in(struct usb_device *udev)
{
	for (uint8_t i = 1U; i < 16U; i++) {
		const struct usb_ep_descriptor *ep = udev->ep_in[i].desc;

		if (ep == NULL) {
			continue;
		}
		if ((ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) !=
		    USB_EP_TYPE_INTERRUPT) {
			continue;
		}

		hid.ep_in = ep->bEndpointAddress;
		hid.mps = ep->wMaxPacketSize;
		hid.interval = ep->bInterval;

		return true;
	}

	return false;
}

static int hid_probe(struct usbh_class_data *const c_data,
		     struct usb_device *const udev, const uint8_t iface)
{
	const struct usb_desc_header *dhp;
	const struct usb_if_descriptor *if_desc;

	ARG_UNUSED(c_data);

	/* Empty filter table, so screen for HID here (and hand non-HID
	 * functions, including the device pseudo-interface, back). */
	if (iface > UHC_INTERFACES_MAX) {
		return -ENOTSUP;
	}

	dhp = udev->ifaces[iface].dhp;
	if (dhp == NULL || dhp->bDescriptorType != USB_DESC_INTERFACE) {
		return -ENOTSUP;
	}

	if_desc = (const struct usb_if_descriptor *)dhp;
	if (if_desc->bInterfaceClass != HID_INTERFACE_CLASS) {
		return -ENOTSUP;
	}

	memset(&hid, 0, sizeof(hid));
	hid.udev = udev;
	hid.uhc = ((const struct usbh_context *)udev->ctx)->dev;

	if (!hid_find_interrupt_in(udev)) {
		LOG_ERR("FAIL: HID interface without an interrupt IN endpoint");
		return -ENOTSUP;
	}

	LOG_INF("HID device: VID:PID %04x:%04x, interrupt IN 0x%02x (mps %u, bInterval %u)",
		udev->dev_desc.idVendor, udev->dev_desc.idProduct,
		hid.ep_in, hid.mps, hid.interval);
	LOG_INF("polling -- move the mouse or press a key to generate reports");

	hid_poll();

	return 0;
}

static int hid_removed(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	LOG_INF("HID device removed (%u reports seen)", hid.reports);

	/*
	 * Only mark the device gone: the driver reports every in-flight
	 * transfer with -ESHUTDOWN, and that completion may run before or
	 * after this callback (they are on different host-stack threads), so
	 * freeing or zeroing the state here would race with it. `uhc` and the
	 * transfer are still needed by the completion path.
	 */
	hid.stopped = true;
	hid.udev = NULL;

	return 0;
}

static struct usbh_class_api hid_api = {
	.init = hid_init,
	.probe = hid_probe,
	.removed = hid_removed,
};

/* Empty filter table: the probe above does the matching. */
USBH_DEFINE_CLASS(usb_host_hid, &hid_api, &hid, NULL);

int main(void)
{
	int ret;

	LOG_INF("AgRV2K USB host HID sample");

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

	LOG_INF("USB0 host is up -- plug a mouse/keyboard (or its receiver) into USB0");

	while (true) {
		k_sleep(K_SECONDS(5));

		if (!hid.stopped && hid.ep_in != 0U && hid.reports == 0U) {
			LOG_INF("armed on 0x%02x, no reports yet -- move the mouse",
				hid.ep_in);
		}
	}

	return 0;
}
