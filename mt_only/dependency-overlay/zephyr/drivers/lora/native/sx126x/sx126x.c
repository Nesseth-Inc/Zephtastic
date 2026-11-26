/*
 * Copyright (c) 2026 Carlo Caione <ccaione@baylibre.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <errno.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/byteorder.h>

#include "sx126x.h"
#include <zephyr/drivers/lora/sx126x_ext.h>
#include <zephyr/drivers/lora/sx126x_bench_gate.h>
#include <zephyr/drivers/lora/sx126x_dual_gate.h>
#include <zephyr/drivers/lora/zc_lora_timing.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sx126x, CONFIG_LORA_LOG_LEVEL);

/* Dedicated DIO1 work queue -- keeps LoRa interrupt processing off the
 * system work queue so USB/BLE/timer work items cannot delay packet RX. */
#define SX126X_DIO1_WQ_STACK_SIZE 2560
/* Each sx126x_data owns a distinct kernel worker stack. */

/* PA ramp time handed to SetTxParams at every site that programs TX power.
 *
 * Deliberately 800 us where RadioLib, loramac-node and Arduino MeshCore all
 * use 200 us -- do not "fix" this back to match them without reading the
 * following, and do not change one call site alone: two writers disagreeing
 * on the ramp would make TX spectral behaviour depend on which ran last.
 *
 * What it buys: a slower PA envelope puts less spectral energy at the burst
 * edges, which is mask margin (ETSI EN 300 220 / FCC) at TX start and stop.
 * That is worth most on the boards carrying an external PA or FEM, where
 * splatter gets amplified along with the wanted signal.
 *
 * What it does NOT buy, despite being an easy assumption: any link-budget or
 * noise-immunity benefit.  Ramp time shapes our own transmitter's skirts.  It
 * does not raise TX power, change what a distant receiver hears in-band, or
 * help its demodulator.  The only way it helps anyone's reception is by not
 * desensitising a CO-SITED or adjacent-channel radio -- real on a hilltop
 * with several co-located nodes, negligible between nodes kilometres apart.
 *
 * What it costs: 600 us more TX startup than the 200 us default, and nothing
 * else.  The ramp completes before modulation begins -- the chip holds BUSY
 * after SetTx until ramp-up is done, which is why RadioLib waits on exactly
 * that edge -- so it consumes no preamble symbols at any SF/BW and does not
 * eat into the RX duty-cycle preamble budget.  Airtime is unchanged; both
 * reference stacks likewise exclude ramp from their time-on-air maths.  The
 * one behavioural side effect is that the gap between "LBT reports the
 * channel clear" and "we are actually radiating" grows by those 600 us,
 * which is negligible against a contention window measured in milliseconds.
 */
#define SX126X_TX_RAMP_TIME SX126X_RAMP_800_US

#define SX126X_REST_STATE \
	(IS_ENABLED(CONFIG_LORA_SX126X_NATIVE_SLEEP) \
	 ? SX126X_STATE_SLEEP : SX126X_STATE_IDLE)

#define SX126X_SF56_MIN_PREAMBLE_LEN	12

/* Startup observation must not introduce USB console waits. Each device owns
 * its finite record array; the single startup/main thread writes and seals it. */
static void bench_record(const struct device *dev,const char *stage,int result,uint32_t value)
{
#if defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || defined(CONFIG_MT_ONLY)
 struct sx126x_data *data=dev->data;
 if (!data->bench_trace_sealed && data->bench_trace_count<48) {
  data->bench_trace[data->bench_trace_count++]=(struct sx126x_bench_record){k_uptime_get_32(),stage,result,value};
 }
#else
 ARG_UNUSED(dev);ARG_UNUSED(stage);ARG_UNUSED(result);ARG_UNUSED(value);
#endif
}
size_t sx126x_bench_trace(const struct device *dev,struct sx126x_bench_record *out,size_t cap)
{
#if defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || defined(CONFIG_MT_ONLY)
 struct sx126x_data *data=dev->data;
 data->bench_trace_sealed=true;
 size_t n=MIN(cap,data->bench_trace_count);
 memcpy(out,data->bench_trace,n*sizeof(*out));return n;
#else
 ARG_UNUSED(dev);ARG_UNUSED(out);ARG_UNUSED(cap);return 0;
#endif
}

#ifdef CONFIG_ZEPHCORE_DUAL_RX_BENCH
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
static struct sx126x_dual_gate dual_gate;
#else
static struct sx126x_bench_gate bench_gate;
#endif
static struct k_spinlock bench_gate_lock;
static bool bench_mt(const struct device *dev) { return dev==DEVICE_DT_GET(DT_ALIAS(lora1)); }
static bool bench_pair_ok(void) {
 const struct device *a=DEVICE_DT_GET(DT_ALIAS(lora0)),*b=DEVICE_DT_GET(DT_ALIAS(lora1));
 return device_is_ready(a) && device_is_ready(b) &&
  !atomic_get(&((struct sx126x_data *)a->data)->bench_faults) &&
  !atomic_get(&((struct sx126x_data *)b->data)->bench_faults);
}
#endif
void sx126x_bench_revoke(void) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	dg_revoke(&dual_gate);
	k_spin_unlock(&bench_gate_lock, key);
#elif defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH)
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);bg_revoke(&bench_gate);k_spin_unlock(&bench_gate_lock,key);
#endif
}
void sx126x_bench_finish_grant(void) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	dual_gate.stage = 0U;
	dual_gate.mt.stage = 0U;
	k_spin_unlock(&bench_gate_lock, key);
#elif defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH)
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);bench_gate.stage=0;bench_gate.len=0;k_spin_unlock(&bench_gate_lock,key);
#endif
}
int sx126x_bench_fault(const struct device *dev,int result) {
#if defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || defined(CONFIG_MT_ONLY)
 if(result<0) { atomic_inc(&((struct sx126x_data *)dev->data)->bench_faults);sx126x_bench_revoke(); }
#else
 ARG_UNUSED(dev);
#endif
 return result;
}
int sx126x_bench_health(const struct device *dev) {
#if defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || defined(CONFIG_MT_ONLY)
 if(!device_is_ready(dev)) return -ENODEV;
 struct sx126x_data *data=dev->data;
 if(atomic_get(&data->bench_faults)) return -EIO;
 return data->config_valid && atomic_get(&data->state)==SX126X_STATE_RX ? 1 : 0;
#else
 ARG_UNUSED(dev);return -ENOTSUP;
#endif
}
int sx126x_bench_arm(const struct device *dev,uint8_t relay,uint32_t deadline) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
	ARG_UNUSED(dev);
	ARG_UNUSED(relay);
	ARG_UNUSED(deadline);
	return -EPERM;
#elif defined(CONFIG_ZEPHCORE_FINITE_MT_TX_BENCH)
 if(!bench_mt(dev) || !bench_pair_ok() || sx126x_bench_health(dev)!=1) return -EPERM;
 struct sx126x_data *data=dev->data;const struct lora_modem_config *c=&data->config;
 if(c->frequency!=906875000 || c->bandwidth!=BW_250_KHZ || c->datarate!=SF_11 || c->coding_rate!=CR_4_5 ||
    c->preamble_len!=16 || c->sync_word!=0x2b || c->tx_power!=2 || c->iq_inverted || c->packet_crc_disable ||
    c->cad.mode!=LORA_CAD_MODE_LBT || c->cad.symbol_num!=LORA_CAD_SYMB_4) return -EPERM;
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool ok=bg_arm(&bench_gate,relay,(uintptr_t)k_current_get(),k_uptime_get_32(),deadline);
 k_spin_unlock(&bench_gate_lock,key);return ok ? 0 : -EPERM;
#else
 ARG_UNUSED(dev);ARG_UNUSED(relay);ARG_UNUSED(deadline);return -EPERM;
#endif
}
static uint32_t sx126x_lora_airtime(const struct device *,uint32_t);
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
static bool continuous_modems_allowed(void) {
 for(uint8_t i=0;i<2;++i) {
  const struct device *dev=i==0 ? DEVICE_DT_GET(DT_ALIAS(lora0)):DEVICE_DT_GET(DT_ALIAS(lora1));
  const struct lora_modem_config *c=&((struct sx126x_data *)dev->data)->config;
  if(c->frequency!=(i==0 ? 910525000U:906875000U) || c->bandwidth!=(i==0 ? BW_62_KHZ:BW_250_KHZ) ||
     c->datarate!=(i==0 ? SF_7:SF_11) || c->coding_rate!=CR_4_5 || c->preamble_len!=(i==0 ? 32U:16U) ||
#ifdef CONFIG_ZEPHCORE_DUAL_MANAGEMENT
     (c->tx_power < -9 || c->tx_power > 22) ||
#else
     c->tx_power!=2 ||
#endif
     (i==0 ? (c->sync_word!=0x12U && (c->sync_word!=0U || c->public_network)):c->sync_word!=0x2bU) ||
     c->iq_inverted || c->packet_crc_disable || c->cad.mode!=LORA_CAD_MODE_LBT || c->cad.symbol_num!=LORA_CAD_SYMB_4)return false;
 }
 return true;
}
#endif

int sx126x_bench_grant(const struct device *dev,const uint8_t *p,size_t n) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 if(!continuous_modems_allowed())return -EPERM;
#endif
	if ((dev != DEVICE_DT_GET(DT_ALIAS(lora0)) && !bench_mt(dev)) || !bench_pair_ok()) {
		return -EPERM;
	}
	uint8_t network = bench_mt(dev) ? 1U : 0U;
	uint32_t air = sx126x_lora_airtime(dev, n);
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	bool ok = dg_issue(&dual_gate, network, p, n, (uintptr_t)k_current_get(),
			   k_uptime_get_32(), air);
	k_spin_unlock(&bench_gate_lock, key);
	return ok ? 0 : -EPERM;
#elif defined(CONFIG_ZEPHCORE_FINITE_MT_TX_BENCH)
 if(!bench_mt(dev) || !bench_pair_ok()) return -EPERM;
 uint32_t air=sx126x_lora_airtime(dev,n);
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool ok=bg_issue(&bench_gate,p,n,(uintptr_t)k_current_get(),k_uptime_get_32(),air);
 k_spin_unlock(&bench_gate_lock,key);return ok ? 0 : -EPERM;
#else
 ARG_UNUSED(dev);ARG_UNUSED(p);ARG_UNUSED(n);return -EPERM;
#endif
}
static bool bench_admit(const struct device *dev,const uint8_t *p,size_t n) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 if(!continuous_modems_allowed())return false;
#endif
	if ((dev != DEVICE_DT_GET(DT_ALIAS(lora0)) && !bench_mt(dev)) || !bench_pair_ok()) {
		return false;
	}
	uint8_t network = bench_mt(dev) ? 1U : 0U;
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	bool ok = dg_admit(&dual_gate, network, p, n, (uintptr_t)k_current_get(),
			   k_uptime_get_32());
	k_spin_unlock(&bench_gate_lock, key);
	return ok;
#elif defined(CONFIG_ZEPHCORE_FINITE_MT_TX_BENCH)
 if(!bench_mt(dev) || !bench_pair_ok()) return false;
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool ok=bg_admit(&bench_gate,p,n,(uintptr_t)k_current_get(),k_uptime_get_32());
 k_spin_unlock(&bench_gate_lock,key);return ok;
#else
 ARG_UNUSED(dev);ARG_UNUSED(p);ARG_UNUSED(n);return false;
#endif
}
bool sx126x_bench_tx_ready(const struct device *dev) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 if(!continuous_modems_allowed())return false;
#endif
	if ((dev != DEVICE_DT_GET(DT_ALIAS(lora0)) && !bench_mt(dev)) || !bench_pair_ok()) {
		return false;
	}
	uint8_t network = bench_mt(dev) ? 1U : 0U;
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	bool ok = dg_ready(&dual_gate, network, (uintptr_t)k_current_get(),
			   k_uptime_get_32());
	k_spin_unlock(&bench_gate_lock, key);
	return ok;
#elif defined(CONFIG_ZEPHCORE_FINITE_MT_TX_BENCH)
 if(!bench_mt(dev) || !bench_pair_ok()) return false;
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool ok=bg_ready(&bench_gate,(uintptr_t)k_current_get(),k_uptime_get_32());
 k_spin_unlock(&bench_gate_lock,key);return ok;
#else
 ARG_UNUSED(dev);return false;
#endif
}
bool sx126x_bench_tx_consume(const struct device *dev,const uint8_t *p,size_t n) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 if(!continuous_modems_allowed())return false;
#endif
	if ((dev != DEVICE_DT_GET(DT_ALIAS(lora0)) && !bench_mt(dev)) || !bench_pair_ok()) {
		return false;
	}
	uint8_t network = bench_mt(dev) ? 1U : 0U;
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	bool ok = dg_consume(&dual_gate, network, (uintptr_t)k_current_get(),
			     k_uptime_get_32(), p, n);
	k_spin_unlock(&bench_gate_lock, key);
	return ok;
#elif defined(CONFIG_ZEPHCORE_FINITE_MT_TX_BENCH)
 if(!bench_mt(dev) || !bench_pair_ok()) return false;
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool ok=bg_consume(&bench_gate,(uintptr_t)k_current_get(),k_uptime_get_32(),p,n);
 k_spin_unlock(&bench_gate_lock,key);return ok;
#else
 ARG_UNUSED(dev);ARG_UNUSED(p);ARG_UNUSED(n);return false;
#endif
}
uint32_t sx126x_bench_tx_timeout(void) {
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	uint32_t timeout = dual_gate.timeout_ms;
	k_spin_unlock(&bench_gate_lock, key);
	return timeout;
#elif defined(CONFIG_ZEPHCORE_FINITE_MT_TX_BENCH)
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);uint32_t n=bench_gate.timeout_ms;k_spin_unlock(&bench_gate_lock,key);return n;
#else
 return 0;
#endif
}

int sx126x_bench_arm_dual(uint8_t relay, const uint8_t identity[3], uint32_t deadline)
{
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
	if (!bench_pair_ok()) {
		return -EPERM;
	}
	for (uint8_t i = 0U; i < 2U; ++i) {
		const struct device *dev = i == 0U ? DEVICE_DT_GET(DT_ALIAS(lora0)) :
			DEVICE_DT_GET(DT_ALIAS(lora1));
		const struct lora_modem_config *c = &((struct sx126x_data *)dev->data)->config;

		if (sx126x_bench_health(dev) != 1 || c->frequency != (i == 0U ? 910525000U :
		    906875000U) || c->bandwidth != (i == 0U ? BW_62_KHZ : BW_250_KHZ) ||
		    c->datarate != (i == 0U ? SF_7 : SF_11) || c->coding_rate != CR_4_5 ||
		    c->preamble_len != (i == 0U ? 32U : 16U) ||
#ifdef CONFIG_ZEPHCORE_DUAL_MANAGEMENT
            (c->tx_power < -9 || c->tx_power > 22) ||
#else
            c->tx_power != 2 ||
#endif
		    (i == 0U ? (c->sync_word != 0x12U && (c->sync_word != 0U ||
		    c->public_network)) : c->sync_word != 0x2bU) || c->iq_inverted ||
		    c->packet_crc_disable || c->cad.mode != LORA_CAD_MODE_LBT ||
		    c->cad.symbol_num != LORA_CAD_SYMB_4) {
			return -EPERM;
		}
	}
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	bool ok = dg_arm(&dual_gate, relay, identity, (uintptr_t)k_current_get(),
			 k_uptime_get_32(), deadline);

	k_spin_unlock(&bench_gate_lock, key);
	return ok ? 0 : -EPERM;
#else
	ARG_UNUSED(relay);
	ARG_UNUSED(identity);
	ARG_UNUSED(deadline);
	return -EPERM;
#endif
}

int sx126x_bench_select_networks(uint8_t mask)
{
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool ok=(mask==1U || mask==3U) && dual_gate.active && !dual_gate.in_flight &&
  !dual_gate.admitted && dual_gate.stage==0U && dual_gate.owner==(uintptr_t)k_current_get();
 if(ok)dual_gate.network_mask=mask;
 k_spin_unlock(&bench_gate_lock,key);return ok ? 0:-EPERM;
#else
 ARG_UNUSED(mask);return -EPERM;
#endif
}

int sx126x_bench_complete(const struct device *dev, bool success)
{
#ifdef CONFIG_ZEPHCORE_FINITE_DUAL_TX_BENCH
	if (dev != DEVICE_DT_GET(DT_ALIAS(lora0)) && !bench_mt(dev)) {
		return -EPERM;
	}
	k_spinlock_key_t key = k_spin_lock(&bench_gate_lock);
	bool ok = dual_gate.owner == (uintptr_t)k_current_get() &&
		dg_complete(&dual_gate, bench_mt(dev) ? 1U : 0U, success);

	k_spin_unlock(&bench_gate_lock, key);
	return ok ? 0 : -EPERM;
#else
	ARG_UNUSED(dev);
	ARG_UNUSED(success);
	return -EPERM;
#endif
}

int sx126x_continuous_budget(const struct device *dev, uint32_t air_ms)
{
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 if ((dev != DEVICE_DT_GET(DT_ALIAS(lora0)) && !bench_mt(dev)) || !bench_pair_ok() ||
     !air_ms || air_ms > CD_MAX_AIR_MS || !continuous_modems_allowed()) return -EPERM;
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 bool permitted=dual_gate.active && dual_gate.owner==(uintptr_t)k_current_get();
 bool available=permitted && cd_allows(&dual_gate.ledger,bench_mt(dev) ? 1U : 0U,
                                     k_uptime_get_32(),air_ms+251U);
 k_spin_unlock(&bench_gate_lock,key);
 return !permitted ? -EPERM : available ? 1 : 0;
#else
 ARG_UNUSED(dev);ARG_UNUSED(air_ms);return -ENOTSUP;
#endif
}
uint32_t sx126x_continuous_used(uint8_t network)
{
#ifdef CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER
 k_spinlock_key_t key=k_spin_lock(&bench_gate_lock);
 cd_expire(&dual_gate.ledger,k_uptime_get_32());
 uint32_t used=network<2U ? dual_gate.ledger.used[network] :
               dual_gate.ledger.used[0]+dual_gate.ledger.used[1];
 k_spin_unlock(&bench_gate_lock,key);return used;
#else
 ARG_UNUSED(network);return 0U;
#endif
}

static int bandwidth_to_reg(enum lora_signal_bandwidth bw, uint8_t *reg)
{
	switch (bw) {
	case BW_7_KHZ:
		*reg = SX126X_LORA_BW_7_8;
		return 0;
	case BW_10_KHZ:
		*reg = SX126X_LORA_BW_10_4;
		return 0;
	case BW_15_KHZ:
		*reg = SX126X_LORA_BW_15_6;
		return 0;
	case BW_20_KHZ:
		*reg = SX126X_LORA_BW_20_8;
		return 0;
	case BW_31_KHZ:
		*reg = SX126X_LORA_BW_31_25;
		return 0;
	case BW_41_KHZ:
		*reg = SX126X_LORA_BW_41_7;
		return 0;
	case BW_62_KHZ:
		*reg = SX126X_LORA_BW_62_5;
		return 0;
	case BW_125_KHZ:
		*reg = SX126X_LORA_BW_125;
		return 0;
	case BW_250_KHZ:
		*reg = SX126X_LORA_BW_250;
		return 0;
	case BW_500_KHZ:
		*reg = SX126X_LORA_BW_500;
		return 0;
	default:
		return -EINVAL;
	}
}

