"""Independent actual-D2-formula differential; offline, no radio access."""
from pathlib import Path
import hashlib,json,subprocess,os,re,sys,argparse,shlex,shutil
if not __debug__:raise RuntimeError("Optimized qualification refused")
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"airtime_quality"))
from bounded_process import run_group,install_termination_handler
install_termination_handler()
r=Path(__file__).resolve().parent
old=r/'baseline-airtime.c'
new=r.parents[2]/'dependency-overlay/zephyr/drivers/lora/native/sx126x/sx126x_airtime_math.h'
def extract(s,signature):
 match=list(re.finditer(re.escape(signature)+r'[^;{]*\)\s*\{',s));assert len(match)==1
 a=match[0].start();b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
body=extract(old.read_text(),'static uint32_t sx126x_lora_airtime(')
prefix='''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "sx126x_airtime_math.h"
#define __ASSERT_NO_MSG assert
struct config {uint32_t bandwidth;uint16_t preamble_len;uint8_t datarate,coding_rate;bool packet_crc_disable;};
struct sx126x_data {struct config config;bool config_valid;};
struct device {struct sx126x_data *data;bool *config;};
static int bandwidth_to_hz(uint32_t b,uint32_t *out){*out=b;return 0;}
static bool should_enable_ldro(uint8_t sf,uint32_t bw,const bool *de){(void)sf;(void)bw;return *de;}
'''
cases='''
int main(void) {
 uint32_t bws[]={7810,10420,15630,20830,31250,41670,62500,125000,250000,500000,1,UINT32_MAX};
 uint16_t ps[]={0,1,8,12,16,255,4095,65535};unsigned long count=0;
 struct sx126x_data data={0};bool de=false;struct device dev={&data,&de};data.config_valid=true;
 for(unsigned b=0;b<sizeof(bws)/sizeof(*bws);++b)
 for(unsigned p=0;p<sizeof(ps)/sizeof(*ps);++p)
 for(unsigned sf=5;sf<=12;++sf)for(unsigned cr=1;cr<=4;++cr)
 for(unsigned d=0;d<2;++d)for(unsigned crc=0;crc<2;++crc)
 for(unsigned n=0;n<=255;++n) {
  de=d;data.config=(struct config){bws[b],ps[p],sf,cr,!crc};
  struct sx126x_airtime_input c={bws[b],ps[p],sf,cr,d,crc};
  struct sx126x_airtime_value got=sx126x_airtime_estimate(&c,n);
  assert(got.ms==sx126x_lora_airtime(&dev,n));
  uint64_t sym=((UINT64_C(1)<<sf)*1000000)/bws[b];
  int64_t bits=8*(int64_t)n-4*(int64_t)sf+28+16*crc;if(bits<0)bits=0;
  uint64_t den=4*(sf-2*d),symbols=8+(((uint64_t)bits+den-1)/den)*(cr+4);
  uint64_t pre=(ps[p]+UINT64_C(4))*sym+sym/4,post=symbols*sym;
  bool trustworthy=sym>0&&pre<=UINT32_MAX&&post<=UINT32_MAX&&pre+post+500<=UINT32_MAX;
  assert(got.trustworthy==trustworthy);++count;
 }
 assert(!sx126x_airtime_estimate(0,1).trustworthy);
 struct sx126x_airtime_input c={250000,16,11,1,false,true};
 assert(!sx126x_airtime_estimate(&c,256).trustworthy);c.sf=4;assert(!sx126x_airtime_estimate(&c,1).trustworthy);
 c.sf=13;assert(!sx126x_airtime_estimate(&c,1).trustworthy);c.sf=11;c.cr=0;assert(!sx126x_airtime_estimate(&c,1).trustworthy);
 c.cr=5;assert(!sx126x_airtime_estimate(&c,1).trustworthy);c.cr=1;c.bandwidth_hz=0;assert(!sx126x_airtime_estimate(&c,1).trustworthy);
 printf("PASS actual-D2 formula and independent 64-bit quality oracle: %lu combinations\\n",count);
}
'''
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();out=args.output.resolve()
assert not out.is_relative_to(r.parents[3]);out.mkdir(exist_ok=False)
compiler=shlex.split(os.environ.get('CC','cc'));compiler[0]=str(Path(shutil.which(compiler[0])).resolve())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
paths=[old,new,Path(__file__).resolve(),Path(compiler[0]),r.parents[1]/"airtime_quality/bounded_process.py"]
pins={str(p):sha(p)for p in paths};(out/'fixture.c').write_text(prefix+body+cases)
result={'status':'FAIL','hardware':False,'RF':False,'input_sha256':pins,'variants':[]}
try:
 for name,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
  binary=out/name
  cmd=[*compiler,'-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*extra,'-I'+str(new.parent),str(out/'fixture.c'),'-o',str(binary)]
  with (out/(name+'-compile.log')).open('w')as log:run_group(cmd,timeout=30,stdout=log)
  with (out/(name+'-run.log')).open('w')as log:run_group([str(binary)],timeout=30,stdout=log,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
  result['variants'].append({'name':name,'command':cmd,'exit':0,'stdout':(out/(name+'-run.log')).read_text(),'binary_sha256':sha(binary)})
 assert all(sha(Path(p))==pin for p,pin in pins.items());result['source_unchanged']=True;result['status']='PASS'
finally:
 (out/'SUMMARY.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'status':result['status'],'variants':[v['stdout'].strip()for v in result['variants']]}))
