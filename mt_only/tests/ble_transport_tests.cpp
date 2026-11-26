// SPDX-License-Identifier: GPL-3.0-only
#include "../src/BleTransport.cpp"
#include <iostream>
using namespace mt::ble;
int main() {
 mockSecurityChanged=callbacks.security_changed;
 bt_conn connection;uint8_t bytes[600]{},out[600]{};Packet p{};
 assert(init(0x12345678)==0&&ready()&&!connected()&&!mockBondable);
 callbacks.connected(&connection,0);assert(connected()&&mockSecurity==BT_SECURITY_L2);
 assert(meshService.attrs[2].permissions==(BT_GATT_PERM_WRITE_ENCRYPT|BT_GATT_PERM_PREPARE_WRITE));
 assert(meshService.attrs[4].permissions==BT_GATT_PERM_READ_ENCRYPT);
 assert(meshService.attrs[7].permissions==(BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT));
 std::cout<<"PASS BLE encrypted nonbonding access requirement\n";
 for(unsigned n:{1u,20u,244u,512u}) {
  memset(bytes,n,sizeof(bytes));assert(writeTo(&connection,nullptr,bytes,n,0,BT_GATT_WRITE_FLAG_EXECUTE)==n);
  assert(receive(p)&&p.length==n&&!memcmp(bytes,p.bytes,n));
 }
 assert(writeTo(&connection,nullptr,bytes,513,0,0)==BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
 assert(writeTo(&connection,nullptr,bytes,20,500,BT_GATT_WRITE_FLAG_PREPARE)==BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
 assert(writeTo(&connection,nullptr,bytes,20,10,0)==BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
 assert(writeTo(&connection,nullptr,bytes,0,0,0)==BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
 assert(writeTo(&connection,nullptr,bytes,20,20,BT_GATT_WRITE_FLAG_PREPARE)==0&&!receive(p));
 // Zephyr owns assembly; invoke the characteristic for every prepared
 // fragment and supply its assembled value on Execute as ATT does.
 for (unsigned mtu : {23u,247u}) {
  uint8_t value[512];
  for (size_t i=0;i<sizeof(value);++i)value[i]=uint8_t(i);
  unsigned fragments=0;
  for (size_t offset=0;offset<sizeof(value);) {
   const size_t left=sizeof(value)-offset;
   const size_t size=left<mtu-5?left:mtu-5;
   assert(writeTo(&connection,nullptr,value+offset,size,offset,BT_GATT_WRITE_FLAG_PREPARE)==0);
   assert(!receive(p));offset+=size;++fragments;
  }
  assert(fragments==(mtu==23?29u:3u));
  assert(writeTo(&connection,nullptr,value,sizeof(value),0,BT_GATT_WRITE_FLAG_EXECUTE)==512);
  assert(receive(p)&&p.session==session()&&p.length==512&&!memcmp(p.bytes,value,512));
 }
 std::cout<<"PASS BLE bounded writes and prepare/execute semantics\n";
 for(unsigned i=0;i<8;++i)assert(writeTo(&connection,nullptr,bytes,1,0,0)==1);
 assert(writeTo(&connection,nullptr,bytes,1,0,0)==BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES));
 while(receive(p)){};
 bytes[0]=0x94;bytes[1]=0xc3;bytes[2]=2;bytes[3]=0;
 for(unsigned i=0;i<512;++i)bytes[i+4]=i;
 for(unsigned mtu:{23u,247u}) {
  assert(send(bytes,516,session(),true));
  size_t offset=0;
  while(offset<512){
   const size_t expected=std::min<size_t>(512-offset,mtu-1);
   assert(readFrom(&connection,nullptr,out,mtu-1,offset)==ssize_t(expected));
   assert(!memcmp(out,bytes+4+offset,expected));offset+=expected;
  }
  assert(readFrom(&connection,nullptr,out,mtu-1,512)==0);
 }
 assert(send(bytes,516,session()));
 assert(readFrom(&connection,nullptr,out,20,0)==20&&!memcmp(out,bytes+4,20));
 assert(readFrom(&connection,nullptr,out,244,20)==244&&!memcmp(out,bytes+24,244));
 assert(readFrom(&connection,nullptr,out,512,264)==248&&!memcmp(out,bytes+268,248));
 assert(readFrom(&connection,nullptr,out,1,513)==BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
 std::cout<<"PASS BLE queue capacity and stable long-read offsets\n";
 assert(send(bytes,516,session()));assert(writeTo(&connection,nullptr,bytes,1,0,0)==1);
 const auto oldSession=session();assert(receive(p)&&p.session==oldSession);
 callbacks.disconnected(&connection,0);assert(!connected()&&disconnected()&&!disconnected());
 assert(!receive(p)&&!send(bytes,516,session()));
 callbacks.connected(&connection,0);
 assert(!reading.length);for(auto b:reading.bytes)assert(!b);
 unsigned dispatched=0;auto handler=[](const Packet &,void *arg){++*static_cast<unsigned*>(arg);};
 assert(session()!=oldSession&&!send(bytes,516,oldSession));
 assert(!dispatch(p,handler,&dispatched)&&dispatched==0);
 assert(writeTo(&connection,nullptr,bytes,1,0,0)==1&&receive(p));
 assert(p.session==session()&&dispatch(p,handler,&dispatched)&&dispatched==1);
 auto reply=[](const Packet &request,void *){const uint8_t response[]={0x94,0xc3,0,1,0x08};assert(send(response,sizeof(response),request.session));};
 assert(dispatch(p,reply,nullptr));assert(readFrom(&connection,nullptr,out,512,0)==1&&out[0]==0x08);
 // Empty read clears the response snapshot before checking old retained data.
 assert(readFrom(&connection,nullptr,out,512,0)==0);

 bt_conn staleConnection;
 // Current transport rejects every unadmitted connection at the auth gate.
 assert(writeTo(&staleConnection,nullptr,bytes,1,0,0)==BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION));
 assert(readFrom(&staleConnection,nullptr,out,1,0)==BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION));
 assert(readNum(&staleConnection,nullptr,out,4,0)==BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION));
 callbacks.disconnected(&staleConnection,0);assert(connected());
 assert(readFrom(&connection,nullptr,out,1,1)==BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
 assert(readFrom(&connection,nullptr,out,512,0)==0&&!receive(p));
 assert(!reading.length);
 std::cout<<"PASS BLE retained snapshot and queues cleared on reconnect\n";
 assert(!send(nullptr,4,session()));assert(!send(bytes,3,session()));assert(!send(bytes,517,session()));
 assert(!send(bytes,4,session()));bytes[3]=1;assert(!send(bytes,516,session()));bytes[3]=0;
 assert(send(bytes,516,session(),true));assert(!send(bytes,516,session(),true));
 assert(readFrom(&connection,nullptr,out,sizeof(out),0)>0);
 for(unsigned i=0;i<MT_BLE_OUT_QUEUE_DEPTH;++i){bytes[4]=uint8_t(i);assert(send(bytes,516,session()));}
 bytes[4]=0xff;assert(!send(bytes,516,session()));
 assert(readFrom(&connection,nullptr,out,sizeof(out),0)==512&&out[0]==0);
 assert(send(bytes,516,session()));
 for(unsigned i=1;i<MT_BLE_OUT_QUEUE_DEPTH;++i)
  assert(readFrom(&connection,nullptr,out,sizeof(out),0)==512&&out[0]==i);
 assert(readFrom(&connection,nullptr,out,sizeof(out),0)==512&&out[0]==0xff);
 assert(readFrom(&connection,nullptr,out,sizeof(out),0)==0);
 mockNotifyResult=-ENOMEM;poll();assert(mockNotifyCalls==1);mockNotifyResult=0;poll();assert(mockNotifyCalls==2);poll();assert(mockNotifyCalls==2);
 callbacks.disconnected(&connection,0);mockAdvertiseResult=-EAGAIN;poll();const int attempts=mockAdvertiseCalls;
 mockAdvertiseResult=0;poll();assert(mockAdvertiseCalls==attempts+1&&ready());
 std::cout<<"PASS BLE malformed frames and transient retry paths\n";
}
