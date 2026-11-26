// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct bt_conn;
// Internal diagnostic hooks. Only an opaque connection pointer is compared;
// it is never retained by the observer or rendered in diagnostics.
uint32_t mt_ble_trace_token(struct bt_conn *conn);
void mt_ble_trace_rx(struct bt_conn *conn, uint8_t opcode, bool complete);
void mt_ble_trace_tx(struct bt_conn *conn, uint32_t token, uint8_t opcode, int result);
#ifdef __cplusplus
}
#endif
