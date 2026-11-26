// SPDX-License-Identifier: GPL-3.0-only
#include "../../src/BleTransport.cpp"
#include "../../src/security/LogRadioWire.h"
#include <pb_decode.h>
#include <cstdio>
PB_BIND(meshtastic_LogRecord,meshtastic_LogRecord,2)
using namespace mt::ble;
using Log=mt::api::ApiLogBuffer;

static void testAttPressure(bt_conn &peer,LogRadioValue firstLog,LogRadioValue secondLog){
  const auto epoch=session();
  const uint8_t data[]={0x94,0xc3,0,1,0x71};
  const auto *num=&meshService.attrs[6],*log=&meshService.attrs[9];
  mockDeferWork=true;mockAttBudget=0;mockAttDelivered.clear();
  assert(send(data,sizeof(data),epoch));publishLogRadio(epoch,firstLog);
  poll();assert(mockPendingWork&&peer.references==2);
  // Park the system worker for longer than the 8 s hardware watchdog. Main
  // must still complete every poll, without trying to allocate an ATT buffer.
  const auto startCalls=mockNotifyCalls;
  for(unsigned i=0;i<1700;++i){mockUptime+=5;poll();}
  assert(mockNotifyCalls==startCalls&&mockPendingWork);
  mockRunWork();assert(mockNotifyCalls==startCalls+2&&mockAttDelivered.empty());

  assert(send(data,sizeof(data),epoch));publishLogRadio(epoch,secondLog);
  // One TX buffer goes to the latest FromNum. The first log remains at the
  // head until a buffer is available, then the second follows it.
  mockAttBudget=1;poll();mockRunWork();
  assert(mockAttDelivered.size()==1&&mockAttDelivered[0].attr==num);
  uint8_t current[4]{};
  assert(readNum(&peer,num,current,sizeof(current),0)==4);
  assert(mockAttDelivered[0].data==std::vector<uint8_t>(current,current+4));
  mockUptime+=249;poll();mockRunWork();assert(mockAttDelivered.size()==1);
  ++mockUptime;poll();mockRunWork();assert(mockAttDelivered.size()==1);
  mockAttBudget=2;mockUptime+=250;poll();mockRunWork();poll();mockRunWork();
  assert(mockAttDelivered.size()==3&&mockAttDelivered[1].attr==log&&mockAttDelivered[2].attr==log);
  assert(mockAttDelivered[1].data==std::vector<uint8_t>(firstLog.bytes,firstLog.bytes+firstLog.size));
  assert(mockAttDelivered[2].data==std::vector<uint8_t>(secondLog.bytes,secondLog.bytes+secondLog.size));
  poll();assert(!mockPendingWork);

  // Ring overflow after submission must neither send the evicted oldest
  // value nor consume a later replacement when the attempt completes.
  publishLogRadio(epoch,firstLog);poll();
  mockDuringSubscribe=[&](){for(unsigned i=0;i<LogQueueDepth;++i)publishLogRadio(epoch,secondLog);};
  mockAttBudget=LogQueueDepth;mockRunWork();mockDuringSubscribe={};
  const auto before=mockAttDelivered.size();
  for(unsigned i=0;i<LogQueueDepth;++i){poll();mockRunWork();}
  assert(mockAttDelivered.size()==before+LogQueueDepth);
  for(size_t i=before;i<mockAttDelivered.size();++i)
    assert(mockAttDelivered[i].data==std::vector<uint8_t>(secondLog.bytes,secondLog.bytes+secondLog.size));
  poll();assert(!mockPendingWork);

  // A queued batch has copied bytes and no peer reference. Revocation during
  // the stack's subscription check and a reconnect before execution both win.
  mockAttBudget=3;publishLogRadio(epoch,firstLog);poll();
  mockDuringSubscribe=[](){setLogRadioEnabled(false);};
  mockRunWork();mockDuringSubscribe={};poll();assert(!mockPendingWork);
  const auto completed=before+LogQueueDepth;assert(mockAttDelivered.size()==completed);
  setLogRadioEnabled(true);publishLogRadio(epoch,secondLog);assert(send(data,sizeof(data),epoch));poll();
  callbacks.disconnected(&peer,0x13);assert(peer.references==1);
  bt_conn replacement;callbacks.connected(&replacement,0);
  replacement.security=BT_SECURITY_L2;callbacks.security_changed(&replacement,BT_SECURITY_L2,BT_SECURITY_ERR_SUCCESS);
  mockRunWork();poll();assert(mockAttDelivered.size()==completed);
  assert(session()!=epoch&&replacement.references==2);
  assert(send(data,sizeof(data),session()));publishLogRadio(session(),firstLog);poll();
  auto disabled=mt::BluetoothSettings::defaults();disabled.enabled=false;
  assert(requestConfiguration(disabled,true));mockRunWork();
  assert(mockAttDelivered.size()==completed);
  callbacks.disconnected(&replacement,0x13);poll();assert(!ready()&&!mockPendingWork);
  mockDeferWork=false;
  puts("PASS bounded watchdog owner polling, ATT exhaustion, ordered retries, rollover of full log queue, subscription revocation, policy disable and connection replacement");
}

