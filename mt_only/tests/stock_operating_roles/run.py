#!/usr/bin/env python3
"""Compare production operating role behavior with pinned stock defaults."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, sys, tempfile
APP=Path(__file__).resolve().parents[2]
ROOT=APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host
P=argparse.ArgumentParser(description=__doc__)
P.add_argument("--stock",required=True,type=Path)
P.add_argument("--release", choices=["2.8.1", "2.8.2"], default="2.8.1")
P.add_argument("--output", type=Path, help="Write the 2.8.2 DeviceConfig assertion counts as JSON")
args=P.parse_args()
if args.output and args.release!="2.8.2":
    P.error("--output records the 2.8.2 DeviceConfig comparison; use --release 2.8.2")
if args.output and args.output.exists():
    P.error("--output must be a new file")
# Hashes allow source archives as well as git checkouts of the 2.8.2 donor.
pin282="d5f16191ca28b01b5024f590f04a67f25d66f1b1"
hashes282={
    "src/mesh/NodeDB.cpp":"006921660b5b4b7e8ee32fecc4dcb84cde3e31d0c0853c335e0c43b02f31d3e1",
    "src/modules/AdminModule.cpp":"6690b141c8ea4dafc30c1bfa1ae64b6acd6e5cc939d99b33ac9f90892493d76c",
    "src/modules/AtakPluginModule.cpp":"6077bc559397a9183c4d7108275e8cfe2e700388fffdc8fd7335b9d6a6d77115",
    "src/mesh/TypeConversions.cpp":"12f110a1a3d9aeacf7e4d2320dede31ef8747399a0ce5c4d6ebe2d2d232e13a7",
    "src/mesh/NodeDB.h":"15b7679f066c8061ee92dba6242d50e1cbe786f08765ee970af0239b26edb819",
    "src/mesh/Default.h":"0a468de454cb7da41536b90715cde20d506b2430d3577310a32347b67fed0116",
}
stock282=args.release=="2.8.2"
pin="8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
def source(path):
    if stock282:
        data=(args.stock/path).read_bytes()
        if hashlib.sha256(data).hexdigest()!=hashes282[path]:
            raise ValueError(f"{path} does not match Meshtastic {pin282}")
        return data.decode()
    return subprocess.check_output(["git","-C",args.stock,"show",f"{pin}:{path}"],text=True)
def method(unit,signature):
    i=unit.index(signature)
    return unit[i:unit.index("\n}",i)+2]+'\n'


src=source("src/mesh/NodeDB.cpp")
atak=method(source("src/modules/AtakPluginModule.cpp"),"ProcessMessage AtakPluginModule::handleReceived(")
assert [line.strip() for line in atak.splitlines()[2:-1]]==["(void)mp; // Passthrough - no processing needed, apps handle compression/decompression","return ProcessMessage::CONTINUE;"]
donor=""
roles=["SENSOR","TRACKER","LOST_AND_FOUND","TAK","CLIENT_HIDDEN"]
if stock282:
    roles.extend(["ROUTER_LATE","TAK_TRACKER"])
for role in roles:
    start=src.index(f"if (role == meshtastic_Config_DeviceConfig_Role_{role})", src.index("void NodeDB::installRoleDefaults"))
    donor+=src[start:src.index("\n    }",start)+6]+"\n"
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
results=[]
with tempfile.TemporaryDirectory(prefix="zt-operating-roles-") as td:
    out=Path(td)
    (out/"donor.inc").write_text(donor)
    main=(APP/"src/main.cpp").read_text()
    starts=["if(mt::modules::OperatingRole::handles(c.role())"]
    if stock282:
        convert=source("src/mesh/TypeConversions.cpp")
        db=source("src/mesh/NodeDB.h")
        flags=db[db.index("#define NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_SHIFT"):
                 db.index("/// A node that the eviction/migration paths")]
        (out/"stock-nodeinfo-flags.inc").write_text(flags)
        (out/"stock-nodeinfo-export.inc").write_text(
            method(convert,"meshtastic_NodeInfo TypeConversions::ConvertToNodeInfo(const meshtastic_NodeInfoLite *lite,")+
            method(convert,"meshtastic_NodeInfo TypeConversions::ConvertToNodeInfoThin("))
        admin=source("src/modules/AdminModule.cpp")
        start=admin.index("    case meshtastic_Config_device_tag: {",admin.index("auto existingRole = config.device.role;"))
        end="    } // case meshtastic_Config_device_tag"
        (out/"stock-device-admin.inc").write_text(admin[start:admin.index(end,start)+len(end)])
        (out/"stock-nodeinfo-floor.inc").write_text(next(
            line for line in source("src/mesh/Default.h").splitlines()
            if line.startswith("#define min_node_info_broadcast_secs ")))
        starts.extend(["if(c.role()==mt::DeviceRolePolicy::Role::ROUTER_LATE",
                       "if(c.role()==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER"])
    bodies=[]
    for marker in starts:
        start=main.index("    "+marker)
        stop=main.index("\n    }",start)+6
        bodies.append(main[start:stop].replace("mt::notification::available()","true"))
    (out/"application-role-save.inc").write_text("\n".join(bodies))
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"]]:
        flags=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,HERE/"mocks",APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        if stock282:
            flags.extend(["-DTEST_DEVICE_ADMIN_282=1","-DCONFIG_MT_LOCAL_CLOCK=1","-DCONFIG_MT_TAK_TRACKER=1"])
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"roles.cpp",HERE/"admin.cpp",APP/"src/power/PowerPolicy.cpp",APP/"src/clock/LocalTime.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
        result=subprocess.run([out/"tests"],check=True,text=True,stdout=subprocess.PIPE,
                              env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
        print(result.stdout,end="")
        if stock282:
            counts=[int(line.split()[1]) for line in result.stdout.splitlines() if line.startswith("DEVICE_CONFIG_ASSERTIONS ")]
            if len(counts)!=1 or counts[0]<=0:
                raise RuntimeError("missing native DeviceConfig result")
            peer_counts=[int(line.split()[1]) for line in result.stdout.splitlines() if line.startswith("NODEINFO_SIGNER_COMPARISONS ")]
            if len(peer_counts)!=1 or peer_counts[0]!=8:
                raise RuntimeError("missing restored peer signer comparisons")
            results.append({"enum_abi":"short" if abi else "native","device_config_assertions":counts[0],
                            "restored_peer_signer_comparisons":peer_counts[0]})
if args.output:
    args.output.write_text(json.dumps({"stock_commit":pin282,"variants":results},indent=2)+"\n")