static int bandwidth_to_hz(enum lora_signal_bandwidth bw, uint32_t *bw_hz)
{
	switch (bw) {
	case BW_7_KHZ:
		*bw_hz = 7810;
		return 0;
	case BW_10_KHZ:
		*bw_hz = 10420;
		return 0;
	case BW_15_KHZ:
		*bw_hz = 15630;
		return 0;
	case BW_20_KHZ:
		*bw_hz = 20830;
		return 0;
	case BW_31_KHZ:
		*bw_hz = 31250;
		return 0;
	case BW_41_KHZ:
		*bw_hz = 41670;
		return 0;
	case BW_62_KHZ:
		*bw_hz = 62500;
		return 0;
	case BW_125_KHZ:
		*bw_hz = 125000;
		return 0;
	case BW_250_KHZ:
		*bw_hz = 250000;
		return 0;
	case BW_500_KHZ:
		*bw_hz = 500000;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Reset all software state that indicates "we are currently receiving":
 * the rx_packet_active latch (and its deadline timestamp), the
 * preamble-grace timestamp and the raw-HEADER_VALID deadline.  Paired write
 * so the fields never drift out of sync.  Called from every RX (re)start
 * site, every terminal-event handler (RX_DONE / CRC_ERR / RX_TX_TIMEOUT),
 * and on TX-state entry. */
/* cad_rx_state values -- see the field comment in sx126x.h. */
#define SX126X_CAD_RX_IDLE   0
#define SX126X_CAD_RX_ARMED  1
#define SX126X_CAD_RX_PACKET 2
#define SX126X_CAD_RX_TMOUT  3

/* Resolve an in-flight CAD_RX with the terminal event that just arrived.
 * A no-op unless a CAD_RX is actually armed, so the packet path pays one
 * atomic compare for it.  Returns true when this call is the one that
 * resolved it, which is how sx126x_handle_irq_timeout() tells the probe's own
 * cadTimeout apart from a duty-cycle timeout raising the identical IRQ. */
static inline bool sx126x_cad_rx_resolve(struct sx126x_data *data, int outcome)
{
	return atomic_cas(&data->cad_rx_state, SX126X_CAD_RX_ARMED, outcome);
}

#ifdef CONFIG_MT_NORMAL_ENDPOINT
enum { LBT_IDLE, LBT_SCAN, LBT_RX };

#endif

static inline void sx126x_reset_rx_busy_signals(struct sx126x_data *data)
{
#ifdef CONFIG_MT_NORMAL_ENDPOINT
	atomic_set(&data->lbt_handoff, LBT_IDLE);
#endif
	data->rx_packet_active = false;
	atomic_set(&data->preamble_seen_at_ms, 0);
	atomic_set(&data->header_seen_at_ms, 0);
	atomic_set(&data->raw_header_seen_at_ms, 0);
}

/* Grace period for the PREAMBLE_DETECTED -> HEADER_VALID gap, SF/BW-aware.
 * (preamble_len + 8) symbols covers worst-case (early-detect at ~4 syms)
 * preamble remainder + sync word (4.25 sym) + header decode (~5 sym) plus
 * a safety margin.  Used by sx126x_is_receiving() as the timeout after
 * which a sticky PREAMBLE_DETECTED bit is assumed to be a foreign sync
 * word and explicitly cleared. */
static uint32_t sx126x_preamble_grace_ms(struct sx126x_data *data)
{
	uint32_t bw_hz;

	if (bandwidth_to_hz(data->config.bandwidth, &bw_hz) < 0) {
		bw_hz = 0;  /* unconfigured: the helper's safe default */
	}
	return zc_lora_preamble_grace_ms((uint8_t)data->config.datarate, bw_hz,
					 data->config.preamble_len);
}

/* Upper bound on the payload phase: airtime of a maximum-length (255 byte)
 * explicit-header packet at the current SF/BW, worst-case coding rate 4/8,
 * plus margin.  Used by sx126x_is_receiving() to bound the lifetime of the
 * rx_packet_active latch.
 *
 * Why the latch needs a deadline at all: it is cleared only by a terminal IRQ
 * (RX_DONE / CRC_ERR / RX_TX_TIMEOUT) or an RX (re)start.  In continuous RX
 * (SX126X_RX_TIMEOUT_CONTINUOUS) there is no symbol timer, so none of those is
 * guaranteed to arrive -- a HEADER_VALID that never completes would pin the TX
 * gate true forever and silently mute the node until reboot.  (The parked-RX
 * watchdog cannot cover this: it is duty-cycle only and deliberately treats
 * rx_packet_active as a legitimate in-flight packet.)  Mirrors the payload
 * deadline Arduino MeshCore added in 79ef74ea / 0bd871cd.
 *
 * Deliberately generous: this is a stuck-state safety net, and clearing the
 * latch early would let TX start on top of a packet that is still arriving.
 * The low-data-rate-optimisation term is pinned at 1 (rather than read from
 * the modem config) because DE=1 yields the larger symbol count of the two,
 * i.e. the safer bound. */
static uint32_t sx126x_max_payload_ms(struct sx126x_data *data)
{
	uint32_t bw_hz;

	if (bandwidth_to_hz(data->config.bandwidth, &bw_hz) < 0) {
		bw_hz = 0;  /* unconfigured: the helper's safe default */
	}
	return zc_lora_max_payload_ms((uint8_t)data->config.datarate, bw_hz);
}

static bool should_enable_ldro(enum lora_datarate sf, enum lora_signal_bandwidth bw,
			       const struct sx126x_hal_config *config)
{
	uint32_t bw_hz;
	int ret;

	if (config->force_ldro) {
		return true;
	}

	ret = bandwidth_to_hz(bw, &bw_hz);
	__ASSERT_NO_MSG(ret == 0);
	/* Symbol time = 2^SF / BW (in seconds) */
	/* 16.38 ms = 16380 us */
	/* 2^SF / BW > 0.01638 => 2^SF * 1000000 / BW > 16380 */
	uint32_t symbol_time_us = ((1 << sf) * 1000000UL) / bw_hz;

	return symbol_time_us > 16380;
}

static int sx126x_validate_config(const struct lora_modem_config *config)
{
	uint8_t bw_reg;

	if (bandwidth_to_reg(config->bandwidth, &bw_reg) < 0) {
		LOG_ERR("Unsupported bandwidth: %d kHz", config->bandwidth);
		return -EINVAL;
	}

	return 0;
}

/* Optional metadata-only observer. Its queue is multi-producer-safe and never
 * calls an application callback or takes an application lock. No modem I/O.
 * A cached epoch changes only after a successful configuration with a consumed
 * proof from the joined, empty-IRQ storage-pause path. Generic unproved changes
 * remain unsupported. Already-computed queued observations/loss are retained. */
void sx126x_airtime_observer_reconfigure_invalidate(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	if (!atomic_get(&data->observation_enabled)) return;
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	atomic_set(&data->observation_reconfigure_permit, 0);
	/* RF/IRQ before the first commit also invalidates its cold-start proof. */
	if (atomic_get(&data->observation_config_state) == 0)
		atomic_set(&data->observation_config_state, 2);
	k_spin_unlock(&data->observation_lock, key);
}

static void sx126x_observe_reconfigure_reserve(struct sx126x_data *data)
{
	if (!atomic_get(&data->observation_enabled)) return;
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	atomic_set(&data->observation_reconfigure_permit,
		atomic_get(&data->observation_reconfigure_permit) == 1 ? 2 : 0);
	k_spin_unlock(&data->observation_lock, key);
}

static void sx126x_observe_reconfigure_quiescent(struct sx126x_data *data)
{
	if (!atomic_get(&data->observation_enabled)) return;
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	atomic_set(&data->observation_reconfigure_permit, 1);
	k_spin_unlock(&data->observation_lock, key);
}

static void sx126x_observe_loss(struct sx126x_data *data)
{
	if (atomic_get(&data->observation_enabled)) atomic_set(&data->observation_loss, 1);
}

static void sx126x_observe_rx(struct sx126x_data *data, uint16_t irq,
                             uint8_t length, bool known)
{
	if (!atomic_get(&data->observation_enabled)) return;
	int64_t now = k_uptime_get();
	struct sx126x_airtime_observation event = {
		.observed_at_ms = now < 0 ? UINT64_MAX : (uint64_t)now,
		.irq = irq, .length = length, .known = false,
	};
	struct sx126x_airtime_input captured = {0};
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	atomic_set(&data->observation_reconfigure_permit, 0);
	bool supported = atomic_get(&data->observation_config_state) == 1;
	if (supported) captured = data->observation_config;
	k_spin_unlock(&data->observation_lock, key);
	if (known && length && supported) {
		struct sx126x_airtime_value value =
			sx126x_airtime_estimate(&captured, length);
		event.airtime_ms = value.ms;
		event.known = value.trustworthy && now >= 0;
	}
	if (k_msgq_put(&data->observation_queue, &event, K_NO_WAIT) != 0)
		atomic_set(&data->observation_loss, 1);
}

static void sx126x_observe_config(const struct device *dev,
                                  const struct lora_modem_config *config,
                                  bool success, bool commit)
{
	struct sx126x_data *data = dev->data;
	if (!atomic_get(&data->observation_enabled)) return;
	uint32_t bw = 0;
	if (!success || bandwidth_to_hz(config->bandwidth, &bw) < 0) {
		k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
		atomic_set(&data->observation_config_state, 2);
		atomic_set(&data->observation_reconfigure_permit, 0);
		k_spin_unlock(&data->observation_lock, key);
		return;
	}
	struct sx126x_airtime_input current = {
		.bandwidth_hz = bw, .preamble_len =
			(config->datarate <= SF_6 && config->preamble_len < 12) ? 12 : config->preamble_len,
		.sf = config->datarate, .cr = config->coding_rate,
		.ldro = should_enable_ldro(config->datarate, config->bandwidth, dev->config),
		.crc = !config->packet_crc_disable,
	};
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	uint32_t state = (uint32_t)atomic_get(&data->observation_config_state);
	if (commit && (state == 0 ||
	    atomic_get(&data->observation_reconfigure_permit) == 2)) {
		data->observation_config = current;
		data->observation_frequency = config->frequency;
		data->observation_sync_word = config->sync_word ? config->sync_word : (config->public_network ? 0x34 : 0x12);
		data->observation_iq = config->iq_inverted;
		atomic_set(&data->observation_config_state, 1);
	} else if (state == 1) {
		const struct sx126x_airtime_input *old = &data->observation_config;
		if (old->bandwidth_hz != current.bandwidth_hz || old->preamble_len != current.preamble_len ||
		    old->sf != current.sf || old->cr != current.cr || old->ldro != current.ldro ||
		    old->crc != current.crc || data->observation_frequency != config->frequency ||
		    data->observation_sync_word != (config->sync_word ? config->sync_word : (config->public_network ? 0x34 : 0x12)) ||
		    data->observation_iq != config->iq_inverted)
			atomic_set(&data->observation_config_state, 2);
	}
	if (commit) atomic_set(&data->observation_reconfigure_permit, 0);
	k_spin_unlock(&data->observation_lock, key);
}

int sx126x_airtime_observer_start(const struct device *dev)
{
	if (!device_is_ready(dev)) return -ENODEV;
	struct sx126x_data *data = dev->data;
	k_mutex_lock(&data->lock, K_FOREVER);
	int ret = 0;
	if (atomic_get(&data->observation_enabled)) ret = -EALREADY;
	else if (atomic_get(&data->state) != SX126X_REST_STATE || data->config_valid ||
	         data->rx_cb || k_work_busy_get(&data->irq_work)) ret = -EBUSY;
	else {
		k_msgq_init(&data->observation_queue, (char *)data->observation_buffer,
		            sizeof(struct sx126x_airtime_observation), 16);
		atomic_set(&data->observation_enabled, 1);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

bool sx126x_airtime_observer_supported(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	return atomic_get(&data->observation_enabled) &&
	       atomic_get(&data->observation_config_state) == 1;
}

/* RAM-only TX facts. Never reset by observer enable/config, storage pause or
 * radio stop. Accepted commands/IRQ bits are not delivery acknowledgements. */
static void sx126x_observe_tx_started(struct sx126x_data *data)
{
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	if (data->observation_tx_started != UINT32_MAX) ++data->observation_tx_started;
	k_spin_unlock(&data->observation_lock, key);
}

static void sx126x_observe_tx_irq(struct sx126x_data *data, uint16_t irq)
{
	/* The IRQ snapshot belongs to TX only before handlers clear that state. */
	if (atomic_get(&data->state) != SX126X_STATE_TX) return;
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	if ((irq & SX126X_IRQ_TX_DONE) && data->observation_tx_done != UINT32_MAX)
		++data->observation_tx_done;
	if ((irq & SX126X_IRQ_RX_TX_TIMEOUT) && data->observation_tx_timeouts != UINT32_MAX)
		++data->observation_tx_timeouts;
	data->observation_last_tx_irq = irq;
	k_spin_unlock(&data->observation_lock, key);
}

/* Metadata lock only. Never held across modem I/O, waits or logging. */
static uint32_t sx126x_stage_begin(struct sx126x_data *data, struct sx126x_stage_info *slot)
{
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	if (slot->calls != UINT32_MAX) ++slot->calls;
	uint32_t call = slot->calls;
	k_spin_unlock(&data->observation_lock, key);
	return call;
}
static void sx126x_stage_fail(struct sx126x_data *data, struct sx126x_stage_info *slot,
			      uint32_t call, uint32_t stage, int result, bool *recorded)
{
	if (*recorded || result >= 0) return;
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	slot->failed_call = call; slot->stage = stage; slot->result = result;
	k_spin_unlock(&data->observation_lock, key);
	*recorded = true;
}

int sx126x_airtime_observer_info(const struct device *dev,
			       struct sx126x_airtime_observer_info *out)
{
	if (!out) return -EINVAL;
	*out = (struct sx126x_airtime_observer_info){0};
	if (!dev || !dev->data) return -EINVAL;
	struct sx126x_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->observation_lock);
	struct sx126x_airtime_observer_info value = {
		.enabled = atomic_get(&data->observation_enabled) ? 1U : 0U,
		.config_state = (uint32_t)atomic_get(&data->observation_config_state),
		.tx_started = data->observation_tx_started,
		.tx_done = data->observation_tx_done,
		.tx_timeouts = data->observation_tx_timeouts,
		.last_tx_irq = data->observation_last_tx_irq,
		.send_stage = data->send_stage, .config_stage = data->config_stage,
		.restart_stage = data->restart_stage,
	};
	if (value.config_state == 1U) {
		/* The metadata lock covers the complete committed tuple, including
		 * publication of a subsequent proved epoch. No modem operation. */
		value.sf = data->observation_config.sf;
		value.cr = data->observation_config.cr;
		value.bandwidth_hz = data->observation_config.bandwidth_hz;
		value.preamble_len = data->observation_config.preamble_len;
		value.ldro = data->observation_config.ldro ? 1U : 0U;
		value.crc = data->observation_config.crc ? 1U : 0U;
		uint32_t final_state = (uint32_t)atomic_get(&data->observation_config_state);
		if (final_state != 1U) value = (struct sx126x_airtime_observer_info){
			.enabled = value.enabled, .config_state = final_state,
			.tx_started = value.tx_started, .tx_done = value.tx_done,
			.tx_timeouts = value.tx_timeouts, .last_tx_irq = value.last_tx_irq,
			.send_stage = value.send_stage, .config_stage = value.config_stage,
			.restart_stage = value.restart_stage,
		};
	}
	k_spin_unlock(&data->observation_lock, key);
	*out = value;
	return 0;
}

int sx126x_airtime_observer_take(const struct device *dev, struct sx126x_airtime_observation *event)
{
	struct sx126x_data *data = dev->data;
	if (!event || !atomic_get(&data->observation_enabled)) return -EINVAL;
	return k_msgq_get(&data->observation_queue, event, K_NO_WAIT);
}

bool sx126x_airtime_observer_take_loss(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	return atomic_set(&data->observation_loss, 0) != 0;
}

static int sx126x_set_standby(const struct device *dev, uint8_t mode)
{
	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_STANDBY, &mode, 1);
}

static int sx126x_set_regulator_mode(const struct device *dev, uint8_t mode)
{
	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_REGULATOR_MODE, &mode, 1);
}

static int sx126x_set_buffer_base_address(const struct device *dev,
					  uint8_t tx_base, uint8_t rx_base)
{
	uint8_t buf[2] = { tx_base, rx_base };

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_BUFFER_BASE_ADDRESS, buf, 2);
}

static int sx126x_set_packet_type(const struct device *dev, uint8_t type)
{
	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_PACKET_TYPE, &type, 1);
}

static int sx126x_set_dio_irq_params(const struct device *dev,
				     uint16_t irq_mask, uint16_t dio1_mask,
				     uint16_t dio2_mask, uint16_t dio3_mask)
{
	uint8_t buf[8];

	sys_put_be16(irq_mask, &buf[0]);
	sys_put_be16(dio1_mask, &buf[2]);
	sys_put_be16(dio2_mask, &buf[4]);
	sys_put_be16(dio3_mask, &buf[6]);

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_DIO_IRQ_PARAMS, buf, 8);
}

static int sx126x_clear_irq_status(const struct device *dev, uint16_t mask)
{
	uint8_t buf[2];

	sys_put_be16(mask, buf);
	return sx126x_hal_write_cmd(dev, SX126X_CMD_CLR_IRQ_STATUS, buf, 2);
}

static int sx126x_get_irq_status(const struct device *dev, uint16_t *status)
{
	uint8_t buf[2];
	int ret;

	ret = sx126x_hal_read_cmd(dev, SX126X_CMD_GET_IRQ_STATUS, buf, 2);
	if (ret == 0) {
		*status = ((uint16_t)buf[0] << 8) | buf[1];
	}

	return ret;
}

static int sx126x_set_dio2_as_rf_switch(const struct device *dev, bool enable)
{
	uint8_t val = enable;

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_DIO2_AS_RF_SWITCH, &val, 1);
}

static int sx126x_set_dio3_as_tcxo_ctrl(const struct device *dev,
					uint8_t voltage, uint32_t timeout_ms)
{
	/* Timeout in units of 15.625 us */
	uint32_t timeout = SX126X_MS_TO_TIMEOUT(timeout_ms);
	uint8_t buf[4];

	buf[0] = voltage;
	sys_put_be24(timeout, &buf[1]);

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_DIO3_AS_TCXO_CTRL, buf, 4);
}

static int sx126x_calibrate(const struct device *dev, uint8_t mask)
{
	return sx126x_hal_write_cmd(dev, SX126X_CMD_CALIBRATE, &mask, 1);
}

static int sx126x_calibrate_image(const struct device *dev, uint32_t freq)
{
	uint8_t buf[2];

	if (freq > 900000000) {
		buf[0] = 0xE1;
		buf[1] = 0xE9;
	} else if (freq > 850000000) {
		buf[0] = 0xD7;
		buf[1] = 0xDB;
	} else if (freq > 770000000) {
		buf[0] = 0xC1;
		buf[1] = 0xC5;
	} else if (freq > 460000000) {
		buf[0] = 0x75;
		buf[1] = 0x81;
	} else {
		buf[0] = 0x6B;
		buf[1] = 0x6F;
	}

	return sx126x_hal_write_cmd(dev, SX126X_CMD_CALIBRATE_IMAGE, buf, 2);
}

static int sx126x_set_rf_frequency(const struct device *dev, uint32_t freq)
{
	uint32_t freq_reg = SX126X_FREQ_TO_REG(freq);
	uint8_t buf[4];

	sys_put_be32(freq_reg, buf);

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_RF_FREQUENCY, buf, 4);
}

static int sx126x_set_modulation_params(const struct device *dev,
					uint8_t sf, uint8_t bw, uint8_t cr,
					bool ldro)
{
	uint8_t buf[4] = { sf, bw, cr, ldro };

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_MODULATION_PARAMS, buf, 4);
}

/*
 * Workaround — Modulation Quality with 500 kHz LoRa Bandwidth
 * (DS_SX1261-2_V1.2, chapter 15.1)
 *
 * Must be called before each packet transmission.
 */
static int sx126x_apply_tx_modulation_workaround(const struct device *dev,
						  enum lora_signal_bandwidth bw)
{
	uint8_t reg_val;
	int ret;

	ret = sx126x_hal_read_regs(dev, SX126X_REG_TX_MODULATION, &reg_val, 1);
	if (ret < 0) {
		return ret;
	}

	if (bw == BW_500_KHZ) {
		reg_val &= ~BIT(2);
	} else {
		reg_val |= BIT(2);
	}

	return sx126x_hal_write_regs(dev, SX126X_REG_TX_MODULATION, &reg_val, 1);
}

static int sx126x_set_packet_params(const struct device *dev,
				    uint16_t preamble_len, uint8_t header_type,
				    uint8_t payload_len, uint8_t crc_mode,
				    uint8_t invert_iq)
{
	uint8_t buf[6];
	uint8_t reg_val;
	int ret;

	sys_put_be16(preamble_len, &buf[0]);
	buf[2] = header_type;
	buf[3] = payload_len;
	buf[4] = crc_mode;
	buf[5] = invert_iq;

	ret = sx126x_hal_write_cmd(dev, SX126X_CMD_SET_PACKET_PARAMS, buf, 6);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Workaround — Optimizing the Inverted IQ Operation
	 * (DS_SX1261-2_V1.2, chapter 15.4)
	 */
	ret = sx126x_hal_read_regs(dev, SX126X_REG_IQ_POLARITY, &reg_val, 1);
	if (ret < 0) {
		return ret;
	}

	if (invert_iq == SX126X_LORA_IQ_INVERTED) {
		reg_val &= ~BIT(2);
	} else {
		reg_val |= BIT(2);
	}

	return sx126x_hal_write_regs(dev, SX126X_REG_IQ_POLARITY, &reg_val, 1);
}

static int sx126x_set_sync_word(const struct device *dev, const struct lora_modem_config *config)
{
	uint16_t sync_word;
	uint8_t buf[2];

	if (config->sync_word) {
		/* Matching behaviour for other LoRa backends */
		sync_word = 0x0404 | ((config->sync_word & 0xF0) << 8) |
			    ((config->sync_word & 0x0F) << 4);
	} else {
		sync_word = config->public_network ? SX126X_LORA_SYNC_WORD_PUBLIC
						   : SX126X_LORA_SYNC_WORD_PRIVATE;
	}

	sys_put_be16(sync_word, buf);
	return sx126x_hal_write_regs(dev, SX126X_REG_LORA_SYNC_WORD_MSB, buf, 2);
}

/* RSSI / AGC band calibration (DS Sec 6.1.6, Table 6-4).
 *
 * Semtech: "The power seen by the SX1261/2 analog front-end is affected by
 * external components such as the matching network and RF switches.  An
 * incorrect RSSI results in a sensitivity degradation in (G)FSK mode and an
 * incorrect gain selection in LoRa and GFSK mode.  An incorrect gain can
 * result in a missed detection (packet loss) or decreased resistance to
 * interference."  So this is an RX-performance register set, not telemetry
 * cosmetics.
 *
 * The chip ships calibrated for 868-915 MHz on the Semtech EVK, which is why
 * the high-band tunes are all zero -- writing them is a no-op against a fresh
 * chip and exists only so that a runtime band change (`set freq`) restores
 * them after a low-band build has installed the other set.
 *
 * IMPORTANT: the low-band column is the datasheet's 470-490 MHz EVK
 * calibration.  It is NOT a 433 MHz calibration -- Semtech publishes no such
 * column.  Applying it below 600 MHz is "the nearest published values for a
 * low band" and is expected to beat leaving the 868-915 calibration in place,
 * but a true 433 MHz figure needs the DS Sec 6.1.6 generator sweep on real
 * hardware.  Do not present these numbers as calibrated for 433.
 */
struct sx126x_agc_band_cal {
	uint8_t rssi_meas_cal_h;                          /* 0x089C, bits 4:0 */
	uint8_t rssi_meas_cal_l;                          /* 0x089D */
	uint8_t gforst_powthr;                            /* 0x08B9 */
	uint8_t sensi_adjust;                             /* 0x08AC, bits 7:2 */
	uint8_t gain_tune[SX126X_AGC_GAIN_TUNE_LEN];      /* 0x08F5 .. 0x08FB */
};

static const struct sx126x_agc_band_cal sx126x_agc_cal_high = {
	.rssi_meas_cal_h = 0x01,
	.rssi_meas_cal_l = 0x53,
	.gforst_powthr   = 0x0A,
	.sensi_adjust    = SX126X_AGC_SENSI_ADJUST_HIGH_BAND,
	.gain_tune       = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

static const struct sx126x_agc_band_cal sx126x_agc_cal_low = {
	.rssi_meas_cal_h = 0x01,
	.rssi_meas_cal_l = 0x27,
	.gforst_powthr   = 0x04,
	.sensi_adjust    = SX126X_AGC_SENSI_ADJUST_LOW_BAND,
	/* GainTune_1_2, _3_4, _5_6, _7_8, _9_10, _11_12, _13 */
	.gain_tune       = { 0xDE, 0xE2, 0x32, 0x44, 0x33, 0x34, 0x04 },
};

/* Band split.  The datasheet only characterises 470-490 and 868-915, so any
 * threshold between them is a judgement call; 600 MHz sits clear of both the
 * 433/470 group and the 779/868/915 group that sx126x_calibrate_image()
 * already distinguishes. */
#define SX126X_AGC_LOW_BAND_MAX_HZ 600000000

static const struct sx126x_agc_band_cal *sx126x_agc_cal_for(uint32_t freq)
{
	return (freq < SX126X_AGC_LOW_BAND_MAX_HZ) ? &sx126x_agc_cal_low
						   : &sx126x_agc_cal_high;
}

/* Apply the band's RSSI/AGC calibration.  Caches AgcSensiAdjust for the RX
 * gain writes, which share register 0x08AC, but does NOT touch 0x08AC itself
 * -- sx126x_set_rx_gain() owns that register and runs later in
 * sx126x_config(). */
static int sx126x_apply_agc_band_cal(const struct device *dev, uint32_t freq)
{
	struct sx126x_data *data = dev->data;
	const struct sx126x_agc_band_cal *cal = sx126x_agc_cal_for(freq);
	uint8_t val;
	int ret;

	data->agc_sensi_adjust = cal->sensi_adjust;

	/* Bits 4:0 only -- preserve the rest of 0x089C. */
	ret = sx126x_hal_read_regs(dev, SX126X_REG_AGC_RSSI_MEAS_CAL_H, &val, 1);
	if (ret < 0) {
		return ret;
	}
	val = (val & ~0x1FU) | (cal->rssi_meas_cal_h & 0x1FU);
	ret = sx126x_hal_write_regs(dev, SX126X_REG_AGC_RSSI_MEAS_CAL_H, &val, 1);
	if (ret < 0) {
		return ret;
	}

	val = cal->rssi_meas_cal_l;
	ret = sx126x_hal_write_regs(dev, SX126X_REG_AGC_RSSI_MEAS_CAL_L, &val, 1);
	if (ret < 0) {
		return ret;
	}

	val = cal->gforst_powthr;
	ret = sx126x_hal_write_regs(dev, SX126X_REG_AGC_GFORST_POWTHR, &val, 1);
	if (ret < 0) {
		return ret;
	}

	return sx126x_hal_write_regs(dev, SX126X_REG_AGC_GAIN_TUNE_1_2,
				     cal->gain_tune,
				     SX126X_AGC_GAIN_TUNE_LEN);
}

/* Byte to write to 0x08AC: band AgcSensiAdjust in bits 7:2, gain mode in 1:0. */
static uint8_t sx126x_rx_gain_byte(const struct device *dev, bool boosted)
{
	struct sx126x_data *data = dev->data;

	return SX126X_RX_GAIN_VAL(data->agc_sensi_adjust, boosted);
}

static int sx126x_set_rx_gain(const struct device *dev, bool boosted)
{
	uint8_t val = sx126x_rx_gain_byte(dev, boosted);

	/* Host-driven RX paths re-apply the desired gain right after SetRx
	 * (after BUSY goes low, before any preamble can arrive) in
	 * sx126x_restart_rx() / recv_async.  Note that DS sec 9.6 documents
	 * the gain being lost on *warm-start wake from sleep* only -- it does
	 * not say SetRx clears it -- so that re-apply is cheap insurance
	 * rather than a documented requirement, and is kept on that basis.
	 * Duty-cycle mode is the case the datasheet does cover: its sleep->RX
	 * wakes are chip-internal warm starts the host never sees, which is
	 * why 0x08AC is in the retention list installed by
	 * sx126x_install_retention_list() at chip init. */
	return sx126x_hal_write_regs(dev, SX126X_REG_RX_GAIN, &val, 1);
}

static int sx126x_set_tx(const struct device *dev, uint32_t timeout_ms)
{
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) && !sx126x_bench_tx_ready(dev)) return -EPERM;
	uint32_t timeout = SX126X_MS_TO_TIMEOUT(timeout_ms);
	uint8_t buf[3];

	sys_put_be24(timeout, buf);
	int ret = sx126x_hal_write_cmd(dev, SX126X_CMD_SET_TX, buf, 3);
	if (ret == 0) sx126x_observe_tx_started(dev->data);
	return ret;
}

static int sx126x_set_stop_timer_on_preamble(const struct device *dev,
					    bool enable)
{
	uint8_t val = enable;

	return sx126x_hal_write_cmd(dev, SX126X_CMD_STOP_TIMER_ON_PREAMBLE,
				    &val, 1);
}

static int sx126x_set_rx_duty_cycle(const struct device *dev,
				    uint32_t rx_period, uint32_t sleep_period)
{
	uint8_t buf[6];
	int ret;

	sys_put_be24(rx_period, &buf[0]);
	sys_put_be24(sleep_period, &buf[3]);

	ret = sx126x_set_stop_timer_on_preamble(dev, true);
	if (ret < 0) {
		return ret;
	}

	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_RX_DUTY_CYCLE, buf, 6);
}

static int sx126x_set_rx(const struct device *dev, uint32_t timeout_ms)
{
	uint32_t timeout;

	if (timeout_ms == 0) {
		timeout = SX126X_RX_TIMEOUT_CONTINUOUS;
	} else {
		timeout = SX126X_MS_TO_TIMEOUT(timeout_ms);
	}

	uint8_t buf[3];

	sys_put_be24(timeout, buf);
	return sx126x_hal_write_cmd(dev, SX126X_CMD_SET_RX, buf, 3);
}

static int sx126x_get_rx_buffer_status(const struct device *dev,
				       uint8_t *payload_len, uint8_t *offset)
{
	uint8_t buf[2];
	int ret;

	ret = sx126x_hal_read_cmd(dev, SX126X_CMD_GET_RX_BUFFER_STATUS, buf, 2);
	if (ret == 0) {
		*payload_len = buf[0];
		*offset = buf[1];
	}

	return ret;
}

static int sx126x_get_packet_status(const struct device *dev,
				    int16_t *rssi, int8_t *snr,
				    struct sx126x_packet_status *status)
{
	uint8_t buf[3];
	int ret;
	*status = (struct sx126x_packet_status){0};

	ret = sx126x_hal_read_cmd(dev, SX126X_CMD_GET_PACKET_STATUS, buf, 3);
	if (ret == 0) {
		/* RSSI is -value/2 dBm */
		*rssi = -((int16_t)buf[0] >> 1);
		/* SNR is value/4 dB (signed) */
		*snr = ((int8_t)buf[1]) >> 2;
		status->snr_raw = buf[1];
		status->snr_known = true;
	}

