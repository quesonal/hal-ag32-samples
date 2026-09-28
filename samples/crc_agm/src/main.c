/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * crc_agm — CRC32/ISO-HDLC on the AgRV2K CRC unit.
 *
 * What this checks, and why each line is here:
 *
 *   - the standard check value: crc32_ieee("123456789") must be 0xCBF43926.
 *     That is the number every implementation of CRC-32/ISO-HDLC produces,
 *     and the one this port's records, slots and containers are built on;
 *   - that the CRC device is present and ready: with the `zephyr,crc` chosen
 *     node enabled, Zephyr compiles subsys/crc/crc_hardware.c and routes
 *     crc32_ieee() through the driver (the weak software implementation in
 *     subsys/crc/crc32_sw.c only survives when no CRC driver is present);
 *   - the cycles a slot-sized buffer costs. The software version measured
 *     13.39 M cycles (~67 ms at 200 MHz) for 99944 B; the hardware one
 *     measures ~0.70 M (~3.5 ms). The sample prints both the time and a
 *     verdict so a dev board run cannot silently regress to software.
 *   - the **chunked** accumulation (crc32_ieee_update() in 256-byte steps)
 *     must agree with the one-shot crc32_ieee() of the same buffer. That is
 *     how the bootloader CRCs a store, a slot or an upload, and it is the one
 *     pattern a one-shot check cannot cover: the unit's seed is in the
 *     bit-reversed domain and has to be written *after* the RESET, neither of
 *     which shows up while the seed is the symmetric 0xffffffff. Measured on
 *     the dev board 2026-09-24: with both mistakes present the chain degraded to
 *     "the CRC of the last chunk", and the loader refused an image it had
 *     just written (its own CRC of the same bytes disagreed).
 *
 * The unit is fed byte by byte (the driver does that), so this is a
 * throughput measurement of the feed loop plus the unit's 4-AHB-cycle-per-
 * 32-bit-word pipeline, not a DMA benchmark.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

/* CRC32/ISO-HDLC check value of "123456789". */
#define CRC32_IEEE_CHECK 0xCBF43926U

/* The fabric slot the loader CRCs on every boot (agm,agrv2k-bitstream-size). */
#define SLOT_BYTES 99944U

/* A software CRC of this buffer measured 13.39 M cycles on this board; the
 * hardware one is ~0.70 M. Anything under 2 M cycles is the unit doing it. */
#define HW_CYCLE_CEILING 2000000U

static uint8_t slot[SLOT_BYTES];

int main(void)
{
	const struct device *crc = DEVICE_DT_GET(DT_CHOSEN(zephyr_crc));
	uint32_t got = crc32_ieee((const uint8_t *)"123456789", 9U);
	uint32_t crc_slot;
	uint32_t cycles;
	bool ok = true;

	printk("\ncrc_agm: %s ready=%d\n", crc->name, (int)device_is_ready(crc));

	printk("crc_agm: check \"123456789\" -> 0x%08x (expect 0x%08x) %s\n", got,
	       CRC32_IEEE_CHECK, got == CRC32_IEEE_CHECK ? "PASS" : "FAIL");
	ok = ok && (got == CRC32_IEEE_CHECK);

	for (size_t i = 0U; i < sizeof(slot); i++) {
		slot[i] = (uint8_t)(i * 31U + 7U);
	}

	uint32_t t0 = k_cycle_get_32();

	crc_slot = crc32_ieee(slot, sizeof(slot));
	cycles = k_cycle_get_32() - t0;

	/* The chunked chain, on the same bytes: this is what the bootloader
	 * does (drivers/misc/boot_agm*.c feed 256-byte reads through
	 * crc32_ieee_update()), and it is the case a one-shot check misses. */
	uint32_t chunked = 0U;

	for (size_t off = 0U; off < sizeof(slot); off += 256U) {
		size_t n = MIN(256U, sizeof(slot) - off);

		chunked = crc32_ieee_update(chunked, &slot[off], n);
	}
	printk("crc_agm: chunked (256 B x %u) -> 0x%08x %s\n",
	       (unsigned int)(sizeof(slot) / 256U), chunked,
	       chunked == crc_slot ? "PASS" : "FAIL -- seed not honored");
	ok = ok && (chunked == crc_slot);

	/* Reference: the same computation with a direct, constant-address feed
	 * loop, to separate the driver's code from the peripheral's cost. */
	volatile uint32_t *regs = (volatile uint32_t *)0x41002000U;
	uint32_t direct;

	/* Same order the driver uses (RESET first, then the reversed seed):
	 * the default 0xffffffff is symmetric, so a wrong order still gives the
	 * right number here -- see the driver's header. */
	regs[0x14 / 4] = 0x04C11DB7U;
	regs[0x08 / 4] = (1U << 5) | (1U << 7);
	regs[0x08 / 4] |= 1U;
	regs[0x10 / 4] = 0xFFFFFFFFU;
	t0 = k_cycle_get_32();
	for (size_t i = 0U; i < sizeof(slot); i++) {
		*(volatile uint8_t *)regs = slot[i];
	}
	direct = ~regs[0];
	{
		uint32_t dcycles = k_cycle_get_32() - t0;

		printk("crc_agm: reference direct feed: 0x%08x in %u cycles (%u cyc/B)\n",
		       direct, dcycles, (unsigned int)(dcycles / sizeof(slot)));
	}
	printk("crc_agm: driver: %u cyc/B -- %u cyc/B\n", (unsigned int)(cycles / sizeof(slot)),
	       (unsigned int)(cycles / sizeof(slot)));

	printk("crc_agm: %u B -> 0x%08x in %u cycles (%u us at %u Hz)\n",
	       (unsigned int)sizeof(slot), crc_slot, cycles,
	       (unsigned int)((uint64_t)cycles * 1000000U / sys_clock_hw_cycles_per_sec()),
	       sys_clock_hw_cycles_per_sec());
	printk("crc_agm: %s (hardware-speed feed: %u cycles <= %u)\n",
	       cycles <= HW_CYCLE_CEILING ? "PASS" : "FAIL -- not the hardware path",
	       cycles, (unsigned int)HW_CYCLE_CEILING);
	ok = ok && (cycles <= HW_CYCLE_CEILING);

	/* Print forever: the console reader attaches after boot, so a one-shot
	 * banner is easy to miss (the dev board guide's capture discipline). */
	while (true) {
		printk("crc_agm: check=\"123456789\" -> 0x%08x (%s) | %u B in %u cycles "
		       "(%s) | chunked 0x%08x (%s) | %s\n",
		       crc32_ieee((const uint8_t *)"123456789", 9U),
		       got == CRC32_IEEE_CHECK ? "PASS" : "FAIL", (unsigned int)sizeof(slot),
		       cycles, cycles <= HW_CYCLE_CEILING ? "hw-path" : "SW!",
		       chunked, chunked == crc_slot ? "PASS" : "FAIL",
		       ok ? "ALL PASS" : "FAILED");
		k_msleep(3000);
	}
}
