// SPDX-License-Identifier: GPL-3.0-only
// Actual Service, endpoint, query window and checkpoint dispatch. Only the
// private frame provider, radio, settings, and unrelated app gates are fakes.
#define main ordinaryPrivateUplinkTests
#include "../service_private_uplink_tests.cpp"
#undef main

static Fixture f;
static auto &service=f.service;
static auto &radio=f.radio;
static bool storageReady=true;
struct EmptySettings {
    unsigned writes=0;
    int read(const char *,uint8_t *,size_t){return -ENOENT;}
    int write(const char *name,const uint8_t *,size_t n){
        assert(!strcmp(name,"mthop/state")&&n==524&&service.storageQuiescent());
        ++writes;return 0;
    }
};
static EmptySettings settings;
struct Deadline {bool pending(){return false;}} rebootSchedule,firmwareUpdate;
struct Beacon {bool radioHeld(){return false;}} beaconTargets;
struct Journal {unsigned channelGeneration(){return 1;}} journal;
static int waypoints;
struct Waypoints {
    bool due(int,unsigned,uint64_t){return false;}
    void retry(uint64_t){assert(false);}
    void save(EmptySettings &,int,int,int,uint64_t){assert(false);}
} waypointHistory;
static int waypointHistoryDomain(){return 0;}
static int waypointHistoryContacts(){return 0;}
static bool persistentWorkPending(){return false;}
static int rxQueue,usbRx;
static int k_msgq_num_used_get(int *){return 0;}
static int ring_buf_size_get(int *){return 0;}
namespace mt::ble { unsigned pendingInput(){return 0;} }
#include "owner.inc"

static std::vector<uint8_t> message(bool reply,uint32_t id){
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant=reply?meshtastic_AdminMessage_get_owner_response_tag:
                                      meshtastic_AdminMessage_get_owner_request_tag;
    if(reply){admin.session_passkey.size=8;memset(admin.session_passkey.bytes,0x5a,8);}
    else admin.get_owner_request=true;
    meshtastic_Data data{};
    data.portnum=meshtastic_PortNum_ADMIN_APP;
    data.has_bitfield=true;
    if(reply)data.request_id=id;else {data.want_response=true;data.bitfield=2;}
    auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    assert(pb_encode(&payload,meshtastic_AdminMessage_fields,&admin));
    data.payload.size=payload.bytes_written;
    std::vector<uint8_t> bytes(227);auto out=pb_ostream_from_buffer(bytes.data(),bytes.size());
    assert(pb_encode(&out,meshtastic_Data_fields,&data));
    bytes.resize(out.bytes_written);return bytes;
}
int main(){
    auto &hist=service.hopHistory();
    hist.loadHistory(settings);hist.seed(17);
    // Advance only its scheduler until one tick before the first hourly roll.
    for(unsigned i=0;i<12;++i)hist.runOnce(30000+i*300000,31,0);
    constexpr uint32_t roll=3630000,id=4567;
    auto request=message(false,id);
    assert(!service.submitPrivateAdminGet(0,Peer,id,request.data(),request.size(),3,false,nullptr,0,true,roll-1500));
    f.pollSend(roll-1000,71);assert(radio.sends==1);
    f.complete(roll-800);assert(service.storageStopSafe()&&!radio.busy()&&service.privateAdminQueriesPending());
    f.pollSend(roll,71);
    unsigned before=radio.stops;
    pollBackgroundHistories(roll,false);
    assert(radio.stops==before&&!settings.writes&&service.receiving()&&service.endpointArmed());
    // A matching authenticated response can still find the requesting phone.
    auto response=message(true,id);uint8_t key[32]{};assert(f.provider.peerPublicIdentity(Peer,key));
    assert(service.beginPrivateAdministration());
    assert(service.privateAdminResponseOwners(Self,Peer,key,8057,response.data(),response.size(),roll+2,true)==1u);
    assert(service.endPrivateAdministration());
    // The query window remembers that delivery until expiry, then the owner
    // releases the one coalesced checkpoint and restores the receive session.
    f.pollSend(roll+31000,71);assert(!service.privateAdminQueriesPending());
    before=radio.stops;pollBackgroundHistories(roll+31000,false);
    assert(radio.stops==before+1&&settings.writes==1&&service.receiving()&&service.endpointArmed());
    pollBackgroundHistories(roll+31001,false);
    assert(settings.writes==1&&radio.stops==before+1);
    std::puts("PASS idle endpoint, outstanding private Admin reply owners and deferred hop checkpoint");
}
