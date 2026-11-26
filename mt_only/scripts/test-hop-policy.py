#!/usr/bin/env python3
import argparse,json,os,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');p.add_argument('--origin',action='store_true');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
app=Path(__file__).resolve().parents[1];root=app.parent;
import hashlib
pins=json.loads((app/'tests/hop_policy/PINS.json').read_text());assert pins['commit']=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
for name,row in pins['functions'].items():assert hashlib.sha256((app/'tests/hop_policy'/name).read_bytes()).hexdigest()==row['sha256']
sys.path.insert(0,str(root));import host_test_toolchain as host
vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
boot=[line.strip()for line in (app/'src/main.cpp').read_text().splitlines()if'service.route.configureContinuous'in line]
assert len(boot)==1
(out/'startup-route-binding.h').write_text('static void boundBootRoute(mt::Service &service,uint32_t node){'+boot[0]+'}\n')

for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=(["-fshort-enums"]if a.short_enums else[])+['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),*host.crypto_compile_flags()]
commands=[];objects=[]
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');cmd=[*host.cc(),'-std=c11',*flags,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/('tests/hop_origin_cases.cpp'if a.origin else'tests/hop_policy_cases.cpp')),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),str(app/'src/pki/PkiSigner.cpp'),str(app/'src/pki/PkiIdentity.cpp'),*objects,*host.crypto_link_flags(),'-o',str(out/'role-channel-tests')]
subprocess.run(cmd,check=True);commands.append(cmd)
r=subprocess.run([str(out/'role-channel-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'role-channel.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
if a.origin:
 rows=[json.loads(line)for line in r.stdout.splitlines()];assert rows[-1]['summary']is True and rows[-1]['vectors']==154 and rows[-1]['mismatches']==0 and all(type(row.get('match'))is int and row['match']==1 for row in rows[:-1])
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'hardware':False,'short_enums':a.short_enums,'groups':len([x for x in r.stdout.splitlines()if x.startswith('PASS ')])})+'\n')
