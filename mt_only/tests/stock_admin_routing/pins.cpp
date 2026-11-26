// SPDX-License-Identifier: GPL-3.0-only
#include "admin/HardwarePinInventory.h"
#include <cassert>
#include <cstdio>
PB_BIND(meshtastic_NodeRemoteHardwarePinsResponse,meshtastic_NodeRemoteHardwarePinsResponse,AUTO)
PB_BIND(meshtastic_NodeRemoteHardwarePin,meshtastic_NodeRemoteHardwarePin,AUTO)
PB_BIND(meshtastic_RemoteHardwarePin,meshtastic_RemoteHardwarePin,AUTO)
namespace donor {
struct {uint32_t getNodeNum(){return 42;}} nodes,*nodeDB=&nodes;
struct {meshtastic_ModuleConfig_RemoteHardwareConfig remote_hardware;} moduleConfig;
struct {uint8_t node_remote_hardware_pins_count=0;meshtastic_NodeRemoteHardwarePin node_remote_hardware_pins[12]{};} devicestate;
struct AdminModule {
    meshtastic_MeshPacket packet{},*myReply=nullptr;
    meshtastic_NodeRemoteHardwarePinsResponse snapshot{};
    void setPassKey(meshtastic_AdminMessage *){}
    meshtastic_MeshPacket *allocDataProtobuf(const meshtastic_AdminMessage &m){snapshot=m.get_node_remote_hardware_pins_response;return &packet;}
    void handleGetNodeRemoteHardwarePins(const meshtastic_MeshPacket &);
};
#include "stock_pins.inc"
}
int main(){
  auto &remote=donor::devicestate.node_remote_hardware_pins[0];remote.node_num=43;remote.has_pin=true;remote.pin.gpio_pin=5;
  auto &local=donor::moduleConfig.remote_hardware.available_pins[0];local.gpio_pin=2;
  donor::devicestate.node_remote_hardware_pins_count=donor::moduleConfig.remote_hardware.available_pins_count=1;
  donor::AdminModule module;meshtastic_MeshPacket req{};req.pki_encrypted=true;module.handleGetNodeRemoteHardwarePins(req);
  assert(module.myReply->pki_encrypted&&module.snapshot.node_remote_hardware_pins_count==0);
  uint8_t bytes[227]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  assert(pb_encode(&stream,meshtastic_NodeRemoteHardwarePinsResponse_fields,&module.snapshot)&&stream.bytes_written==0);
  size_t n=0;assert(mt::admin::HardwarePinInventory::encode(42,&remote,1,&local,1,bytes,sizeof(bytes),n)&&n>0);
  printf("CONFIRMED pinned hardware pin list encodes 0 payload bytes for two descriptors; ZT encodes %zu bytes\n",n);
}
