// SPDX-License-Identifier: GPL-3.0-only
// Adapt stock's ingress and observation points to a real ZT Service, radio
// recorder and phone serializer. The selected stock test bodies are unchanged.
#include "../device_config_fixture.h"
#include "unity.h"

using PacketId = uint32_t;
using Mode = meshtastic_Config_DeviceConfig_RebroadcastMode;
constexpr uint32_t ADMIN_NODE = 0x0C0C0C0C, NODENUM_BROADCAST = UINT32_MAX;
#define IS_ONE_OF(value,a,b,c) ((value)==(a)||(value)==(b)||(value)==(c))
static constexpr Mode ALL_MODES[] = {
    meshtastic_Config_DeviceConfig_RebroadcastMode_ALL,
    meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY,
    meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY,
    meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY,
    meshtastic_Config_DeviceConfig_RebroadcastMode_NONE,
};
struct { struct { Mode rebroadcast_mode; uint32_t role; } device; } config;

constexpr uint32_t TARGET_NODE = 0x0D0D0D0D, LOCAL_NODE = Node;
struct HeaderNodeDB {
    uint8_t getLastByteOfNodeNum(uint32_t node) const { return static_cast<uint8_t>(node); }
} headerDB;
static auto *nodeDB = &headerDB;

using NodeNum = uint32_t;
#include "relay-vectors.inc"
struct RelayIdentity { uint32_t num; const uint8_t *pub; };
static RelayIdentity makeIdentity(uint32_t node) {
    assert(node==ADMIN_NODE || node==TARGET_NODE);
    return {node, node==ADMIN_NODE ? senderPublic : recipientPublic};
}
static meshtastic_MeshPacket makePkiUnicastBetween(RelayIdentity from, RelayIdentity to,
                                                   meshtastic_PortNum port, PacketId id, bool ack=false) {
    meshtastic_MeshPacket p{};
    p.from=from.num; p.to=to.num; p.id=id; p.want_ack=ack;
    p.hop_limit=2; p.hop_start=3;
    p.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    for(const auto &v : vectors) if(v.id==id && v.port==uint32_t(port)) {
        memcpy(p.encrypted.bytes,v.body,v.size); p.encrypted.size=v.size;
        return p;
    }
    std::abort();
}

static std::unique_ptr<Fixture> relay;
static unsigned observations;
static uint32_t tick;
struct { bool is_licensed; } owner;
static auto signaturePolicy=meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_COMPATIBLE;
static void setPolicy(meshtastic_Config_SecurityConfig_PacketSignaturePolicy value){signaturePolicy=value;}
struct Nodes {
    using Entry=mt::NodeCache::Entry;
    std::map<NodeNum,Entry> rows;
    void addNode(NodeNum id) { rows[id].identity(id,81,0); }
    void markHasUser(NodeNum id) {
        auto &e=rows.at(id);
        assert(Entry::text(e.bytes+Entry::Name,40,"Fixture peer",13));
    }
    void setPublicKey(NodeNum id,const uint8_t *key) {
        auto &e=rows.at(id);
        e.bytes[Entry::KeyLength]=32;
        memcpy(e.bytes+Entry::Key,key,32);
    }
    void markLicenseStatus(NodeNum id,bool licensed) {
        markHasUser(id);
        if(licensed) rows.at(id).bytes[Entry::Flags]|=Entry::LicensedUser;
    }
} nodesForTest;
static auto *mockNodeDB=&nodesForTest;
struct RadioCapture {
    unsigned sendCalls=0;
    std::vector<std::vector<uint8_t>> frames;
    void reset(){sendCalls=0; frames.clear(); relay.reset();}
    unsigned sentCountFor(uint32_t from,uint32_t id)const{
        unsigned count=0;
        for(const auto &w:frames) if(w.size()>=16 && bench::le32(w.data()+4)==from && bench::le32(w.data()+8)==id) ++count;
        return count;
    }
} capture;
static auto *pipelineRadio=&capture;
struct ReplyQueue {
    void reset() { /* Recreated with each fixture. */ }
} replyQueue;
static auto *pipelineRouting=&replyQueue;
struct RouterHistory {
    bool historyContains(const meshtastic_MeshPacket *packet) const {
        return relay->service.route.hasHistory(packet->from, packet->id);
    }
} history;
static auto *pipelineRouter = &history;
struct PhoneQueue {
    size_t cursor = 0;
    meshtastic_MeshPacket *getForPhone() {
        auto &output = relay->usb.out;
        while (cursor < output.size()) {
            auto &message = output[cursor++];
            if (message.which_payload_variant == meshtastic_FromRadio_packet_tag)
                return &message.packet;
        }
        return nullptr;
    }
} phone;
static auto *pipelineService = &phone;
struct { void release(meshtastic_MeshPacket *) {} } packetPool; // USB recorder owns these values.

