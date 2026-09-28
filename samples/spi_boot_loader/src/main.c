/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief spi_boot_loader -- the console in front of the AgRV2K boot driver.
 *
 * All of the storage and DFU logic lives in the driver
 * (drivers/misc/boot_agm.c, include/zephyr/drivers/misc/boot_agm.h); this
 * file is the operator interface:
 *
 *   - the line-oriented console (help/info/mode/once/install-slot/confirm/
 *     rollback/erase/boot/upload/an-status/reboot);
 *   - the demultiplexer that gives the console UART to the two protocol
 *     servers: 0x7F starts an AN3155 session (agrv32flash) and the
 *     0x0609/0x0414 line headers feed mcumgr (smpmgr);
 *   - the 1.5 s boot window: any character cancels the boot and keeps the
 *     console, which is the only way back in when the record points at a bad
 *     image.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <stdio.h>
#include <string.h>

#if IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_SERIAL_HAS_SMP_OVER_CONSOLE)
#include <zephyr/mgmt/mcumgr/transport/serial.h>
/* mcumgr support is compiled in (CONFIG_BOOT_AGM_SMP). */
#define BOOT_LOADER_SMP 1
#endif

/* The AN3155 server (agrv32flash's protocol) is a driver-side option, so the
 * console's entry point into it -- the 0x7F INIT byte and the `an-status`
 * command -- has to disappear with it: with CONFIG_BOOT_AGM_AN3155=n the
 * header declares neither `agm_boot_an3155_run()` nor its status printer, and
 * the driver source is not compiled in. Leaving these calls unguarded was what
 * made "the trimmed build" (AN3155 off, along with mcumgr's ~13 KB) fail to
 * link instead of just getting smaller. */
#if IS_ENABLED(CONFIG_BOOT_AGM_AN3155)
#define BOOT_LOADER_AN3155 1
#endif

#define BOOT_DEV DEVICE_DT_GET(DT_NODELABEL(boot))

/* The loader is linked against the `boot_loader` partition (the board overlay
 * points /chosen/zephyr,flash at it) while the boot node separately says how
 * much of the flash below the record belongs to the loader. Nothing bound
 * those two numbers together: change one and the mismatch shows up as a link
 * error at best, or as a loader that grew into the record at worst (the
 * 2026-09-18 incident). Bind them here (RF-NEW-008).
 *
 * Which number that is depends on the layout family, same as in the driver:
 * the on-die family derives everything from `loader-size`, the two-flash
 * family spells the slot out and the loader owns everything below it. */
#define T_BOOT_NODE DT_NODELABEL(boot)

#if DT_SAME_NODE(DT_PROP(T_BOOT_NODE, store_flash), DT_PROP(T_BOOT_NODE, on_die_flash))
#define T_LOADER_REGION_SIZE ((uint32_t)DT_PROP(T_BOOT_NODE, loader_size))
#else
#define T_LOADER_REGION_SIZE ((uint32_t)DT_PROP(T_BOOT_NODE, slot_address) - \
			      (uint32_t)DT_REG_ADDR(DT_PROP(T_BOOT_NODE, on_die_flash)))
#endif

BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(loader_region)) == T_LOADER_REGION_SIZE,
	     "the boot_loader partition and the boot node's loader region disagree");

/* How long the boot decision waits for a keypress before it acts on the
 * record. Without this the console is unreachable whenever the record says
 * "boot the image" -- including when that image is exactly what needs
 * replacing. */
#define BOOT_ABORT_WINDOW_MS 1500

static const struct device *const console_uart =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ---- console -------------------------------------------------------- */

static bool console_poll_char(char *c)
{
	return agm_boot_console_poll(BOOT_DEV, c) == 0;
}

#define SMP_HDR_PKT_1  0x06U
#define SMP_HDR_FRAG_1 0x04U

/* How long one mcumgr line may take before it is dropped. A host writes a
 * line in one burst (the 5 ms header window above is the same idea); this
 * only has to outlast the USB CDC latency, not a slow human. */
#define SMP_LINE_TIMEOUT_MS 1000

#ifdef BOOT_LOADER_SMP
/* Collect the rest of one mcumgr line and hand it to the SMP server.
 *
 * mcumgr's UART framing is base64 lines behind a header: 0x0609 starts a
 * packet, 0x0414 continues a fragmented one, and the line ends with '\n'
 * (exactly what Zephyr's own UART transport driver feeds to
 * mcumgr_serial_process_frag(), newline included). Nothing is echoed: the
 * host reads protocol replies, not our console.
 *
 * Returns false when the bytes are not a frame header after all (a stray 0x06
 * typed at the console), so the caller can keep treating them as text.
 */
