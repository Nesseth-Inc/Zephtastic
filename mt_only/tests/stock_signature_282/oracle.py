"""Compile the pinned 2.8.2 router and identity policies against controlled crypto/NodeDB outcomes."""
import subprocess

PIN = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"

def generate(store, output):
    def source(path):
        return subprocess.check_output(["git", "-C", str(store), "show", f"{PIN}:src/mesh/{path}"], text=True)
    def method(source, start):
        begin = source.index(start)
        return source[begin:source.index("\n}", begin)+2]

    router = source("Router.cpp")
    database = source("NodeDB.cpp")
    tx = router[router.index("            const bool signingPolicy ="):router.index("                if (crypto->xeddsa_sign")]
    preamble = r'''
namespace donor282 {
#define LOG_WARN(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
constexpr unsigned XEDDSA_SIGNATURE_SIZE=64,NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK=1;
using NodeNum=uint32_t;
struct {struct {unsigned packet_signature_policy;}security;}config{};
struct {bool is_licensed=false;}owner;
struct {bool aead=false;bool isAEADEnabled(unsigned)const{return aead;}}channels;
struct State {bool known=false,key=false,valid=false,consistent=true,room=true,learned=false,fits=true;} st;
struct meshtastic_NodeInfoLite {};
struct meshtastic_NodeInfoLite_public_key_t {unsigned size;uint8_t bytes[32];};
struct NodeDB {
  meshtastic_NodeInfoLite node{};
  bool refusesUnsignedIdentity(NodeNum,uint8_t,bool);
  bool isKnownXeddsaSigner(NodeNum){return st.known;}
  NodeNum getNodeNum(){return 10;}
  meshtastic_NodeInfoLite *getMeshNode(NodeNum){return st.known?&node:nullptr;}
  meshtastic_NodeInfoLite *getOrCreateMeshNode(NodeNum){return st.room?&node:nullptr;}
  bool copyPublicKeyAuthoritative(NodeNum,meshtastic_NodeInfoLite_public_key_t&){return st.key;}
}db,*nodeDB=&db;
void nodeInfoLiteSetBit(meshtastic_NodeInfoLite*,unsigned,bool set){st.learned=set;}
struct Crypto {bool xeddsa_verify(const uint8_t*,uint32_t,uint32_t,uint32_t,const meshtastic_Data*,const uint8_t*){return st.valid;}}cryptoObject,*crypto=&cryptoObject;
enum class NodeInfoBootstrapResult {INVALID,VERIFIED,UNKNOWN};
NodeInfoBootstrapResult verifyFirstContactNodeInfo(const meshtastic_MeshPacket*){return NodeInfoBootstrapResult::UNKNOWN;}
bool signedNodeInfoConsistent(const meshtastic_MeshPacket*,const uint8_t*){return st.consistent;}
bool signedDataFits(const meshtastic_Data*){return st.fits;}
bool isBroadcast(uint32_t id){return id==UINT32_MAX;}
'''
    # Preserve the stock branch condition and return its decision without performing crypto.
    tx = "bool signs(meshtastic_MeshPacket *p){\n" + tx + "return true;\n}return false;\n}"
    body = [preamble, method(router, "bool checkXeddsaReceivePolicy("),
            method(database, "bool NodeDB::refusesUnsignedIdentity("), tx,
            "#undef LOG_WARN\n#undef LOG_TRACE\n#undef LOG_DEBUG\n}"]
    output.write_text("\n".join(body))