	return ret;
}

/*
 * Add a register to the chip's internal retention list so its value
 * survives warm-start sleep cycles (STDBY_RC -> SLEEP -> STDBY_RC).
 */
static int sx126x_add_reg_to_retention(const struct device *dev, uint16_t addr)
{
	uint8_t buf[9];
	uint8_t n;
	int ret;

	ret = sx126x_hal_read_regs(dev, SX126X_REG_RETENTION_LIST, buf, sizeof(buf));
	if (ret < 0) {
		return ret;
	}

	n = buf[0];

	/* A blank or corrupt count byte would otherwise be taken as a live
	 * entry count and index past the end of buf[]. */
	if (n > SX126X_MAX_RETENTION_REGS) {
		n = 0;
	}

	for (uint8_t i = 0; i < n; i++) {
		if (sys_get_be16(&buf[1 + 2 * i]) == addr) {
			return 0;
		}
	}

	if (n >= SX126X_MAX_RETENTION_REGS) {
		LOG_ERR("Retention list full");
		return -ENOSPC;
	}

	buf[0] = n + 1;
	sys_put_be16(addr, &buf[1 + 2 * n]);

	return sx126x_hal_write_regs(dev, SX126X_REG_RETENTION_LIST, buf, sizeof(buf));
}

/* The registers that must survive a warm-start wake.
 *
 * 0x08AC (Rx gain): DS sec 9.6 calls this mandatory when using
 * SetRxDutyCycle.  Without it every window after the first sleep reverts to
 * power-saving gain (-3 dB sensitivity) with no way for the host to
 * intervene, because those sleep->RX transitions are chip-internal.
 *
 * 0x0889 (Tx modulation): carries the sec 15.1 500 kHz workaround, which is
 * otherwise lost on the same wakes. */
#ifdef CONFIG_ZEPHCORE_LORA_RETENTION_DEBUG
/* Does installing the retention list once actually hold?
 *
 * The list is what restores 0x08AC across the chip-internal sleep->RX wakes of
 * SetRxDutyCycle.  Those wakes are invisible to the host, so if the list is
 * lost, every window after the first sleep listens at power-saving gain -- a
 * silent 3 dB with nothing in the log.  This check is what makes it visible.
 *
 * Called from the RX_DONE path *before* sx126x_restart_rx() rewrites the gain,
 * so what it observes is the gain the packet was actually received with rather
 * than the one we are about to reapply.  That placement costs two SPI reads
 * inside the duty-cycle deaf window, which is why this is a debug-only build
 * option: it perturbs the timing of the thing it measures, but not the gain.
 *
 * At install time the modem is not configured yet, so the gain half is
 * skipped and only the list is checked. */
static void sx126x_retention_debug_check(const struct device *dev,
					 const char *when, bool check_gain)
{
	struct sx126x_data *data = dev->data;
	uint8_t list[1 + 2 * SX126X_MAX_RETENTION_REGS];
	uint8_t gain = 0, want = 0;
	bool bad;

	if (sx126x_hal_read_regs(dev, SX126X_REG_RETENTION_LIST,
				 list, sizeof(list)) < 0) {
		LOG_WRN("retention[%s]: list readback failed", when);
		return;
	}

	bad = (list[0] != 2) ||
	      (sys_get_be16(&list[1]) != SX126X_REG_RX_GAIN) ||
	      (sys_get_be16(&list[3]) != SX126X_REG_TX_MODULATION);

	if (check_gain) {
		want = sx126x_rx_gain_byte(dev, data->rx_boost_enabled);
		if (sx126x_hal_read_regs(dev, SX126X_REG_RX_GAIN, &gain, 1) < 0) {
			LOG_WRN("retention[%s]: gain readback failed", when);
			return;
		}
		bad = bad || (gain != want);
		data->retention_checks++;
		if (bad) {
			data->retention_fails++;
		}
	}

	if (bad) {
		LOG_WRN("retention[%s]: FAIL n=%u [0]=0x%04X [1]=0x%04X gain=0x%02X want=0x%02X (%u of %u bad)",
			when, list[0], sys_get_be16(&list[1]),
			sys_get_be16(&list[3]), gain, want,
			data->retention_fails, data->retention_checks);
		return;
	}

	if (!check_gain) {
		LOG_INF("retention[%s]: list ok n=%u [0]=0x%04X [1]=0x%04X",
			when, list[0], sys_get_be16(&list[1]),
			sys_get_be16(&list[3]));
	} else if (data->retention_checks == 1U ||
		   (data->retention_checks % 64U) == 0U) {
		LOG_INF("retention[%s]: ok gain=0x%02X (%u checked, %u bad)",
			when, gain, data->retention_checks,
			data->retention_fails);
	}
}
#else
#define sx126x_retention_debug_check(dev, when, check_gain) ((void)0)
#endif /* CONFIG_ZEPHCORE_LORA_RETENTION_DEBUG */

static void sx126x_install_retention_list(const struct device *dev)
{
	(void)sx126x_add_reg_to_retention(dev, SX126X_REG_RX_GAIN);
	(void)sx126x_add_reg_to_retention(dev, SX126X_REG_TX_MODULATION);
	sx126x_retention_debug_check(dev, "install", false);
}

static int sx126x_chip_init(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;
	int ret;

	/* Hardware reset */
	ret = sx126x_hal_reset(dev);
	bench_record(dev,"sx126x_hal_reset",ret,0);
	if (ret < 0) {
		bench_record(dev,"Reset failed",ret,0);
		LOG_ERR("Reset failed: %d", ret);
		return ret;
	}

	/* Set standby mode */
	ret = sx126x_set_standby(dev, SX126X_STANDBY_RC);
	bench_record(dev,"sx126x_set_standby",ret,0);
	if (ret < 0) {
		bench_record(dev,"Set standby failed",ret,0);
		LOG_ERR("Set standby failed: %d", ret);
		return ret;
	}

	/* Reject a missing/floating chip rather than trusting successful SPI writes.
	 * The family ROM marker is the same SX1261 prefix on these Wio SX1262s. */
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH)) {
		uint8_t family[6]={0};
		ret=sx126x_hal_read_regs(dev,0x0320,family,sizeof(family));
	bench_record(dev,"sx126x_hal_read_regs",ret,0);
		bench_record(dev,"family_read",ret,sys_get_be32(family));
		if (ret<0 || memcmp(family,"SX1261",6)!=0) { bench_record(dev,"family_validate",-ENODEV,sys_get_be32(family));return -ENODEV; }
	}

	/* Configure TCXO if enabled */
	if (config->dio3_tcxo_enable) {
		ret = sx126x_set_dio3_as_tcxo_ctrl(dev, config->dio3_tcxo_voltage,
						   config->tcxo_startup_delay_ms);
	bench_record(dev,"sx126x_set_dio3_as_tcxo_ctrl",ret,0);
		if (ret < 0) {
			bench_record(dev,"Set TCXO failed",ret,0);
			LOG_ERR("Set TCXO failed: %d", ret);
			return ret;
		}

		/* Run full calibration after TCXO setup */
		ret = sx126x_calibrate(dev, SX126X_CALIBRATE_ALL);
	bench_record(dev,"sx126x_calibrate",ret,0);
		if (ret < 0) {
			bench_record(dev,"Calibration failed",ret,0);
			LOG_ERR("Calibration failed: %d", ret);
			return ret;
		}
	}

	/* Configure DIO2 as RF switch if enabled */
	if (config->dio2_tx_enable) {
		ret = sx126x_set_dio2_as_rf_switch(dev, true);
	bench_record(dev,"sx126x_set_dio2_as_rf_switch",ret,0);
		if (ret < 0) {
			bench_record(dev,"Set DIO2 RF switch failed",ret,0);
			LOG_ERR("Set DIO2 RF switch failed: %d", ret);
			return ret;
		}
	}

	/* Set regulator mode */
	ret = sx126x_set_regulator_mode(dev, config->regulator_ldo ?
					SX126X_REGULATOR_LDO : SX126X_REGULATOR_DCDC);
	bench_record(dev,"sx126x_set_regulator_mode",ret,0);
	if (ret < 0) {
		bench_record(dev,"Set regulator failed",ret,0);
		LOG_ERR("Set regulator failed: %d", ret);
		return ret;
	}

	/* Set buffer base addresses */
	ret = sx126x_set_buffer_base_address(dev, 0x00, 0x00);
	bench_record(dev,"sx126x_set_buffer_base_address",ret,0);
	if (ret < 0) {
		bench_record(dev,"Set buffer base failed",ret,0);
		LOG_ERR("Set buffer base failed: %d", ret);
		return ret;
	}

	/* After TX/RX, fall back to FS mode instead of the default STDBY_RC.
	 * FS keeps both XOSC and PLL active, so the next SetRx skips both the
	 * ~500 us XOSC startup AND the ~50-100 us PLL settle -- minimising the
	 * deaf window between packets.  Costs ~5-10 mA while in this state, but
	 * we are typically only here for the brief ISR-to-handler window before
	 * sleep or the next operation.  Critical for duty-cycle restart where
	 * every microsecond of deafness can mean a missed preamble.
	 * (datasheet Table 13-23: 0x40=FS, 0x30=STDBY_XOSC, 0x20=STDBY_RC) */
	{
		uint8_t fallback = 0x40; /* FS mode */

		ret = sx126x_hal_write_cmd(dev,
					   SX126X_CMD_SET_RX_TX_FALLBACK_MODE,
					   &fallback, 1);
	bench_record(dev,"sx126x_hal_write_cmd",ret,0);
		if (ret < 0) {
			bench_record(dev,"Set fallback mode failed",ret,0);
			LOG_ERR("Set fallback mode failed: %d", ret);
			return ret;
		}
	}

	/* Set packet type to LoRa */
	ret = sx126x_set_packet_type(dev, SX126X_PACKET_TYPE_LORA);
	bench_record(dev,"sx126x_set_packet_type",ret,0);
	if (ret < 0) {
		bench_record(dev,"Set packet type failed",ret,0);
		LOG_ERR("Set packet type failed: %d", ret);
		return ret;
	}

	/* Global mask includes preamble + header so both are visible in the IRQ
	 * status register for sx126x_is_receiving() to poll non-destructively.
	 * DIO1 excludes PREAMBLE_DETECTED only -- it fires on noise/foreign
	 * traffic and would cause spurious work-queue wakeups.  HEADER_VALID
	 * is routed to DIO1 because it fires at most once per real packet
	 * (after sync-word match + header CRC) and is the trigger that
	 * promotes the rx_packet_active latch in the work handler. */
	uint16_t irq_mask = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE |
			    SX126X_IRQ_RX_TX_TIMEOUT | SX126X_IRQ_CRC_ERR |
			    SX126X_IRQ_CAD_DONE | SX126X_IRQ_CAD_ACTIVITY_DETECTED |
			    SX126X_IRQ_PREAMBLE_DETECTED | SX126X_IRQ_HEADER_VALID |
			    SX126X_IRQ_HEADER_ERR;
	/* HEADER_ERR is unmasked (so it latches and is readable in the RX_DONE
	 * snapshot) but excluded from DIO1 -- like PREAMBLE_DETECTED, it fires
	 * on noise/foreign traffic and must not wake the work queue on its own. */
	uint16_t dio1_mask = irq_mask & ~(SX126X_IRQ_PREAMBLE_DETECTED | SX126X_IRQ_HEADER_ERR);
#ifdef CONFIG_MT_NORMAL_ENDPOINT
	/* A CAD-to-RX damaged header may be its sole terminal edge. */
	dio1_mask |= SX126X_IRQ_HEADER_ERR;
#endif
	ret = sx126x_set_dio_irq_params(dev, irq_mask, dio1_mask, 0, 0);
	bench_record(dev,"sx126x_set_dio_irq_params",ret,0);
	if (ret < 0) {
		bench_record(dev,"Set IRQ params failed",ret,0);
		LOG_ERR("Set IRQ params failed: %d", ret);
		return ret;
	}

	/* Clear any pending IRQs */
	ret = sx126x_clear_irq_status(dev, SX126X_IRQ_ALL);
	bench_record(dev,"sx126x_clear_irq_status",ret,0);
	if (ret < 0) {
		bench_record(dev,"Clear IRQ failed",ret,0);
		LOG_ERR("Clear IRQ failed: %d", ret);
		return ret;
	}

	/* Install the warm-start retention list once, here, rather than on
	 * every duty-cycle re-arm. */
	sx126x_install_retention_list(dev);

	LOG_INF("SX126x initialized");
	return 0;
}

static void sx126x_dio1_callback(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	sx126x_airtime_observer_reconfigure_invalidate(dev);

	if (k_work_submit_to_queue(&data->dio1_wq, &data->irq_work) < 0)
		sx126x_observe_loss(data);
}

static void sx126x_set_rf_path(const struct device *dev, bool enable, bool tx)
{
	const struct sx126x_hal_config *config = dev->config;
	struct sx126x_data *data = dev->data;

	sx126x_hal_set_antenna_enable(dev, enable);

	/* FEM receive-path select, on boards that wire one (lna-bypass-gpios).
	 * HIGH = bypass, LOW = through the LNA.  Deliberately independent of
	 * `enable`: the FEM's own chip-enable is antenna_enable, so while that
	 * is deasserted this line only decides which path the part will select
	 * when it comes back.  Leaving it in the receive state across sleep is
	 * what lets an ESP32 GPIO sleep-hold keep the RX path intact.
	 *
	 * TX drives it HIGH for the same reason the FEM vendors do: with the
	 * TX/RX path itself selected elsewhere (DIO2 into CPS), bypass is the
	 * inert state for the receive branch while the PA is keyed. */
	if (config->lna_bypass.port != NULL) {
		gpio_pin_set_dt(&config->lna_bypass, tx || data->fem_rx_off);
	}

	if (config->dio2_tx_enable) {
		/* DIO2 handles TX enable in hardware -- but rx-enable-gpios
		 * (e.g. E22-900M30S RXEN) and some board-specific tx-enable
		 * lines still need explicit GPIO control.
		 *
		 * RXEN: HIGH when receiving, LOW otherwise.
		 * TXEN: HIGH when transmitting, LOW otherwise. */
		if (config->tx_enable.port != NULL) {
			gpio_pin_set_dt(&config->tx_enable, enable && tx);
		}
		if (config->rx_enable.port != NULL) {
			gpio_pin_set_dt(&config->rx_enable, enable && !tx);
		}
	} else {
		sx126x_hal_set_rf_switch(dev, enable, tx);
	}
}

#ifdef CONFIG_PM_DEVICE
static void sx126x_disconnect_gpio(const struct gpio_dt_spec *gpio)
{
	if (gpio->port != NULL) {
		gpio_pin_configure(gpio->port, gpio->pin, GPIO_DISCONNECTED);
	}
}

static void sx126x_disconnect_rf_gpios(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;

	sx126x_disconnect_gpio(&config->antenna_enable);
	sx126x_disconnect_gpio(&config->tx_enable);
	sx126x_disconnect_gpio(&config->rx_enable);
	sx126x_disconnect_gpio(&config->lna_bypass);
}

static int sx126x_reconnect_rf_gpios(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;
	int ret;

	ret = sx126x_hal_configure_gpio(&config->antenna_enable,
					GPIO_OUTPUT_INACTIVE, "antenna enable");
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_hal_configure_gpio(&config->tx_enable,
					GPIO_OUTPUT_INACTIVE, "TX enable");
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_hal_configure_gpio(&config->rx_enable,
					GPIO_OUTPUT_INACTIVE, "RX enable");
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_hal_configure_gpio(&config->lna_bypass,
					GPIO_OUTPUT_INACTIVE, "FEM LNA bypass");
	if (ret < 0) {
		return ret;
	}

	return 0;
}
#endif /* CONFIG_PM_DEVICE */

/* -- Duty-cycle support helpers -- */

/* Watchdog sampling period, scaled to the active preset.  The two-strike
 * design needs the period to exceed the longest legitimate preamble-detect ->
 * header gap so two consecutive parked sightings can never both fall inside
 * one real packet's gap (which would abort a live reception).  A fixed value
 * has to be sized for the slowest preset (SF12/BW62.5, ~1.9 s) -- ~15x larger
 * than needed at SF8/BW62.5, so every false-preamble park then pins the radio
 * in full RX for up to two periods, wasting the duty-cycle power savings.
 * Scale it instead: 2x the (preamble+8)-symbol grace covers the gap with
 * margin at every preset (SF8/BW62.5 ~0.3 s, SF12/BW62.5 ~3.1 s), floored so
 * fast presets don't poll the SPI bus raw.  Never costs packets -- a parked
 * radio is still receiving; the period only bounds how long it stays parked. */
#define SX126X_DC_WATCHDOG_MIN_MS 250

static uint32_t sx126x_dc_watchdog_ms(struct sx126x_data *data)
{
	uint32_t ms = 2U * sx126x_preamble_grace_ms(data);

	return (ms < SX126X_DC_WATCHDOG_MIN_MS) ? SX126X_DC_WATCHDOG_MIN_MS : ms;
}

/* (Re)arm the parked-RX watchdog and forget any parked evidence -- called
 * from every duty-cycle (re)start site, which all bulk-clear the IRQ
 * register first. */
static void sx126x_dc_watchdog_kick(struct sx126x_data *data)
{
	data->dc_watchdog_strikes = 0;
	k_work_reschedule_for_queue(&data->dio1_wq, &data->dc_watchdog_work,
				    K_MSEC(sx126x_dc_watchdog_ms(data)));
}

/* -- Lightweight RX restart -- */

/* Restart RX as fast as possible -- used for RX->RX transitions in the
 * IRQ handler.  Issues SetRx(continuous) directly without going through
 * the standby/sleep round-trip that lora_recv_async would do. */
static int sx126x_restart_rx(const struct device *dev, struct sx126x_data *data)
{
	uint32_t diagnostic_call=sx126x_stage_begin(data,&data->restart_stage);
	bool diagnostic_recorded=false;
	/* Clear any stale IRQ bits before we re-enter RX, so a leftover
	 * PREAMBLE_DETECTED from a partial reception cannot pin
	 * sx126x_is_receiving() true on the next cycle.  Also resets the
	 * software latch + preamble-grace timestamp -- old packet is done. */
	int ret=sx126x_clear_irq_status(dev, SX126X_IRQ_ALL);
	if(ret<0) {sx126x_stage_fail(data,&data->restart_stage,diagnostic_call,1,ret,&diagnostic_recorded);sx126x_observe_rx(data, 0, 0, false);return ret;}
	sx126x_reset_rx_busy_signals(data);

	if (data->rx_duty_cycle_enabled) {
		/* No Calibrate(ALL) here.
		 *
		 * This branch used to run a full recalibration on every
		 * duty-cycle re-arm -- after every received packet AND every
		 * noise header error -- at ~5 ms of busy-wait apiece, plus three
		 * repair steps that existed only to undo it: Calibrate reverts
		 * image calibration to the 902-928 MHz default, clears the AGC
		 * band calibration, resets the DIO2 RF-switch role and resets
		 * StopTimerOnPreamble.  Four of the seven steps in this branch
		 * were repairing the second one.
		 *
		 * The AGC work itself is real and is NOT being dropped: Semtech
		 * give warm sleep plus recalibration as the remedy for a jammed
		 * AGC, and sx126x_reset_agc() still performs exactly that.  What
		 * was wrong was the trigger.  Firing on reception is
		 * anti-correlated with need -- it runs most often on a busy
		 * channel, where the packet that triggered it has just proved
		 * the AGC is working, and no more often on a silent one, where a
		 * desensitised receiver is invisible and nothing else reveals
		 * it.  It now runs from LoRaRadioBase::agcMaintenance() on
		 * silence instead: after AGC_IDLE_RESET_MS with nothing heard,
		 * when by definition there is no traffic to miss.  Arduino
		 * MeshCore reached the same place from the other direction --
		 * its periodic `agc_reset_interval` ships defaulted to 0 (off).
		 *
		 * Per-packet AGC state is still handled, and always was: the
		 * RX-gain write at the end of this function re-applies 0x08AC
		 * after SetRx on every single re-entry, which is what actually
		 * gives each packet a clean gain state.
		 *
		 * StopTimerOnPreamble is KEPT below.  Its comment blamed
		 * Calibrate for resetting it, but nobody has verified it
		 * survives the duty-cycle sleep unaided, and it is one cheap
		 * command guarding a failure mode (preambles clipped at the
		 * window tail) that is hard to spot.  Drop it only on evidence.
		 */

		/* StopTimerOnPreamble: without it, the duty-cycle RX timer
		 * keeps running even after a preamble is detected -- a
		 * preamble arriving at the tail of the RX window can get
		 * cut off when the sleep phase begins.  Kept as insurance --
		 * see the note above. */
		/* No retention-list write here.  Keeping boosted RX gain alive
		 * across the chip-internal sleep->RX wakes is still MANDATORY
		 * (DS sec 9.6: the Rx gain configuration "is not saved when
		 * entering sleep mode with retention activated", so without the
		 * list every wake after the first loses 3 dB) -- but the list
		 * itself lives in retention memory and survives those wakes, so
		 * installing it once in sx126x_chip_init() is sufficient.
		 * Re-writing it on every re-arm added SPI to the deaf window
		 * after each received packet, which on a duty-cycled node is
		 * every packet.  This chip is the only one of the three that
		 * needs the list at all -- the LR2021 retains the gain
		 * (DS sec 6.3.8) and the LR11xx documents no caveat either way. */

		/* Re-apply duty cycle with stored timing.  Upstream's helper
		 * issues StopTimerOnPreamble (see above) and then SetRxDutyCycle. */
		const int duty_result=sx126x_set_rx_duty_cycle(dev, data->duty_cycle.rx_period,
					       data->duty_cycle.sleep_period);
		sx126x_stage_fail(data,&data->restart_stage,diagnostic_call,2,duty_result,&diagnostic_recorded);

		sx126x_dc_watchdog_kick(data);
	} else {
		/* Non-DC continuous RX: plain SetRx(continuous), no symbol
		 * timer.  An earlier draft armed SetLoRaSymbNumTimeout here for
		 * "foreign-preamble auto-release", but the chip's symbol timer
		 * counts down from SetRx regardless of preamble detection (with
		 * STOP_TIMER_ON_PREAMBLE = 0).  In a quiet bench it therefore
		 * fired IRQ_RX_TX_TIMEOUT every ~N symbols, causing a
		 * (timeout → restart_rx) busy-poll cycle that raced with TX
		 * attempts.  Foreign-preamble defense is instead handled by
		 * is_receiving() gating on HEADER_VALID only (raw bit) — the
		 * level-latched PREAMBLE_DETECTED bit is ignored there, so a
		 * foreign preamble does not pin the TX gate. */
		uint8_t buf[3];

		sys_put_be24(SX126X_RX_TIMEOUT_CONTINUOUS, buf);
		ret=sx126x_hal_write_cmd(dev, SX126X_CMD_SET_RX, buf, 3);
		if(ret<0) {sx126x_stage_fail(data,&data->restart_stage,diagnostic_call,3,ret,&diagnostic_recorded);sx126x_observe_rx(data, 0, 0, false);return ret;}
	}

	/* Re-apply RX gain.  DS sec 9.6 documents the gain being lost on
	 * warm-start wake from sleep, not on SetRx, so this is belt-and-braces
	 * rather than a documented requirement -- it is two bytes of SPI and it
	 * guarantees a known gain state for the packet about to arrive.
	 * sx126x_hal_write_cmd() above waits for BUSY before issuing the command;
	 * sx126x_hal_write_regs() below also waits for BUSY (the ~3.4 ms RC startup
	 * after SetRx) before writing.  The gain is therefore applied after the radio
	 * has entered RX but before any preamble can arrive -- giving a clean AGC
	 * state for each new packet regardless of the previous packet's signal level. */
	uint8_t gain = sx126x_rx_gain_byte(dev, data->rx_boost_enabled);

	ret = sx126x_hal_write_regs(dev, SX126X_REG_RX_GAIN, &gain, 1);
	sx126x_stage_fail(data,&data->restart_stage,diagnostic_call,4,ret,&diagnostic_recorded);
	if (ret < 0) sx126x_observe_rx(data, 0, 0, false);
	return ret;
}

/* Parked-RX watchdog (see struct field comment).  Runs on the dedicated
 * DIO1 work queue so it serialises with the IRQ handler and restart
 * paths.  Self-terminating: returns without re-arming when duty-cycle RX
 * is no longer active -- every DC (re)start kicks it again. */
