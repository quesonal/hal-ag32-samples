/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K USB host enumeration sample.
 *
 * The Zephyr host stack (subsys/usb/host) does the work: usbh_init() +
 * usbh_enable() bring up the controller and leave it watching the root port,
 * and every device the stack enumerates is offered to each registered class
 * through its filter table. This sample registers one class with an empty
 * filter table (an empty table matches everything), so it is handed every
 * device that finishes enumeration -- whatever it is, with no class driver
 * needed -- and prints what the stack learned about it.
 *
 * Output shape (one PASS/FAIL verdict per event, per the repo's sample
 * convention):
 *   Device connected, full speed          <- uhc_agm
 *   PASS: enumerated a USB device         <- this sample
 *     address / speed / VID:PID / class   <- from the device descriptor
 *   device removed
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* The controller node the board overlay enabled and labelled. */
USBH_CONTROLLER_DEFINE(uhs_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));

struct host_enum_ctx {
	atomic_t enumerated;
	atomic_t removed;
};

static struct host_enum_ctx enum_ctx;

static int enum_init(struct usbh_class_data *const c_data)
{
	LOG_INF("host class '%s' ready", c_data->name);

	return 0;
}

static int enum_completion(struct usbh_class_data *const c_data,
			   struct uhc_transfer *const xfer)
{
	ARG_UNUSED(c_data);

	LOG_DBG("transfer %p finished, err %d", (void *)xfer, xfer->err);

	return 0;
}

static int enum_probe(struct usbh_class_data *const c_data,
		      struct usb_device *const udev, const uint8_t iface)
{
	const struct usb_device_descriptor *desc = &udev->dev_desc;

	ARG_UNUSED(c_data);
	ARG_UNUSED(iface);

	LOG_INF("PASS: enumerated a USB device");
	LOG_INF("  address   %u", udev->addr);
	LOG_INF("  speed     %u", (unsigned int)udev->speed);
	LOG_INF("  VID:PID   %04x:%04x", (unsigned int)desc->idVendor,
		(unsigned int)desc->idProduct);
	LOG_INF("  bcdUSB    %04x", (unsigned int)desc->bcdUSB);
	LOG_INF("  bcdDevice %04x", (unsigned int)desc->bcdDevice);
	LOG_INF("  class     %02x/%02x/%02x", desc->bDeviceClass,
		desc->bDeviceSubClass, desc->bDeviceProtocol);
	LOG_INF("  maxpacket %u, configurations %u", desc->bMaxPacketSize0,
		desc->bNumConfigurations);

	atomic_inc((atomic_t *)&enum_ctx.enumerated);

	return 0;
}

static int enum_removed(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	LOG_INF("device removed");
	atomic_inc((atomic_t *)&enum_ctx.removed);

	return 0;
}

static int enum_suspended(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	LOG_INF("bus suspended");

	return 0;
}

static int enum_resumed(struct usbh_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	LOG_INF("bus resumed");

	return 0;
}

static struct usbh_class_api enum_api = {
	.init = enum_init,
	.completion_cb = enum_completion,
	.probe = enum_probe,
	.removed = enum_removed,
	.suspended = enum_suspended,
	.resumed = enum_resumed,
};

/* Empty filter table -> the class is offered every enumerated device. */
USBH_DEFINE_CLASS(usb_host_enum, &enum_api, &enum_ctx, NULL);

int main(void)
{
	int ret;

	LOG_INF("AgRV2K USB host enumeration sample");

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

	LOG_INF("USB0 host is up -- plug a device into the USB0 connector");

	while (true) {
		k_sleep(K_SECONDS(5));

		if (atomic_get(&enum_ctx.enumerated) == 0) {
			LOG_INF("no device yet (bitstream in USB0 host mode?)");
		}
	}

	return 0;
}
