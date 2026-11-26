/*
 * Copyright (c) 2026 Carlo Caione <ccaione@baylibre.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_LORA_SX126X_SX126X_INTERNAL_H_
#define ZEPHYR_DRIVERS_LORA_SX126X_SX126X_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/lora/sx126x_bench.h>

#include "sx126x_hal.h"
#include "sx126x_regs.h"
#include "sx126x_airtime_math.h"
#include <zephyr/drivers/lora/sx126x_ext.h>

enum sx126x_state {
	SX126X_STATE_SLEEP = 0,
	SX126X_STATE_IDLE,
	SX126X_STATE_TX,
	SX126X_STATE_RX,
	SX126X_STATE_RX_DUTY_CYCLE,
};

struct sx126x_tx_result {
	int status;
};

struct sx126x_rx_result {
	int16_t rssi;
	int8_t snr;
	struct sx126x_packet_status packet_status;
	uint8_t len;
	int status;
};

struct sx126x_data {
#if defined(CONFIG_ZEPHCORE_DUAL_RX_BENCH) || defined(CONFIG_MT_ONLY)
	struct sx126x_bench_record bench_trace[48];
	uint8_t bench_trace_count;
	bool bench_trace_sealed;
	atomic_t bench_faults;
#endif
	struct sx126x_hal_data hal;

	/* Current state (atomic for lock-free state transitions) */
	atomic_t state;
	struct k_mutex lock;

	/* Current configuration */
	struct lora_modem_config config;
	bool config_valid;

	/* Startup-only scalar observer; no application callback or payload bytes. */
	atomic_t observation_enabled, observation_loss, observation_config_state;
	/* Metadata only:0 none,1 joined/empty-IRQ storage pause,2 owned config.
	 * Never a transmit grant. Cached tuple and permit share one short lock. */
	struct k_spinlock observation_lock;
	atomic_t observation_reconfigure_permit;
	/* Passive boot-lifetime counters, saturated and protected by observation_lock. */
	uint32_t observation_tx_started, observation_tx_done, observation_tx_timeouts;
	uint32_t observation_last_tx_irq;
	struct sx126x_stage_info send_stage, config_stage, restart_stage;
	struct sx126x_airtime_input observation_config;
	uint32_t observation_frequency;
	uint8_t observation_sync_word;
	bool observation_iq;
	struct k_msgq observation_queue;
	struct sx126x_airtime_observation observation_buffer[16];

	/* TX completion via message queue */
	struct k_msgq tx_msgq;
	struct sx126x_tx_result tx_result;

	/* RX completion via message queue */
	struct k_msgq rx_msgq;
	struct sx126x_rx_result rx_result;

	/* RX data buffer (shared between IRQ handler and recv) */
	uint8_t rx_buf[SX126X_MAX_PAYLOAD_LEN];

	/* Async RX callback */
	lora_recv_cb rx_cb;
	sx126x_recv_packet_cb rx_packet_cb;
	void *rx_cb_user_data;
	atomic_t rx_cb_gen;

	/* RX duty cycle parameters (in SX126x timeout ticks) */
	struct {
		uint32_t rx_period;
		uint32_t sleep_period;
	} duty_cycle;

	/* Async TX signal */
	struct k_poll_signal *tx_async_signal;

	/* Deferred work for interrupt handling */
	struct k_work irq_work;
	struct k_work_q dio1_wq;
	K_KERNEL_STACK_MEMBER(dio1_stack, 2560);
	const struct device *dev;

	/* Extension features (duty cycle, boost) */
	bool rx_duty_cycle_enabled;
	bool rx_boost_enabled;

	/* External FEM receive-path select, set by sx126x_set_fem_rx_enable()
	 * and acted on through lna-bypass-gpios.  Negative logic so the
	 * zero-initialised default is the LNA path, which is what every board
	 * wants and what the driver did before the knob existed.
	 *
	 * NOT the FEM chip enable.  antenna-enable-gpios stays owned by the
	 * RX/TX/sleep state machine: deasserting it in RX would shut the part
	 * down, which on the KCT8103L boards costs the LNA gain *and* the
	 * through path -- tens of dB, not the ~17 dB the bypass costs. */
	bool fem_rx_off;

	/* AgcSensiAdjust value for the configured band (DS Table 6-4), cached
	 * because it shares register 0x08AC with the RX gain bits and so has
	 * to be re-applied by every RX-gain write.  Defaults to the 868-915
	 * value the chip ships with; sx126x_config() re-selects it per
	 * frequency. */
	uint8_t agc_sensi_adjust;

	/* RX-busy latch: set by the work handler when HEADER_VALID fires for
	 * a real packet, cleared on RX_DONE / CRC_ERR / RX_TX_TIMEOUT / RX
	 * (re)start / TX-state entry.  Read by sx126x_is_receiving() as the
	 * primary source of truth for "payload phase in progress" so the TX
	 * gate stays busy across the entire packet, not just during the
	 * one-shot preamble/header IRQ window. */
	bool rx_packet_active;
	atomic_t rx_stale_releases; /* bounded activity recovery, diagnostic only */

	/* Timestamp (k_uptime_get_32() units, ms) of the first observation of
	 * PREAMBLE_DETECTED by sx126x_is_receiving() in the current RX cycle.
	 * Zero means "no preamble being tracked".  Used to bridge the gap
	 * between PREAMBLE_DETECTED (IRQ register only, not on DIO1) and
	 * HEADER_VALID (which sets rx_packet_active via the work handler).
	 * Real packets fire HEADER_VALID within an SF-aware grace period and
	 * the latch takes over; foreign-sync-word preambles never produce
	 * HEADER_VALID and are released by sx126x_is_receiving() when grace
	 * expires (it clears PREAMBLE_DETECTED and resets this field).
	 * Reset together with rx_packet_active at every RX (re)start, every
	 * terminal-event handler, and on TX-state entry. */
	atomic_t preamble_seen_at_ms;

	/* Timestamp (k_uptime_get_32() units, ms) at which rx_packet_active was
	 * promoted by HEADER_VALID.  Zero means "no payload phase being timed".
	 * Bounds the latch's lifetime: continuous RX has no symbol timer, so a
	 * HEADER_VALID whose packet never completes produces no terminal IRQ and
	 * would keep the TX gate closed forever.  sx126x_is_receiving() releases
	 * the latch once sx126x_max_payload_ms() has elapsed. */
	atomic_t header_seen_at_ms;

	/* Timestamp (k_uptime_get_32() units, ms) of the first observation of a
	 * raw HEADER_VALID bit by sx126x_is_receiving() while rx_packet_active
	 * is still false -- i.e. DIO1 fired but the work handler has not yet
	 * promoted the latch.  Zero means "not tracking".
	 *
	 * That handoff normally takes microseconds, so this exists only to bound
	 * the case where it never happens because DIO1 interrupts have stopped
	 * arriving at all.  Nothing then clears the chip's sticky IRQ bits, and
	 * rx_packet_active is never set, so header_seen_at_ms above cannot bound
	 * anything -- without this field the TX gate stays true until the
	 * Dispatcher's 4 s CAD-timeout recovery clears it, once per packet.
	 * Reset together with the other RX-busy signals. */
	atomic_t raw_header_seen_at_ms;

	/* Parked-RX watchdog.  StopTimerOnPreamble=1 means a false preamble
	 * detect parks the chip in full RX "until the next received frame"
	 * (AN1200.36 sec 3) with no IRQ fired -- nothing is lost (parked RX
	 * still receives) but duty cycling silently stops costing full RX
	 * current.  The watchdog samples the IRQ register every few seconds
	 * and re-arms the duty cycle after two consecutive sightings of
	 * PREAMBLE_DETECTED without HEADER_VALID (one packet can never span
	 * two sightings; restart sites and terminal IRQs clear the bit). */
	struct k_work_delayable dc_watchdog_work;
	uint8_t dc_watchdog_strikes;

	/* Duty-cycle preamble false-positive counter: incremented whenever
	 * IRQ_RX_TX_TIMEOUT fires during duty-cycle RX (preamble detector
	 * tripped but no valid sync/header followed -- re-arm triggered).
	 * High values indicate a noisy RF environment or preamble-detect
	 * threshold too loose; each event extends the real RX time past
	 * the nominal rx_period, increasing current draw. */
	atomic_t dc_timeout_restarts;