static void sx126x_dc_watchdog_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct sx126x_data *data = CONTAINER_OF(dwork, struct sx126x_data,
						dc_watchdog_work);
	const struct device *dev = data->dev;
	uint16_t irq_status = 0;
	bool parked = false;

	if (!data->rx_duty_cycle_enabled ||
	    atomic_get(&data->state) != SX126X_STATE_RX) {
		return;
	}

	/* Legit packet mid-decode (HEADER_VALID latched): check again later. */
	if (data->rx_packet_active) {
		data->dc_watchdog_strikes = 0;
		goto rearm;
	}

	/* BUSY high = duty-cycle sleep phase = cycling normally. */
	if (sx126x_hal_is_busy(dev)) {
		data->dc_watchdog_strikes = 0;
		goto rearm;
	}

	if (k_mutex_lock(&data->lock, K_NO_WAIT) != 0) {
		/* Another operation owns the chip right now -- not parked. */
		data->dc_watchdog_strikes = 0;
		goto rearm;
	}

	/* Re-check BUSY under the lock (same pattern as get_rssi_inst): a
	 * sleep transition between the check above and the SPI read below
	 * would stall the HAL and force-wake the chip. */
	if (!sx126x_hal_is_busy(dev) &&
	    sx126x_get_irq_status(dev, &irq_status) == 0) {
		parked = (irq_status & SX126X_IRQ_PREAMBLE_DETECTED) &&
			 !(irq_status & SX126X_IRQ_HEADER_VALID);
	}

	if (!parked) {
		data->dc_watchdog_strikes = 0;
		k_mutex_unlock(&data->lock);
		goto rearm;
	}

	if (++data->dc_watchdog_strikes < 2) {
		/* First sighting: could be a real preamble whose header is
		 * still in flight.  A second sighting one full period later
		 * cannot be the same packet (period > worst-case
		 * preamble + sync + header airtime). */
		k_mutex_unlock(&data->lock);
		goto rearm;
	}

	/* Two strikes: a false preamble detect parked the chip in full RX
	 * (StopTimerOnPreamble behaviour, AN1200.36 sec 3).  Nothing was
	 * lost -- parked RX still receives -- but the duty cycle stopped.
	 * Re-arm it.  Shares the dc_timeout_restarts stat: both paths mean
	 * "false preamble cost extra RX-on time". */
	atomic_inc(&data->dc_timeout_restarts);
	(void)sx126x_restart_rx(dev, data);
	k_mutex_unlock(&data->lock);
	return; /* restart_rx re-kicked the watchdog */

rearm:
	k_work_schedule_for_queue(&data->dio1_wq, &data->dc_watchdog_work,
				  K_MSEC(sx126x_dc_watchdog_ms(data)));
}

static int sx126x_set_sleep(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	uint8_t cfg = SX126X_SLEEP_WARM_START;
	int ret;

	/* Disable DIO1 admission for both real sleep and the configured standby
	 * substitute. Terminal cleanup retires software admission even if the
	 * hardware transition reports a failure. */
	(void)sx126x_hal_set_dio1_callback(dev, NULL);
	sx126x_hal_retire_dio1_callback(dev);

	if (!IS_ENABLED(CONFIG_LORA_SX126X_NATIVE_SLEEP)) {
		atomic_set(&data->state, SX126X_STATE_IDLE);
		sx126x_set_rf_path(dev, false, false);
		return 0;
	}

	if (atomic_get(&data->state) == SX126X_STATE_SLEEP) {
		return 0;
	}

	sx126x_set_rf_path(dev, false, false);

	ret = sx126x_hal_write_cmd(dev, SX126X_CMD_SET_SLEEP, &cfg, 1);
	if (ret == 0) {
		atomic_set(&data->state, SX126X_STATE_SLEEP);
	}

	return ret;
}

static int sx126x_ensure_ready(const struct device *dev)
{
	int ret;

	/* Re-enable DIO1 interrupt */
	ret = sx126x_hal_set_dio1_callback(dev, sx126x_dio1_callback);
	if (ret < 0) {
		return ret;
	}

	if (!IS_ENABLED(CONFIG_LORA_SX126X_NATIVE_SLEEP)) {
		return 0;
	}

	/*
	 * Wake the chip from sleep by sending GET_STATUS. The NSS edge
	 * wakes the chip which then initializes into STDBY_RC (warm start
	 * retains configuration). Cannot use sx126x_set_standby() here
	 * because the HAL waits for BUSY LOW before sending the SPI command,
	 * but BUSY stays HIGH until the chip is woken by an NSS edge.
	 */
	ret = sx126x_hal_wakeup(dev);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static void sx126x_handle_irq_tx_done(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	struct sx126x_tx_result result = { .status = 0 };

	LOG_DBG("TX done");
	sx126x_set_sleep(dev);

	if (data->tx_async_signal != NULL) {
		k_poll_signal_raise(data->tx_async_signal, 0);
		data->tx_async_signal = NULL;
	}
	k_msgq_put(&data->tx_msgq, &result, K_NO_WAIT);
}

static int sx126x_handle_irq_rx_done(const struct device *dev, uint16_t irq_status)
{
	struct sx126x_data *data = dev->data;
	struct sx126x_rx_result result = { 0 };
	uint8_t payload_len = 0, offset = 0;
	lora_recv_cb rx_cb = NULL;
	sx126x_recv_packet_cb rx_packet_cb = NULL;
	void *rx_cb_user_data = NULL;
	atomic_val_t rx_cb_gen = 0;
	int ret;

	/* Terminal event for the in-flight packet -- drop the RX-busy latch
	 * and preamble-grace timestamp.  Covers both success and CRC_ERR
	 * (they co-fire here). */
	sx126x_reset_rx_busy_signals(data);

	/* Get received packet info */
	ret = sx126x_get_rx_buffer_status(dev, &payload_len, &offset);
	if (ret < 0) {
		LOG_ERR("Failed to get RX buffer status");
		result.status = ret;
	} else {
		/* Get signal quality */
		sx126x_get_packet_status(dev, &result.rssi, &result.snr,
					 &result.packet_status);

		/* Reject CRC errors AND header errors.  A header error with no
		 * valid header means no payload was written to the FIFO, so
		 * GetRxBufferStatus still points at the PREVIOUS packet -- reading
		 * it re-delivers a stale "zombie" packet with a fresh (noise) SNR.
		 * Mirrors RadioLib SX126x::readData() and the LR11xx/LR20xx
		 * drivers.  The !HEADER_VALID term keeps a normal
		 * valid-header-then-CRC-fail packet on the CRC_ERR path. */
		if ((irq_status & SX126X_IRQ_CRC_ERR) ||
		    ((irq_status & SX126X_IRQ_HEADER_ERR) &&
		     !(irq_status & SX126X_IRQ_HEADER_VALID))) {
			LOG_WRN("RX rejected: CRC=%d HDR=%d",
				(irq_status & SX126X_IRQ_CRC_ERR) ? 1 : 0,
				(irq_status & SX126X_IRQ_HEADER_ERR) ? 1 : 0);
			result.status = -EIO;
		} else if (payload_len == 0) {
			/* Clean RX_DONE but the reported payload length is 0 --
			 * nothing to deliver.  Route to the error path so it is
			 * counted; otherwise result.status stays 0 and falls
			 * through both delivery arms below (silent drop). */
			LOG_DBG("RX done: zero-length payload");
			result.status = -EIO;
		} else {
			/* Read payload into shared buffer */
			result.len = MIN(payload_len, sizeof(data->rx_buf));
			ret = sx126x_hal_read_buffer(dev, offset,
						     data->rx_buf, result.len);
			if (ret < 0) {
				LOG_ERR("Failed to read RX buffer");
				result.status = ret;
			} else {
				result.status = result.len;
				LOG_DBG("RX done: %d bytes, RSSI=%d, SNR=%d",
					result.len, result.rssi, result.snr);
			}
		}
	}

	sx126x_observe_rx(data, irq_status, payload_len, result.status > 0 &&
	                  !(irq_status & (SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR)));

	k_mutex_lock(&data->lock, K_FOREVER);
	rx_cb = data->rx_cb;
	rx_packet_cb = data->rx_packet_cb;
	rx_cb_user_data = data->rx_cb_user_data;
	rx_cb_gen = atomic_get(&data->rx_cb_gen);
	k_mutex_unlock(&data->lock);

	/* Handle async callback or signal sync receiver */
	if (rx_cb != NULL) {
		/* Restart RX FIRST -- minimise the deaf window.
		 * The callback (mesh processing) can take 100s of us;
		 * doing it before restart would lose back-to-back packets.
		 * Safe: we're on a cooperative work queue so the next
		 * DIO1 work item won't preempt us, and the callback
		 * copies rx_buf before returning. */
		if (data->rx_duty_cycle_enabled) {
			/* Must run before restart_rx rewrites 0x08AC -- see
			 * sx126x_retention_debug_check().  Compiles to nothing
			 * without CONFIG_ZEPHCORE_LORA_RETENTION_DEBUG. */
			sx126x_retention_debug_check(dev, "dc-rx", true);
		}
		ret = sx126x_restart_rx(dev, data);
		if (ret < 0) {
			LOG_WRN("RX restart failed (%d)", ret);
			sx126x_observe_rx(data, irq_status, 0, false);
			result.status = ret;
			(void)sx126x_set_sleep(dev);
		}

		k_mutex_lock(&data->lock, K_FOREVER);
		if ((data->rx_cb != rx_cb) || (data->rx_packet_cb != rx_packet_cb) ||
		    (atomic_get(&data->rx_cb_gen) != rx_cb_gen)) {
			rx_cb = NULL;
		}
		k_mutex_unlock(&data->lock);

		if (rx_cb != NULL && rx_packet_cb != NULL && result.status != 0) {
			struct sx126x_packet_status status = result.packet_status;
			if (result.status < 0) {
				status = (struct sx126x_packet_status){0};
			}
			rx_packet_cb(dev, result.status > 0 ? data->rx_buf : NULL,
				     result.status > 0 ? result.len : 0,
				     result.rssi, result.snr, &status, rx_cb_user_data);
		} else if (rx_cb != NULL && result.status < 0) {
			rx_cb(dev, NULL, 0, result.rssi, result.snr, rx_cb_user_data);
		} else if (rx_cb != NULL && result.status > 0) {
			rx_cb(dev, data->rx_buf, result.len, result.rssi, result.snr,
			      rx_cb_user_data);
		}
	} else {
		/* Sync mode.
		 *
		 * SetStandby first: DS sec 13.1.1 states SetSleep "can be sent
		 * only while in STDBY mode".  Under the old (transposed)
		 * timeout constants this path was reached with the chip already
		 * parked in STDBY_RC, because what the driver called continuous
		 * RX was really Rx Single.  With true continuous RX the chip is
		 * still in RX here, so the standby step is now load-bearing.
		 *
		 * Nothing in ZephCore calls the blocking lora_recv() API -- the
		 * mesh always passes a callback -- so this is correctness for
		 * the next caller rather than a live fix. */
		sx126x_set_standby(dev, SX126X_STANDBY_RC);
		sx126x_set_sleep(dev);
		k_msgq_put(&data->rx_msgq, &result, K_NO_WAIT);
	}

	return ret < 0 ? ret : 0;
}

static void sx126x_handle_irq_timeout(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	lora_recv_cb rx_cb = NULL;
	bool was_cad_rx;

	LOG_DBG("Timeout");

	/* cadTimeout expired with nothing decoded: the CAD_RX detection had no
	 * packet behind it.  Resolved first, before anything below re-arms the
	 * node's normal Rx mode, and the verdict is kept because the duty-cycle
	 * branch must not count this timeout (see there). */
	was_cad_rx = sx126x_cad_rx_resolve(data, SX126X_CAD_RX_TMOUT);

	/* Terminal event -- clear the RX-busy latch and preamble-grace
	 * timestamp.  Fires for DC preamble false-positive re-arm and any
	 * future non-DC timeout path. */
	sx126x_reset_rx_busy_signals(data);

	if (data->cad_active) {
		return;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	rx_cb = data->rx_cb;
	k_mutex_unlock(&data->lock);

	/* Duty-cycle RX: a preamble false-positive (noise, partial preamble
	 * from another modulation) can fire RxTimeout -- but the chip has left
	 * duty cycle and is parked in STDBY_RC.  Without this re-arm the
	 * driver silently falls out of duty cycle after the first noisy
	 * window and goes to sleep.  Re-entering via sx126x_restart_rx()
	 * preserves the stored rx/sleep timing and re-applies AGC reset +
	 * StopTimerOnPreamble.
	 *
	 * Gated on state != TX.  A chip-side Tx timeout raises the same
	 * IRQ_RX_TX_TIMEOUT, and on a duty-cycled node both conditions above
	 * hold during a transmit -- rx_duty_cycle_enabled stays set across TX
	 * and the mesh never deregisters rx_cb.  Without the gate a Tx timeout
	 * took this branch, re-armed the duty cycle and returned, leaving
	 * data->state stuck at SX126X_STATE_TX and tx_async_signal unraised:
	 * the sender waits forever and every later entry CAS fails with "Busy",
	 * the same permanent deaf-and-mute end state as the AGC bug above but
	 * reached from the transmit side.  Falling through instead reaches the
	 * tail of this function, which sleeps the chip (restoring REST_STATE)
	 * and reports -ETIMEDOUT to the waiting sender; the C++ failure path
	 * then calls startReceive() and re-arms the duty cycle properly. */
	if (data->rx_duty_cycle_enabled && rx_cb != NULL &&
	    atomic_get(&data->state) != SX126X_STATE_TX) {
		int ret;

		/* Not when this timeout is the CAD_RX probe's own cadTimeout:
		 * that is a calibration false positive, it raises the identical
		 * IRQ, and counting it would mix one tick per busy probe into a
		 * figure that is supposed to describe duty-cycle preamble
		 * behaviour.  The re-arm below still runs -- CAD_RX left the
		 * chip in a plain Rx, so the duty cycle does need handing
		 * back. */
		if (!was_cad_rx) {
			atomic_inc(&data->dc_timeout_restarts);
		}
		ret = sx126x_restart_rx(dev, data);
		if (ret < 0) {
			LOG_WRN("Duty-cycle restart failed (%d), retry from sleep", ret);
			(void)sx126x_set_sleep(dev);

			ret = sx126x_ensure_ready(dev);
			if (ret < 0) {
				LOG_WRN("Duty-cycle wake failed (%d)", ret);
				return;
			}

			atomic_set(&data->state, SX126X_STATE_RX);
			ret = sx126x_restart_rx(dev, data);
			if (ret < 0) {
				LOG_WRN("Duty-cycle rearm failed (%d)", ret);
				(void)sx126x_set_sleep(dev);
			}
		}
		return;
	}

	/* Non-DC continuous RX safety net: SetRx(0xFFFFFF) means no symbol
	 * timer, so IRQ_RX_TX_TIMEOUT should not normally fire here.  But if
	 * any future setup (or a chip glitch) does fire it while async RX is
	 * active, re-arm rather than silently falling back to sleep. */
	/* The async RX callback remains registered during TX. A TX timeout must
	 * reach its completion signal, never the continuous-RX rearm branch. */
	if (!data->rx_duty_cycle_enabled && rx_cb != NULL &&
	    atomic_get(&data->state) != SX126X_STATE_TX) {
		int ret = sx126x_restart_rx(dev, data);

		if (ret < 0) {
			LOG_WRN("Non-DC timeout re-arm failed (%d)", ret);
			(void)sx126x_set_sleep(dev);
		}
		return;
	}

	sx126x_set_sleep(dev);

	if (data->tx_async_signal != NULL) {
		struct sx126x_tx_result result = { .status = -ETIMEDOUT };

		k_poll_signal_raise(data->tx_async_signal, -ETIMEDOUT);
		data->tx_async_signal = NULL;
		k_msgq_put(&data->tx_msgq, &result, K_NO_WAIT);
	} else if (data->rx_cb == NULL) {
		/* Sync RX timeout */
		struct sx126x_rx_result result = { .status = -EAGAIN };

		k_msgq_put(&data->rx_msgq, &result, K_NO_WAIT);
	}
}

#ifdef CONFIG_MT_NORMAL_ENDPOINT
/* CAD_DONE and RX_DONE can share one IRQ word. Publish the chip's RX state
 * before the ordinary RX handlers run, and never re-arm over that packet. */
static void sx126x_lbt_irq(const struct device *dev, uint16_t flags)
{
	struct sx126x_data *data = dev->data;
	if ((flags & SX126X_IRQ_CAD_DONE) &&
	    (flags & SX126X_IRQ_CAD_ACTIVITY_DETECTED) &&
	    atomic_cas(&data->lbt_handoff, LBT_SCAN, LBT_RX)) {
		atomic_set(&data->state, SX126X_STATE_RX);
		uint32_t now = k_uptime_get_32();
		data->rx_packet_active = true;
		atomic_set(&data->header_seen_at_ms, now ? now : 1U);
	}

	if ((flags & (SX126X_IRQ_RX_DONE | SX126X_IRQ_RX_TX_TIMEOUT |
		      SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR)) &&
	    atomic_cas(&data->lbt_handoff, LBT_RX, LBT_IDLE) &&
	    !(flags & (SX126X_IRQ_RX_DONE | SX126X_IRQ_RX_TX_TIMEOUT))) {
		/* A bad header can end bounded RX with no RX_DONE. Normal RX_DONE
		 * and TIMEOUT already rearm the continuous receiver themselves. */
		if (sx126x_restart_rx(dev, data) < 0) {
			(void)sx126x_set_sleep(dev);
		}
	}
}
#endif

/* Process one already-captured IRQ word.  Keeping the snapshot separate from
 * the work wrapper lets the storage-pause path freeze the chip, take the final
 * IRQ snapshot itself, and deliver a just-completed packet without reopening
 * the race between an empty status read and a later DIO work submission. */
static int sx126x_handle_irq_status(const struct device *dev, uint16_t irq_status)
{
	struct sx126x_data *data = dev->data;
	int rx_result = 0;

	sx126x_observe_tx_irq(data, irq_status);

	LOG_DBG("IRQ status: 0x%04x", irq_status);

	/* Clear handled IRQs */
	if (sx126x_clear_irq_status(dev, irq_status) < 0)
		sx126x_observe_rx(data, irq_status, 0, false);
	if (!(irq_status & SX126X_IRQ_RX_DONE) &&
	    ((irq_status & (SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR)) ||
	     ((irq_status & SX126X_IRQ_RX_TX_TIMEOUT) &&
	      (atomic_get(&data->state) == SX126X_STATE_RX ||
	       atomic_get(&data->state) == SX126X_STATE_RX_DUTY_CYCLE))))
		sx126x_observe_rx(data, irq_status, 0, false);

#ifdef CONFIG_MT_NORMAL_ENDPOINT
	sx126x_lbt_irq(dev, irq_status);
#endif
	if (irq_status & SX126X_IRQ_TX_DONE) {
		sx126x_handle_irq_tx_done(dev);
	}

	if (irq_status & SX126X_IRQ_RX_DONE) {
		/* Before the handler, which re-arms Rx and clears the latches:
		 * a packet completing is the ground truth a CAD_RX was waiting
		 * for.  CRC_ERR rides in the same status word and counts too --
		 * a corrupt packet is still a real transmitter, which is the
		 * question the probe is asking. */
		sx126x_cad_rx_resolve(data, SX126X_CAD_RX_PACKET);
		rx_result = sx126x_handle_irq_rx_done(dev, irq_status);
	}
	if ((irq_status & SX126X_IRQ_CRC_ERR) &&
	    !(irq_status & SX126X_IRQ_RX_DONE)) {
		sx126x_cad_rx_resolve(data, SX126X_CAD_RX_PACKET);
	}

	if (irq_status & SX126X_IRQ_CAD_DONE) {
		bool detected = (irq_status & SX126X_IRQ_CAD_ACTIVITY_DETECTED) != 0;

		LOG_DBG("CAD done: %s", detected ? "activity" : "free");
		data->cad_active = false;

		/* CAD_RX with a positive verdict: the chip did NOT return to
		 * standby, it is in Rx on the signal it found.  Arm the outcome
		 * tracker so whichever terminal IRQ follows -- RX_DONE, CRC_ERR
		 * or RX_TX_TIMEOUT at cadTimeout -- records what it was.  A
		 * negative CAD_RX exits to STDBY_RC exactly like CAD_ONLY, so
		 * there is nothing to track. */
		if (data->cad_exit_rx && detected) {
			/* CAD_ONLY has left the chip in STDBY_RC (DS 13.1.8), so
			 * arm the follow-on Rx ourselves with an explicit
			 * SetRx(timeout).  That re-invokes and re-programs the RTC,
			 * which is exactly what cadTimeout fails to do after
			 * SetRxDutyCycle -- so the terminal IRQ (RX_DONE, CRC_ERR or
			 * RX_TX_TIMEOUT) is genuinely guaranteed here, duty cycle or
			 * not.  Bound is max-length-packet airtime +25% +100 ms, the
			 * same figure cadTimeout used, so a real packet still
			 * completes inside it.  Cost is the ~1 ms of SPI between
			 * CAD_DONE and Rx, well inside the (P-8)-symbol preamble
			 * budget the probe already spends. */
			uint8_t rxbuf[3];
			/* Saturated, not masked: at SF12/BW7.81 the bound
			 * (~273 s) exceeds the 24-bit field (262 s at
			 * 15.625 us), and a mask wrapped it to ~11 s. */
			uint32_t tmo = zc_lora_ms_to_steps24(
					sx126x_max_payload_ms(data), 64000U);

			sys_put_be24(tmo, rxbuf);
			atomic_set(&data->cad_rx_state, SX126X_CAD_RX_ARMED);
			sx126x_hal_write_cmd(dev, SX126X_CMD_SET_RX, rxbuf, 3);
		}

		if (data->cad_cb) {
			lora_cad_cb cb = data->cad_cb;
			void *ud = data->cad_user_data;

			data->cad_cb = NULL;
			data->cad_user_data = NULL;
			cb(dev, detected, ud);
		} else {
			data->cad_result = detected ? 1 : 0;
			k_sem_give(&data->cad_sem);
		}
	}

	if (irq_status & SX126X_IRQ_RX_TX_TIMEOUT) {
		/* The CAD_RX resolve for this event lives at the top of
		 * sx126x_handle_irq_timeout(), which needs its verdict to keep
		 * the probe's own cadTimeout out of dc_timeout_restarts. */
		sx126x_handle_irq_timeout(dev);
	}

	/* HEADER_VALID: promote rx_packet_active latch so sx126x_is_receiving()
	 * keeps reporting busy across the payload phase, not just during the
	 * narrow window before the work handler runs.  Gated on no terminal
	 * bit also being set in this same handler pass -- terminal events
	 * (RX_DONE / CRC_ERR / RX_TX_TIMEOUT) own the false-write and must win
	 * on co-fire.  CAD_DONE is handled before RX_TX_TIMEOUT so restart_rx
	 * cannot abort an in-flight CAD. */
	if ((irq_status & SX126X_IRQ_HEADER_VALID) &&
	    !(irq_status & (SX126X_IRQ_RX_DONE | SX126X_IRQ_RX_TX_TIMEOUT |
			    SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR))) {
		k_mutex_lock(&data->lock, K_FOREVER);
		data->rx_packet_active = true;
		/* Latch is the truth source now; the preamble-grace timestamp
		 * is stale and we don't want a future is_receiving() to treat
		 * it as live when the latch eventually clears. */
		atomic_set(&data->preamble_seen_at_ms, 0);
		/* Start the payload deadline (see sx126x_max_payload_ms).  Use 1
		 * as the "set" sentinel if k_uptime is 0 right after boot. */
		{
			uint32_t now = k_uptime_get_32();

			atomic_set(&data->header_seen_at_ms,
				   (atomic_val_t)(now == 0 ? 1U : now));
		}
		k_mutex_unlock(&data->lock);
	}

	/* Re-enable the DIO1 interrupt for the next event (unless sleeping) */
	if (atomic_get(&data->state) != SX126X_REST_STATE) {
		sx126x_hal_dio1_irq_enable(dev);
	}

	return rx_result;
}

static void sx126x_irq_work_handler(struct k_work *work)
{
	struct sx126x_data *data = CONTAINER_OF(work, struct sx126x_data, irq_work);
	const struct device *dev = data->dev;
	sx126x_airtime_observer_reconfigure_invalidate(dev);
	uint16_t irq_status = 0;

	if (sx126x_get_irq_status(dev, &irq_status) < 0) {
		LOG_ERR("Failed to get IRQ status");
		sx126x_observe_rx(data, 0, 0, false);
		return;
	}
	(void)sx126x_handle_irq_status(dev, irq_status);
}

static int sx126x_lora_config(const struct device *dev,
			      const struct lora_modem_config *config)
{
	struct sx126x_data *data = dev->data;
	const struct sx126x_hal_config *hal_config = dev->config;
	uint8_t bw_reg;
	bool ldro;
	int ret;
	const char *stage="validate";
	uint32_t diagnostic_stage=1, diagnostic_call=sx126x_stage_begin(data,&data->config_stage);
	bool diagnostic_recorded=false;

	ret = sx126x_validate_config(config);
	bench_record(dev,"sx126x_validate_config",ret,0);
	if (ret < 0) {
		sx126x_stage_fail(data,&data->config_stage,diagnostic_call,1,ret,&diagnostic_recorded);
		return ret;
	}

	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_IDLE)) {
		bench_record(dev,"config_busy",-EBUSY,atomic_get(&data->state));
		sx126x_stage_fail(data,&data->config_stage,diagnostic_call,2,-EBUSY,&diagnostic_recorded);
		return -EBUSY;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	sx126x_observe_reconfigure_reserve(data);
	sx126x_observe_config(dev, config, true, false);
	stage="sx126x_ensure_ready";diagnostic_stage=3;
	ret = sx126x_ensure_ready(dev);
	bench_record(dev,"sx126x_ensure_ready",ret,0);
	if (ret < 0) {
		sx126x_stage_fail(data,&data->config_stage,diagnostic_call,diagnostic_stage,ret,&diagnostic_recorded);
		sx126x_observe_config(dev, config, false, false);
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	/* Store configuration */
	memcpy(&data->config, config, sizeof(*config));

	/*
	 * SX126x requires a minimum preamble length of 12 symbols for SF5
	 * and SF6 (DS.SX1261-2.W.APP, Rev 2.2, section 6.1.1).
	 */
	if ((config->datarate == SF_5 || config->datarate == SF_6) &&
	    data->config.preamble_len < SX126X_SF56_MIN_PREAMBLE_LEN) {
		LOG_WRN("SF%d requires minimum %d preamble symbols, clamping from %d",
			config->datarate, SX126X_SF56_MIN_PREAMBLE_LEN,
			data->config.preamble_len);
		data->config.preamble_len = SX126X_SF56_MIN_PREAMBLE_LEN;
	}

	/* Run image calibration for frequency band */
	stage="sx126x_calibrate_image";diagnostic_stage=4;
	ret = sx126x_calibrate_image(dev, config->frequency);
	bench_record(dev,"sx126x_calibrate_image",ret,0);
	if (ret < 0) {
		goto out;
	}

	/* Set RF frequency */
	stage="sx126x_set_rf_frequency";diagnostic_stage=5;
	ret = sx126x_set_rf_frequency(dev, config->frequency);
	bench_record(dev,"sx126x_set_rf_frequency",ret,0);
	if (ret < 0) {
		goto out;
	}

	/* Install the band's RSSI/AGC calibration (DS Sec 6.1.6) before the
	 * RX gain write below, which consumes the AgcSensiAdjust this caches.
	 * Re-run on every config so a runtime frequency change switches bands
	 * instead of leaving the previous band's tunes installed. */
	stage="sx126x_apply_agc_band_cal";diagnostic_stage=6;
	ret = sx126x_apply_agc_band_cal(dev, config->frequency);
	bench_record(dev,"sx126x_apply_agc_band_cal",ret,0);
	if (ret < 0) {
		goto out;
	}

	/* Configure PA and TX power based on chip variant and frequency */
	stage="sx126x_hal_configure_tx_params";diagnostic_stage=7;
	ret = sx126x_hal_configure_tx_params(dev, config->tx_power,
						config->frequency,
						SX126X_TX_RAMP_TIME);
	bench_record(dev,"sx126x_hal_configure_tx_params",ret,0);
	if (ret < 0) {
		goto out;
	}

	/* Set modulation parameters */
	ret = bandwidth_to_reg(config->bandwidth, &bw_reg);
	__ASSERT_NO_MSG(ret == 0);
	ldro = should_enable_ldro(config->datarate, config->bandwidth, hal_config);
	stage="sx126x_set_modulation_params";diagnostic_stage=8;
	ret = sx126x_set_modulation_params(dev,
					   config->datarate,
					   bw_reg,
					   config->coding_rate,
					   ldro);
	bench_record(dev,"sx126x_set_modulation_params",ret,0);
	if (ret < 0) {
		goto out;
	}

	/* Sec 15.1 TX Modulation workaround (datasheet errata). */
	stage="sx126x_apply_tx_modulation_workaround";diagnostic_stage=9;
	ret = sx126x_apply_tx_modulation_workaround(dev, config->bandwidth);
	bench_record(dev,"sx126x_apply_tx_modulation_workaround",ret,0);
	if (ret < 0) {
		goto out;
	}

	/* Set sync word */
	stage="sx126x_set_sync_word";diagnostic_stage=10;
	ret = sx126x_set_sync_word(dev, config);
	bench_record(dev,"sx126x_set_sync_word",ret,0);
	if (ret==0 && IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH)) {
		uint8_t sync[2]={0};uint8_t word=config->sync_word ? config->sync_word : (config->public_network ? 0x34 : 0x12);
		stage="sync_readback";diagnostic_stage=11;
		ret=sx126x_hal_read_regs(dev,SX126X_REG_LORA_SYNC_WORD_MSB,sync,2);
	bench_record(dev,"sx126x_hal_read_regs",ret,0);
		bench_record(dev,"sync_read",ret,((uint32_t)word<<16)|sys_get_be16(sync));
		if (ret==0 && (sync[0]!=(uint8_t)(0x04|(word&0xf0)) || sync[1]!=(uint8_t)(0x04|((word&0x0f)<<4)))) ret=-EIO;
	}
	if (ret < 0) {
		goto out;
	}

	/* Set RX gain */
	stage="sx126x_set_rx_gain";diagnostic_stage=12;
	ret = sx126x_set_rx_gain(dev, hal_config->rx_boosted);
	bench_record(dev,"sx126x_set_rx_gain",ret,0);
	if (ret < 0) {
		goto out;
	}

	data->config_valid = true;
	LOG_DBG("Config: freq=%u, SF=%d, BW=%d, CR=%d, power=%d",
		config->frequency, config->datarate, config->bandwidth,
		config->coding_rate, config->tx_power);

out:
	sx126x_stage_fail(data,&data->config_stage,diagnostic_call,diagnostic_stage,ret,&diagnostic_recorded);
	sx126x_observe_config(dev, &data->config, ret == 0, true);
	bench_record(dev,stage,ret,0);
	sx126x_set_sleep(dev);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int sx126x_lora_cad(const struct device *dev, k_timeout_t timeout);
#ifdef CONFIG_MT_NORMAL_ENDPOINT
static int sx126x_lora_cad_mode(const struct device *dev, k_timeout_t timeout, bool handoff);
#endif
static uint32_t sx126x_cad_timeout_ms(struct sx126x_data *data);
static uint32_t sx126x_lora_airtime(const struct device *dev, uint32_t data_len);

static int sx126x_lora_send_async(const struct device *dev,
				  uint8_t *data_buf, uint32_t data_len,
				  struct k_poll_signal *async)
{
	struct sx126x_data *data = dev->data;
	uint32_t diagnostic_call=sx126x_stage_begin(data,&data->send_stage), diagnostic_stage=1;
	bool diagnostic_recorded=false;
	if (IS_ENABLED(CONFIG_MT_ONLY) && !IS_ENABLED(CONFIG_MT_ALLOW_TX)) {
		sx126x_stage_fail(data,&data->send_stage,diagnostic_call,1,-EPERM,&diagnostic_recorded);return -EPERM;
	}
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) && (!async || !bench_admit(dev,data_buf,data_len))) {
		sx126x_stage_fail(data,&data->send_stage,diagnostic_call,1,-EPERM,&diagnostic_recorded);return -EPERM;
	}
	int ret;

	if (!data->config_valid) {
		LOG_ERR("Not configured");
		sx126x_stage_fail(data,&data->send_stage,diagnostic_call,2,-EINVAL,&diagnostic_recorded);
		return -EINVAL;
	}

	if (data_len > SX126X_MAX_PAYLOAD_LEN) {
		LOG_ERR("Payload too long: %u", data_len);
		sx126x_stage_fail(data,&data->send_stage,diagnostic_call,3,-EINVAL,&diagnostic_recorded);
		return -EINVAL;
	}

	/* Entry: accept transition from either REST_STATE or RX to TX.
	 * RX -> TX is the Phase 2 fast path that lets us track was_rx and have
	 * the LBT branch restore RX in-driver on CAD-busy, saving the ~1-3 ms
	 * C++ round-trip.  Either CAS succeeds -> we own the state; both fail ->
	 * a concurrent TX is already in progress. */
	bool was_rx = false;

	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_TX)) {
		if (!atomic_cas(&data->state, SX126X_STATE_RX, SX126X_STATE_TX)) {
			LOG_ERR("Busy");
			sx126x_stage_fail(data,&data->send_stage,diagnostic_call,4,-EBUSY,&diagnostic_recorded);
			return -EBUSY;
		}
		was_rx = true;
	}

	/* TX is starting (or about to, after LBT): the RX-busy latch and
	 * preamble-grace timestamp are now stale.  If LBT-busy restores RX
	 * below, restart_rx will reset them again as part of the fresh RX
	 * cycle. */
	sx126x_reset_rx_busy_signals(data);

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = sx126x_ensure_ready(dev);
	if (ret < 0) {
		sx126x_stage_fail(data,&data->send_stage,diagnostic_call,5,ret,&diagnostic_recorded);
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	/* LBT: perform blocking CAD before transmitting */
	if (data->config.cad.mode == LORA_CAD_MODE_LBT) {
#ifdef CONFIG_MT_NORMAL_ENDPOINT
		const bool handoff = was_rx && data->rx_cb && !data->rx_duty_cycle_enabled;
#endif
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		const k_timeout_t timeout = K_MSEC(sx126x_cad_timeout_ms(data));
#ifdef CONFIG_MT_NORMAL_ENDPOINT
		int cad_ret = sx126x_lora_cad_mode(dev, timeout, handoff);
#else
		int cad_ret = sx126x_lora_cad(dev, timeout);
#endif
		if (cad_ret > 0) {
			sx126x_stage_fail(data,&data->send_stage,diagnostic_call,6,-EBUSY,&diagnostic_recorded);
			LOG_DBG("LBT: channel busy");
			k_mutex_lock(&data->lock, K_FOREVER);
			if (was_rx && data->rx_cb != NULL &&
			    atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
				sx126x_restart_rx(dev, data);
			}
			k_mutex_unlock(&data->lock);
			return -EBUSY;
		}
		if (cad_ret < 0 && cad_ret != -ENOSYS) {
			sx126x_stage_fail(data,&data->send_stage,diagnostic_call,7,cad_ret,&diagnostic_recorded);
			LOG_WRN("LBT: CAD failed (%d), restoring RX", cad_ret);
			k_mutex_lock(&data->lock, K_FOREVER);
			if (was_rx && data->rx_cb != NULL &&
			    atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
				sx126x_restart_rx(dev, data);
			}
			k_mutex_unlock(&data->lock);
			return cad_ret;
		}
		/* Re-acquire state and lock for TX */
		if (!atomic_cas(&data->state, SX126X_REST_STATE,
				SX126X_STATE_TX)) {
			LOG_ERR("Busy after CAD");
			sx126x_stage_fail(data,&data->send_stage,diagnostic_call,8,-EBUSY,&diagnostic_recorded);
			k_mutex_lock(&data->lock, K_FOREVER);
			if (was_rx && data->rx_cb != NULL &&
			    atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
				sx126x_restart_rx(dev, data);
			}
			k_mutex_unlock(&data->lock);
			return -EBUSY;
		}
		k_mutex_lock(&data->lock, K_FOREVER);
		ret = sx126x_ensure_ready(dev);
		if (ret < 0) {
			sx126x_stage_fail(data,&data->send_stage,diagnostic_call,9,ret,&diagnostic_recorded);
			k_mutex_unlock(&data->lock);
			atomic_set(&data->state, SX126X_REST_STATE);
			k_mutex_lock(&data->lock, K_FOREVER);
			if (was_rx && data->rx_cb != NULL &&
			    atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
				sx126x_restart_rx(dev, data);
			}
			k_mutex_unlock(&data->lock);
			return ret;
		}
	}

	data->tx_async_signal = async;
	k_msgq_purge(&data->tx_msgq);

	/* Set packet parameters */
	diagnostic_stage=10;ret = sx126x_set_packet_params(dev,
				       data->config.preamble_len,
				       SX126X_LORA_HEADER_EXPLICIT,
				       data_len,
				       data->config.packet_crc_disable ?
				       SX126X_LORA_CRC_OFF : SX126X_LORA_CRC_ON,
				       data->config.iq_inverted ?
				       SX126X_LORA_IQ_INVERTED : SX126X_LORA_IQ_STANDARD);
	if (ret < 0) {
		goto out_error;
	}

	/* Write payload to buffer */
	diagnostic_stage=11;ret = sx126x_hal_write_buffer(dev, 0x00, data_buf, data_len);
	if (ret < 0) {
		goto out_error;
	}

	diagnostic_stage=12;ret = sx126x_apply_tx_modulation_workaround(dev, data->config.bandwidth);
	if (ret < 0) {
		goto out_error;
	}

	/* Enable antenna and set TX path */
	sx126x_set_rf_path(dev, true, true);

	/* Re-assert PA config, OCP and TX power immediately before SetTx.
	 *
	 * sx126x_config() is otherwise the only writer of these three, and the
	 * C++ adapter's config cache (LoRaRadioBase::configure) short-circuits
	 * on every transmit after the first -- so between two lora_config()
	 * calls nothing re-verifies what the chip is actually holding.  The
	 * chip keeps them in warm-start retention RAM across its sleep cycles,
	 * which makes them precisely the state a disturbed SPI bus or a stray
	 * command corrupts silently: the node goes on relaying, because
	 * restart_rx re-issues SetRx constantly, while transmitting tens of dB
	 * down until someone reboots it.  That is not hypothetical -- ESP32
	 * light sleep leaving NSS undriven produced exactly it; the cause is
	 * fixed at source in boards/common/pm_esp32.overlay and this is the
	 * belt-and-braces half that stops the class of fault rather than the
	 * one instance.
	 *
	 * Costs no receive time.  It runs after LBT has passed, with the chip
	 * already committed to TX, and is ~15 bytes of SPI -- unlike forcing a
	 * full lora_config(), which would have to tear RX down first (its
	 * entry CAS rejects SX126X_STATE_RX) and re-run image calibration.
	 *
	 * Keep the ramp time identical to the sx126x_config() call site: two
	 * writers disagreeing on it would make TX spectral behaviour depend on
	 * which one ran last. */
	diagnostic_stage=13;ret = sx126x_hal_configure_tx_params(dev, data->config.tx_power,
					     data->config.frequency,
					     SX126X_TX_RAMP_TIME);
	if (ret < 0) {
		goto out_error;
	}

#ifdef CONFIG_LORA_SX126X_NATIVE_STANDALONE /* STM32WL HAL clamps in its own configure_tx_params; its config has no is_sx1261 */
	/* Sec 15.2 TX Clamp workaround (datasheet errata) -- SX1262 only.
	 * Set bits [4:1] of register 0x08D8 before SetTx to prevent
	 * PA overshoot that can damage the device. */
	{
		const struct sx126x_hal_config *hal_cfg = dev->config;

		if (!hal_cfg->is_sx1261) {
			uint8_t clamp;

			diagnostic_stage=14;ret = sx126x_hal_read_regs(dev, SX126X_REG_TX_CLAMP_CFG,
						   &clamp, 1);
			if (ret < 0) {
				goto out_error;
			}
			clamp |= 0x1E;
			diagnostic_stage=15;ret = sx126x_hal_write_regs(dev, SX126X_REG_TX_CLAMP_CFG,
						    &clamp, 1);
			if (ret < 0) {
				goto out_error;
			}
		}
	}
#endif /* CONFIG_LORA_SX126X_NATIVE_STANDALONE */

	/* Chip-side Tx safeguard, scaled from airtime.
	 *
	 * DS §13.1.5: when the Tx timer fires the transmission is stopped, so
	 * this has to exceed real airtime — and a fixed 10 s cannot. A 255-byte
	 * packet at CR 4/8 is 17.7 s at SF10/BW31.25 and 28.6 s at SF12/BW62.5;
	 * on those presets the old constant truncated every packet from 136 B
	 * and 76 B up respectively, mid-transmission and with nothing in the log
	 * tying the loss to this line.
	 *
	 * Floored at the previous 10000 so no preset that works today gets a
	 * tighter deadline, and clamped to the 24-bit field: sx126x_set_tx()
	 * converts with SX126X_MS_TO_TIMEOUT (64 steps/ms at 15.625 us) and then
	 * sys_put_be24(), which would silently truncate above 262143 ms. That
	 * ceiling is 262 s against a worst reachable airtime of ~229 s
	 * (SF12/BW7.81, 255 B), so the clamp never binds on a real packet. */
	{
		uint32_t air_ms = sx126x_lora_airtime(dev, data_len);
		uint32_t tmo_ms = air_ms + (air_ms / 4U) + 500U;

		if (tmo_ms < 10000U) {
			tmo_ms = 10000U;
		}
		if (tmo_ms > 262143U) {
			tmo_ms = 262143U;
		}
		LOG_DBG("SET_TX: airtime=%u ms, timeout=%u ms", air_ms, tmo_ms);
		if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH)) tmo_ms=sx126x_bench_tx_timeout();
		diagnostic_stage=16;ret = sx126x_set_tx(dev, tmo_ms);
	}
	if (ret < 0) {
		goto out_error;
	}

	k_mutex_unlock(&data->lock);
	return 0;

