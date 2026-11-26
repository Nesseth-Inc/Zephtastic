// SPDX-License-Identifier: GPL-3.0-only
#include "SignedBleOta.h"
#include "SignedOta.h"
#include "BleOtaStream.h"
#include "../BleTransport.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

namespace mt::update {
namespace {
static bt_uuid_128 serviceUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x4fafc201,0x1fb5,0x459e,0x8fcc,0xc5c9c331914b));
static bt_uuid_128 notifyUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x62ec0272,0x3ec5,0x11eb,0xb378,0x0242ac130003));
static bt_uuid_128 writeUuid=BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x62ec0272,0x3ec5,0x11eb,0xb378,0x0242ac130005));
struct Chunk {uint32_t session;uint16_t size;uint8_t data[512];};
K_MSGQ_DEFINE(incoming,sizeof(Chunk),4,4);
static atomic_t armed,peerEpoch,subscription,overflow;

// This transient GATT database must not add an OTA handle to a bonded phone's
// durable CCC settings. Zephyr's subscription check reads this descriptor.
static ssize_t readCcc(bt_conn *conn,const bt_gatt_attr *attr,void *out,uint16_t size,uint16_t offset){
  const auto token=ble::otaSession(conn,uint32_t(atomic_get(&subscription)));
  if(!atomic_get(&armed)||!token)return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  const uint8_t value[]={uint8_t(token==uint32_t(atomic_get(&subscription))),0};
  return bt_gatt_attr_read(conn,attr,out,size,offset,value,sizeof(value));
}
static ssize_t writeCcc(bt_conn *conn,const bt_gatt_attr *,const void *data,uint16_t size,uint16_t offset,uint8_t flags){
  const auto token=ble::otaSession(conn);
  if(!atomic_get(&armed)||!token)return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(offset||flags&(BT_GATT_WRITE_FLAG_PREPARE|BT_GATT_WRITE_FLAG_EXECUTE))return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  if(!data||size!=2)return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  const auto *value=static_cast<const uint8_t *>(data);
  if(value[0]>1||value[1])return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  atomic_set(&subscription,value[0]?token:0);
  return size;
}

static ssize_t receive(bt_conn *,const bt_gatt_attr *,const void *,uint16_t,uint16_t,uint8_t);
static bt_gatt_attr attributes[]={
  BT_GATT_PRIMARY_SERVICE(&serviceUuid.uuid),
  BT_GATT_CHARACTERISTIC(&notifyUuid.uuid,BT_GATT_CHRC_NOTIFY,BT_GATT_PERM_NONE,nullptr,nullptr,nullptr),
  BT_GATT_DESCRIPTOR(BT_UUID_GATT_CCC,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT,readCcc,writeCcc,nullptr),
  BT_GATT_CHARACTERISTIC(&writeUuid.uuid,BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                         BT_GATT_PERM_WRITE_ENCRYPT,nullptr,receive,nullptr),
};
static bt_gatt_service otaService=BT_GATT_SERVICE(attributes);
static const bt_gatt_attr *notifyAttribute(){return &attributes[2];}
static ssize_t receive(bt_conn *conn,const bt_gatt_attr *,const void *bytes,uint16_t size,uint16_t offset,uint8_t flags){
  if(!atomic_get(&armed))return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  // The CCC write already established this secured epoch, including for the
  // first control write. Main rechecks native admission before consuming it.
  const auto token=ble::otaSession(conn,uint32_t(atomic_get(&subscription)));
  if(!token)return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  if(offset||flags&(BT_GATT_WRITE_FLAG_PREPARE|BT_GATT_WRITE_FLAG_EXECUTE))return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  if(!bytes||!size||size>512)return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  if(!bt_gatt_is_subscribed(conn,notifyAttribute(),BT_GATT_CCC_NOTIFY))return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
  if(!atomic_cas(&peerEpoch,0,token)&&uint32_t(atomic_get(&peerEpoch))!=token)return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
  Chunk chunk{};chunk.session=token;chunk.size=size;memcpy(chunk.data,bytes,size);
  if(k_msgq_put(&incoming,&chunk,K_NO_WAIT)){
    // Write Commands have no ATT error response. Abort the whole transfer
    // rather than continuing with silently missing bytes.
    atomic_set(&overflow,1);return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
  }
  return size;
}

