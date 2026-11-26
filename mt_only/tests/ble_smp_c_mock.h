// SPDX-License-Identifier: GPL-3.0-only
// C ABI stand-ins for the actual wrapper translation unit; no stack behavior.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
struct bt_conn;
struct bt_l2cap_chan {struct bt_conn *conn;};
struct bt_l2cap_le_chan {struct bt_l2cap_chan chan;struct {uint16_t cid;} tx;};
struct net_buf {uint8_t *data;uint16_t len;};
typedef void (*bt_conn_tx_cb_t)(struct bt_conn *,void *,int);
#define BT_L2CAP_CID_SMP 6
#define BT_L2CAP_HDR_SIZE 4
static inline uint16_t sys_get_le16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
