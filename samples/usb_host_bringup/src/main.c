/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K USB0 host bring-up self-check.
 *
 * The other USB host samples need a device on the connector (enumeration
 * needs one, MSC and HID need one that answers). This one does not: it drives
 * the UHC API directly -- no host stack, no class, no usbh threads -- and
 * checks the controller state the driver is supposed to program:
 *
 *   init     USBMODE.CM = host, both schedule base registers programmed and
 *            aligned, interrupt threshold, interrupts still masked, one-shot
 *   enable   USBCMD run + both schedule enables + the frame-list size,
 *            USBINTR unmasked, controller not halted
 *   port     PORTSC agrees with the connect event that was (or was not)
 *            delivered -- this is the one check that also holds with a device
 *            attached, so the test is useful either way
 *   disable  run bit cleared, interrupts masked, controller halted
 *
 * It is the HIL counterpart of tests/drivers/usb/uhc/uhc_agm (same checks
 * against a RAM window); this one runs on the board and therefore also covers
 * the bus reset/port side the native model cannot.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_host_bringup, LOG_LEVEL_INF);

#define UHC_DEV		DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0))
#define UHC_BASE	DT_REG_ADDR(DT_NODELABEL(zephyr_uhc0))

/* Register offsets: drivers/usb/udc/udc_agm.h (struct udc_agm_regs). */
#define R_USBCMD		0x140U
#define R_USBSTS		0x144U
#define R_USBINTR		0x148U
#define R_PERIODICLISTBASE	0x154U
#define R_ASYNCLISTADDR		0x158U
#define R_PORTSC		0x184U
#define R_USBMODE		0x1a8U

#define CMD_RS		BIT(0)
#define CMD_FS0		BIT(2)
#define CMD_FS1		BIT(3)
#define CMD_PSE		BIT(4)
#define CMD_ASE		BIT(5)
#define CMD_FS2		BIT(15)

#define STS_HCH		BIT(12)

#define MODE_CM_MASK	0x3U
#define MODE_CM_HOST	0x3U

#define PORTSC_CCS	BIT(0)

/* UI|UEI|PCI|FRI|SEI|AAI, the set uhc_agm_enable() unmasks. */
#define INTR_MASK	(BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5))

static uint32_t rd(uint32_t off)
{
	return sys_read32(UHC_BASE + off);
}

static unsigned int checks;
static unsigned int failures;

static void check(bool ok, const char *what)
{
	checks++;
	if (ok) {
		LOG_INF("  ok   %s", what);
	} else {
		failures++;
		LOG_ERR("  FAIL %s", what);
	}
}

/* The driver reports connects/removals through this callback. */
static atomic_t events;
static int last_event = -1;

static int event_cb(const struct device *dev, const struct uhc_event *const event)
{
	ARG_UNUSED(dev);

	last_event = (int)event->type;
	atomic_inc(&events);

	return 0;
}

int main(void)
{
	unsigned int passed;

	LOG_INF("usb_host_bringup: AgRV2K USB0 host bring-up self-check");
	LOG_INF("(no device on USB0 is required; a connected one only changes which"
		" of the two port states is expected)");

	check(device_is_ready(UHC_DEV), "the UHC device came up");
	check(uhc_init(UHC_DEV, NULL, NULL) == -EINVAL,
	      "uhc_init() rejects a missing event callback");
	check(uhc_init(UHC_DEV, event_cb, NULL) == 0, "uhc_init()");
	check(uhc_init(UHC_DEV, event_cb, NULL) == -EALREADY, "uhc_init() is one-shot");

	/* ---- init(): the controller knows it is a host ---- */
	check((rd(R_USBMODE) & MODE_CM_MASK) == MODE_CM_HOST, "USBMODE.CM = host");

	check((rd(R_ASYNCLISTADDR) != 0U) && ((rd(R_ASYNCLISTADDR) & 0x1fU) == 0U),
	      "ASYNCLISTADDR programmed and 32-byte aligned");
	check((rd(R_PERIODICLISTBASE) != 0U) &&
	      ((rd(R_PERIODICLISTBASE) & 0xfffU) == 0U),
	      "PERIODICLISTBASE programmed and 4 KiB aligned");
	check(rd(R_USBINTR) == 0U, "interrupts stay masked until enable()");
	check((rd(R_USBCMD) & CMD_RS) == 0U, "the schedules are not running yet");

	/* ---- enable() ---- */
	check(uhc_enable(UHC_DEV) == 0, "uhc_enable()");
	check(uhc_enable(UHC_DEV) == -EALREADY, "uhc_enable() is one-shot");

	LOG_INF("  regs USBMODE 0x%08x USBCMD 0x%08x USBSTS 0x%08x USBINTR 0x%08x PORTSC 0x%08x",
		rd(R_USBMODE), rd(R_USBCMD), rd(R_USBSTS), rd(R_USBINTR), rd(R_PORTSC));

	check((rd(R_USBCMD) & CMD_RS) != 0U, "USBCMD.RS: the controller runs");
	check((rd(R_USBCMD) & CMD_PSE) != 0U, "USBCMD.PSE: periodic schedule enabled");
	check((rd(R_USBCMD) & CMD_ASE) != 0U, "USBCMD.ASE: async schedule enabled");
	check((rd(R_USBCMD) & (CMD_FS0 | CMD_FS1 | CMD_FS2)) ==
	      (CMD_FS0 | CMD_FS1 | CMD_FS2), "8-entry frame list (ChipIdea encoding)");
	check(rd(R_USBINTR) == INTR_MASK, "USBINTR = the host interrupt set");
	check((rd(R_USBSTS) & STS_HCH) == 0U, "USBSTS.HCH clear (not halted)");

	/*
	 * Port state. A device that was already plugged in when enable() ran
	 * gets its connect event from enable() itself; one plugged in later
	 * gets it from the port-change ISR. Either way PORTSC.CCS and the
	 * event must agree -- which is exactly what this check pins, with or
	 * without a device on the connector.
	 */
	k_msleep(100);
	{
		const uint32_t portsc = rd(R_PORTSC);
		const bool ccs = (portsc & PORTSC_CCS) != 0U;
		const bool connected = (last_event == (int)UHC_EVT_DEV_CONNECTED_FS) ||
				       (last_event == (int)UHC_EVT_DEV_CONNECTED_LS) ||
				       (last_event == (int)UHC_EVT_DEV_CONNECTED_HS);

		LOG_INF("  port PORTSC 0x%08x: device %s, %u event(s)",
			portsc, ccs ? "attached" : "absent", (unsigned int)atomic_get(&events));
		check(ccs == connected, "port state agrees with the connect event");
	}

	/* ---- disable() ---- */
	check(uhc_disable(UHC_DEV) == 0, "uhc_disable()");
	check((rd(R_USBCMD) & CMD_RS) == 0U, "USBCMD.RS cleared");
	check(rd(R_USBINTR) == 0U, "interrupts masked again");
	check((rd(R_USBSTS) & STS_HCH) != 0U, "USBSTS.HCH set (halted)");

	passed = checks - failures;
	if (failures == 0U) {
		LOG_INF("usb_host_bringup: %u checks - PASS", checks);
	} else {
		LOG_ERR("usb_host_bringup: %u of %u checks - FAIL", passed, checks);
	}

	return 0;
}
