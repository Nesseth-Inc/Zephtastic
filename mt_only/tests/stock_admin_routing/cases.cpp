// SPDX-License-Identifier: GPL-3.0-only
#include "stock/admin/AdminModule.h"
#include "HopPolicy.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

namespace donor {
enum class UserLicenseStatus {NotKnown,NotLicensed,Licensed};
struct NodeDB {
    UserLicenseStatus state[2]{};
    UserLicenseStatus getLicenseStatus(uint32_t id){return id==2?state[0]:id==3?state[1]:UserLicenseStatus::NotKnown;}
    bool getMeshNode(uint32_t id){return getLicenseStatus(id)!=UserLicenseStatus::NotKnown;}
} nodes;
NodeDB *nodeDB=&nodes;
bool nodeInfoLiteHasUser(bool has){return has;}
bool isBroadcast(uint32_t id){return id==UINT32_MAX;}
bool isToUs(const meshtastic_MeshPacket *mp){return mp->to==3;}
int getHopsAway(const meshtastic_MeshPacket &mp){return mp.hop_start-mp.hop_limit;}
namespace Default { uint8_t getConfiguredOrDefaultHopLimit(uint8_t n){return n;} }
void printPacket(const char *,const meshtastic_MeshPacket *){}
#define LOG_DEBUG(...) ((void)0)
struct {bool is_licensed;} owner;
struct {meshtastic_Config_DeviceConfig device;meshtastic_Config_LoRaConfig lora;} config{};
struct {int count=0;void sniffReceived(const meshtastic_MeshPacket *,meshtastic_Routing *){++count;}} routerObj,*router=&routerObj;
struct {int count=0;void handleFromRadio(const meshtastic_MeshPacket *){++count;}} serviceObj,*service=&serviceObj;
struct RoutingModule {
    bool handleReceivedProtobuf(const meshtastic_MeshPacket &,meshtastic_Routing *);
    uint8_t getHopLimitForResponse(const meshtastic_MeshPacket &);
};
#include "stock.inc"
}
int main() {
    for(unsigned tag=0;tag<=255;++tag)
        assert(mt::stock::admin::responseForRequest(tag)==donor::adminResponseForRequest(tag));
    donor::RoutingModule module;
    unsigned admits=0,hops=0;
    for(int mode=0;mode<=5;++mode)for(int licensed=0;licensed<=1;++licensed)
    for(int encrypted=0;encrypted<=1;++encrypted)for(int ch:{0,13})
    for(auto to:{3u,UINT32_MAX})for(int fromState=0;fromState<=2;++fromState)for(int toState=0;toState<=2;++toState) {
        donor::config.device.rebroadcast_mode=static_cast<meshtastic_Config_DeviceConfig_RebroadcastMode>(mode);
        donor::owner.is_licensed=licensed;
        donor::nodes.state[0]=static_cast<donor::UserLicenseStatus>(fromState);
        donor::nodes.state[1]=static_cast<donor::UserLicenseStatus>(toState);
        meshtastic_MeshPacket mp{};mp.from=2;mp.to=to;mp.channel=ch;
        mp.which_payload_variant=encrypted?meshtastic_MeshPacket_encrypted_tag:meshtastic_MeshPacket_decoded_tag;
        donor::router->count=donor::service->count=0;module.handleReceivedProtobuf(mp,nullptr);
        const bool actual=mt::stock::routing::accepts(encrypted,ch,mp.from,mp.to,mode,licensed,
          [](uint32_t id){return donor::nodes.getMeshNode(id);},
          [](uint32_t id){return donor::nodes.getLicenseStatus(id)==donor::UserLicenseStatus::NotLicensed;});
        assert(actual==(donor::router->count==1)&&actual==(donor::service->count==1));++admits;
    }
    for(unsigned start=0;start<=7;++start)for(unsigned rem=0;rem<=start;++rem)for(unsigned configured=0;configured<=7;++configured){
        meshtastic_MeshPacket mp{};mp.hop_start=start;mp.hop_limit=rem;donor::config.lora.hop_limit=configured;
        assert(mt::HopPolicy::response(start,rem,configured)==module.getHopLimitForResponse(mp));++hops;
    }
    printf("PASS 256 Admin request tags, %u licensed/opaque routing admissions, %u response hop selections\n",admits,hops);
}
