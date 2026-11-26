// SPDX-License-Identifier: GPL-3.0-only
#define CONFIG_MT_BLE_DIAGNOSTICS 1
#include "../src/BleTransport.cpp"
#include "ble_smp_c_mock.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
extern "C" int __wrap_bt_l2cap_send_pdu(bt_l2cap_le_chan *,net_buf *,bt_conn_tx_cb_t,void *);
extern "C" void __wrap_bt_l2cap_recv(bt_conn *,net_buf *,bool);
using namespace mt::ble;
static bt_l2cap_le_chan *expectedChan;
static net_buf *expectedBuf;
static bt_conn *expectedConn;
static bt_conn_tx_cb_t expectedCallback;
static void *expectedUser;
static bool expectedComplete;
static unsigned txCalls=0,rxCalls=0;
static int txReturn=0;
static std::function<void()> duringTx;
extern "C" int __real_bt_l2cap_send_pdu(bt_l2cap_le_chan *c,net_buf *b,bt_conn_tx_cb_t cb,void *u) {
 assert(c==expectedChan&&b==expectedBuf&&cb==expectedCallback&&u==expectedUser);++txCalls;
 if(duringTx)duringTx();
 return txReturn;
}
extern "C" void __real_bt_l2cap_recv(bt_conn *c,net_buf *b,bool full) {
 assert(c==expectedConn&&b==expectedBuf&&full==expectedComplete);++rxCalls;
}
static void tx(bt_l2cap_le_chan *c,net_buf *b) {
 expectedChan=c;expectedBuf=b;const auto before=txCalls;
 assert(__wrap_bt_l2cap_send_pdu(c,b,expectedCallback,expectedUser)==txReturn);assert(txCalls==before+1);
}
static void rx(bt_conn *c,net_buf *b,bool full=true) {
 expectedConn=c;expectedBuf=b;expectedComplete=full;const auto before=rxCalls;
 __wrap_bt_l2cap_recv(c,b,full);assert(rxCalls==before+1);
}
static void callback(bt_conn *,void *,int){assert(false&&"observer must not invoke completion callback");}
int main() {
 // Keep this observer scenario's original single explicit security callback.
 mockAutoSecurity=false;
 mockAuthRegisterResult=-ENOMEM;assert(init(1)==-ENOMEM&&!ready()&&!mockAuthCallbacks);
 mockAuthRegisterResult=0;assert(init(1)==0&&ready()&&mockAuthCallbacks&&!mockBondable);
 bt_conn conn,other;mockUptime=100;callbacks.connected(&conn,0);assert(mockSecurity==BT_SECURITY_L2);
 bt_l2cap_le_chan chan{{&conn},{6}};expectedCallback=callback;int user=7;expectedUser=&user;
 uint8_t request[]={1,2,3,4,5,6,7};net_buf buf{request,sizeof(request)};
 mockUptime=123;tx(&chan,&buf);assert(diagnostic.txCount==1&&diagnostic.txLast==1&&diagnostic.txAt==23&&diagnostic.txMask==2);
 assert(request[0]==1&&buf.data==request&&buf.len==sizeof(request));
 txReturn=-ENOBUFS;request[0]=11;tx(&chan,&buf);assert(diagnostic.txResult==-ENOBUFS&&diagnostic.txLast==11);
 // Transfer ownership can destroy both arguments; observer must not touch them afterwards.
 auto *heapChan=new bt_l2cap_le_chan(chan);auto *heapBuf=new net_buf(buf);
 duringTx=[&]{delete heapChan;delete heapBuf;};tx(heapChan,heapBuf);duringTx={};
 std::cout<<"PASS exact TX arguments/return/callback ownership preserved\n";

 // Prefix-only allocation with advertised full length catches any payload access under ASAN.
 auto *prefix=static_cast<uint8_t*>(malloc(5));prefix[0]=65;prefix[1]=0;prefix[2]=6;prefix[3]=0;prefix[4]=12;
 net_buf privatePayload{prefix,69};mockUptime=140;rx(&conn,&privatePayload);
 assert(diagnostic.rxCount==1&&diagnostic.rxLast==12&&diagnostic.rxMask==(1u<<12)&&diagnostic.rxAt==40);free(prefix);
 auto *opcodeOnly=static_cast<uint8_t*>(malloc(1));*opcodeOnly=12;net_buf privateTx{opcodeOnly,65};tx(&chan,&privateTx);free(opcodeOnly);
 uint8_t bytes[]={1,0,6,0,1};net_buf in{bytes,5};
 for(unsigned n=0;n<5;++n){auto *shortData=static_cast<uint8_t*>(malloc(n?n:1));memcpy(shortData,bytes,n);net_buf shortBuf{shortData,uint16_t(n)};rx(&conn,&shortBuf);free(shortData);}
 assert(diagnostic.rxCount==1&&diagnostic.incomplete==1);
 rx(&conn,&in,false);assert(diagnostic.incomplete==2&&diagnostic.rxCount==1);
 bytes[0]=2;rx(&conn,&in);assert(diagnostic.incomplete==3);bytes[0]=1;
 bytes[2]=4;rx(&conn,&in);assert(diagnostic.rxCount==1);bytes[2]=6;
 bytes[4]=255;rx(&conn,&in);assert(diagnostic.rxLast==0&&(diagnostic.rxMask&1));
 rx(&other,&in);assert(diagnostic.rxCount==2);rx(&conn,nullptr);
 chan.tx.cid=4;tx(&chan,&buf);chan.tx.cid=6;tx(&chan,nullptr);tx(nullptr,&buf);
 std::cout<<"PASS bounded header/opcode-only reads; truncated, incomplete and non-SMP frames forwarded\n";

 const auto oldToken=session();duringTx=[&]{callbacks.disconnected(&conn,0);mockUptime=200;callbacks.connected(&conn,0);};
 tx(&chan,&buf);duringTx={};assert(session()!=oldToken&&diagnostic.txCount==0&&diagnostic.rxCount==0);
 mt_ble_trace_tx(&conn,oldToken,1,0);assert(diagnostic.txCount==0);
 mockAuthCallbacks->pairing_complete(&other,true);mockAuthCallbacks->pairing_failed(&other,BT_SECURITY_ERR_UNSPECIFIED);
 assert(!diagnostic.complete&&!diagnostic.failed);
 mockAuthCallbacks->pairing_complete(&conn,false);
 conn.security=BT_SECURITY_L2;
 callbacks.security_changed(&conn,2,BT_SECURITY_ERR_SUCCESS);
 uint8_t number[4];assert(readNum(&conn,&meshService.attrs[6],number,4,0)==4);
 // A failed pairing now closes application access for this session. Exercise
 // the successful read before failure and verify the new denial afterwards.
 mockAuthCallbacks->pairing_failed(&conn,BT_SECURITY_ERR_UNSPECIFIED);
 assert(readNum(&conn,&meshService.attrs[6],number,4,0)==BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION));
 assert(diagnostic.complete==1&&!diagnostic.bonded&&diagnostic.failed==1&&diagnostic.reason==9&&diagnostic.security==1&&diagnostic.fromNum==1);
 assert((meshService.attrs[6].permissions&BT_GATT_PERM_READ_ENCRYPT)!=0);
 diagnostic.txCount=UINT32_MAX;tx(&chan,&buf);assert(diagnostic.txCount==UINT32_MAX);
 char report[384];assert(diagnostics(report,sizeof(report))>0);assert(std::string(report).find("pair_ok=1 bonded=0 pair_fail=1 pair_e=9")!=std::string::npos);
 assert(std::string(report).find("seen=1 live=1 allow=1 health=1 fail=0 wait=0 active=0 reject=0 conn_e=0 bond_r=0 overlay_r=0 request_r=0 disc=0 level=2 sec_e=0")!=std::string::npos);
 assert(std::string(report).find("num=1 ")!=std::string::npos);
 char small[2];assert(diagnostics(small,sizeof(small))==0&&diagnostics(nullptr,0)==0);
 callbacks.disconnected(&conn,19);const auto saved=diagnostic;
 assert(diagnostic.disconnectReason==19);assert(diagnostics(report,sizeof(report))>0&&std::string(report).find("live=0 ")!=std::string::npos);
 mockAuthCallbacks->pairing_complete(&conn,true);mockAuthCallbacks->pairing_failed(&conn,BT_SECURITY_ERR_UNSPECIFIED);rx(&conn,&in);
 assert(diagnostic.complete==saved.complete&&diagnostic.failed==saved.failed&&diagnostic.rxCount==saved.rxCount);
 mockUptime=UINT32_MAX-2;callbacks.connected(&conn,0);mockUptime=3;tx(&chan,&buf);assert(diagnostic.txAt==6);
 assert(!diagnostic.complete&&!diagnostic.failed&&!diagnostic.fromNum);
 std::cout<<"PASS connection reset, stale completion, passive callbacks, saturation and time wrap\n";
 callbacks.disconnected(&conn,19);callbacks.connected(&conn,42);
 assert(diagnostic.seen&&diagnostic.connectError==42&&diagnostic.level==0);
 assert(diagnostics(report,sizeof(report))>0&&std::string(report).find("conn_e=42 ")!=std::string::npos);

}