out_error:
	sx126x_stage_fail(data,&data->send_stage,diagnostic_call,diagnostic_stage,ret,&diagnostic_recorded);
	data->tx_async_signal = NULL;
	sx126x_set_sleep(dev);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int sx126x_lora_send(const struct device *dev,
			    uint8_t *data_buf, uint32_t data_len)
{
	struct sx126x_data *data = dev->data;
	struct sx126x_tx_result result;
	int ret;

	ret = sx126x_lora_send_async(dev, data_buf, data_len, NULL);
	if (ret < 0) {
		return ret;
	}

	/* Wait for TX completion */
	ret = k_msgq_get(&data->tx_msgq, &result, K_SECONDS(15));
	if (ret < 0) {
		LOG_ERR("TX timeout");
		/* Chip is still transmitting, abort first */
		sx126x_set_standby(dev, SX126X_STANDBY_RC);
		sx126x_set_sleep(dev);
		return -ETIMEDOUT;
	}

	return result.status;
}

static int sx126x_lora_recv(const struct device *dev, uint8_t *data_buf,
			    uint8_t size, k_timeout_t timeout,
			    int16_t *rssi, int8_t *snr)
{
	struct sx126x_data *data = dev->data;
	struct sx126x_rx_result result;
	uint32_t timeout_ms;
	int ret;

	if (!data->config_valid) {
		LOG_ERR("Not configured");
		return -EINVAL;
	}

	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
		LOG_ERR("Busy");
		return -EBUSY;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = sx126x_ensure_ready(dev);
	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	data->rx_cb = NULL;
	data->rx_cb_user_data = NULL;
	atomic_inc(&data->rx_cb_gen);
	k_msgq_purge(&data->rx_msgq);

	/* Set packet parameters for variable length reception */
	ret = sx126x_set_packet_params(dev,
				       data->config.preamble_len,
				       SX126X_LORA_HEADER_EXPLICIT,
				       SX126X_MAX_PAYLOAD_LEN,
				       data->config.packet_crc_disable ?
				       SX126X_LORA_CRC_OFF : SX126X_LORA_CRC_ON,
				       data->config.iq_inverted ?
				       SX126X_LORA_IQ_INVERTED : SX126X_LORA_IQ_STANDARD);
	if (ret < 0) {
		sx126x_set_sleep(dev);
		k_mutex_unlock(&data->lock);
		return ret;
	}

	/* Enable antenna and set RX path */
	sx126x_set_rf_path(dev, true, false);

	/* Start reception (0 = continuous for K_FOREVER) */
	timeout_ms = K_TIMEOUT_EQ(timeout, K_FOREVER)
		     ? 0 : k_ticks_to_ms_ceil32(timeout.ticks);
	ret = sx126x_set_rx(dev, timeout_ms);
	if (ret < 0) {
		sx126x_set_sleep(dev);
		k_mutex_unlock(&data->lock);
		return ret;
	}

	k_mutex_unlock(&data->lock);

	/* Wait for RX completion */
	ret = k_msgq_get(&data->rx_msgq, &result, timeout);
	if (ret < 0) {
		LOG_DBG("RX timeout");
		/* Chip is still receiving, abort first */
		sx126x_set_standby(dev, SX126X_STANDBY_RC);
		sx126x_set_sleep(dev);
		return -EAGAIN;
	}

	/* Copy received data from shared buffer */
	if (result.status > 0) {
		int copy_len = MIN(result.status, size);

		memcpy(data_buf, data->rx_buf, copy_len);
		if (rssi != NULL) {
			*rssi = result.rssi;
		}
		if (snr != NULL) {
			*snr = result.snr;
		}
		return copy_len;
	}

	return result.status;
}

#ifdef CONFIG_MT_NORMAL_ENDPOINT
/* Called with the driver mutex held. A previous ordinary owner has already
 * joined DIO/watchdog work, frozen RX, delivered any completed frame and
 * retired its callbacks. This does not take a listening modem off the air. */
static bool sx126x_calibrate_joined(const struct device *dev)
{
	const struct sx126x_data *data = dev->data;
	const struct sx126x_hal_config *config = dev->config;
	uint8_t sleep = SX126X_SLEEP_WARM_START;
	uint8_t patch, verified;

	if (sx126x_set_standby(dev, SX126X_STANDBY_RC) < 0 ||
	    sx126x_hal_write_cmd(dev, SX126X_CMD_SET_SLEEP, &sleep, 1) < 0)
		return false;
	/* HAL waits for the warm-sleep transition before releasing the bus. */
	if (sx126x_hal_wakeup(dev) < 0 || sx126x_calibrate(dev, SX126X_CALIBRATE_ALL) < 0)
		return false;
	k_msleep(5);
	if (sx126x_hal_wait_busy(dev, 50) < 0 ||
	    sx126x_calibrate_image(dev, data->config.frequency) < 0)
		return false;
	/* Image calibration outlives BUSY; an early register write can fail. */
	k_msleep(50);
	if (config->dio2_tx_enable && sx126x_set_dio2_as_rf_switch(dev, true) < 0)
		return false;
	if (sx126x_add_reg_to_retention(dev, SX126X_REG_RX_GAIN) < 0 ||
	    sx126x_add_reg_to_retention(dev, SX126X_REG_TX_MODULATION) < 0 ||
	    sx126x_set_rx_gain(dev, data->rx_boost_enabled) < 0 ||
	    sx126x_hal_read_regs(dev, 0x08b5, &patch, 1) < 0)
		return false;
	patch |= 1;
	return sx126x_hal_write_regs(dev, 0x08b5, &patch, 1) == 0 &&
	       sx126x_hal_read_regs(dev, 0x08b5, &verified, 1) == 0 && verified == patch;
}
#endif

static int sx126x_recv_async_common(const struct device *dev, lora_recv_cb cb,
				  sx126x_recv_packet_cb packet_cb, void *user_data, bool *calibrated)
{
	struct sx126x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (cb == NULL) {
		sx126x_observe_rx(data, 0, 0, false);
		/* Stop async reception */
		data->rx_cb = NULL;
		data->rx_packet_cb = NULL;
		data->rx_cb_user_data = NULL;
		atomic_inc(&data->rx_cb_gen);
		sx126x_reset_rx_busy_signals(data);
		if (atomic_cas(&data->state, SX126X_STATE_RX, SX126X_STATE_IDLE)) {
			sx126x_set_standby(dev, SX126X_STANDBY_RC);
			sx126x_set_sleep(dev);
		}
		k_mutex_unlock(&data->lock);
		return 0;
	}

	if (!data->config_valid) {
		LOG_ERR("Not configured");
		k_mutex_unlock(&data->lock);
		bench_record(dev,"rx_unconfigured",-EINVAL,0);return -EINVAL;
	}

	/* Phase 2 idempotent fast-path: if the driver is already in RX (e.g.,
	 * because the LBT path in send_async restored RX on CAD-busy), just
	 * refresh the callback and return.  The chip is in valid RX, the
	 * IRQ register has been cleared by the prior restart_rx, and the
	 * software latch has been reset.  No re-init needed. */
	if (atomic_get(&data->state) == SX126X_STATE_RX) {
		data->rx_cb = cb;
		data->rx_packet_cb = packet_cb;
		data->rx_cb_user_data = user_data;
		atomic_inc(&data->rx_cb_gen);
		k_mutex_unlock(&data->lock);
		return 0;
	}

#ifdef CONFIG_MT_NORMAL_ENDPOINT
	if (calibrated && (data->rx_cb || data->rx_duty_cycle_enabled ||
	    data->cad_active || data->tx_async_signal || k_work_busy_get(&data->irq_work) ||
	    k_work_busy_get(&data->dc_watchdog_work.work))) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
#endif
	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
		LOG_ERR("Busy");
		k_mutex_unlock(&data->lock);
		bench_record(dev,"rx_busy",-EBUSY,0);return -EBUSY;
	}

	ret = sx126x_ensure_ready(dev);
	bench_record(dev,"sx126x_ensure_ready",ret,0);
	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		sx126x_observe_rx(data, 0, 0, false);return ret;
	}

#ifdef CONFIG_MT_NORMAL_ENDPOINT
	if (calibrated != NULL) {
		*calibrated = sx126x_calibrate_joined(dev);
		if (!*calibrated) {
			/* A failed warm-sleep wake is the only point that may have left
			 * the chip asleep. Restore standby before the ordinary RX arm. */
			ret = sx126x_hal_wakeup(dev);
			if (ret == 0)
				ret = sx126x_set_standby(dev, SX126X_STANDBY_RC);
			if (ret < 0) {
				(void)sx126x_set_sleep(dev);
				k_mutex_unlock(&data->lock);
				return ret;
			}
		}
	}
#else
	(void)calibrated;