static void putLE32(uint8_t *wire, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) wire[i] = uint8_t(value >> (8 * i));
}

static void startRelay()
{
    relay=std::make_unique<Fixture>(81,false);
    auto cfg=relay->service.config;
    cfg.bytes[4]=1;
    cfg.seal();
    if(owner.is_licensed) TEST_ASSERT_TRUE(cfg.enableLicensedOperation());
    TEST_ASSERT_TRUE(cfg.setDeviceRole(config.device.role,config.device.rebroadcast_mode));
    TEST_ASSERT_TRUE(cfg.setPacketSignaturePolicy(signaturePolicy));
    TEST_ASSERT_TRUE(relay->service.apply(cfg));
    TEST_ASSERT_TRUE(relay->service.start(false,0));
    TEST_ASSERT_TRUE(relay->service.armEndpoint(true));
    for(const auto &row:nodesForTest.rows)
        TEST_ASSERT_TRUE(relay->service.nodes.accept(row.second,Node,-20,1)>=0);
    meshtastic_ToRadio handshake{};
    handshake.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
    handshake.want_config_id=71;
    relay->input(handshake);
    relay->usb.out.clear();
    phone.cursor=0;
    tick=1000;
    relay->radio.onSend=[](const uint8_t *p,size_t n){capture.frames.emplace_back(p,p+n);++capture.sendCalls;};
}

static void runPipelineIngress(const meshtastic_MeshPacket &packet)
{
    if(!relay) startRelay();
    uint8_t wire[255]{};
    size_t size=0;
    if(packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag){
        // Channel frames pass the same codec as ZT-originated text.
        uint8_t bytes[128];
        auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
        TEST_ASSERT_TRUE(pb_encode(&out,meshtastic_Data_fields,&packet.decoded));
        TEST_ASSERT_TRUE(relay->service.codec.encodeZeroHopData(packet.from,packet.to,packet.id,
            bytes,out.bytes_written,wire,sizeof(wire),size));
    } else {
        TEST_ASSERT_EQUAL(meshtastic_MeshPacket_encrypted_tag,packet.which_payload_variant);
        size=16+packet.encrypted.size;
        TEST_ASSERT_TRUE(size<=sizeof(wire));
        putLE32(wire,packet.to);putLE32(wire+4,packet.from);putLE32(wire+8,packet.id);
        wire[13]=packet.channel;
        memcpy(wire+16,packet.encrypted.bytes,packet.encrypted.size);
    }
    wire[12]=uint8_t((packet.hop_start<<5)|packet.hop_limit|(packet.want_ack ? 8u : 0u));
    wire[14]=packet.next_hop; wire[15]=packet.relay_node;
    TEST_ASSERT_TRUE(relay->knownChannelClock.set(tick));
    relay->service.rx(wire,size,3,tick,0);
    const auto queued=relay->service.route.pending();
    const size_t before=capture.frames.size();
    relay->c.receive(wire,size,-90,3,tick,mt::Companion::WorkspaceProof{true});
    relay->service.poll(tick+9000,0);
    TEST_ASSERT_EQUAL_UINT(queued,capture.frames.size()-before);
    for(size_t i=before;i<capture.frames.size();++i){
        const auto &sent=capture.frames[i];
        TEST_ASSERT_EQUAL_UINT(size,sent.size());
        TEST_ASSERT_EQUAL_MEMORY(wire,sent.data(),12); // destination, sender and packet ID
        TEST_ASSERT_EQUAL_MEMORY(wire+16,sent.data()+16,size-16);
        TEST_ASSERT_EQUAL_UINT((wire[12]&7u)-1,sent[12]&7u);
    }
    if(capture.frames.size()>before) {
        relay->radio.done=true;
        relay->service.poll(tick+9050,0);
    }
    tick+=10000;
    ++observations;
}

