/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DRIVERS_LORA_SX126X_DUAL_GATE_H_
#define ZEPHYR_DRIVERS_LORA_SX126X_DUAL_GATE_H_
#include "sx126x_bench_gate.h"
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
#include "sx126x_continuous_gate.h"
#else

/* Caller holds the native spinlock. Network 0 is MeshCore; 1 is Meshtastic. */
struct sx126x_dual_gate {
	struct sx126x_bench_gate mt;
	bool used, active, in_flight, mc_done;
	uint8_t network, mc_attempts, stage, identity[3], body[184], body_len, hash_size;
	uint16_t len;
	uint32_t deadline, expires, timeout_ms, budget_ms;
	uintptr_t owner;
	uint8_t frame[255];
};

static inline void dg_revoke(struct sx126x_dual_gate *g)
{
	g->active = false;
	g->stage = 0U;
	bg_revoke(&g->mt);
}

static inline bool dg_arm(struct sx126x_dual_gate *g, uint8_t relay,
			  const uint8_t identity[3], uintptr_t owner,
			  uint32_t now, uint32_t deadline)
{
	if (g->used || identity == NULL || !bg_arm(&g->mt, relay, owner, now, deadline)) {
		return false;
	}
	g->used = true;
	g->active = true;
	g->owner = owner;
	g->deadline = deadline;
	memcpy(g->identity, identity, 3U);
	return true;
}

static inline bool dg_issue(struct sx126x_dual_gate *g, uint8_t network,
			    const uint8_t *p, size_t n, uintptr_t owner,
			    uint32_t now, uint32_t air_ms)
{
	if (!g->active || g->in_flight || g->stage != 0U || g->owner != owner ||
	    p == NULL || n > 255U || network > 1U || air_ms == 0U || air_ms > 1749U ||
	    g->budget_ms + air_ms + 251U > 6000U || bg_due(now, g->deadline) ||
	    g->deadline - now < air_ms + 751U) {
		return false;
	}
	if (network == 1U) {
		if (!bg_issue(&g->mt, p, n, owner, now, air_ms)) {
			return false;
		}
	} else {
		if (n < 6U || p[0] != 0x15U || (p[1] & 0x3fU) != 1U ||
		    (p[1] >> 6) == 3U || g->mc_attempts >= 3U || g->mc_done) {
			return false;
		}
		uint8_t size = 1U + (p[1] >> 6);
		size_t body_len = n - 2U - size;

		if (body_len < 3U || body_len > sizeof(g->body) ||
		    memcmp(p + 2, g->identity, size) != 0) {
			return false;
		}
		if (g->body_len != 0U && (g->body_len != body_len || g->hash_size != size ||
		    memcmp(g->body, p + 2U + size, body_len) != 0)) {
			return false;
		}
		g->body_len = body_len;
		g->hash_size = size;
		memcpy(g->body, p + 2U + size, body_len);
	}
	g->network = network;
	g->len = n;
	memcpy(g->frame, p, n);
	g->timeout_ms = air_ms + 251U;
	g->expires = now + 250U;
	g->stage = 1U;
	return true;
}

static inline bool dg_admit(struct sx126x_dual_gate *g, uint8_t network,
			    const uint8_t *p, size_t n, uintptr_t owner, uint32_t now)
{
	bool ok = g->active && !g->in_flight && g->stage == 1U && g->network == network &&
		g->owner == owner && !bg_due(now, g->expires) && !bg_due(now, g->deadline) &&
		p != NULL && n == g->len && memcmp(p, g->frame, n) == 0;

	if (ok && network == 1U) {
		ok = bg_admit(&g->mt, p, n, owner, now);
	}
	if (!ok) {
		g->stage = 0U;
		g->mt.stage = 0U;
		return false;
	}
	if (network == 0U) {
		g->mc_attempts++;
	}
	g->budget_ms += g->timeout_ms;
	g->stage = 2U;
	return true;
}

static inline bool dg_ready(const struct sx126x_dual_gate *g, uint8_t network,
			    uintptr_t owner, uint32_t now)
{
	return g->active && !g->in_flight && g->stage == 2U && g->network == network &&
		g->owner == owner && !bg_due(now, g->expires) && !bg_due(now, g->deadline);
}

static inline bool dg_consume(struct sx126x_dual_gate *g, uint8_t network,
			      uintptr_t owner, uint32_t now, const uint8_t *p, size_t n)
{
	bool ok = dg_ready(g, network, owner, now) && p != NULL && n == 3U;
	uint32_t ticks = ok ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) : 0U;

	ok = ok && ticks == g->timeout_ms * 64U && ticks > 0U && ticks <= 2000U * 64U;
	g->stage = 0U;
	g->mt.stage = 0U;
	if (ok) {
		g->in_flight = true;
	}
	return ok;
}

static inline bool dg_complete(struct sx126x_dual_gate *g, uint8_t network, bool success)
{
	if (network != g->network) {
		return false;
	}
	if (success && network == 0U && g->in_flight) {
		g->mc_done = true;
	}
	g->in_flight = false;
	g->stage = 0U;
	g->mt.stage = 0U;
	return true;
}
#endif /* continuous versus finite */
#endif