#endif
	/* Fresh RX start: bulk-clear any stale IRQ bits (e.g., a stuck
	 * PREAMBLE_DETECTED that survived an error path) and reset the latch
	 * + preamble-grace timestamp.  Without this, recoverRxState() from
	 * the dispatcher cannot actually recover -- the chip would come back
	 * into RX with the same bits set. */
	ret=sx126x_clear_irq_status(dev, SX126X_IRQ_ALL);
	if(ret<0) { atomic_set(&data->state,SX126X_REST_STATE);k_mutex_unlock(&data->lock);sx126x_observe_rx(data, 0, 0, false);return ret; }
	sx126x_reset_rx_busy_signals(data);

	data->rx_cb = cb;
	data->rx_packet_cb = packet_cb;
	data->rx_cb_user_data = user_data;
	atomic_inc(&data->rx_cb_gen);
	data->rx_duty_cycle_enabled = false;

	/* Set packet parameters */
	ret = sx126x_set_packet_params(dev,
				       data->config.preamble_len,
				       SX126X_LORA_HEADER_EXPLICIT,
				       SX126X_MAX_PAYLOAD_LEN,
				       data->config.packet_crc_disable ?
				       SX126X_LORA_CRC_OFF : SX126X_LORA_CRC_ON,
				       data->config.iq_inverted ?
				       SX126X_LORA_IQ_INVERTED : SX126X_LORA_IQ_STANDARD);
	bench_record(dev,"sx126x_set_packet_params",ret,0);
	if (ret < 0) {
		data->rx_cb = NULL;
		atomic_inc(&data->rx_cb_gen);
		sx126x_set_sleep(dev);
		k_mutex_unlock(&data->lock);
		sx126x_observe_rx(data, 0, 0, false);return ret;
	}

	/* Enable antenna and set RX path */
	sx126x_set_rf_path(dev, true, false);

	/* Start continuous reception */
	ret = sx126x_set_rx(dev, 0);
	bench_record(dev,"sx126x_set_rx",ret,0);
	if (ret < 0) {
		data->rx_cb = NULL;
		data->rx_cb_user_data = NULL;
		atomic_inc(&data->rx_cb_gen);
		sx126x_set_sleep(dev);
		k_mutex_unlock(&data->lock);
		sx126x_observe_rx(data, 0, 0, false);return ret;
	}

	/* Re-apply RX gain after SetRx -- register 0x08AC is reset on every
	 * SetRx without retention, and lora_config sets gain before going to
	 * sleep, so without this the first packet would be received in
	 * power-saving gain regardless of DTS rx-boosted. */
	uint8_t gain = sx126x_rx_gain_byte(dev, data->rx_boost_enabled);

	ret=sx126x_hal_write_regs(dev, SX126X_REG_RX_GAIN, &gain, 1);
	if(ret<0) { data->rx_cb=NULL;atomic_inc(&data->rx_cb_gen);(void)sx126x_set_sleep(dev);k_mutex_unlock(&data->lock);sx126x_observe_rx(data, 0, 0, false);return ret; }

	k_mutex_unlock(&data->lock);
	bench_record(dev,"rx_started",0,0);
	return 0;
}

static int sx126x_lora_recv_async(const struct device *dev,
				  lora_recv_cb cb, void *user_data)
{
	return sx126x_recv_async_common(dev, cb, NULL, user_data, NULL);
}

/* State marker only: the metadata callback is captured beside this marker and
 * invoked directly with packet-local status after the same generation check. */
static void sx126x_packet_callback_marker(const struct device *dev,
		uint8_t *payload, uint16_t length, int16_t rssi, int8_t snr, void *user_data)
{
	(void)dev;(void)payload;(void)length;(void)rssi;(void)snr;(void)user_data;
}

int sx126x_recv_async_packet(const struct device *dev,
			    sx126x_recv_packet_cb cb, void *user_data)
{
	if (dev == NULL || dev->data == NULL) {
		return -EINVAL;
	}
	return sx126x_recv_async_common(dev, cb ? sx126x_packet_callback_marker : NULL,
					cb, user_data, NULL);
}

#ifdef CONFIG_MT_NORMAL_ENDPOINT
int sx126x_recv_after_agc(const struct device *dev, sx126x_recv_packet_cb cb,
			  void *user_data, bool *calibrated)
{
	if (!device_is_ready(dev) || !cb || !calibrated)
		return -EINVAL;
	*calibrated = false;
	return sx126x_recv_async_common(dev, sx126x_packet_callback_marker, cb, user_data, calibrated);
}
#endif

/*
 * Read this if you have trouble making the duty cycle working reliably.
 * I have wasted many hours on this so I'm passing this along.
 *
 * The driver sets StopTimerOnPreamble = true before entering duty
 * cycle mode. This makes the radio stay in RX once a preamble is
 * detected, instead of going back to sleep when the RX window
 * expires. The radio then searches for the sync word within a
 * window sized by the receiver's preamble_len. This value must
 * match the sender's: a mismatch causes the demodulator to stop
 * searching before the sync word arrives.
 *
 * Matching preamble_len — OK (works even if RX joins mid-preamble)
 *
 *   TX  |#########< 100 preamble symbols >###########|SW|HDR| payload |
 *                       ^                            ^
 *   RX  ..sleep..|==RX==|<<< stays in RX >>>>>>>>>>>>|
 *                   ^                                |
 *                   preamble detected                |
 *                   timer stops (StopTimerOnPreamble)|
 *                   search window = 100 symbols      |
 *                   sync word found -----------------+
 *                   --> full packet received, RX_DONE
 *
 * Mismatched preamble_len (RX = 8, TX = 100) — FAIL
 *
 *   TX  |#########< 100 preamble symbols >###########|SW|HDR| payload |
 *                       ^                            ^
 *   RX  ..sleep..|==RX==|<<< stays in RX             |
 *                   ^        but gives up here       |
 *                   |        |<-8->|                 |
 *                   preamble detected                |
 *                   search window = only 8 symbols   |
 *                   sync word NOT found within window
 *                   --> demodulator fails, packet lost
 *
 * See samples/drivers/lora/duty_cycle/
 */
static void sx126x_duty_cycle_stop(const struct device *dev)
{
	/*
	 * During duty cycle the BUSY pin stays asserted even in the
	 * sleep phase. Wake the radio via a raw NSS edge (bypasses
	 * BUSY check) so the standby command can go through
	 * immediately.
	 */
	if (sx126x_hal_is_busy(dev)) {
		sx126x_hal_wakeup(dev);
	}
	sx126x_set_standby(dev, SX126X_STANDBY_RC);
	sx126x_set_sleep(dev);
}

static uint32_t sx126x_lora_airtime(const struct device *dev, uint32_t data_len)
{
	struct sx126x_data *data = dev->data;
	uint32_t t_preamble_us, t_payload_us, t_sym_us, n_payload, bw_hz;
	uint8_t sf, cr;
	int32_t tmp;
	bool de, crc;
	int ret;

	if (!data->config_valid) {
		return 0;
	}

	/* Calculate symbol time in microseconds */
	ret = bandwidth_to_hz(data->config.bandwidth, &bw_hz);
	__ASSERT_NO_MSG(ret == 0);
	sf = data->config.datarate;

	/* Symbol time = 2^SF / BW (seconds) */
	/* In microseconds: (2^SF * 1000000) / BW */
	t_sym_us = ((1UL << sf) * 1000000UL) / bw_hz;

	/* Preamble time (4.25 extra symbols) */
	t_preamble_us = (data->config.preamble_len + 4) * t_sym_us +
			(t_sym_us / 4);

	/* Payload symbol count calculation (from LoRa modem designer's guide) */
	de = should_enable_ldro(sf, data->config.bandwidth, dev->config);
	crc = !data->config.packet_crc_disable;
	cr = data->config.coding_rate;

	tmp = 8 * data_len - 4 * sf + 28 + 16 * crc;
	if (tmp < 0) {
		tmp = 0;
	}

	n_payload = 8 + (((tmp + 4 * (sf - 2 * de) - 1) /
			  (4 * (sf - 2 * de))) * (cr + 4));
	t_payload_us = n_payload * t_sym_us;

	/* Total airtime in milliseconds */
	return (t_preamble_us + t_payload_us + 500) / 1000;
}

static int sx126x_lora_rssi(const struct device *dev, int16_t *rssi)
{
	uint8_t buf[1];
	int ret;

	ret = sx126x_hal_read_cmd(dev, SX126X_CMD_GET_RSSI_INST, buf, 1);
	if (ret == 0) {
		/* RSSI is -value/2 dBm */
		*rssi = -((int16_t)buf[0] >> 1);
	}

	return ret;
}

static int sx126x_lora_test_cw(const struct device *dev, uint32_t frequency,
			       int8_t tx_power, uint16_t duration)
{
	if (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || IS_ENABLED(CONFIG_MT_ONLY)) return -EPERM;
	struct sx126x_data *data = dev->data;
	int ret;

	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_TX)) {
		return -EBUSY;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = sx126x_ensure_ready(dev);
	if (ret < 0) {
		goto out_unlock;
	}

	/* Set frequency */
	ret = sx126x_set_rf_frequency(dev, frequency);
	if (ret < 0) {
		goto out_sleep;
	}

	/* Set PA config and TX power */
	ret = sx126x_hal_configure_tx_params(dev, tx_power, frequency,
						SX126X_TX_RAMP_TIME);
	if (ret < 0) {
		goto out_sleep;
	}

	/* Enable antenna and TX path */
	sx126x_set_rf_path(dev, true, true);

	/* Start CW transmission */
	ret = sx126x_hal_write_cmd(dev, SX126X_CMD_SET_TX_CONTINUOUS_WAVE, NULL, 0);
	if (ret < 0) {
		goto out_sleep;
	}

	k_mutex_unlock(&data->lock);

	/* Wait for duration */
	k_sleep(K_SECONDS(duration));

	/* Stop CW */
	k_mutex_lock(&data->lock, K_FOREVER);
	sx126x_set_standby(dev, SX126X_STANDBY_RC);

out_sleep:
	sx126x_set_sleep(dev);
out_unlock:
	k_mutex_unlock(&data->lock);
	atomic_set(&data->state, SX126X_REST_STATE);

	return ret;
}

/* -- Extension API (sx126x_ext.h) -- */

bool sx126x_is_chip_busy(const struct device *dev)
{
	return sx126x_hal_is_busy(dev);
}

int16_t sx126x_get_rssi_inst(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	uint8_t buf[1];
	int ret;

	/* GPIO read -- bail before stalling wait_busy on the duty-cycle
	 * sleep phase. wait_busy's recovery would force-wake the chip and
	 * break duty cycle. */
	if (sx126x_hal_is_busy(dev)) {
		return -128;
	}
	if (k_mutex_lock(&data->lock, K_NO_WAIT) != 0) {
		return -128;
	}

	if (sx126x_hal_is_busy(dev)) {
		k_mutex_unlock(&data->lock);
		return -128;
	}

	ret = sx126x_hal_read_cmd(dev, SX126X_CMD_GET_RSSI_INST, buf, 1);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		return -128;
	}

	return -((int16_t)buf[0] >> 1);
}

/* Shared by the ordinary MC adapter and the dual-radio TX broker.
 * A raw preamble is only a short grace period; the worker's HEADER_VALID
 * latch protects the entire payload after the raw header IRQ is cleared.
 * Return negative SPI/GPIO errors to the broker; the bool API stays busy. */
/* The low five bits name this observation; upper seven bits retain the
 * magnitude of a negative result, clamped to127. Only RAM scalar output is
 * added: no extra SPI/RTOS operation, observer retry or altered return value. */
static int sx126x_activity_result(uint16_t *detail, uint16_t reason, int result)
{
	if (detail != NULL) {
		uint32_t error = result < 0 ? (uint32_t)(-(int64_t)result) : 0;
		if (error > 127U) error = 127U;
		*detail = (uint16_t)((reason & 31U) | (error << 5));
	}
	return result;
}

static bool sx126x_channel_stale_uncertain(uint16_t stale_mask, uint16_t latest)
{
	uint16_t preamble = (uint16_t)SX126X_IRQ_PREAMBLE_DETECTED;

	if (stale_mask != preamble)
		return true;
	if ((uint16_t)(latest & ~preamble) != 0)
		return true;
	return false;
}

static int sx126x_rx_activity_detail(const struct device *dev, uint16_t *detail)
{
	struct sx126x_data *data;
	uint32_t now, seen, bound;
	atomic_t *stamp;
	uint16_t irq = 0, latest = 0, stale_mask;
	int busy, ret = 1;
	uint16_t reason = SX126X_RX_BUSY_CLEAR, kind = 0;
	bool observe_stale;

	if (!device_is_ready(dev)) {
		return sx126x_activity_result(detail, SX126X_RX_BUSY_NOT_READY, -ENODEV);
	}
	data = dev->data;
	busy = sx126x_hal_is_busy(dev);
	if (busy != 0) {
		return sx126x_activity_result(detail, busy < 0 ? SX126X_RX_BUSY_GPIO_ERROR : SX126X_RX_BUSY_CHIP, busy < 0 ? busy : 1);
	}
	if (k_mutex_lock(&data->lock, K_NO_WAIT) != 0) {
		return sx126x_activity_result(detail, SX126X_RX_BUSY_MUTEX, 1);
	}

	/* The worker copies RX buffers and resets latches outside this mutex.
	 * Never declare clear while it is pending, running or cancelling.
	 */
	if (k_work_busy_get(&data->irq_work) != 0) {
		reason = SX126X_RX_BUSY_WORK;
		goto out;
	}
	ret = sx126x_get_irq_status(dev, &irq);
	if (ret < 0) {
		sx126x_observe_rx(data, 0, 0, false);
		reason = SX126X_RX_BUSY_STATUS_ERROR;
		goto out;
	}

	if (!(irq & SX126X_IRQ_RX_DONE) &&
	    (irq & (SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR)))
		sx126x_observe_rx(data, irq, 0, false);

	/* Preserve completed packets awaiting service, even past a deadline.
	 * The per-device worker can recover a missed terminal interrupt edge.
	 */
	if ((irq & SX126X_IRQ_RX_DONE) != 0) {
		sx126x_airtime_observer_reconfigure_invalidate(dev);
		ret = k_work_submit_to_queue(&data->dio1_wq, &data->irq_work);
		if (ret < 0) sx126x_observe_loss(data);
		reason = ret < 0 ? SX126X_RX_BUSY_DISPATCH_ERROR : SX126X_RX_BUSY_RX_DONE;
		if (ret >= 0) {
			ret = 1;
		}
		goto out;
	}

	now = k_uptime_get_32();
	if (data->rx_packet_active) {
		kind = SX126X_RX_BUSY_ACTIVE_FIRST;
		stamp = &data->header_seen_at_ms;
		bound = sx126x_max_payload_ms(data);
		stale_mask = SX126X_IRQ_PREAMBLE_DETECTED | SX126X_IRQ_SYNC_WORD_VALID |
			     SX126X_IRQ_HEADER_VALID | SX126X_IRQ_HEADER_ERR;
	} else if ((irq & SX126X_IRQ_HEADER_VALID) != 0) {
		kind = SX126X_RX_BUSY_HEADER_FIRST;
		stamp = &data->raw_header_seen_at_ms;
		bound = sx126x_max_payload_ms(data);
		stale_mask = SX126X_IRQ_PREAMBLE_DETECTED | SX126X_IRQ_SYNC_WORD_VALID |
			     SX126X_IRQ_HEADER_VALID | SX126X_IRQ_HEADER_ERR;
	} else if ((irq & SX126X_IRQ_PREAMBLE_DETECTED) != 0) {
		kind = SX126X_RX_BUSY_PREAMBLE_FIRST;
		stamp = &data->preamble_seen_at_ms;
		bound = sx126x_preamble_grace_ms(data);
		stale_mask = SX126X_IRQ_PREAMBLE_DETECTED;
	} else {
		atomic_set(&data->preamble_seen_at_ms, 0);
		atomic_set(&data->raw_header_seen_at_ms, 0);
		ret = 0;
		goto out;
	}

	seen = (uint32_t)atomic_get(stamp);
	if (seen == 0) {
		reason = kind;
		atomic_set(stamp, (atomic_val_t)(now == 0 ? 1U : now));
		ret = 1;
		goto out;
	}
	/* Worker timestamps use 1 as the time-zero sentinel. */
	if ((now == 0 && seen == 1) || (uint32_t)(now - seen) < bound) {
		reason = kind + 3;
		ret = 1;
		goto out;
	}

	/* Re-snapshot before clearing: a header, completion or worker may have
	 * arrived since the first read. Never clear RX_DONE or the FIFO.
	 */
	if (k_work_busy_get(&data->irq_work) != 0) {
		reason = SX126X_RX_BUSY_RECHECK_WORK;
		ret = 1;
		goto out;
	}
	ret = sx126x_get_irq_status(dev, &latest);
	if (ret < 0) {
		sx126x_observe_rx(data, 0, 0, false);
		reason = SX126X_RX_BUSY_RECHECK_ERROR;
		goto out;
	}
	if ((latest & SX126X_IRQ_RX_DONE) != 0 ||
	    (stale_mask == SX126X_IRQ_PREAMBLE_DETECTED &&
	     ((latest & SX126X_IRQ_HEADER_VALID) != 0 || data->rx_packet_active))) {
		reason = (latest & SX126X_IRQ_RX_DONE) != 0 ? SX126X_RX_BUSY_LATE_DONE : SX126X_RX_BUSY_LATE_HEADER;
		ret = 1;
		goto out;
	}
	/* A bounded stale preamble with no header/terminal IRQ is not a packet.
	 * Do not restart channel-quality recovery merely for this busy-latch
	 * cleanup. Preserve uncertainty for every other IRQ and for clear errors.
	 * All native rechecks, grace periods, HAL calls and return values stay put.
	 */
	observe_stale = sx126x_channel_stale_uncertain(stale_mask, latest);
	if (observe_stale) sx126x_observe_rx(data, latest, 0, false);
	ret = sx126x_clear_irq_status(dev, stale_mask);
	if (ret < 0) {
		if (!observe_stale) sx126x_observe_rx(data, latest, 0, false);
		reason = SX126X_RX_BUSY_CLEAR_ERROR;
		goto out;
	}
	if (stale_mask == SX126X_IRQ_PREAMBLE_DETECTED) {
		atomic_set(&data->preamble_seen_at_ms, 0);
	} else {
		sx126x_reset_rx_busy_signals(data);
	}
	atomic_inc(&data->rx_stale_releases);
	ret = 0;

out:
	k_mutex_unlock(&data->lock);
	return sx126x_activity_result(detail, reason, ret);
}

static int sx126x_rx_activity(const struct device *dev)
{
	return sx126x_rx_activity_detail(dev, NULL);
}

bool sx126x_is_receiving_diagnostic(const struct device *dev, uint16_t *detail)
{
	return sx126x_rx_activity_detail(dev, detail) != 0;
}

bool sx126x_is_receiving(const struct device *dev)
{
	return sx126x_rx_activity(dev) != 0;
}


void sx126x_set_rx_boost(const struct device *dev, bool enable)
{
	struct sx126x_data *data = dev->data;

	data->rx_boost_enabled = enable;

	k_mutex_lock(&data->lock, K_FOREVER);
	sx126x_set_rx_gain(dev, enable);
	k_mutex_unlock(&data->lock);
}

bool sx126x_set_fem_rx_enable(const struct device *dev, bool enable)
{
	struct sx126x_data *data = dev->data;
	const struct sx126x_hal_config *config = dev->config;

	/* False when the board wires no receive-path select: there is no FEM
	 * RX gain to trade away, so the caller can say so instead of
	 * acknowledging a no-op.  Boards whose only FEM line is the chip
	 * enable land here too -- dropping that would not lower the gain, it
	 * would shut the part down and open the receive path entirely. */
	if (config->lna_bypass.port == NULL) {
		/* Still record it: a board can gain the line later without the
		 * caller having to re-apply the pref. */
		data->fem_rx_off = !enable;
		return false;
	}

	/* Under data->lock, which is what sx126x_set_rf_path()'s callers hold.
	 * This runs on the CLI thread: without it, a state read of RX can be
	 * overtaken by send_async() taking the chip to TX and driving the line
	 * HIGH, after which the write below pulls it back LOW and leaves the
	 * FEM's LNA in circuit for the whole transmission. */
	k_mutex_lock(&data->lock, K_FOREVER);

	/* Stored inverted so the zero-initialised default is "LNA in the RX
	 * path", which is what the driver did before this knob existed. */
	data->fem_rx_off = !enable;

	/* Apply now rather than at the next sx126x_set_rf_path().  This is a
	 * bare GPIO with no SPI and no chip state behind it, and a node parked
	 * in continuous RX may not transition for minutes -- long enough for
	 * the CLI to have acknowledged a change the radio had not made.  Only
	 * in RX: every other state either drives the line itself on the way in
	 * (TX) or wants it left where sleep parked it. */
	if (atomic_get(&data->state) == SX126X_STATE_RX) {
		gpio_pin_set_dt(&config->lna_bypass, data->fem_rx_off);
	}

	k_mutex_unlock(&data->lock);

	return true;
}

void sx126x_reset_agc(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	const struct sx126x_hal_config *config = dev->config;

	/* Bounded wait: a long TX/RX (e.g. SF12 packet ~3 s airtime) holds
	 * data->lock for its full duration.  K_FOREVER here would stall the
	 * caller (typically the dispatcher main thread) for that whole time
	 * and freeze packet routing.  Instead, give up after 50 ms and let
	 * the caller retry on the next maintenance interval. */
	if (k_mutex_lock(&data->lock, K_MSEC(50)) != 0) {
		LOG_WRN("reset_agc: mutex busy, skipping this cycle");
		return;
	}

	/* Stand the chip down before commanding it.  In duty-cycle mode it
	 * spends most of its time in the DC sleep phase with BUSY asserted, and
	 * the HAL's wait_busy() then burns its full 1000 ms timeout before the
	 * wake+retry recovery fires -- observed on hardware as
	 * "Busy timeout after 1000 ms, wake+retry" immediately after every AGC
	 * reset, i.e. a full second of dead radio each time.  A raw NSS poke
	 * wakes it without waiting on BUSY, the same trick the duty-cycle
	 * re-arm path in recv_duty_cycle() already uses. */
	if (sx126x_hal_is_busy(dev)) {
		sx126x_hal_wakeup(dev);
	}

	/* Warm sleep -- powers down the analog frontend (resets AGC state)
	 * but preserves register configuration. */
	uint8_t sleep_cfg = SX126X_SLEEP_WARM_START;

	sx126x_hal_write_cmd(dev, SX126X_CMD_SET_SLEEP, &sleep_cfg, 1);
	k_busy_wait(500);

	/* Wake to STANDBY_RC -- required before Calibrate.
	 *
	 * MUST be a raw NSS edge, never sx126x_set_standby().  The chip is in
	 * sleep here by construction -- we just put it there -- and in sleep
	 * BUSY is held HIGH, so sx126x_hal_write_cmd()'s wait_busy() burns its
	 * full 1000 ms timeout before the wake+retry recovery fires.  That is a
	 * full second of dead radio on every AGC reset, and it was visible on
	 * hardware as "Busy timeout after 1000 ms, wake+retry" immediately after
	 * every "agc: ... resetting AGC" line.
	 *
	 * sx126x_hal_wakeup() drives the NSS edge directly and the chip
	 * initialises itself into STDBY_RC with its configuration retained
	 * (DS sec 13.1.1); sx126x_ensure_ready() depends on exactly this
	 * property and documents the same trap. */
	sx126x_hal_wakeup(dev);

	/* Full recalibration: ADC, PLL, image, RC oscillators */
	sx126x_calibrate(dev, SX126X_CALIBRATE_ALL);
	k_busy_wait(5000);
	sx126x_hal_wait_busy(dev, 50);

	/* Calibrate(0x7F) defaults image cal to 902-928 MHz band.
	 * Re-calibrate for the actual operating frequency. */
	if (data->config_valid) {
		sx126x_calibrate_image(dev, data->config.frequency);
	}

	/* Calibrate(ALL) is the one operation in the driver that might plausibly
	 * disturb the retention list, and it is off the packet path, so re-assert
	 * it here rather than defending the list on every duty-cycle re-arm.
	 * Idempotent: sx126x_add_reg_to_retention() returns early when the entry
	 * is already present. */
	sx126x_install_retention_list(dev);

	/* Re-apply DIO2 as RF switch if configured */
	if (config->dio2_tx_enable) {
		sx126x_set_dio2_as_rf_switch(dev, true);
	}

	/* Re-apply RX boosted gain if it was enabled */
	if (data->rx_boost_enabled) {
		sx126x_set_rx_gain(dev, true);
	}

	/* Hand the chip back in REST_STATE, never IDLE.
	 *
	 * Every entry point CASes from SX126X_REST_STATE, and that macro is
	 * SLEEP whenever CONFIG_LORA_SX126X_NATIVE_SLEEP is on -- which is the
	 * default.  IDLE therefore satisfies none of them, and leaving it here
	 * made the node permanently deaf AND mute the first time
	 * agcMaintenance() fired on a quiet minute: recv_duty_cycle(),
	 * recv_async() and send_async() all failed their entry CAS and logged
	 * "Busy" forever, with startSendRaw() misreporting the -EBUSY as an LBT
	 * channel-busy so the log blamed the channel. Only a reboot cleared it.
	 * Confirmed on hardware 2026-08-21: the "agc: 60000 ms without RX
	 * activity" line is immediately followed by the first "Busy".
	 *
	 * The original comment here had the right hazard and the wrong constant
	 * -- it reasoned about CAS(REST->RX) and then wrote IDLE, which only
	 * happens to work when NATIVE_SLEEP is disabled.
	 *
	 * sx126x_set_sleep() both sleeps the chip and sets state to SLEEP, so
	 * state and hardware agree; ensure_ready() wakes it warm on the next
	 * entry with configuration retained.  Do NOT "simplify" this back to a
	 * bare atomic_set -- that reintroduces a state/hardware disagreement. */
	sx126x_set_sleep(dev);

	k_mutex_unlock(&data->lock);
}

uint32_t sx126x_get_dc_timeout_restarts(const struct device *dev)
{
	struct sx126x_data *data = dev->data;

	return (uint32_t)atomic_get(&data->dc_timeout_restarts);
}

void sx126x_reset_dc_timeout_restarts(const struct device *dev)
{
	struct sx126x_data *data = dev->data;

	atomic_set(&data->dc_timeout_restarts, 0);
}

