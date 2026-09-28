/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sample_usbd.h>

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* 4-byte mouse report: buttons, X (rel), Y (rel), wheel (rel). */
#define MOUSE_REPORT_SIZE	4
#define MOUSE_BTN_LEFT		BIT(0)
#define MOUSE_BTN_RIGHT		BIT(1)

/* Full-speed HID interrupt IN endpoint bInterval comes from
 * in-polling-period-us (10 ms); report cadence matches it. */
#define REPORT_INTERVAL_MS	10

#define CIRCLE_STEPS		64
#define CIRCLE_RADIUS		10

/* Relative X/Y deltas precomputed for one circle (radius 10 px,
 * 64 steps). Host cursor traces the shape on its own — no GPIO
 * keys / input subsystem required on this board. */
static const int8_t circle_dx[CIRCLE_STEPS] = {
	10, 10, 10, 10, 9, 9, 8, 8,
	7, 6, 6, 5, 4, 3, 2, 1,
	0, -1, -2, -3, -4, -5, -6, -6,
	-7, -8, -8, -9, -9, -10, -10, -10,
	-10, -10, -10, -10, -9, -9, -8, -8,
	-7, -6, -6, -5, -4, -3, -2, -1,
	0, 1, 2, 3, 4, 5, 6, 6,
	7, 8, 8, 9, 9, 10, 10, 10,
};

static const int8_t circle_dy[CIRCLE_STEPS] = {
	0, 1, 2, 3, 4, 5, 6, 6,
	7, 8, 8, 9, 9, 10, 10, 10,
	10, 10, 10, 10, 9, 9, 8, 8,
	7, 6, 6, 5, 4, 3, 2, 1,
	0, -1, -2, -3, -4, -5, -6, -6,
	-7, -8, -8, -9, -9, -10, -10, -10,
	-10, -10, -10, -10, -9, -9, -8, -8,
	-7, -6, -6, -5, -4, -3, -2, -1,
};

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const uint8_t mouse_rdesc[] = HID_MOUSE_REPORT_DESC(2);

/* Report buffer: must stay USB-buffer aligned (hid_device_submit_report()
 * asserts on it) and valid until the report has been sent. */
UDC_STATIC_BUF_DEFINE(mouse_report, MOUSE_REPORT_SIZE);

static atomic_t iface_ready;
K_SEM_DEFINE(iface_sem, 0, 1);

static void mouse_iface_ready(const struct device *dev, const bool ready)
{
	LOG_INF("HID interface %s", ready ? "ready" : "not ready");

	if (ready) {
		atomic_set(&iface_ready, 1);
		k_sem_give(&iface_sem);
	} else {
		atomic_set(&iface_ready, 0);
	}
}

static int mouse_get_report(const struct device *dev, const uint8_t type,
			    const uint8_t id, const uint16_t len,
			    uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(type);
	ARG_UNUSED(id);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);

	/* Mouse has no Get Report traffic in practice; reply empty. */
	LOG_WRN("Get Report 0x%02x id %u not supported", type, id);
	return 0;
}

static const struct hid_device_ops mouse_ops = {
	.iface_ready = mouse_iface_ready,
	.get_report = mouse_get_report,
};

int main(void)
{
	const struct device *hid_dev;
	struct usbd_context *usbd;
	bool led_ok;
	uint16_t step;
	uint16_t count;
	int ret;

	led_ok = gpio_is_ready_dt(&led0);
	if (led_ok) {
		ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
		if (ret != 0) {
			LOG_WRN("Failed to configure led0 (%d)", ret);
			led_ok = false;
		}
	} else {
		LOG_WRN("led0 not ready, heartbeat disabled");
	}

	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev)) {
		LOG_ERR("HID device not ready");
		return -EIO;
	}

	ret = hid_device_register(hid_dev, mouse_rdesc, sizeof(mouse_rdesc),
				  &mouse_ops);
	if (ret != 0) {
		LOG_ERR("Failed to register HID device (%d)", ret);
		return ret;
	}

	usbd = sample_usbd_init_device(NULL);
	if (usbd == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return -ENODEV;
	}

	ret = usbd_enable(usbd);
	if (ret != 0) {
		LOG_ERR("Failed to enable device support (%d)", ret);
		return ret;
	}

	LOG_INF("USB HID mouse demo enabled: plug into a host");

	step = 0;
	count = 0;
	while (true) {
		/* Do not submit until the host has set the configuration
		 * and the HID interface is active. */
		if (!atomic_get(&iface_ready)) {
			k_sem_take(&iface_sem, K_FOREVER);
			continue;
		}

		mouse_report[0] = 0U;			/* buttons: none */
		mouse_report[1] = (uint8_t)circle_dx[step];
		mouse_report[2] = (uint8_t)circle_dy[step];
		mouse_report[3] = 0U;			/* wheel: none */

		ret = hid_device_submit_report(hid_dev, MOUSE_REPORT_SIZE,
					       mouse_report);
		if (ret != 0) {
			LOG_WRN("HID submit report failed (%d)", ret);
			atomic_set(&iface_ready, 0);
			k_sem_reset(&iface_sem);
			continue;
		}

		/* LED heartbeat (~160 ms), only when reports actually flow. */
		if (led_ok && ((count & 0x0FU) == 0U)) {
			(void)gpio_pin_toggle(led0.port, led0.pin);
		}

		step = (step + 1U) % CIRCLE_STEPS;
		count++;
		k_sleep(K_MSEC(REPORT_INTERVAL_MS));
	}

	return 0;
}