static bool console_smp_line(uint8_t c_in)
{
	static uint8_t frag[MCUMGR_SERIAL_MAX_FRAME + 8U];
	uint32_t n = 0U;
	int64_t from = k_uptime_get();
	char c;

	frag[n++] = c_in;

	/* The second header byte follows immediately in the same burst. */
	while (k_uptime_get() - from < 5) {
		if (console_poll_char(&c)) {
			if ((uint8_t)c != MCUMGR_SERIAL_HDR_PKT_2 &&
			    (uint8_t)c != MCUMGR_SERIAL_HDR_FRAG_2) {
				return false;
			}
			frag[n++] = (uint8_t)c;
			break;
		}
		arch_nop();
	}
	if (n != 2U) {
		return false;
	}

	/* Then the base64 payload and the newline that ends the line. A
	 * fragmented packet arrives as several such lines.
	 *
	 * Bounded on purpose: a host that starts a line and never ends it (or a
	 * stray 0x06 typed at the console) used to park this loop forever, and
	 * the only way back was a reset (measured 2026-09-18). A line longer
	 * than the buffer is a bad frame too -- do not silently drop its tail. */
	for (;;) {
		if (console_poll_char(&c)) {
			if (n >= sizeof(frag)) {
				printk("loader: mcumgr line too long, dropped\n");
				return false;
			}
			frag[n++] = (uint8_t)c;
			from = k_uptime_get();
			if (c == '\n') {
				break;
			}
			continue;
		}
		if (k_uptime_get() - from > SMP_LINE_TIMEOUT_MS) {
			printk("loader: mcumgr line timed out, dropped\n");
			return false;
		}
		arch_nop();
	}

	agm_boot_smp_rx(BOOT_DEV, frag, n);
	return true;
}
#endif /* BOOT_LOADER_SMP */

/* Blocking line reader with echo: CR/LF ends the line, backspace edits it.
 *
 * Returns true when @a buf holds a command. It returns false when the console
 * was handed to the AN3155 server instead -- and then @a buf must not be
 * parsed: it used to be left holding the *previous* command, so every
 * agrv32flash session ended by running that command again (measured
 * 2026-09-18: `erase` re-ran after a read-only session). */
static bool console_read_line(char *buf, size_t len)
{
	size_t n = 0;
	char c;

	for (;;) {
		/* Tight poll: the PL011 RX FIFO is 16 bytes deep (the SDK
		 * configures UART_LCR_FIFO_16) and the probe's CDC-ACM bridge
		 * delivers a typed line as one burst, so anything slower than a
		 * busy loop drops the tail of the line -- measured 2026-09-17:
		 * a 19-byte command arrived as 15 bytes and never terminated. */
		if (!console_poll_char(&c)) {
			arch_nop();
			continue;
		}
		if (c == 0x7F) {
			/* The vendor tool's INIT byte: hand the console over to
			 * the AN3155 server (agrv32flash -m 8n1 ...). It has to be
			 * recognised even mid-line -- leftover bytes from a
			 * previous session used to leave n > 0, and then the INIT
			 * was eaten as text and the tool reported "Failed to init
			 * device". 0x7F is not treated as backspace (use 0x08). */
			buf[0] = '\0';
#ifdef BOOT_LOADER_AN3155
			agm_boot_an3155_run(BOOT_DEV);
			return false;
#else
			/* No AN3155 server in this build: swallow the byte so it
			 * cannot end up in the next command line. */
			printk("loader: AN3155 is not compiled in "
			       "(CONFIG_BOOT_AGM_AN3155=n)\n");
			continue;
#endif
		}
#ifdef BOOT_LOADER_SMP
		if (c == (char)SMP_HDR_PKT_1 || c == (char)SMP_HDR_FRAG_1) {
			/* mcumgr (SMP over console): hand the line over and stay
			 * in the console loop. */
			if (console_smp_line((uint8_t)c)) {
				continue;
			}
		}
#endif
		if (c == '\r' || c == '\n') {
			/* The LF of a CRLF ending is left in the FIFO on purpose:
			 * the upload phase skips stray line endings before the
			 * first frame byte, while peeking here would swallow the
			 * first byte of a binary phase that follows immediately. */
			printk("\r\n");
			break;
		}
		if (c == '\b') {
			if (n > 0) {
				n--;
				printk("\b \b");
			}
			continue;
		}
		if (n + 1 < len) {
			buf[n++] = c;
			uart_poll_out(console_uart, c);
		}
	}
	buf[n] = '\0';
	return true;
}

