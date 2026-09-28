/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * verify_flow — the application half of the end-to-end verification sample.
 *
 * The loader (samples/spi_boot_loader, built with the production profile)
 * does the verifying; this image is what that verification produced. Its job
 * is to prove the chain held, on the console, in one place:
 *
 *   - it reads the boot record back through the driver's public API, so the
 *     slot state, the image CRC and the anti-rollback floor it prints are the
 *     loader's own view, not a copy this image brought along;
 *   - it reads the MCUboot header at the slot base — the container the loader
 *     verified before it copied and jumped — and prints the version it finds.
 *     The header layout is MCUboot's, the same one the driver parses
 *     (drivers/misc/boot_agm_verify.c::boot_agm_image_version_read);
 *   - it answers the trial-boot handshake with agm_boot_trial_confirm(),
 *     which is the application's side of "this image runs, promote it";
 *   - it ends with a single line tools/verify_flow.sh asserts on:
 *
 *         verify_flow: VERIFY-FLOW: PASS
 *
 * Only the positive verdict lives here. Everything that must be *refused*
 * (tampered container, older version, unsigned bitstream, unauthorized
 * publish) is asserted by the host script against the loader's own output,
 * because the loader is where those decisions are made.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/sys/printk.h>

#define BOOT_DEV DEVICE_DT_GET(DT_NODELABEL(boot))

/* The slot base is this image's link address minus the MCUboot header the
 * signed build leaves in front of it (CONFIG_ROM_START_OFFSET).
 */
#define SLOT_BASE 0x8007c000U

/* MCUboot's image header, only as far as the version: magic(4) load(4)
 * hdr_size(2) protect_tlv_size(2) img_size(4) flags(4) version(8).
 */
struct img_header_head {
	uint32_t magic;
	uint32_t load_addr;
	uint16_t hdr_size;
	uint16_t protect_tlv_size;
	uint32_t img_size;
	uint32_t flags;
	uint8_t ver[4]; /* major, minor, revision(LE) */
} __packed;

#define MCUBOOT_IMAGE_MAGIC 0x96f3b83dU

static void ledger(void)
{
	const struct img_header_head *hdr = (const struct img_header_head *)SLOT_BASE;
	struct agm_boot_info info;
	bool ok = true;
	int ret;

	printk("\n");
	printk("verify_flow: ===== verification ledger =====\n");

	if (!device_is_ready(BOOT_DEV)) {
		printk("verify_flow: boot device not ready -- FAIL\n");
		printk("verify_flow: VERIFY-FLOW: FAIL\n");
		return;
	}

	ret = agm_boot_info_get(BOOT_DEV, &info);
	if (ret < 0 || !info.record_valid) {
		printk("verify_flow: boot record invalid (%d) -- FAIL\n", ret);
		printk("verify_flow: VERIFY-FLOW: FAIL\n");
		return;
	}

	const struct agm_boot_slot_info *slot = &info.slot[info.active];

	printk("verify_flow: record   : valid, mode %s, active slot %c\n",
	       agm_boot_mode_name(info.mode), 'A' + info.active);
	printk("verify_flow: slot     : %s, %s, %u B, crc 0x%08x, load 0x%08x\n",
	       agm_boot_slot_state_name(slot->state),
	       slot->src == AGM_BOOT_SRC_ON_DIE ? "on-die" : "store",
	       slot->len, slot->crc, slot->load);
	printk("verify_flow: floor    : v%u.%u.%u (anti-rollback)\n",
	       (unsigned int)((info.sec_ver >> 24) & 0xff),
	       (unsigned int)((info.sec_ver >> 16) & 0xff),
	       (unsigned int)((info.sec_ver >> 8) & 0xff));
	printk("verify_flow: my header: magic 0x%08x, v%u.%u.%u, hdr %u B, img %u B\n",
	       hdr->magic, hdr->ver[0], hdr->ver[1], hdr->ver[2] | (hdr->ver[3] << 8),
	       hdr->hdr_size, hdr->img_size);

	ok = ok && (hdr->magic == MCUBOOT_IMAGE_MAGIC);
	ok = ok && (hdr->hdr_size == 0x20U);
	ok = ok && (slot->src == AGM_BOOT_SRC_ON_DIE);
	ok = ok && (slot->load == SLOT_BASE);
	ok = ok && (slot->state == AGM_BOOT_SLOT_TRIAL || slot->state == AGM_BOOT_SLOT_CONFIRMED);

	/* The application's half of the trial handshake: "this image runs". */
	ret = agm_boot_trial_confirm();
	if (ret == 0) {
		printk("verify_flow: handshake: trial confirmed (slot becomes "
		       "CONFIRMED on the next boot)\n");
	} else if (ret == -ENOENT) {
		printk("verify_flow: handshake: not a trial boot (already confirmed)\n");
	} else {
		printk("verify_flow: handshake: failed (%d)\n", ret);
		ok = false;
	}

	printk("verify_flow: VERIFY-FLOW: %s\n", ok ? "PASS" : "FAIL");
}

int main(void)
{
	uint32_t tick = 0U;

	ledger();

	/* Keep printing a slow heartbeat: the ledger is one shot, and the dev board
	 * capture attaches after boot (the port record's capture discipline).
	 */
	while (true) {
		printk("verify_flow: alive %u (uptime %u ms)\n", tick++,
		       (uint32_t)k_uptime_get());
		k_msleep(2000);
	}

	return 0;
}
