/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DUAL_IP_RAM_WINDOW_H_
#define DUAL_IP_RAM_WINDOW_H_

/* Write/read the RAM block of ip/dual_ip.v through the cpld0 window. Returns 0
 * when every word read back, a negative errno otherwise. */
int dual_ip_ram_test(void);

/* The decode check: a write one block above the RAM must not reach it. Kept
 * separate so main.c can run it *after* the SPI half -- it touches the SPI
 * IP's side of the window, and that is the access most likely to stall. */
int dual_ip_alias_test(void);

#endif /* DUAL_IP_RAM_WINDOW_H_ */
