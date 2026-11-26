// SPDX-License-Identifier: GPL-3.0-only
// Opt-in observer of pinned host boundaries. No buffer ownership is acquired,
// no arguments are changed, and no pairing material is copied or inspected.
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/sys/byteorder.h>
#include "conn_internal.h"
#include "l2cap_internal.h"
#include "BleDiagnostics.h"

int __real_bt_l2cap_send_pdu(struct bt_l2cap_le_chan *, struct net_buf *, bt_conn_tx_cb_t, void *);
void __real_bt_l2cap_recv(struct bt_conn *, struct net_buf *, bool);

int __wrap_bt_l2cap_send_pdu(struct bt_l2cap_le_chan *chan, struct net_buf *buf,
                           bt_conn_tx_cb_t cb, void *user_data)
{
    struct bt_conn *conn = NULL;
    uint32_t token = 0;
    uint8_t opcode = 0;
    if (chan && chan->tx.cid == BT_L2CAP_CID_SMP && buf && buf->len) {
        conn = chan->chan.conn;
        token = mt_ble_trace_token(conn);
        opcode = buf->data[0];
    }
    // Ownership can transfer here; never dereference chan or buf afterwards.
    int result = __real_bt_l2cap_send_pdu(chan, buf, cb, user_data);
    if (token) {
        mt_ble_trace_tx(conn, token, opcode, result);
    }
    return result;
}

void __wrap_bt_l2cap_recv(struct bt_conn *conn, struct net_buf *buf, bool complete)
{
    if (buf && buf->len >= BT_L2CAP_HDR_SIZE &&
        sys_get_le16(buf->data + 2) == BT_L2CAP_CID_SMP) {
        // conn.c reassembles ACL fragments before complete=true. Incomplete or
        // length-inconsistent PDUs count separately, without reading an opcode.
        bool full = complete && buf->len > BT_L2CAP_HDR_SIZE &&
                    sys_get_le16(buf->data) == buf->len - BT_L2CAP_HDR_SIZE;
        mt_ble_trace_rx(conn, full ? buf->data[BT_L2CAP_HDR_SIZE] : 0, full);
    }
    __real_bt_l2cap_recv(conn, buf, complete);
}
