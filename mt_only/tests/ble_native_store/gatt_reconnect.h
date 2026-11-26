// SPDX-License-Identifier: Apache-2.0
// The security-request decision and connection callback order below are copied
// from Zephyr 1ee3b93134be23c16ab16b9eb4183bfef63447a6:
// subsys/bluetooth/host/gatt.c (bt_gatt_connected) and conn.c (bt_conn_connected).
// Its attribute walk has already selected the strongest security level required
// by active restored CCCs matching this peer (update_ccc).
namespace gatt_reconnect {
constexpr int CONFIG_BT_SMP=1;
constexpr int CONFIG_BT_GATT_AUTO_SEC_REQ=ZT_TEST_XIAO_GATT_AUTO_SEC_REQ;
constexpr int BT_HCI_ROLE_CENTRAL=0;
struct bt_conn {
  ::bt_conn radio;
  int role=1;
  bool restoredCcc=true;
  unsigned earlyRequests=0;
};
struct conn_data { bt_security_t sec; };

static bt_security_t bt_conn_get_security(bt_conn *conn){return conn->radio.level;}
static int bt_conn_set_security(bt_conn *conn,bt_security_t level){
  assert(conn->radio.auth==nullptr && level==BT_SECURITY_L2);
  ++conn->earlyRequests;
  // get_auth()/get_io_capa() latch the global auth table in the SMP object.
  // Zephyr's overlay compare-and-set now returns -EALREADY for this connection.
  mockOverlayError=-EALREADY;
  return 0;
}
#define LOG_WRN(...) ((void)0)
static void bt_l2cap_connected(bt_conn *conn){
  const struct conn_data data{conn->restoredCcc?BT_SECURITY_L2:BT_SECURITY_L1};
	if (IS_ENABLED(CONFIG_BT_SMP) &&
	    (conn->role == BT_HCI_ROLE_CENTRAL ||
	     IS_ENABLED(CONFIG_BT_GATT_AUTO_SEC_REQ)) &&
	    bt_conn_get_security(conn) < data.sec) {
		int err = bt_conn_set_security(conn, data.sec);

		if (err) {
			LOG_WRN("Failed to set security for bonded peer (%d)", err);
		}
	}
}
#undef LOG_WRN
static void schedule_auto_initiated_procedures(bt_conn *){}
static void notify_connected(bt_conn *conn){
  if(conn->role!=BT_HCI_ROLE_CENTRAL)attach(conn->radio);
}

void bt_conn_connected(struct bt_conn *conn)
{
	schedule_auto_initiated_procedures(conn);
	bt_l2cap_connected(conn);
	notify_connected(conn);
}

static void check(const std::string &test){
  bt_conn conn;
  conn.restoredCcc=test!="restored_ccc_absent";
  mockBootBond=true;
  store::initialize();
  assert(store::beginBoot(true));
  mt::BluetoothSettings settings;
  settings.mode=mt::BluetoothSettings::FixedPin;
  settings.fixedPin=234567;
  const int result=init(123,settings,true);
  store::endBoot(result);
  assert(result==0);
  poll();
  if(test=="restored_ccc_central"){
    conn.role=BT_HCI_ROLE_CENTRAL;
    bt_conn_connected(&conn);
    assert(conn.earlyRequests==1 && !connected());
    return;
  }
  bt_conn_connected(&conn);
  poll();
  assert(mockDisconnects==0 && conn.earlyRequests==0);
  assert(conn.radio.bondable && conn.radio.auth==&pinCallbacks && conn.radio.requested==BT_SECURITY_L3);
  assert(write(conn.radio)<0);
  if(test=="restored_ccc_missing_key"){
    onSecurityChanged(&conn.radio,BT_SECURITY_L1,BT_SECURITY_ERR_PIN_OR_KEY_MISSING);
    poll();
    assert(mockDisconnects==1 && write(conn.radio)<0);
    return;
  }
  secure(conn.radio,BT_SECURITY_L2);
  assert(write(conn.radio)<0);
  secure(conn.radio,BT_SECURITY_L3);
  assert(write(conn.radio)==1 && nativeWrites==0);
}
} // namespace gatt_reconnect