static void assertOpaqueRelay(const meshtastic_MeshPacket &packet,Mode mode,bool expect,const char *why)
{
    capture.reset();
    config.device.rebroadcast_mode=mode;
    auto copy=packet;
    // This adapter starts a fresh relay per mode. Use the original packet ID
    // and, when the donor case changes it between policies, its matching body.
    for(const auto &v:vectors) if(v.id==copy.id) {
        copy.encrypted.size=v.size; memcpy(copy.encrypted.bytes,v.body,v.size);
        break;
    }
    runPipelineIngress(copy);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(expect?1:0,capture.sendCalls,why);
    TEST_ASSERT_FALSE(history.historyContains(&copy));
    TEST_ASSERT_NULL(phone.getForPhone());
    TEST_ASSERT_EQUAL_UINT(0,relay->service.endpointControls());
    TEST_ASSERT_EQUAL_UINT(nodesForTest.rows.size(),relay->service.nodes.count());
    for(const auto &row:nodesForTest.rows) {
        const int slot=relay->service.nodes.find(row.first);
        TEST_ASSERT_TRUE(slot>=0);
        TEST_ASSERT_EQUAL_UINT(0,relay->service.nodes.get(slot)->lastHeard());
    }
}
static meshtastic_MeshPacket makeChannelBroadcastFrom(NodeNum from,NodeNum to,PacketId id){
    meshtastic_MeshPacket p{};
    p.from=from;p.to=to;p.id=id;p.hop_limit=1;p.hop_start=2;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.payload.size=4;memcpy(p.decoded.payload.bytes,"test",4);
    p.decoded.has_bitfield=true;
    return p;
}

#include "stock-rebroadcast.inc"
#include "rebroadcast_oracle.h"

// The donor's remote-admin case first asks its CryptoEngine to classify the
// ciphertext. ZT has a different ingress and no public verdict of that type.
// Use a recipient-checked ciphertext and observe the real router, radio and phone.
static void test_directed_opaque_between_known_parties(){
    setPolicy(meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_STRICT);
    for(NodeNum id:{ADMIN_NODE,TARGET_NODE}){
        mockNodeDB->addNode(id);mockNodeDB->setPublicKey(id,makeIdentity(id).pub);mockNodeDB->markHasUser(id);
    }
    auto packet=makePkiUnicastBetween(makeIdentity(ADMIN_NODE),makeIdentity(TARGET_NODE),
        meshtastic_PortNum_ADMIN_APP,0xADA10001,true);
    for(auto mode:ALL_MODES) assertOpaqueRelay(packet,mode,mode!=meshtastic_Config_DeviceConfig_RebroadcastMode_NONE,
                                                "known-party directed opaque transit");
}

static void test_zt_companion_stamps_local_module_reply()
{
    startRelay();
    const uint32_t id=0x11229977;
    meshtastic_ToRadio query{};
    query.which_payload_variant=meshtastic_ToRadio_packet_tag;
    query.packet.id=id;
    query.packet.to=Node;
    query.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    query.packet.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;
    query.packet.decoded.want_response=true;
    relay->input(query);
    unsigned replies=0;
    for(const auto &message:relay->usb.out) {
        if(message.which_payload_variant!=meshtastic_FromRadio_packet_tag ||
           message.packet.decoded.portnum!=meshtastic_PortNum_NODEINFO_APP) continue;
        const auto &packet=message.packet;
        TEST_ASSERT_EQUAL_UINT32(Node,packet.from);
        TEST_ASSERT_EQUAL_UINT32(Node,packet.to);
        TEST_ASSERT_EQUAL_UINT32(id,packet.decoded.request_id);
        TEST_ASSERT_EQUAL_UINT8(0,packet.channel);
        TEST_ASSERT_EQUAL_UINT8(relay->service.config.hopLimit(),packet.hop_limit);
        TEST_ASSERT_FALSE(packet.want_ack);
        TEST_ASSERT_EQUAL(meshtastic_MeshPacket_Priority_RELIABLE,packet.priority);
        ++replies;
    }
    TEST_ASSERT_EQUAL_UINT(1,replies);
    TEST_ASSERT_EQUAL_UINT(0,capture.sendCalls);
}

void setUp() {
    capture.reset();nodesForTest.rows.clear();owner.is_licensed=false;
    signaturePolicy=meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_COMPATIBLE;
    config.device.role=mt::DeviceRolePolicy::Role::CLIENT;
    config.device.rebroadcast_mode=meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
}
void tearDown() { relay.reset(); }
int main()
{
    if(mt::DeviceRolePolicy::supportedRebroadcast(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING))
        return 1;
    UNITY_BEGIN();
#include "rebroadcast-run-tests.inc"
    return UNITY_END();
}
