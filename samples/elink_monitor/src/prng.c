/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny xorshift32 PRNG. The point here is "looks plausible enough on a
 * dashboard", not crypto: a single 32-bit state, three shifts, no attempts
 * to fix xorshift's well-known (and for this use irrelevant) weaknesses.
 */
#include "prng.h"

static uint32_t prng_state = 0x12345678U;

void prng_seed(uint32_t seed)
{
	/* xorshift32 must never see 0 -- it locks up. */
	prng_state = seed ? seed : 0x12345678U;
}

uint32_t prng_next(void)
{
	uint32_t x = prng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	prng_state = x;
	return x;
}

uint32_t prng_range(uint32_t lo, uint32_t hi)
{
	uint32_t span = (hi >= lo) ? (hi - lo + 1U) : 1U;

	return lo + (prng_next() % span);
}

uint8_t prng_pick_u8(const uint8_t *table, size_t n)
{
	return table[prng_range(0U, (uint32_t)(n - 1U))];
}
