#!/usr/bin/env python3
"""Independent cross-lane PKI lifecycle test; only synthetic, memory-only keys."""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--pki-source',type=Path,required=True)
p.add_argument('--identity-source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--openssl',type=Path,default=Path('/opt/homebrew/opt/openssl@3'))
a=p.parse_args();pk=a.pki_source.resolve();identity=a.identity_source.resolve();out=a.output.resolve()
sys.path.insert(0,str(pk));import host_test_toolchain as host
out.mkdir(parents=True,exist_ok=True);local=Path(__file__).resolve().parents[1]
test=local/'tests/m9_independent_identity_session.cpp'
mono=pk/'zephcore/lib/monocypher';vendor=pk/'zephcore/adapters/bench/vendor';proto=pk/'zephcore/bench/upstream-differential/source'
inputs=[pk/'mt_only/src/pki'/name for name in ('PkiSession.h','PkiSession.cpp','PkiKeyProvider.h')]
inputs += [pk/'mt_only/experimental/pki'/name for name in ('PkiCrypto.h','PkiCrypto.cpp','PkiAck.h','PkiAck.cpp')]
inputs += [identity/'mt_only/src/pki'/name for name in ('PkiIdentity.h','PkiIdentity.cpp')]
inputs += [identity/'mt_only/src'/name for name in ('Config.h','SettingsJournal.h')]
inputs += [path for folder in (mono,vendor) for path in sorted(folder.glob('*')) if path.is_file() and path.suffix in ('.h','.c','.cpp')]
inputs += [test,Path(__file__).resolve(),pk/'mt_only/src/Protobuf.cpp']
inputs += [x for x in proto.rglob('*') if x.is_file() and x.suffix in ('.h','.c')]
def state():
    return {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
def head(root):
    try:return subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],stderr=subprocess.DEVNULL,text=True).strip() if (root/'.git').exists() else None
    except (OSError,subprocess.CalledProcessError):return None
before=state();revisions_before={'pki':head(pk),'identity':head(identity)};commands=[];objects=[]
# Assemble unchanged lane files so pragma-once sees one provider header, just
# as the final integrated source will. Never edit either lane's source tree.
stage=out/'source';(stage/'pki').mkdir(parents=True,exist_ok=True)
(stage/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,stage/'meshtastic'/header.name)
for name in ('Config.h','SettingsJournal.h'):shutil.copyfile(identity/'mt_only/src'/name,stage/name)
for name in ('PkiSession.h','PkiSession.cpp','PkiKeyProvider.h'):shutil.copyfile(pk/'mt_only/src/pki'/name,stage/'pki'/name)
for name in ('PkiIdentity.h','PkiIdentity.cpp'):shutil.copyfile(identity/'mt_only/src/pki'/name,stage/'pki'/name)
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',*[f'-I{path}' for path in (stage/'pki',pk/'mt_only/experimental/pki',mono,vendor,a.openssl/'include',stage,proto/'nanopb',pk/'mt_only/src')]]
def run(cmd):
    commands.append(cmd);subprocess.run(cmd,check=True)
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
    obj=out/(src.stem+'.o');run([*host.cc(),'-std=c11',*flags,'-c',str(src),'-o',str(obj)]);objects.append(str(obj))
sources=[test,pk/'mt_only/src/Protobuf.cpp',stage/'pki/PkiIdentity.cpp',stage/'pki/PkiSession.cpp',pk/'mt_only/experimental/pki/PkiCrypto.cpp',pk/'mt_only/experimental/pki/PkiAck.cpp',vendor/'MtSignature.cpp']
run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,*map(str,sources),*objects,f'-L{a.openssl/"lib"}','-lcrypto','-o',str(out/'review-tests')])
result=subprocess.run([str(out/'review-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
stable=state()==before
revisions_after={'pki':head(pk),'identity':head(identity)}
report=dict(status='PASS' if stable and result.returncode==0 else 'FAIL',exit_code=result.returncode,source_stable=stable,hardware=False,persistent_credentials=False,ASAN_UBSAN=True,pki_source=revisions_before['pki'],identity_source=revisions_before['identity'],source_revisions_after=revisions_after,source_revisions_stable=revisions_before==revisions_after,backend='OpenSSL host; target PSA is not exercised',source_sha256=before,test_sha256=hashlib.sha256(test.read_bytes()).hexdigest(),runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
(out/'RESULTS.json').write_text(json.dumps(report,indent=2)+'\n')
print(result.stdout+result.stderr,end='');raise SystemExit(result.returncode if stable else 2)