/* ---- boot window ---------------------------------------------------- */

static bool console_abort_requested(void)
{
	char c;

	printk("loader: booting in %u ms -- send any character to stay in the "
	       "console\n", BOOT_ABORT_WINDOW_MS);

	for (uint32_t waited = 0; waited < BOOT_ABORT_WINDOW_MS; waited += 50) {
		if (console_poll_char(&c)) {
			printk("loader: console input detected -- boot cancelled\n");
			return true;
		}
		k_msleep(50);
	}
	return false;
}

/* ---- console commands ----------------------------------------------- */

static void print_help(void)
{
	printk("commands:\n"
	       "  info                             one-shot + record + slot state\n"
	       "  mode <auto|internal>             persistent boot mode\n"
	       "  once <none|internal>             next boot only (RTC backup domain)\n"
	       "  confirm                          promote the active store to CONFIRMED\n"
	       "  rollback                         make the other store active\n"
	       "  upload <a|b|slot|bitstream>      receive an image over this console\n"
	       "                                   (slot = on-die flash DFU; see\n"
	       "                                   tools/agm_upload.py)\n"
	       "  install-slot                     copy the active store into the\n"
	       "                                   on-die application slot\n"
	       "  erase [a|bitstream]             drop store A's image, or the pending bitstream\n"
#ifdef BOOT_LOADER_AN3155
	       "  an-status                        last AN3155 session's diagnostics\n"
#endif
	       "  reboot                           cold reboot\n"
	       "  boot                             apply the boot policy now\n"
#if defined(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	       /* Review RF-007: an operator who tries upload/erase here gets
		* the driver's refusal, and this is the line that says what to
		* do instead. The two paths cannot be handed the second grant an
		* upload needs (see the Kconfig help). */
	       "production lock is on: upload/publish/erase need a signed command --\n"
	       "  upload from a host instead: tools/smp_cli.py <port> upload <image> <file>\n"
	       "  --authorize <key.pem>  (this console and agrv32flash cannot publish)\n"
#endif
	       );
}

static void print_info(void)
{
	struct agm_boot_info info;

	agm_boot_info_get(BOOT_DEV, &info);

	printk("one-shot : %s\n", agm_boot_mode_name(info.once));
	if (info.record_valid) {
		printk("record   : magic ok, mode %s, active %c\n",
		       agm_boot_mode_name(info.mode), info.active == 0U ? 'A' : 'B');
		/* The anti-rollback floor (0 in builds without
		 * CONFIG_BOOT_AGM_ANTI_ROLLBACK): the lowest version an upload may
		 * carry from here on. */
		printk("floor    : v%u.%u.%u\n", (unsigned)(info.sec_ver >> 24),
		       (unsigned)((info.sec_ver >> 16) & 0xffU),
		       (unsigned)(info.sec_ver & 0xffffU));
	} else if (info.record_read_failed) {
		/* The sector could not be read at init: not the same as an empty
		 * (readable, erased) one, and not the same as a corrupt record --
		 * nothing here says what the flash actually holds. */
		printk("record   : unreadable (the record sector read failed at "
		       "init -- state below is not trusted)\n");
	} else if (info.record_blank) {
		/* Nothing has been installed yet -- the normal state right after a
		 * flash (and the only state an on-die A/B board starts in, since
		 * its record sector is blank until the first upload). */
		printk("record   : empty (nothing installed yet)\n");
	} else {
		printk("record   : invalid (magic 0x%08x, version %u)\n",
		       info.record_magic, info.record_version);
	}

	for (uint32_t i = 0U; i < 2U; i++) {
		const struct agm_boot_slot_info *s = &info.slot[i];

		/* "store" is the offset inside whichever flash backs the record
		 * and the stores; with the on-die A/B layout that flash is the
		 * on-die one, so the label must not say "ext". */
		if (s->src == AGM_BOOT_SRC_ON_DIE) {
			printk("store %c  : %-9s attempts %u/%u, on-die 0x%08x+%u, "
			       "load 0x%08x crc 0x%08x\n", i == 0U ? 'A' : 'B',
			       agm_boot_slot_state_name(s->state), s->attempts,
			       (uint32_t)AGM_BOOT_MAX_ATTEMPTS,
			       agm_boot_target_window_base(BOOT_DEV,
							   AGM_BOOT_TARGET_SLOT),
			       s->len, s->load, s->crc);
		} else {
			printk("store %c  : %-9s attempts %u/%u, store 0x%06x+%u, "
			       "load 0x%08x crc 0x%08x\n", i == 0U ? 'A' : 'B',
			       agm_boot_slot_state_name(s->state), s->attempts,
			       (uint32_t)AGM_BOOT_MAX_ATTEMPTS, s->offset, s->len,
			       s->load, s->crc);
		}
	}
	printk("on-die   : slot at 0x%08x %s\n",
	       agm_boot_target_window_base(BOOT_DEV, AGM_BOOT_TARGET_SLOT),
	       info.slot_programmed ? "(programmed)" : "(blank)");
	if (agm_boot_bind_salt_addr() != 0U) {
		/* What tools/agm_bind.py provisions and reads: printed rather than
		 * kept in a host-side constant, because the address is derived
		 * from the layout sizes (docs/FLASH-LAYOUT.md). */
		printk("bind-salt: 0x%08x (one sector)\n", agm_boot_bind_salt_addr());
	}

	/* The chip's 128-bit ID: the host half of per-chip binding needs it
	 * (tools/agm_bind.py), and printing it here is also how the dev board
	 * checks that the device's own flex-read path agrees with SWD. */
	{
		uint8_t uid[AGM_BOOT_UID_LEN];

		if (agm_boot_unique_id(uid) == 0) {
			printk("uid      : %02x%02x%02x%02x %02x%02x%02x%02x "
			       "%02x%02x%02x%02x %02x%02x%02x%02x\n",
			       uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7],
			       uid[8], uid[9], uid[10], uid[11], uid[12], uid[13], uid[14],
			       uid[15]);
		} else {
			printk("uid      : <read failed>\n");
		}
	}