enum class Action {Stop,Register,Advertise,Notify,Cleanup};
static struct {Action action;uint32_t token;uint16_t length;char text[128];int result;} job;
static atomic_t finished, pendingError;
static void errorSent(bt_conn *,void *epoch){
  (void)atomic_cas(&pendingError,static_cast<uint32_t>(reinterpret_cast<uintptr_t>(epoch)),0);
}
static bool registered,linkParamsRequested; // accessed only on the system workqueue
static int stopAdvertising(){
  const int rc=bt_le_adv_stop();
  if(rc&&rc!=-EALREADY)return rc;
  if(auto *peer=ble::otaDisconnectTarget(job.token)){
    const int disconnected=bt_conn_disconnect(peer,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    bt_conn_unref(peer);
    if(disconnected&&disconnected!=-ENOTCONN&&disconnected!=-EALREADY)return disconnected;
  }
  return 0;
}
static int advertise(){
  static const uint8_t flags=BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR;
  const bt_data ad[]={BT_DATA(BT_DATA_FLAGS,&flags,1),BT_DATA(BT_DATA_UUID128_ALL,serviceUuid.val,16)};
  const char *name=bt_get_name();const bt_data scan[]={BT_DATA(BT_DATA_NAME_COMPLETE,name,uint8_t(strlen(name)))};
  return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,ad,2,scan,1);
}
static int sendLine(){
  bt_conn *peer;
  const int admission=ble::otaAccess(job.token,&peer);
  if(admission)return admission;
  const auto mtu=bt_gatt_get_mtu(peer);
  int rc=-ENOTCONN;
  if(bt_gatt_is_subscribed(peer,notifyAttribute(),BT_GATT_CCC_NOTIFY)){
    if(!linkParamsRequested){
      // Pinned ESP32 OTA loader requests 15 ms, no latency and 4 s supervision.
      // Zephyr queues an early request in place of its normal 5 s GAP update.
      // A central may decline. The stock ACK stream still runs on its chosen link.
      static const bt_le_conn_param params{12,12,0,400};
      linkParamsRequested=true;
      (void)bt_conn_le_param_update(peer,&params);
    }
    const bool error=job.length>=3&&!memcmp(job.text,"ERR",3);
    const char *value=job.text;
    size_t length=job.length;
    if(mtu<3||length>size_t(mtu-3)){
      // Android matches "Hash Mismatch" for terminal image validation. Keep
      // the complete stock error type even in a 20-byte notification.
      value=error?(length>=17&&!memcmp(value,"ERR Hash Mismatch",17)?"ERR Hash Mismatch\n":"ERR Transfer\n"):"ERR MTU\n";
      length=strlen(value);
    }
    if(mtu<3||length>size_t(mtu-3))rc=-EMSGSIZE;
    else if(error){
      bt_gatt_notify_params params{};
      params.attr=notifyAttribute();params.data=value;params.len=uint16_t(length);
      params.func=errorSent;params.user_data=reinterpret_cast<void *>(uintptr_t(job.token));
      rc=bt_gatt_notify_cb(peer,&params);
    }else rc=bt_gatt_notify(peer,notifyAttribute(),value,uint16_t(length));
  }
  bt_conn_unref(peer);return rc;
}
static void runJob(k_work *){
  int rc=-ECANCELED;
  if(job.action==Action::Stop){
    rc=stopAdvertising();
  }else if(job.action==Action::Cleanup){
    rc=registered?bt_gatt_service_unregister(&otaService):0;
    if(!rc)registered=false;
  }else if(atomic_get(&armed)){
    switch(job.action){
      case Action::Register:linkParamsRequested=false;rc=bt_gatt_service_register(&otaService);registered=rc==0;break;
      case Action::Advertise:rc=advertise();break;
      case Action::Notify:rc=sendLine();break;
      case Action::Stop:case Action::Cleanup:break;
    }
  }
  job.result=rc;atomic_set(&finished,1);
}
K_WORK_DEFINE(stackWork,runJob);

