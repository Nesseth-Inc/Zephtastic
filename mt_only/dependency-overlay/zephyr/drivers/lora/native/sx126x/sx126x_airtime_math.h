/*
 * sx126x_airtime_math.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure-arithmetic mirror of the legacy 32-bit SX126x LoRa airtime
 * rounding. The caller supplies committed effective values (bandwidth in
 * Hz, SF 5..12, CR 1..4, ldro, crc). No radio/RTOS I/O, no allocator,
 * no strings, no globals, no floating point.
 *
 * sx126x_airtime_estimate() returns the exact legacy uint32 millisecond
 * value for every valid parameter combination, even when a uint32
 * intermediate overflowed (each original uint32 step wraps mod 2^32).
 * trustworthy is true only when tsym > 0 and the complete preamble,
 * payload, and total+500 operations do not overflow uint32.
 */

#ifndef SX126X_AIRTIME_MATH_H
#define SX126X_AIRTIME_MATH_H

#include <stdbool.h>
#include <stdint.h>

struct sx126x_airtime_input {
	uint32_t bandwidth_hz;
	uint16_t preamble_len;
	uint8_t sf;
	uint8_t cr;
	bool ldro;
	bool crc;
};

struct sx126x_airtime_value {
	uint32_t ms;
	bool trustworthy;
};

static inline struct sx126x_airtime_value
sx126x_airtime_estimate(const struct sx126x_airtime_input *c,
			uint32_t length)
{
	struct sx126x_airtime_value v = { 0, false };

	if (c == 0 || c->bandwidth_hz == 0u || c->sf < 5u || c->sf > 12u ||
	    c->cr < 1u || c->cr > 4u || length > 255u) {
		return v;
	}

	/* Legacy uint32 rounding: every original uint32 intermediate
	 * wraps mod 2^32; uint64 copies feed only the overflow check.
	 */
	uint32_t tsym = ((1u << c->sf) * 1000000u) / c->bandwidth_hz;
	uint64_t p64 = (uint64_t)(c->preamble_len + 4u) * tsym + tsym / 4u;
	uint32_t preamble = (uint32_t)p64;
	int32_t tmp = 8 * (int32_t)length - 4 * (int32_t)c->sf + 28 +
		      16 * (c->crc ? 1 : 0);
	if (tmp < 0) {
		tmp = 0;
	}
	uint32_t den = 4u * (c->sf - 2u * (c->ldro ? 1u : 0u));
	uint32_t npayload = 8u + ((tmp + den - 1u) / den) * (c->cr + 4u);
	uint64_t y64 = (uint64_t)npayload * tsym;
	uint32_t payload = (uint32_t)y64;
	uint32_t sum = preamble + payload;
	uint32_t total = sum + 500u;
	v.ms = total / 1000u;
	v.trustworthy = tsym > 0u && p64 <= UINT32_MAX &&
			y64 <= UINT32_MAX &&
			(uint64_t)preamble + payload + 500u <= UINT32_MAX;
	return v;
}

#endif /* SX126X_AIRTIME_MATH_H */
