/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ELINK_MONITOR_PRNG_H
#define ELINK_MONITOR_PRNG_H

#include <stdint.h>
#include <stddef.h>

/* Seed the PRNG. Must be called once before prng_next(). */
void prng_seed(uint32_t seed);

/* Next 32-bit pseudo-random number. */
uint32_t prng_next(void);

/* Uniformly distributed in [lo, hi] inclusive. Caller must ensure lo <= hi. */
uint32_t prng_range(uint32_t lo, uint32_t hi);

/* Pick one element from a non-empty array. */
uint8_t prng_pick_u8(const uint8_t *table, size_t n);

#endif /* ELINK_MONITOR_PRNG_H */