using Progress=void (*)();
static void tick(Progress progress){progress();k_sleep(K_MSEC(5));}
static bool inFlight; // owned by the single synchronous update controller
static int perform(Action action,Progress progress){
  if(inFlight)return -EBUSY;
  job.action=action;atomic_set(&finished,0);inFlight=true;
  if(k_work_submit(&stackWork)<0){inFlight=false;return -EIO;}
  const auto deadline=k_uptime_get()+10000;
  do{
    if(atomic_get(&finished)){inFlight=false;return job.result;}
    tick(progress);
  }while(k_uptime_get()<deadline);
  // No new job may overwrite a parked worker's static input. The update
  // owner reboots the current or verified trial image immediately on return.
  return -ETIMEDOUT;
}
// Requesting HCI disconnect is asynchronous. Leave the transient service registered
// until the disconnected callback retires this exact epoch or the bound expires.
static bool stopAndWait(Progress progress,int timeoutMs){
  if(perform(Action::Stop,progress))return false;
  const uint32_t epoch=job.token;
  const int64_t deadline=k_uptime_get()+timeoutMs;
  while(epoch&&ble::session()==epoch&&k_uptime_get()<deadline)tick(progress);
  return !ble::connected();
}
static void drain(uint32_t epoch,Progress progress){
  const auto until=k_uptime_get()+2000;
  while(ble::session()==epoch&&k_uptime_get()<until)tick(progress);
}
// A one-connection controller can report the old PhoneAPI link disconnected
// before Zephyr recycles its connection object for connectable advertising.
static int startAdvertising(Progress progress){
  const auto deadline=k_uptime_get()+3000;
  int rc;
  do{
    rc=perform(Action::Advertise,progress);
    if(rc!=-ENOMEM&&rc!=-EBUSY&&rc!=-EAGAIN)return rc;
    tick(progress);
  }while(k_uptime_get()<deadline);
  return rc;
}
static bool notify(uint32_t token,const char *line,size_t size,Progress progress){
  if(!size||size>sizeof(job.text)||inFlight)return false;
  job.token=token;job.length=uint16_t(size);memcpy(job.text,line,size);
  const bool error=size>=3&&!memcmp(line,"ERR",3);
  if(error)atomic_set(&pendingError,token);
  const auto deadline=k_uptime_get()+9000;
  do{
    const int rc=perform(Action::Notify,progress);
    if(!rc){
      if(!error)return true;
      // Controller completion does not acknowledge receipt by the client. Give
      // an error the same delivery window as the terminal OK before teardown.
      drain(token,progress);
      const bool sent=atomic_get(&pendingError)==0;
      (void)atomic_cas(&pendingError,token,0);
      return sent&&ble::session()==token;
    }
    if(rc!=-ENOMEM&&rc!=-EAGAIN)break;
    tick(progress);
  }while(k_uptime_get()<deadline&&ble::session()==token);
  if(error)(void)atomic_cas(&pendingError,token,0);
  return false;
}
static void transfer(const uint8_t expected[32],Progress progress){
  signedOta::Session sink(expected);
  BleOtaStream stream;
  constexpr int64_t StartMs=120000,IdleMs=45000,StockAckMs=10000;
  constexpr size_t MinimumWrite=20; // ATT_MTU 23 less the 3-byte write header
  auto active=k_uptime_get();
  auto deadline=active+StartMs;
  for(;;){
    progress();
    const auto token=uint32_t(atomic_get(&peerEpoch));const auto now=k_uptime_get();
    if(now>=deadline||atomic_get(&overflow)||(token&&ble::session()!=token)||(token&&now-active>=IdleMs))return;
    if(!token&&ble::disconnected()&&!ble::connected()){
      const int rc=startAdvertising(progress);
      if(rc&&rc!=-EALREADY)return;
    }
    Chunk chunk;
    if(k_msgq_get(&incoming,&chunk,K_NO_WAIT)){k_sleep(K_MSEC(5));continue;}
    // Android streams image data in Write Commands without an ATT error response.
    // Retain this exact chunk in RAM while main services a native Settings
    // lease. Recheck transport, link security and epoch before any flash I/O.
    const auto waitUntil=k_uptime_get()+9000;
    int admission;
    while((admission=ble::otaAccess(chunk.session))==-EAGAIN){
      tick(progress);
      if(k_uptime_get()>=waitUntil||atomic_get(&overflow))return;
    }
    if(admission||atomic_get(&overflow))return;
    active=k_uptime_get();
    if(active>=deadline)return;
    const bool hadImage=sink.downloading();
    if(!stream.write(sink,chunk.data,chunk.size,[&](const char *text,size_t size){
          return notify(chunk.session,text,size,progress);
        }))return;
    if(!hadImage&&sink.downloading()){
      // The signed sink accepts only one START, for the armed digest and a
      // slot-bounded size. Budget one stock ACK timeout per minimum-MTU write.
      // Neither commands nor later writes extend this absolute deadline.
      const auto chunks=(sink.imageSize()+MinimumWrite-1)/MinimumWrite;
      deadline=active+IdleMs+int64_t(chunks)*StockAckMs;
    }else if(hadImage&&!sink.downloading()&&!sink.complete())return;
    if(sink.complete()){
      drain(chunk.session,progress);
      return;
    }
  }
}
}
bool signedBleAvailable(){return ble::otaPolicyReady();}
void serveSignedBle(const uint8_t expected[32],void (*progress)()){
  if(!expected||!progress||inFlight||!ble::beginOta())return;
  k_msgq_purge(&incoming);atomic_set(&overflow,0);atomic_set(&peerEpoch,0);atomic_set(&subscription,0);atomic_set(&armed,1);
  if(stopAndWait(progress,3500)){
    (void)ble::disconnected();
    if(!perform(Action::Register,progress)&&!startAdvertising(progress))transfer(expected,progress);
  }
  atomic_set(&armed,0);
  if(!inFlight)(void)stopAndWait(progress,5000);
  if(!inFlight)(void)perform(Action::Cleanup,progress);
  k_msgq_purge(&incoming);
}
}