#if defined(CONFIG_BOOT_AGM_BIND)
	/* The salt and the key it derives: the fingerprint is what the
	 * provisioning tool compares against (tools/agm_bind.py provision),
	 * so "the board read the salt we wrote" is checkable without the key
	 * ever leaving the chip. */
	{
		uint8_t fp[AGM_BOOT_BIND_FP_LEN];

		if (agm_boot_bind_fingerprint(fp) == 0) {
			printk("bind     : salt v1 present, key fp %02x%02x%02x%02x\n",
			       fp[0], fp[1], fp[2], fp[3]);
		} else {
			printk("bind     : no salt provisioned -- this build runs only "
			       "images bound to this chip\n");
		}
	}
#endif
}

static bool parse_mode(const char *s, enum agm_boot_mode *out)
{
	if (!strcmp(s, "auto")) {
		*out = AGM_BOOT_MODE_AUTO;
	} else if (!strcmp(s, "internal")) {
		*out = AGM_BOOT_MODE_INTERNAL;
	} else {
		return false;
	}
	return true;
}

static void handle_line(char *line)
{
	char *cmd = strtok(line, " \t\r\n");

	if (cmd == NULL) {
		return;
	}

	if (!strcmp(cmd, "help")) {
		print_help();
	} else if (!strcmp(cmd, "info")) {
		print_info();
	} else if (!strcmp(cmd, "boot")) {
		/* No abort window: the operator asked for it explicitly. */
		(void)agm_boot_policy_run(BOOT_DEV, NULL);
	} else if (!strcmp(cmd, "confirm")) {
		if (agm_boot_confirm(BOOT_DEV) < 0) {
			printk("loader: nothing to confirm\n");
		}
	} else if (!strcmp(cmd, "rollback")) {
		if (agm_boot_rollback(BOOT_DEV) < 0) {
			printk("loader: no valid boot record\n");
		}
	} else if (!strcmp(cmd, "upload")) {
		char *arg = strtok(NULL, " \t\r\n");
		enum agm_boot_target target;

		if (arg == NULL) {
			printk("usage: upload <a|b|slot|bitstream>\n");
			return;
		}
		if (!strcmp(arg, "a")) {
			target = AGM_BOOT_TARGET_STORE_A;
		} else if (!strcmp(arg, "b")) {
			target = AGM_BOOT_TARGET_STORE_B;
		} else if (!strcmp(arg, "slot")) {
			target = AGM_BOOT_TARGET_SLOT;
		} else if (!strcmp(arg, "bitstream")) {
			target = AGM_BOOT_TARGET_BITSTREAM;
		} else {
			printk("usage: upload <a|b|slot|bitstream>\n");
			return;
		}
		agm_boot_upload_console(BOOT_DEV, target);
	} else if (!strcmp(cmd, "install-slot")) {
		if (agm_boot_install_slot(BOOT_DEV) < 0) {
			printk("loader: install-slot failed (see above)\n");
		}
#ifdef BOOT_LOADER_AN3155
	} else if (!strcmp(cmd, "an-status")) {
		agm_boot_an3155_status_print(BOOT_DEV);
#endif
	} else if (!strcmp(cmd, "erase")) {
		/* `erase [a|bitstream]`; a bare `erase` keeps its old meaning
		 * (drop store A's image). `erase bitstream` drops the pending
		 * bitstream *and* its boot record, which is how the board goes
		 * back to booting the factory slot. */
		char *arg = strtok(NULL, " \t\r\n");
		enum agm_boot_target target = AGM_BOOT_TARGET_STORE_A;

		if (arg == NULL || !strcmp(arg, "a")) {
			target = AGM_BOOT_TARGET_STORE_A;
		} else if (!strcmp(arg, "bitstream")) {
			target = AGM_BOOT_TARGET_BITSTREAM;
		} else {
			printk("usage: erase [a|bitstream]\n");
			return;
		}
		/* The driver prints why it refused (blank records are not a
		 * refusal: dropping an image that was never installed is a
		 * no-op), so only the code has to be added here.
		 */
		if (agm_boot_erase(BOOT_DEV, target) < 0) {
			printk("loader: erase failed\n");
		} else if (target == AGM_BOOT_TARGET_STORE_A) {
			printk("loader: store A is not referenced by the boot record "
			       "any more\n");
		}
	} else if (!strcmp(cmd, "reboot")) {
		printk("loader: rebooting (the boot path programs the FCB from "
		       "0x800e7000)\n");
		k_msleep(50);
		sys_reboot(SYS_REBOOT_COLD);
	} else if (!strcmp(cmd, "mode")) {
		char *arg = strtok(NULL, " \t\r\n");
		enum agm_boot_mode m;

		if (arg == NULL) {
			printk("usage: mode <auto|internal>\n");
			return;
		}
		if (!parse_mode(arg, &m)) {
			printk("usage: mode <auto|internal>\n");
			return;
		}
		(void)agm_boot_mode_set(BOOT_DEV, m);
	} else if (!strcmp(cmd, "once")) {
		char *arg = strtok(NULL, " \t\r\n");
		enum agm_boot_mode m;

		if (arg == NULL) {
			printk("usage: once <none|internal>\n");
			return;
		}
		if (!strcmp(arg, "none")) {
			if (agm_boot_once_arm(BOOT_DEV, AGM_BOOT_MODE_AUTO) < 0) {
				printk("loader: one-shot unavailable (the RTC backup "
				       "domain is not clocked in this build)\n");
				return;
			}
			printk("loader: one-shot cleared\n");
			return;
		}
		if (!parse_mode(arg, &m)) {
			printk("usage: once <none|internal>\n");
			return;
		}
		if (agm_boot_once_arm(BOOT_DEV, m) < 0) {
			printk("loader: one-shot unavailable (the RTC backup domain "
			       "is not clocked in this build)\n");
			return;
		}
		printk("loader: next boot = %s (one-shot)\n", agm_boot_mode_name(m));
	} else {
		printk("unknown command '%s' (try 'help')\n", cmd);
	}
}

int main(void)
{
	char line[96];

	printk("\n");
	printk("=========================================\n");
	printk(" spi_boot_loader (driver-based)\n");
	printk(" boot driver: %s\n",
	       device_is_ready(BOOT_DEV) ? "ready" : "NOT READY");
	printk("=========================================\n");
	printk("type 'help' for commands\n");

	if (device_is_ready(BOOT_DEV)) {
		print_info();
		/* Apply the policy once at startup; if it declines to boot we
		 * land back here and the console takes over. */
		(void)agm_boot_policy_run(BOOT_DEV, console_abort_requested);
	} else {
		printk("loader: boot driver unavailable -- is the devicetree node "
		       "enabled?\n");
	}

	while (1) {
		printk("loader> ");
		if (console_read_line(line, sizeof(line))) {
			handle_line(line);
		}
	}

	return 0;
}
