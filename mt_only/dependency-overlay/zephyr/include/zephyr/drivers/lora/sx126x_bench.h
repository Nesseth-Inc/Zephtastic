/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <zephyr/device.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Captured in RAM without console I/O. Sealed after startup by the main thread.
 * at_ms is device monotonic uptime, not a host reception timestamp. */
struct sx126x_bench_record { uint32_t at_ms; const char *stage; int result; uint32_t value; };
size_t sx126x_bench_trace(const struct device *,struct sx126x_bench_record *,size_t);
/* Finite profile only: caller and exact frame bound, one arm/boot,max3 starts.
 * Return negative errno on fault; activity>=1 means defer. No MC/CW permit. */
int sx126x_bench_arm(const struct device *,uint8_t relay,uint32_t deadline);
int sx126x_bench_grant(const struct device *,const uint8_t *,size_t);
void sx126x_bench_revoke(void);
void sx126x_bench_finish_grant(void);
int sx126x_bench_abort(const struct device *);
int sx126x_bench_health(const struct device *);
int sx126x_bench_activity(const struct device *);
uint32_t sx126x_bench_rx_releases(const struct device *);
int sx126x_bench_fault(const struct device *,int result);
bool sx126x_bench_tx_ready(const struct device *);
bool sx126x_bench_tx_consume(const struct device *,const uint8_t *,size_t);
uint32_t sx126x_bench_tx_timeout(void);
/* Dual finite profile: exact tuples, one arm per boot and common TX ownership. */
int sx126x_bench_arm_dual(uint8_t relay, const uint8_t identity[3], uint32_t deadline);
/* Continuous owner only: bit0 MC, bit1 MT; no outstanding frame permit. */
int sx126x_bench_select_networks(uint8_t mask);
int sx126x_bench_complete(const struct device *dev, bool success);
/* Continuous profile read-only limiter; never grants a transmit. */
int sx126x_continuous_budget(const struct device *dev, uint32_t air_ms);
uint32_t sx126x_continuous_used(uint8_t network);
#ifdef __cplusplus
}
#endif