uint32_t sx126x_get_wakeup_time_us(const struct device *dev)
{
	const struct sx126x_hal_config *config = dev->config;
	/* Context restore + PLL lock: "around 1 ms" per DS fig 13-2 ("not
	 * accurate and may vary"), plus 500 us margin for the save side. */
	uint32_t us = 1500;

	/* DS rev 2.2 sec 13.1.7 note: with a chip-controlled TCXO, the
	 * programmed startup delay is inserted between the sleep and RX
	 * periods of every duty-cycle wake -- pure deaf time, outside both
	 * programmed periods. */
	if (config->dio3_tcxo_enable) {
		us += config->tcxo_startup_delay_ms * 1000U;
	}
	return us;
}

void sx126x_apply_heltec_reg_patch(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	uint8_t val = 0;

	/* Set LSB of undocumented register 0x8B5 to improve RX performance.
	 * Described by Heltec engineer @Quency-D in MeshCore PR#1398 --
	 * consistently improves reception on boards with GC1109/KCT8103L PA
	 * (Heltec V4/V4.3). Must be called after lora_config(). */
	k_mutex_lock(&data->lock, K_FOREVER);
	sx126x_hal_read_regs(dev, 0x08B5, &val, 1);
	val |= 0x01;
	sx126x_hal_write_regs(dev, 0x08B5, &val, 1);
	k_mutex_unlock(&data->lock);
}

/* -- Driver API: CAD -- */

/* Recommended cad_detect_peak for the SX126x is SF + 13 (Semtech
 * DS.SX1261-2.W.APP p92; RadioLib setCad default; Meshtastic pairs the
 * same value with 2-symbol CAD per AN1200.48).  The DetPeak scale is
 * chip-family-specific: LR11xx wants ~48-68 for the same job -- never
 * copy values across families (56-68 here once made CAD near-blind and
 * the LBT gate inert). */
/* Recommended cadDetPeak.  The datasheet gives no table (rev 2.2 §13.4.7 only
 * says the values "must be carefully tested" and defers to AN1200.48, which is
 * paywalled), so the numbers come from Semtech's own reference stack: LoRa
 * Basics Modem v4.9.0, ral_sx126x.c ral_sx126x_get_lora_cad_det_peak().
 *
 * Bandwidth matters, and it did not used to be considered here at all.  It is a
 * mild effect on this family — 1-3 counts per octave, against ~12 on the LR11xx
 * — which is why a flat SF+13 survived so long.  But Semtech's BW500 column
 * runs up to 35 at SF12 where SF+13 gives 25: ten counts too sensitive on a
 * wide-band preset, and `set bw` accepts 250 and 500.
 *
 * Below BW125 Semtech returns RAL_STATUS_UNKNOWN_VALUE.  Our default preset is
 * BW62.5, so that gap is where we normally live.  There we use the BW125 column
 * minus a constant 1, and then let the shared symbol correction below apply
 * normally — which happens to reproduce the old SF+13 heuristic exactly at SF5,
 * SF6, SF7, SF9, SF10 and SF12, and differ from it by a single count at SF8 and
 * SF11.
 *
 * Why not just keep SF+13, given it was measured right at SF7?  Because SF+13
 * has a uniform +1 slope and Semtech's measured row does not:
 *
 *     SF        5   6   7   8   9  10  11  12
 *     Semtech  20  21  22  24  24  25  27  27     steps +1 +1 +2  0 +1 +2  0
 *     SF+13    18  19  20  21  22  23  24  25     steps +1 +1 +1 +1 +1 +1 +1
 *
 * detPeak is empirical PER data; there is no reason for it to be linear in SF,
 * and we already adopted Semtech's shape for BW125, BW250 and BW500 in this
 * very function.  Keeping SF+13 below BW125 would mean the driver used two
 * different SF shapes depending on bandwidth — but bandwidth shifts the level,
 * it does not reshape the curve.  The sub-125 constant (-1 here, against -7 on
 * the LR11xx) is the level shift; Semtech's row is the shape.
 *
 * Field agreement: three SX1262s at SF7/BW62.5 — a house, a roof and a very busy
 * mountaintop — converged to 19/19/20 against this row's base of 20, unchanged
 * by this edit.  The only SF8 datum we have (that hilltop, offset +2/+3 against
 * SF+13's 21) points the same way as Semtech's +2 step at SF8.  Nothing points
 * the other way.
 *
 * Note SF+13 was already a 4-symbol figure (RadioLib programs it with
 * CAD_ON_4_SYMB, citing DS rev 1.1 p.92), which is why it was excluded from the
 * symbol correction.  The row below is a raw table value like every other
 * bracket, so it takes the correction like every other bracket — that is the
 * whole point of removing the special case. */
static uint8_t sx126x_cad_detect_peak(uint8_t sf, uint32_t bw_hz, uint8_t symb_nb)
{
	/*        SF5 SF6 SF7 SF8 SF9 SF10 SF11 SF12 */
	static const uint8_t bw500[8] = { 22, 23, 25, 26, 30, 31, 33, 35 };
	static const uint8_t bw125[8] = { 20, 21, 22, 24, 24, 25, 27, 27 };
	int peak;

	if (sf < 5) {
		sf = 5;
	} else if (sf > 12) {
		sf = 12;
	}

	/* Semtech brackets only >=500 and >=125 on this family — BW250 shares
	 * the BW125 column, unlike the LR11xx which gives it its own.  Below
	 * BW125 they publish nothing; see the note above for the -1. */
	if (bw_hz >= 500000U) {
		peak = (int)bw500[sf - 5];
	} else if (bw_hz >= 125000U) {
		peak = (int)bw125[sf - 5];
	} else {
		peak = (int)bw125[sf - 5] - 1;
	}

	/* More symbols means more looks at the same correlation, so the same
	 * detection quality is reached at a lower threshold.  We run 4 symbols
	 * everywhere (LORA_CAD_SYMB_4 in LoRaRadioBase::buildModemConfig). */
	if (symb_nb >= 8) {
		peak -= 2;
	} else if (symb_nb >= 4) {
		peak -= 1;
	}

	return (uint8_t)peak;
}

/* The detPeak range this driver will actually program.  Exported through
 * sx126x_cad_peak_min/max() so the C++ adaptive-CAD controller can narrow its
 * offset window to match: where base+offset falls outside this, several offsets
 * collapse onto one peak and the staircase reads sampling noise between
 * identical configurations as curvature.
 *
 * The ceiling covers the widest-band base (34 at SF12/BW500 with 4 symbols)
 * plus the full CAD_LEVEL_MAX excursion of +12.  The floor stops two counts
 * above cadDetMin, which is pinned at 10 everywhere: a detPeak at or below the
 * correlator's own noise-estimate floor does not express a threshold at all.
 * At the most sensitive base (18, SF5 narrow-band) that does clip the bottom
 * rungs of the offset window — which is exactly why it is exported rather than
 * applied silently. */
#define SX126X_CAD_PEAK_MIN 12
#define SX126X_CAD_PEAK_MAX 48

uint8_t sx126x_cad_peak_min(void)
{
	return SX126X_CAD_PEAK_MIN;
}

uint8_t sx126x_cad_peak_max(void)
{
	return SX126X_CAD_PEAK_MAX;
}

/* Single source of truth for the base peak, so sx126x_do_cad(), the exported
 * sx126x_cad_base_peak() and sx126x_cad_probe() cannot drift apart.  A base
 * that disagreed with the peak actually programmed would make every rung the
 * C++ staircase reasons about a lie.
 *
 * An unresolvable bandwidth (bw_hz forced to 0) falls through to the sub-125
 * branch, the most sensitive of the three — the safe direction for an unknown
 * configuration, since too sensitive costs airtime while too insensitive costs
 * collisions. */
static uint8_t sx126x_cad_base_peak_cfg(struct lora_modem_config *mc)
{
	uint32_t bw_hz;
	uint8_t symb_nb = mc->cad.symbol_num ? (uint8_t)mc->cad.symbol_num : 2;

	if (bandwidth_to_hz(mc->bandwidth, &bw_hz) < 0) {
		bw_hz = 0;
	}

	return sx126x_cad_detect_peak((uint8_t)mc->datarate, bw_hz, symb_nb);
}

/* Blocking-CAD wait budget scaled to the actual CAD duration:
 * nSym * Tsym + 32/BW radio-side, plus IRQ latency / work-queue margin.
 * The old fixed 200 ms fit 2-symbol CAD everywhere but is exceeded by
 * 4-symbol CAD on slow presets (SF12 @ 62.5 kHz = ~262 ms). */
static uint32_t sx126x_cad_timeout_ms(struct sx126x_data *data)
{
	struct lora_modem_config *mc = &data->config;
	uint8_t sf = (uint8_t)mc->datarate;
	uint8_t symb_nb = mc->cad.symbol_num ?
			  (uint8_t)mc->cad.symbol_num : 2;
	uint32_t bw_hz;

	if (bandwidth_to_hz(mc->bandwidth, &bw_hz) < 0 || sf < 5 || sf > 12) {
		return 200;
	}

	uint32_t tsym_us = ((1UL << sf) * 1000000UL) / bw_hz;
	/* +1 symbol covers the 32/BW startup + internal processing tail */
	uint32_t ms = ((symb_nb + 1U) * tsym_us) / 1000U + 100U;

	return MAX(ms, 200U);
}

static int sx126x_do_cad(const struct device *dev, struct sx126x_data *data)
{
	struct lora_modem_config *mc = &data->config;
	/* No local sf: sx126x_cad_base_peak_cfg() reads the SF, bandwidth and
	 * symbol count off mc itself, which is what keeps it agreeing with
	 * sx126x_cad_base_peak() and sx126x_cad_probe(). */
	uint8_t symb_nb = mc->cad.symbol_num ? (uint8_t)mc->cad.symbol_num : 2;
	uint8_t detect_peak = sx126x_cad_base_peak_cfg(mc);
	int ret;
	uint8_t symbol_code;

	/* Zephyr supplies a symbol count; SX126x expects log2(count). */
	switch (symb_nb) {
	case 1: symbol_code = 0; break;
	case 2: symbol_code = 1; break;
	case 4: symbol_code = 2; break;
	case 8: symbol_code = 3; break;
	case 16: symbol_code = 4; break;
	default: return -EINVAL;
	}

	if (mc->cad.detection_peak != 0) {
		detect_peak = mc->cad.detection_peak;
	} else if (data->cad_peak_offset != 0) {
		/* Adaptive-CAD operating offset (base +/- learned delta).
		 * Clamped to a sane absolute window around the family scale. */
		int peak = (int)detect_peak + data->cad_peak_offset;

		if (peak < SX126X_CAD_PEAK_MIN) {
			peak = SX126X_CAD_PEAK_MIN;
		} else if (peak > SX126X_CAD_PEAK_MAX) {
			peak = SX126X_CAD_PEAK_MAX;
		}
		detect_peak = (uint8_t)peak;
	}
	if (data->cad_probe_peak != 0) {
		/* One-shot calibration probe: absolute peak wins over all. */
		detect_peak = data->cad_probe_peak;
	}

	/* SET_CAD must run from STANDBY_RC (DS).  LBT is entered from continuous RX
	 * (Phase-2 send_async CAS); sx126x_ensure_ready() is a no-op when native
	 * sleep is disabled, so the modem can still be in RX here -- CAD then never
	 * completes and k_sem_take times out. */
	ret = sx126x_set_standby(dev, SX126X_STANDBY_RC);
	if (ret < 0) {
		return ret;
	}

	/* Calibration and duty-cycled radios use CAD_ONLY. A previously
	 * configured duty-cycle RTC does not reliably implement the CAD RX timer.
	 * The normal endpoint has a continuously armed receiver. With its RX
	 * IRQs already mapped we can adopt the hardware CAD-to-RX transition,
	 * and bound it to the longest frame at the active modulation settings. */
	uint8_t buf[7];
	uint32_t cad_timeout = 0;
	buf[0] = symbol_code;
	buf[1] = detect_peak;
	buf[2] = mc->cad.detection_minimum ? mc->cad.detection_minimum : 10;
	buf[3] = 0; /* CAD_ONLY */
#ifdef CONFIG_MT_NORMAL_ENDPOINT
	if (atomic_get(&data->lbt_handoff) == LBT_SCAN) {
		cad_timeout = zc_lora_ms_to_steps24(sx126x_lora_airtime(dev, 233U + 16U), 64000U);
		if (!cad_timeout) return -EINVAL;
		buf[3] = 1; /* CAD_RX */
	}
#endif
	sys_put_be24(cad_timeout & 0xFFFFFFU, &buf[4]);

	ret = sx126x_hal_write_cmd(dev, SX126X_CMD_SET_CAD_PARAMS, buf, 7);
	if (ret < 0) {
		return ret;
	}

	sx126x_clear_irq_status(dev, SX126X_IRQ_CAD_DONE |
				     SX126X_IRQ_CAD_ACTIVITY_DETECTED);
	data->cad_active = true;

	/* Enable antenna in RX mode for CAD */
	sx126x_set_rf_path(dev, true, false);
	ret = sx126x_hal_write_cmd(dev, SX126X_CMD_SET_CAD, NULL, 0);
	if (ret < 0) {
		data->cad_active = false;
		return ret;
	}

	return 0;
}