int main(){
  Log logs;meshtastic_LogRecord scratch{};LogRadioValue line{};
  auto local=[](uint32_t utc){return utc?utc+3600:0;};
  const auto *attr=&meshService.attrs[9];
  assert(attr->uuid==&logUuid.uuid&&attr->read&&attr->permissions==BT_GATT_PERM_READ_ENCRYPT);
  assert(meshService.attrs[10].permissions==(BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT));
  bt_conn first,other;uint8_t out[64]{};
  auto read=[&](bt_conn &conn,unsigned at=0,unsigned count=64){
    return attr->read(&conn,attr,out,count,at);
  };
  mockSecurityChanged=callbacks.security_changed;mockAutoSecurity=false;
  assert(init(0x12345678)==0);callbacks.connected(&first,0);
  setLogRadioEnabled(true);assert(!logRadioSession()&&read(first)<0);
  first.security=BT_SECURITY_L2;callbacks.security_changed(&first,BT_SECURITY_L2,BT_SECURITY_ERR_SUCCESS);
  const uint32_t firstEpoch=logRadioSession();assert(firstEpoch);
  assert(read(other)<0&&read(first)==0);
  assert(!mt::api::encodeLogRadio(logs,scratch,line,local));
  logs.setEnabled(true);assert(!logs.record(Log::Event(255),11,true,1));
  assert(logs.record(Log::Event::UsbSessionOpened,0,true,1));
  assert(mt::api::encodeLogRadio(logs,scratch,line,local));
  const uint8_t stock[]={0x0a,18,'U','S','B',' ','s','e','s','s','i','o','n',' ','o','p','e','n','e','d',
                         0x15,0x11,0x0e,0,0,0x1a,3,'u','s','b',0x20,20};
  assert(line.size==sizeof(stock)&&!memcmp(line.bytes,stock,sizeof(stock)));
  publishLogRadio(firstEpoch,line);poll();assert(mockNotifyCalls==0);
  assert(read(first,0,8)==8&&!memcmp(out,stock,8));
  const auto previous=line;
  assert(logs.record(Log::Event::RadioTxRejectedErrno,-4095,false,0));
  assert(mt::api::encodeLogRadio(logs,scratch,line,local));
  mockLogSubscribed=true;publishLogRadio(firstEpoch,line);poll();
  assert(mockNotifyCalls==1&&mockNotifyPeer==&first&&mockNotifyAttr==attr);
  meshtastic_LogRecord decoded{};
  auto in=pb_istream_from_buffer(mockNotification.data(),mockNotification.size());
  assert(pb_decode(&in,meshtastic_LogRecord_fields,&decoded)&&!in.bytes_left);
  assert(!strcmp(decoded.message,"TX rejected errno=-4095")&&!strcmp(decoded.source,"radio"));
  assert(decoded.time==0&&decoded.level==meshtastic_LogRecord_Level_WARNING);
  assert(read(first,8)==int(sizeof(stock)-8)&&!memcmp(out,stock+8,sizeof(stock)-8));
  assert(read(first)==line.size&&!memcmp(out,line.bytes,line.size));
  // Unsent values survive a read; ordinary FromNum/FromRadio remain independent.
  uint8_t frame[]={0x94,0xc3,0,2,0xaa,0xbb};
  assert(send(frame,sizeof(frame),firstEpoch));poll();
  assert(mockNotifyAttr==&meshService.attrs[6]&&mockNotification.size()==4);
  assert(readFrom(&first,nullptr,out,sizeof(out),0)==2&&out[0]==0xaa&&out[1]==0xbb);
  // A short ATT bearer reads the complete protobuf through ordinary Read Blob.
  mockMtu=23;const auto calls=mockNotifyCalls;publishLogRadio(firstEpoch,previous);poll();
  assert(mockNotifyCalls==calls&&read(first)==previous.size);mockMtu=128;
  // A rejected notification yields to transport traffic for the donor's 250ms window.
  mockNotifyResult=-ENOMEM;publishLogRadio(firstEpoch,line);poll();assert(mockNotifyCalls==calls+1);
  publishLogRadio(firstEpoch,previous);mockNotifyResult=0;mockUptime+=249;poll();
  assert(mockNotifyCalls==calls+1);++mockUptime;poll();assert(mockNotifyCalls==calls+2);
  setLogRadioEnabled(false);assert(read(first)==0&&read(first,1)<0&&!logRadioSession());
  publishLogRadio(firstEpoch,previous);setLogRadioEnabled(true);assert(read(first)==0);
  publishLogRadio(firstEpoch,line);setIngressOpen(false);assert(read(first)<0);
  setIngressOpen(true);assert(read(first)==0);
  publishLogRadio(firstEpoch,previous);first.security=BT_SECURITY_L1;
  callbacks.security_changed(&first,BT_SECURITY_L1,BT_SECURITY_ERR_SUCCESS);assert(read(first)<0);
  first.security=BT_SECURITY_L2;callbacks.security_changed(&first,BT_SECURITY_L2,BT_SECURITY_ERR_SUCCESS);
  assert(read(first)==0);
  publishLogRadio(firstEpoch,line);assert(beginAdmin(firstEpoch));assert(read(first)<0&&!logRadioSession());
  callbacks.disconnected(&first,0x13);assert(requestConfiguration(mt::BluetoothSettings::defaults(),true));poll();
  callbacks.connected(&other,0);other.security=BT_SECURITY_L2;
  callbacks.security_changed(&other,BT_SECURITY_L2,BT_SECURITY_ERR_SUCCESS);
  assert(logRadioSession()!=firstEpoch&&read(other)==0);
  publishLogRadio(firstEpoch,line);assert(read(other)==0);
  testAttPressure(other,line,previous);
  puts("PASS stock bare LogRecord, typed whitelist, encrypted GATT/CCC, session isolation, interleaved read/blob, notification backoff, normal PhoneAPI");
}
