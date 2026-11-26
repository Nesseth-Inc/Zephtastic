#!/usr/bin/env python3
"""Neighbor canonical wire and direct Endpoint helper tests; no crypto or RF."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,stat,sys,time
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host
sys.path.insert(0,str(ROOT/'mt_only/tests/airtime_quality'))
from bounded_process import run_group,install_termination_handler

def require(ok,message):
 if not ok:raise RuntimeError(message)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--short-enums',action='store_true')
 a=p.parse_args();require(__debug__,'Optimized Python qualification refused')
 app=ROOT/'mt_only';proto=ROOT/'zephcore/bench/upstream-differential/source';vendor=ROOT/'zephcore/adapters/bench/vendor';out=a.output.resolve()
 require(not out.is_relative_to(ROOT),'Output must be outside the source tree')
 def snapshot():
  inputs=[Path(__file__).resolve(),ROOT/'host_test_toolchain.py',app/'src/NeighborQuery.h',
          app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',app/'tests/neighbor_query_cases.cpp',
          app/'tests/airtime_quality/bounded_process.py',*app.glob('src/*.h'),*app.glob('src/*.hpp'),
          *vendor.glob('*.h'),*proto.glob('*.pb.h'),
          *proto.glob('nanopb/*.h'),*(proto/'nanopb'/n for n in('pb_common.c','pb_encode.c','pb_decode.c'))]
  rows={}
  for source in sorted(set(inputs)):
   require(source.is_file()and not source.is_symlink(),'Input must be a regular file: '+str(source))
   rows[source.relative_to(ROOT).as_posix()]={'bytes':source.stat().st_size,'mode':stat.S_IMODE(source.stat().st_mode),'sha256':digest(source)}
  return rows
 cc=host.cc();cxx=host.cxx()
 require(cc and cxx and not any(x.startswith('-DNDEBUG')for x in cc+cxx),'Active fixture assertions required')
 def tool_snapshot():
  tools={}
  for name,argv in(('CC',cc),('CXX',cxx)):
   resolved=shutil.which(argv[0]);require(resolved is not None,'Compiler not found')
   path=Path(resolved).resolve();tools[name]={'argv':argv,'path':str(path),'sha256':digest(path)}
  return tools
 before=snapshot();tools=tool_snapshot();out.mkdir(parents=True,exist_ok=False)
 (out/'INPUTS.json').write_text(json.dumps({'sources':before,'tools':tools},sort_keys=True,indent=2)+'\n')
 status='FAIL';error=None;commands=[];processes=[];after={};changed=[];started=time.monotonic()
 install_termination_handler()
 def run(argv,name):
  command=list(map(str,argv));commands.append(command);record={'name':name,'argv':command};processes.append(record)
  try:
   with(out/name).open('x')as log:
    run_group(command,timeout=60,stdout=log,cwd=out,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   record.update(status='PASS',leader_reaped=True,group_gone=True)
  except BaseException as failure:
   record.update(status='FAIL',cleanup=getattr(failure,'owned_process_cleanup',{}));raise
 try:
  (out/'meshtastic').mkdir()
  for header in sorted(proto.glob('*.pb.h')):shutil.copyfile(header,out/'meshtastic'/header.name)
  flags=['-fsanitize=address,undefined','-g','-O1','-fno-omit-frame-pointer',
         *(['-fshort-enums']if a.short_enums else[]),'-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(out),'-I'+str(proto/'nanopb')]
  objects=[]
  for name in('pb_common.c','pb_encode.c','pb_decode.c'):
   source=proto/'nanopb'/name;obj=out/(source.stem+'.o')
   run([*cc,'-std=c11',*flags,'-c',source,'-o',obj],source.stem+'.build.log');objects.append(obj)
  run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,app/'tests/neighbor_query_cases.cpp',app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',*objects,'-o',out/'tests'],'compile.log')
  run([out/'tests'],'tests.log')
  require(sum(x.startswith('PASS ')for x in(out/'tests.log').read_text().splitlines())==19,'Incomplete Neighbor wire/Endpoint fixture')
  status='PASS'
 except BaseException as failure:error=type(failure).__name__+': '+str(failure)
 finally:
  try:
   after=snapshot();changed=sorted(n for n in before.keys()|after.keys()if before.get(n)!=after.get(n))
   require(not changed,'Source input drift');require(tool_snapshot()==tools,'Compiler drift')
  except BaseException as failure:
   status='FAIL';error=(error+'; 'if error else'')+'closeout: '+type(failure).__name__+': '+str(failure)
  text=(out/'tests.log').read_text()if(out/'tests.log').exists()else''
  receipt={'schema':'zt-neighbor-query-host-tests/v1','status':status,'error':error,'short_enums':a.short_enums,
           'groups':sum(x.startswith('PASS ')for x in text.splitlines()),'ASAN_UBSAN':status=='PASS',
           'source_pins':before,'source_pins_after':after,'source_changed':changed,'tools':tools,
           'commands':commands,'owned_processes':processes,'elapsed':time.monotonic()-started,
           'scope':'Canonical Neighbor encoder/validator and direct Endpoint snapshot/window/typed-submit helpers only; no Service, query transport/correlation, private runtime or cryptography.',
           'hardware':False,'RF':False}
  (out/'RESULT.json').write_text(json.dumps(receipt,sort_keys=True,indent=2)+'\n')
  print(json.dumps({n:receipt[n]for n in('status','error','groups','short_enums','source_changed')}))
 return 0 if status=='PASS'else 1
if __name__=='__main__':raise SystemExit(main())
