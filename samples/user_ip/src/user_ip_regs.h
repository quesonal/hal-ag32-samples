/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The register map of one piece of user logic, as macros.
 *
 * Why macros and not devicetree: this map is a property of the *fabric*, not
 * of the board -- the same board with a different bitstream has different
 * registers -- and it is written in the RTL, which lives in the Quartus flow
 * rather than in this build. Keeping it here (a header the RTL and the
 * firmware can share, or that a generator can emit from the RTL) means one
 * source instead of two that drift; devicetree cannot express bitfields
 * anyway. What *is* devicetree's business is the window this block lives in
 * (agm,agrv2k-cpld: an SoC-level address range the driver range-checks) and
 * the pin a fabric interrupt drives -- those are board/SoC facts.
 *
 * Keep this in step with the Verilog. The build checks what can be checked
 * without the RTL: that the block fits inside the window (main.c's
 * BUILD_ASSERT) and that every register is inside the block (here).
 */

#ifndef SAMPLES_USER_IP_REGS_H_
#define SAMPLES_USER_IP_REGS_H_

#include <stdint.h>

#include <zephyr/sys/util.h>

/*
 * Where the block sits inside the fabric window, and how big it is. The
 * vendor examples put their wrappers at 0x0000 / 0x1000 / 0x2000 (ADC0/1/2)
 * and SPI0 at 0x6000; 0x1000 is the one this sample claims.
 */
#define USER_IP_OFFSET 0x1000U
#define USER_IP_SIZE   0x100U

/* Registers (byte offsets from the block start). */
#define USER_IP_REG_ID      0x00U /* RO: magic/version, see below */
#define USER_IP_REG_CTRL    0x04U /* RW */
#define USER_IP_REG_STATUS  0x08U /* RO */
#define USER_IP_REG_COUNTER 0x0cU /* RO: free-running */

#define USER_IP_REG_COUNT   4U

/* USER_IP_REG_ID: the RTL's idea of its own identity, so a firmware/bitstream
 * mismatch is visible on the console instead of showing up as garbage reads. */
#define USER_IP_ID_MAGIC   0x5550U /* "UP" (user IP) */
#define USER_IP_ID_VERSION 0x0001U
#define USER_IP_ID_VALUE \
	(((uint32_t)USER_IP_ID_MAGIC << 16) | (uint32_t)USER_IP_ID_VERSION)

/* USER_IP_REG_CTRL bits. */
#define USER_IP_CTRL_ENABLE BIT(0)
#define USER_IP_CTRL_IRQEN  BIT(1)
#define USER_IP_CTRL_CLR    BIT(2) /* write 1 to clear */

/* USER_IP_REG_STATUS bits. */
#define USER_IP_STATUS_READY BIT(0)
#define USER_IP_STATUS_IRQ   BIT(1)

/* The build-time half of "one source of truth": every register has to be a
 * 32-bit access inside the block. */
#define USER_IP_REG_OK(off) ((off) + 4U <= USER_IP_SIZE)

#endif /* SAMPLES_USER_IP_REGS_H_ */