static int sx126x_lora_cad_mode(const struct device *dev, k_timeout_t timeout, bool handoff)
{
	struct sx126x_data *data = dev->data;
	int ret;

	if (!data->config_valid) {
		return -EINVAL;
	}

	/* Transition from REST -> IDLE for the CAD operation */
	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_IDLE)) {
		/* If we're in RX, force to IDLE */
		if (!atomic_cas(&data->state, SX126X_STATE_RX, SX126X_STATE_IDLE)) {
			return -EBUSY;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = sx126x_ensure_ready(dev);
	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	k_sem_reset(&data->cad_sem);
	data->cad_result = -ETIMEDOUT;
	data->cad_cb = NULL;
#ifdef CONFIG_MT_NORMAL_ENDPOINT
	atomic_set(&data->lbt_handoff, handoff ? LBT_SCAN : LBT_IDLE);
#else
	ARG_UNUSED(handoff);
#endif

	ret = sx126x_do_cad(dev, data);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
#ifdef CONFIG_MT_NORMAL_ENDPOINT
		atomic_set(&data->lbt_handoff, LBT_IDLE);
#endif
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	ret = k_sem_take(&data->cad_sem, timeout);
	if (ret != 0) {
		/* Zephyr may return -EAGAIN or -ETIMEDOUT on wait timeout. */
		k_mutex_lock(&data->lock, K_FOREVER);
		data->cad_active = false;
#ifdef CONFIG_MT_NORMAL_ENDPOINT
		atomic_set(&data->lbt_handoff, LBT_IDLE);
#endif
		(void)sx126x_set_standby(dev, SX126X_STANDBY_RC);
		sx126x_clear_irq_status(dev, SX126X_IRQ_CAD_DONE |
					     SX126X_IRQ_CAD_ACTIVITY_DETECTED);
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		if (ret == -EAGAIN || ret == -ETIMEDOUT) {
			return -ETIMEDOUT;
		}
		return ret;
	}

	/* A positive CAD_RX left the chip in Rx.  Say so, rather than reporting
	 * REST_STATE and letting every later CAS reason about a chip that is
	 * somewhere else: the RX_DONE and timeout handlers both gate on state,
	 * and sx126x_lora_send_async's RX -> TX entry has to abort a real Rx
	 * rather than assume standby. */
#ifdef CONFIG_MT_NORMAL_ENDPOINT
	if (handoff && data->cad_result > 0) return data->cad_result;
	atomic_cas(&data->lbt_handoff, LBT_SCAN, LBT_IDLE);
#endif
	if (data->cad_result > 0 && data->cad_exit_rx) {
		atomic_set(&data->state, SX126X_STATE_RX);
	} else {
		atomic_set(&data->state, SX126X_REST_STATE);
	}
	return data->cad_result;
}

static int sx126x_lora_cad(const struct device *dev, k_timeout_t timeout)
{
	return sx126x_lora_cad_mode(dev, timeout, false);
}

static int sx126x_lora_cad_async(const struct device *dev,
				  lora_cad_cb cb, void *user_data)
{
	struct sx126x_data *data = dev->data;

	if (cb == NULL) {
		data->cad_cb = NULL;
		data->cad_user_data = NULL;
		data->cad_active = false;
		return 0;
	}

	if (!data->config_valid) {
		return -EINVAL;
	}

	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_IDLE)) {
		if (!atomic_cas(&data->state, SX126X_STATE_RX, SX126X_STATE_IDLE)) {
			return -EBUSY;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	int ret = sx126x_ensure_ready(dev);

	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	data->cad_cb = cb;
	data->cad_user_data = user_data;

	ret = sx126x_do_cad(dev, data);
	k_mutex_unlock(&data->lock);

	if (ret < 0) {
		atomic_set(&data->state, SX126X_REST_STATE);
	}

	return ret;
}

/* -- Extension API: adaptive CAD -- */

void sx126x_cad_set_peak_offset(const struct device *dev, int8_t offset)
{
	struct sx126x_data *data = dev->data;

	data->cad_peak_offset = offset;
}

uint8_t sx126x_cad_base_peak(const struct device *dev)
{
	struct sx126x_data *data = dev->data;

	return sx126x_cad_base_peak_cfg(&data->config);
}

int sx126x_cad_probe(const struct device *dev, int8_t peak_offset)
{
	struct sx126x_data *data = dev->data;
	int base = (int)sx126x_cad_base_peak(dev);
	int peak = base + peak_offset;
	int ret;

	if (peak < SX126X_CAD_PEAK_MIN) {
		peak = SX126X_CAD_PEAK_MIN;
	} else if (peak > SX126X_CAD_PEAK_MAX) {
		peak = SX126X_CAD_PEAK_MAX;
	}

	/* One-shot absolute override consumed by sx126x_do_cad().  Probes and
	 * LBT both run on the mesh loop thread, so no concurrent CAD exists --
	 * which is also what makes cad_exit_rx safe as a plain flag. */
	data->cad_probe_peak = (uint8_t)peak;
	data->cad_exit_rx = true;
	atomic_set(&data->cad_rx_state, SX126X_CAD_RX_IDLE);
	ret = sx126x_lora_cad(dev, K_MSEC(sx126x_cad_timeout_ms(data)));
	data->cad_exit_rx = false;
	data->cad_probe_peak = 0;

	/* 2 rather than 1 tells the caller the chip is in Rx on the signal it
	 * detected, so it must NOT re-enter Rx itself, and that an outcome will
	 * be readable from sx126x_cad_rx_outcome() once the chip resolves it. */
	if (ret > 0) {
		return 2;
	}

	return ret;
}

uint32_t sx126x_cad_rx_timeout_ms(const struct device *dev)
{
	struct sx126x_data *data = dev->data;

	/* The same figure do_cad() programs into cadTimeout, so the caller's
	 * wait and the chip's deadline cannot drift apart. */
	return sx126x_max_payload_ms(data);
}

int sx126x_cad_rx_outcome(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	atomic_val_t st = atomic_get(&data->cad_rx_state);

	if (st != SX126X_CAD_RX_PACKET && st != SX126X_CAD_RX_TMOUT) {
		return 0;  /* nothing armed, or still awaiting the terminal IRQ */
	}

	atomic_set(&data->cad_rx_state, SX126X_CAD_RX_IDLE);
	return (st == SX126X_CAD_RX_PACKET) ? 1 : 2;
}

int sx126x_bench_activity(const struct device *dev)
{
	return sx126x_rx_activity(dev);
}

uint32_t sx126x_bench_rx_releases(const struct device *dev)
{
	if (!device_is_ready(dev)) {
		return 0;
	}
	struct sx126x_data *data = dev->data;

	return (uint32_t)atomic_get(&data->rx_stale_releases);
}

int sx126x_bench_abort(const struct device *dev) {
 sx126x_bench_revoke();
 if(!device_is_ready(dev)) return -ENODEV;
 struct sx126x_data *data=dev->data;struct k_work_sync sync,watchdog_sync;
 sx126x_observe_rx(data, 0, 0, false);
 (void)sx126x_hal_set_dio1_callback(dev,NULL);
 sx126x_hal_retire_dio1_callback(dev);
 (void)k_work_cancel_sync(&data->irq_work,&sync);
 (void)k_work_cancel_delayable_sync(&data->dc_watchdog_work,&watchdog_sync);
 /* A joined handler may have re-enabled hardware admission.  Software is
  * already retired, so this second close cannot submit unjoined work. */
 (void)sx126x_hal_set_dio1_callback(dev,NULL);
 sx126x_hal_retire_dio1_callback(dev);
 k_mutex_lock(&data->lock,K_FOREVER);
 data->tx_async_signal=NULL;data->cad_cb=NULL;data->cad_active=false;
 data->rx_cb=NULL;data->rx_cb_user_data=NULL;atomic_inc(&data->rx_cb_gen);
 int ret=sx126x_set_standby(dev,SX126X_STANDBY_RC);
 sx126x_reset_rx_busy_signals(data);sx126x_set_rf_path(dev,false,false);
 if(ret==0) atomic_set(&data->state,SX126X_REST_STATE);
 k_mutex_unlock(&data->lock);return ret;
}

/* Settle software state only after SetStandby has proved the chip is no longer
 * receiving.  A later IRQ-clear, GPIO or sleep failure is still terminal, but
 * it must not leave the driver claiming RX while the modem is known parked. */
static void sx126x_storage_standby_state(const struct device *dev,
					 struct sx126x_data *data)
{
	sx126x_hal_retire_dio1_callback(dev);
	data->rx_cb = NULL;
	data->rx_cb_user_data = NULL;
	atomic_inc(&data->rx_cb_gen);
	data->rx_duty_cycle_enabled = false;
	data->tx_async_signal = NULL;
	data->cad_cb = NULL;
	data->cad_active = false;
	sx126x_reset_rx_busy_signals(data);
	sx126x_set_rf_path(dev, false, false);
	atomic_set(&data->state, SX126X_REST_STATE);
}

/* A storage pause can fail after SetStandby succeeded.  A callback may have
 * been re-enabled before the failure, so retire it, join both work sources
 * without holding data->lock, close admission again, and re-prove standby.
 * A sticky status that survived cancellation is retained and returned as a
 * hard failure; it is never cleared merely to obtain storage quiescence.
 * Return -ESHUTDOWN only after the second barrier and empty snapshot prove a
 * known-off outcome.  No caller may treat it as storage authorization. */
static int sx126x_storage_settle_after_standby(const struct device *dev,
					       struct sx126x_data *data)
{
	struct k_work_sync irq_sync, watchdog_sync;
	uint16_t irq_status = 0;
	int ret;

	/* Software retirement prevents a failed/partial GPIO disable from
	 * admitting another work item while the existing one is joined. */
	sx126x_hal_retire_dio1_callback(dev);
	(void)sx126x_hal_set_dio1_callback(dev, NULL);
	k_mutex_unlock(&data->lock);
	(void)k_work_cancel_sync(&data->irq_work, &irq_sync);
	(void)k_work_cancel_delayable_sync(&data->dc_watchdog_work,
					   &watchdog_sync);
	/* A running handler may have re-enabled its interrupt before completing. */
	(void)sx126x_hal_set_dio1_callback(dev, NULL);
	sx126x_hal_retire_dio1_callback(dev);
	k_mutex_lock(&data->lock, K_FOREVER);

	ret = sx126x_set_standby(dev, SX126X_STANDBY_RC);
	if (ret < 0) {
		return ret;
	}
	ret = sx126x_get_irq_status(dev, &irq_status);
	if (ret < 0) {
		return ret;
	}
	if (irq_status != 0U) {
		return -EBUSY;
	}
	ret = sx126x_clear_irq_status(dev, SX126X_IRQ_ALL);
	if (ret == 0) {
		ret = sx126x_set_sleep(dev);
	}

	sx126x_storage_standby_state(dev, data);
	return -ESHUTDOWN;
}

int sx126x_pause_for_storage(const struct device *dev)
{
	struct sx126x_data *data;
	struct k_work_sync irq_sync, watchdog_sync;
	uint16_t irq_status = 0, restore_status = 0;
	int ret, restore_ret;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	data = dev->data;
	sx126x_observe_rx(data, 0, 0, false);

	/* Close the ISR -> work-queue admission edge first, then join anything
	 * that crossed it.  A completion that happens after this point remains
	 * sticky in the chip and is covered by the frozen snapshot below. */
	ret = sx126x_hal_set_dio1_callback(dev, NULL);
	if (ret < 0) {
		/* HAL restored the prior callback pointer.  The GPIO state is not
		 * proven, so leave driver/service RX state live for a faulted caller. */
		return ret;
	}
	(void)k_work_cancel_sync(&data->irq_work, &irq_sync);
	(void)k_work_cancel_delayable_sync(&data->dc_watchdog_work,
					   &watchdog_sync);

	k_mutex_lock(&data->lock, K_FOREVER);
	if (atomic_get(&data->state) != SX126X_STATE_RX || data->rx_cb == NULL) {
		/* Service expected a live receiver, but the driver cannot prove one.
		 * Do not reopen DIO or label this a clean retry: preserving the
		 * mismatch as a hard fault is safer than restarting without an
		 * application callback or from an unknown modem state. */
		ret = -EIO;
		goto restore;
	}

	/* SetStandby is the RF-side barrier.  IRQ bits and the RX FIFO survive
	 * it, so the following status word includes a completion whose DIO edge
	 * arrived before, during, or immediately after callback disconnection. */
	ret = sx126x_set_standby(dev, SX126X_STANDBY_RC);
	if (ret < 0) {
		goto restore;
	}
	ret = sx126x_get_irq_status(dev, &irq_status);
	if (ret < 0) {
		goto settled_failure;
	}

	if (irq_status != 0U) {
		/* Reconnect while the chip is still frozen.  RX_DONE is processed
		 * from this exact snapshot so its FIFO is published through the
		 * existing callback before -EAGAIN is returned.  Partial or other
		 * activity is conservatively re-armed without a settings write. */
		ret = sx126x_hal_set_dio1_callback(dev, sx126x_dio1_callback);
		if (ret < 0) {
			/* The HAL rolled the callback back to NULL.  If a complete
			 * packet is already frozen, deliver it synchronously before the
			 * terminal standby cleanup; GPIO reattachment is not required to
			 * process this captured status word. */
			if ((irq_status & SX126X_IRQ_RX_DONE) != 0U) {
				k_mutex_unlock(&data->lock);
				(void)sx126x_handle_irq_status(dev, irq_status);
				k_mutex_lock(&data->lock, K_FOREVER);
				if (sx126x_set_standby(dev, SX126X_STANDBY_RC) < 0) {
					k_mutex_unlock(&data->lock);
					return ret;
				}
			}
			goto settled_failure;
		}
		if ((irq_status & SX126X_IRQ_RX_DONE) != 0U) {
			k_mutex_unlock(&data->lock);
			ret = sx126x_handle_irq_status(dev, irq_status);
			if (ret >= 0) {
				return -EAGAIN;
			}
			k_mutex_lock(&data->lock, K_FOREVER);
			if (sx126x_set_standby(dev, SX126X_STANDBY_RC) == 0) {
				ret = sx126x_storage_settle_after_standby(dev, data);
				k_mutex_unlock(&data->lock);
				return ret;
			}
			k_mutex_unlock(&data->lock);
			return ret;
		}
		ret = sx126x_restart_rx(dev, data);
		if (ret < 0) {
			if (sx126x_set_standby(dev, SX126X_STANDBY_RC) < 0) {
				k_mutex_unlock(&data->lock);
				return ret;
			}
			goto settled_failure;
		}
		k_mutex_unlock(&data->lock);
		return -EAGAIN;
	}

	/* The chip is frozen and the final IRQ snapshot was empty: only now is
	 * it safe to retire the callback and expose health==off to storage. */
	ret = sx126x_clear_irq_status(dev, SX126X_IRQ_ALL);
	if (ret == 0) {
		ret = sx126x_set_sleep(dev);
	}
	if (ret == 0) {
		sx126x_storage_standby_state(dev, data);
		/* Only this success branch has retired DIO, joined work/watchdog,
		 * frozen RF and proved the final IRQ word empty. One subsequent
		 * config may use it; HAL RF/IRQ admission retires it immediately. */
		sx126x_observe_reconfigure_quiescent(data);
	} else {
		goto settled_failure;
	}
	k_mutex_unlock(&data->lock);
	return ret;

restore:
	/* A failed barrier never authorizes storage.  Best effort returns the
	 * same async receiver to service.  Reconnection alone is not enough: an
	 * RX_DONE may have become sticky while DIO admission was disconnected.
	 * Snapshot and publish that completion before any restart can clear it.
	 * A restore failure takes precedence so the caller faults closed instead
	 * of believing RX is still healthy. */
	if (atomic_get(&data->state) != SX126X_STATE_RX || data->rx_cb == NULL) {
		k_mutex_unlock(&data->lock);
		return ret;
	}
	restore_ret = sx126x_hal_set_dio1_callback(dev, sx126x_dio1_callback);
	if (restore_ret == 0) {
		restore_ret = sx126x_get_irq_status(dev, &restore_status);
		if (restore_ret == 0 && (restore_status & SX126X_IRQ_RX_DONE) != 0U) {
			k_mutex_unlock(&data->lock);
			restore_ret = sx126x_handle_irq_status(dev, restore_status);
			if (restore_ret >= 0) {
				return -EAGAIN;
			}
			return restore_ret;
		}
		if (restore_ret == 0) {
			restore_ret = sx126x_restart_rx(dev, data);
		}
	}
	if (restore_ret == 0) {
		/* A failed barrier that was completely restored is a clean
		 * deferral.  It never authorizes a storage write or faults service. */
		k_mutex_unlock(&data->lock);
		return -EAGAIN;
	}
	ret = restore_ret;
	k_mutex_unlock(&data->lock);
	return ret;

settled_failure:
	ret = sx126x_storage_settle_after_standby(dev, data);
	k_mutex_unlock(&data->lock);
	return ret;
}

/* -- Driver API: recv_duty_cycle -- */

static int sx126x_lora_recv_duty_cycle_async(const struct device *dev,
					     k_timeout_t rx_period,
					     k_timeout_t sleep_period,
					     lora_recv_cb cb, void *user_data)
{
	struct sx126x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (cb == NULL) {
		data->rx_cb = NULL;
		data->rx_cb_user_data = NULL;
		atomic_inc(&data->rx_cb_gen);
		data->rx_duty_cycle_enabled = false;
		sx126x_reset_rx_busy_signals(data);
		if (atomic_cas(&data->state, SX126X_STATE_RX, SX126X_STATE_IDLE)) {
			/* May be called in the duty-cycle sleep phase with BUSY
			 * asserted; upstream's stop wakes via a raw NSS edge
			 * before STDBY_RC + sleep. */
			sx126x_duty_cycle_stop(dev);
		}
		k_mutex_unlock(&data->lock);
		return 0;
	}

	if (!data->config_valid) {
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	/* Explicit timing only.  The window sizing (detection budget,
	 * datasheet Tpreamble+Theader <= 2*rxPeriod+sleepPeriod rule, TCXO
	 * transition accounting) is owned by the caller -- the adapter layer
	 * knows the mesh preamble policy, the driver just programs periods.
	 * Validated before we claim the state so a malformed request cannot
	 * leave the chip half-configured. */
	if (K_TIMEOUT_EQ(rx_period, K_FOREVER) ||
	    K_TIMEOUT_EQ(sleep_period, K_FOREVER)) {
		LOG_ERR("recv_duty_cycle: explicit rx/sleep periods required");
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	/* Convert us to 15.625us steps */
	uint32_t rx_time = (k_ticks_to_us_ceil32(rx_period.ticks) * 64) / 1000;
	uint32_t sleep_time = (k_ticks_to_us_ceil32(sleep_period.ticks) * 64) / 1000;

	if (rx_time < 64) rx_time = 64;
	if (sleep_time < 64) sleep_time = 64;

	/* Idempotent fast-path: the chip is already in RX.
	 *
	 * The usual way here is the LBT branch of sx126x_lora_send_async: on
	 * CAD-busy it calls sx126x_restart_rx() and returns -EBUSY, leaving
	 * state == RX.  Failing the CAS below would then report -EBUSY to a
	 * caller whose only honest reading of it is "a concurrent TX owns the
	 * chip".  Worse, it is a one-way door: an entry that fell back to
	 * continuous RX (any error path below that returns before
	 * rx_duty_cycle_enabled is set) could never re-arm, because every
	 * later attempt hits the same failed CAS -- the node quietly listens
	 * at full RX current until it is rebooted.
	 *
	 * sx126x_restart_rx() runs the identical entry sequence to the one
	 * below (Calibrate(ALL) AGC reset, CalibrateImage, DIO2 RF switch,
	 * StopTimerOnPreamble, RX-gain retention, SetRxDutyCycle), so re-arming
	 * through it gives up nothing.  When the chip is already duty cycling
	 * on these exact periods there is nothing to re-arm and only the
	 * callback is refreshed: no SPI traffic, no ~5 ms calibration, no
	 * dropped RX window. */
	if (atomic_get(&data->state) == SX126X_STATE_RX) {
		const bool rearm = !data->rx_duty_cycle_enabled ||
				   data->duty_cycle.rx_period != rx_time ||
				   data->duty_cycle.sleep_period != sleep_time;

		data->rx_cb = cb;
	data->rx_packet_cb = NULL;
		data->rx_cb_user_data = user_data;
		atomic_inc(&data->rx_cb_gen);

		if (!rearm) {
			k_mutex_unlock(&data->lock);
			return 0;
		}

		data->duty_cycle.rx_period = rx_time;
		data->duty_cycle.sleep_period = sleep_time;
		data->rx_duty_cycle_enabled = true;
	if (atomic_get(&data->observation_enabled))
		atomic_set(&data->observation_config_state, 2);

		/* Mid-cycle the chip may be in its duty-cycle sleep phase with
		 * BUSY asserted; talking to it then stalls the HAL in
		 * wait_busy().  Poke CS first, same as the cancel path above. */
		if (sx126x_hal_is_busy(dev)) {
			sx126x_hal_wakeup(dev);
		}

		ret = sx126x_restart_rx(dev, data);
		if (ret < 0) {
			/* Hand the chip back un-owned so the caller's
			 * continuous-RX fallback performs a full clean entry
			 * rather than fast-pathing onto a radio we have just
			 * failed to program. */
			data->rx_duty_cycle_enabled = false;
			data->rx_cb = NULL;
			data->rx_cb_user_data = NULL;
			atomic_inc(&data->rx_cb_gen);
			atomic_set(&data->state, SX126X_REST_STATE);
			k_mutex_unlock(&data->lock);
			return ret;
		}

		k_mutex_unlock(&data->lock);
		LOG_DBG("recv_duty_cycle: re-armed in place rx=%uus sleep=%uus",
			(rx_time * 1000) / 64, (sleep_time * 1000) / 64);
		return 0;
	}

	if (!atomic_cas(&data->state, SX126X_REST_STATE, SX126X_STATE_RX)) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}

	ret = sx126x_ensure_ready(dev);
	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		atomic_set(&data->state, SX126X_REST_STATE);
		return ret;
	}

	/* Fresh DC RX start: clear any stale IRQ bits and reset the latch +
	 * preamble-grace timestamp.  Same rationale as the non-DC recv_async
	 * path -- recoverRxState() and other paths can't trust the IRQ
	 * register otherwise. */
	sx126x_clear_irq_status(dev, SX126X_IRQ_ALL);
	sx126x_reset_rx_busy_signals(data);

	/* AGC reset on every entry: Calibrate(ALL) re-initialises the analog
	 * frontend (ADC/PLL/RC) so the receiver starts with a clean AGC state.
	 * Without this, an AGC lock-up from a strong adjacent-channel signal
	 * persists across the cycle (the hardware-driven duty cycle never
	 * fires an IRQ to trigger sx126x_restart_rx's calibration path), and
	 * post-TX entry inherits whatever state the chip was in before TX.
	 * Cost: ~5 ms -- paid once per entry, not per internal duty-cycle wake. */
	{
		const struct sx126x_hal_config *hal_cfg = dev->config;

		sx126x_calibrate(dev, SX126X_CALIBRATE_ALL);
		k_busy_wait(5000);
		ret = sx126x_hal_wait_busy(dev, 50);
		if (ret < 0) {
			k_mutex_unlock(&data->lock);
			atomic_set(&data->state, SX126X_REST_STATE);
			return ret;
		}

		/* Re-issue CalibrateImage for the operating frequency:
		 * Calibrate(ALL) reverts image cal to the chip default
		 * (902-928 MHz band) which kills RX sensitivity on
		 * EU868 / 433 / 779 MHz operation. */
		sx126x_calibrate_image(dev, data->config.frequency);
		/* Same reasoning for the AGC band calibration (DS Sec 6.1.6):
		 * cheap insurance against Calibrate(ALL) disturbing the RSSI /
		 * gain-tune registers.  0x08AC itself is restored by the
		 * RX-gain write on the restart path. */
		sx126x_apply_agc_band_cal(dev, data->config.frequency);

		/* Re-apply DIO2 as RF switch -- Calibrate resets it. */
		if (hal_cfg->dio2_tx_enable) {
			sx126x_set_dio2_as_rf_switch(dev, true);
		}
	}

	data->rx_cb = cb;
	data->rx_packet_cb = NULL;
	data->rx_cb_user_data = user_data;
	atomic_inc(&data->rx_cb_gen);

	/* Set packet parameters (same as recv_async) */
	ret = sx126x_set_packet_params(dev,
				       data->config.preamble_len,
				       SX126X_LORA_HEADER_EXPLICIT,
				       SX126X_MAX_PAYLOAD_LEN,
				       data->config.packet_crc_disable ?
				       SX126X_LORA_CRC_OFF : SX126X_LORA_CRC_ON,
				       data->config.iq_inverted ?
				       SX126X_LORA_IQ_INVERTED : SX126X_LORA_IQ_STANDARD);
	if (ret < 0) {
		data->rx_cb = NULL;
		atomic_inc(&data->rx_cb_gen);
		sx126x_set_sleep(dev);
		k_mutex_unlock(&data->lock);
		return ret;
	}

	sx126x_set_rf_path(dev, true, false);

	/* Store for restart_rx to re-use (periods validated and converted at
	 * function entry, before the state was claimed). */
	data->duty_cycle.rx_period = rx_time;
	data->duty_cycle.sleep_period = sleep_time;
	data->rx_duty_cycle_enabled = true;
	if (atomic_get(&data->observation_enabled))
		atomic_set(&data->observation_config_state, 2);

	/* StopTimerOnPreamble: keep the RX timer from expiring mid-packet
	 * when a preamble is detected late in the RX window.  Without it,
	 * a packet whose preamble lands near the tail of the RX window
	 * gets cut off by the sleep transition. */
	/* Apply RX gain.  The warm-start retention list that makes the
	 * chip-internal sleep->RX wakes restore it (DS sec 9.6: mandatory for
	 * SetRxDutyCycle -- without it every window after the first sleep
	 * listens at power-saving gain, a silent -3 dB sensitivity loss) is
	 * installed once in sx126x_chip_init() and survives those wakes. */
	uint8_t gain = sx126x_rx_gain_byte(dev, data->rx_boost_enabled);

	sx126x_hal_write_regs(dev, SX126X_REG_RX_GAIN, &gain, 1);

	/* StopTimerOnPreamble (comment above) + SetRxDutyCycle. */
	(void)sx126x_set_rx_duty_cycle(dev, rx_time, sleep_time);

	sx126x_dc_watchdog_kick(data);

	k_mutex_unlock(&data->lock);

	LOG_DBG("recv_duty_cycle: rx=%uus sleep=%uus",
		(rx_time * 1000) / 64, (sleep_time * 1000) / 64);
	return 0;
}

static DEVICE_API(lora, sx126x_lora_api) = {
	.config          = sx126x_lora_config,
	.send            = sx126x_lora_send,
	.send_async      = sx126x_lora_send_async,
	.recv            = sx126x_lora_recv,
	.recv_async      = sx126x_lora_recv_async,
	.airtime         = sx126x_lora_airtime,
	.cad             = sx126x_lora_cad,
	.cad_async       = sx126x_lora_cad_async,
	.recv_duty_cycle_async = sx126x_lora_recv_duty_cycle_async,
	.rssi            = sx126x_lora_rssi,
	.test_cw         = sx126x_lora_test_cw,
};

#ifdef CONFIG_PM_DEVICE
static int sx126x_pm_action(const struct device *dev,
			    enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		sx126x_disconnect_rf_gpios(dev);
		return 0;
	case PM_DEVICE_ACTION_RESUME:
		return sx126x_reconnect_rf_gpios(dev);
	default:
		return -ENOTSUP;
	}
}
#endif

static int sx126x_init(const struct device *dev)
{
	struct sx126x_data *data = dev->data;
	const struct sx126x_hal_config *config = dev->config;
	int ret;

	bench_record(dev,"init_begin",0,0);

	/* Initialize data structures */
	k_mutex_init(&data->lock);
	k_msgq_init(&data->tx_msgq, (char *)&data->tx_result,
		    sizeof(struct sx126x_tx_result), 1);
	k_msgq_init(&data->rx_msgq, (char *)&data->rx_result,
		    sizeof(struct sx126x_rx_result), 1);
	k_work_init(&data->irq_work, sx126x_irq_work_handler);
	k_sem_init(&data->cad_sem, 0, 1);
	data->dev = dev;
	atomic_set(&data->state, SX126X_STATE_IDLE);
	data->config_valid = false;
	data->rx_cb = NULL;
	data->rx_packet_cb = NULL;
	data->rx_cb_user_data = NULL;
	atomic_set(&data->rx_cb_gen, 0);
	atomic_set(&data->rx_stale_releases, 0);
	data->rx_duty_cycle_enabled = false;
	sx126x_reset_rx_busy_signals(data);
	/* Mirror the DTS rx-boosted property so restart_rx and recv_duty_cycle
	 * pick up the boost setting even when the user never calls the
	 * sx126x_set_rx_boost() extension API. */
	data->rx_boost_enabled = config->rx_boosted;
	/* Match the calibration the chip ships with, so any 0x08AC write that
	 * happens before the first sx126x_config() keeps the factory value
	 * rather than zeroing AgcSensiAdjust. */
	data->agc_sensi_adjust = SX126X_AGC_SENSI_ADJUST_HIGH_BAND;
	atomic_set(&data->dc_timeout_restarts, 0);
	data->observation_tx_started = data->observation_tx_done = 0;
	data->observation_tx_timeouts = data->observation_last_tx_irq = 0;
	data->send_stage = data->config_stage = data->restart_stage = (struct sx126x_stage_info){0};
	k_work_init_delayable(&data->dc_watchdog_work,
			      sx126x_dc_watchdog_handler);
	data->dc_watchdog_strikes = 0;

	/* Start dedicated DIO1 work queue */
	k_work_queue_start(&data->dio1_wq, data->dio1_stack,
			   K_KERNEL_STACK_SIZEOF(data->dio1_stack),
			   CONFIG_SYSTEM_WORKQUEUE_PRIORITY,
			   &(const struct k_work_queue_config){ .name = "sx126x_dio1" });

	/* Initialize HAL */
	ret = sx126x_hal_init(dev);
	bench_record(dev,"sx126x_hal_init",ret,0);
	if (ret < 0) {
		bench_record(dev,"HAL init failed",ret,0);
		LOG_ERR("HAL init failed: %d", ret);
		return ret;
	}

	/* Setup DIO1 interrupt callback */
	ret = sx126x_hal_set_dio1_callback(dev, sx126x_dio1_callback);
	bench_record(dev,"sx126x_hal_set_dio1_callback",ret,0);
	if (ret < 0) {
		bench_record(dev,"DIO1 callback setup failed",ret,0);
		LOG_ERR("DIO1 callback setup failed: %d", ret);
		return ret;
	}

	/* Initialize chip */
	ret = sx126x_chip_init(dev);
	bench_record(dev,"sx126x_chip_init",ret,0);
	if (ret < 0) {
		bench_record(dev,"Chip init failed",ret,0);
		LOG_ERR("Chip init failed: %d", ret);
		return ret;
	}

	/*
	 * Place the radio into sleep mode upon boot.
	 * The required lora_config call before transmission or reception
	 * will wake the radio. It is automatically placed back into sleep
	 * mode upon TX or RX completion.
	 */
	if (IS_ENABLED(CONFIG_LORA_SX126X_NATIVE_SLEEP)) {
		ret = sx126x_set_sleep(dev);
	bench_record(dev,"sx126x_set_sleep",ret,0);
		if (ret < 0) {
			bench_record(dev,"Initial sleep failed",ret,0);
			LOG_ERR("Initial sleep failed: %d", ret);
			return ret;
		}
	}

	bench_record(dev,"init_done",0,0);
	return 0;
}

/*
 * External SX126x device instantiation
 */
#ifdef CONFIG_LORA_SX126X_NATIVE_STANDALONE

#define SX126X_INIT(inst, is_1261)						\
	static struct sx126x_data sx126x_data_##inst;				\
										\
	static const struct sx126x_hal_config sx126x_config_##inst = {		\
		.spi = SPI_DT_SPEC_INST_GET(inst,				\
					    SPI_WORD_SET(8) | SPI_TRANSFER_MSB), \
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),		\
		.busy = GPIO_DT_SPEC_INST_GET(inst, busy_gpios),		\
		.dio1 = GPIO_DT_SPEC_INST_GET(inst, dio1_gpios),		\
		.is_sx1261 = is_1261,						\
		.antenna_enable = GPIO_DT_SPEC_INST_GET_OR(inst,		\
							   antenna_enable_gpios, \
							   {0}),		\
		.tx_enable = GPIO_DT_SPEC_INST_GET_OR(inst, tx_enable_gpios,	\
						      {0}),			\
		.rx_enable = GPIO_DT_SPEC_INST_GET_OR(inst, rx_enable_gpios,	\
						      {0}),			\
		.lna_bypass = GPIO_DT_SPEC_INST_GET_OR(inst, lna_bypass_gpios,	\
						       {0}),			\
		.dio2_tx_enable = DT_INST_PROP(inst, dio2_tx_enable),		\
		.dio3_tcxo_enable = DT_INST_NODE_HAS_PROP(inst, dio3_tcxo_voltage), \
		.dio3_tcxo_voltage = DT_INST_PROP_OR(inst, dio3_tcxo_voltage, 0), \
		.tcxo_startup_delay_ms = DT_INST_PROP_OR(inst,			\
						tcxo_power_startup_delay_ms, 10), \
		.rx_boosted = DT_INST_PROP(inst, rx_boosted),			\
		.regulator_ldo = DT_INST_PROP(inst, regulator_ldo),		\
		.force_ldro = DT_INST_PROP(inst, force_ldro),			\
	};									\
										\
	PM_DEVICE_DT_INST_DEFINE(inst, sx126x_pm_action);			\
										\
	DEVICE_DT_INST_DEFINE(inst, sx126x_init,				\
			      PM_DEVICE_DT_INST_GET(inst),			\
			      &sx126x_data_##inst, &sx126x_config_##inst,	\
			      POST_KERNEL, CONFIG_LORA_INIT_PRIORITY,		\
			      &sx126x_lora_api);

#define DT_DRV_COMPAT semtech_sx1262
DT_INST_FOREACH_STATUS_OKAY_VARGS(SX126X_INIT, false)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT semtech_sx1261
DT_INST_FOREACH_STATUS_OKAY_VARGS(SX126X_INIT, true)

#undef DT_DRV_COMPAT

#endif /* CONFIG_LORA_SX126X_NATIVE_STANDALONE */

/*
 * STM32WL Sub-GHz radio device instantiation
 */
#ifdef CONFIG_LORA_SX126X_NATIVE_STM32WL

#define SX126X_STM32WL_PA_OUTPUT(inst)						\
	COND_CODE_1(DT_INST_ENUM_IDX(inst, power_amplifier_output),		\
		    (SX126X_PA_OUTPUT_RFO_HP), (SX126X_PA_OUTPUT_RFO_LP))

#define SX126X_STM32WL_INIT(inst)						\
	static struct sx126x_data sx126x_stm32wl_data_##inst;			\
										\
	static const struct sx126x_hal_config sx126x_stm32wl_config_##inst = {	\
		.spi = SPI_DT_SPEC_INST_GET(inst,				\
					    SPI_WORD_SET(8) | SPI_TRANSFER_MSB), \
		.pa_output = SX126X_STM32WL_PA_OUTPUT(inst),			\
		.rfo_lp_max_power = DT_INST_PROP(inst, rfo_lp_max_power),	\
		.rfo_hp_max_power = DT_INST_PROP(inst, rfo_hp_max_power),	\
		.antenna_enable = GPIO_DT_SPEC_INST_GET_OR(inst,		\
							   antenna_enable_gpios, \
							   {0}),		\
		.tx_enable = GPIO_DT_SPEC_INST_GET_OR(inst, tx_enable_gpios,	\
						      {0}),			\
		.rx_enable = GPIO_DT_SPEC_INST_GET_OR(inst, rx_enable_gpios,	\
						      {0}),			\
		.lna_bypass = GPIO_DT_SPEC_INST_GET_OR(inst, lna_bypass_gpios,	\
						       {0}),			\
		.dio2_tx_enable = DT_INST_PROP(inst, dio2_tx_enable),		\
		.dio3_tcxo_enable = DT_INST_NODE_HAS_PROP(inst, dio3_tcxo_voltage), \
		.dio3_tcxo_voltage = DT_INST_PROP_OR(inst, dio3_tcxo_voltage, 0), \
		.tcxo_startup_delay_ms = DT_INST_PROP_OR(inst,			\
						tcxo_power_startup_delay_ms, 10), \
		.rx_boosted = DT_INST_PROP(inst, rx_boosted),			\
		.regulator_ldo = DT_INST_PROP(inst, regulator_ldo),		\
		.force_ldro = DT_INST_PROP(inst, force_ldro),			\
	};									\
										\
	PM_DEVICE_DT_INST_DEFINE(inst, sx126x_pm_action);			\
										\
	DEVICE_DT_INST_DEFINE(inst, sx126x_init,				\
			      PM_DEVICE_DT_INST_GET(inst),			\
			      &sx126x_stm32wl_data_##inst,			\
			      &sx126x_stm32wl_config_##inst,			\
			      POST_KERNEL, CONFIG_LORA_INIT_PRIORITY,		\
			      &sx126x_lora_api);

#define DT_DRV_COMPAT st_stm32wl_subghz_radio
DT_INST_FOREACH_STATUS_OKAY(SX126X_STM32WL_INIT)

#undef DT_DRV_COMPAT

#endif /* CONFIG_LORA_SX126X_NATIVE_STM32WL */