#ifdef CONFIG_ZEPHCORE_LORA_RETENTION_DEBUG
	/* Retention-list verification tally (see sx126x_retention_debug_check).
	 * Debug builds only -- costs nothing in production. */
	uint32_t retention_checks;
	uint32_t retention_fails;
#endif

	/* CAD state */
	lora_cad_cb cad_cb;
	void *cad_user_data;
	struct k_sem cad_sem;
	int cad_result;
	bool cad_active;
	/* Adaptive-CAD: signed offset applied to the per-SF base detPeak on
	 * every LBT CAD (set via sx126x_cad_set_peak_offset).  cad_probe_peak,
	 * when non-zero, overrides everything for one calibration probe. */
	/* CAD_RX exit-mode bookkeeping.
	 *
	 * With cadExitMode = CAD_RX (DS §13.4.7) a positive CAD does not return
	 * to standby: the chip stays in Rx on the signal it just found, until a
	 * packet completes or cadTimeout expires.  That removes the
	 * standby->SetRx round trip that otherwise eats the preamble which
	 * triggered the detection, so the calibration probe can be told whether
	 * what it heard was real instead of having to guess from a poll.
	 *
	 * cad_exit_rx marks the CAD currently in flight as one armed that way,
	 * so the CAD_DONE handler knows the chip is in Rx rather than standby.
	 * cad_rx_state then records which terminal event resolved it -- the
	 * whole point being that the chip always raises one, so nothing has to
	 * poll or infer:
	 *   IDLE   nothing in flight
	 *   ARMED  positive CAD, chip in Rx, awaiting a terminal IRQ
	 *   PACKET RX_DONE or CRC_ERR arrived -- the detection was real
	 *   TMOUT  RX_TX_TIMEOUT at cadTimeout -- nothing decodable followed
	 * Read and consumed by sx126x_cad_rx_outcome(). */
#ifdef CONFIG_MT_NORMAL_ENDPOINT
	/* Continuous stock endpoint only. CAD scan, then bounded hardware RX. */
	atomic_t lbt_handoff;
#endif
	bool cad_exit_rx;
	atomic_t cad_rx_state;

	int8_t cad_peak_offset;
	uint8_t cad_probe_peak;
};

/* Internal HAL/IRQ admission hook; it can only retire metadata authority. */
void sx126x_airtime_observer_reconfigure_invalidate(const struct device *dev);

#endif /* ZEPHYR_DRIVERS_LORA_SX126X_SX126X_INTERNAL_H_ */
