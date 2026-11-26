#!/usr/bin/env python3
"""Stock preset differential, Config migration and exact native methods; fake I/O."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-header',required=True,type=Path);p.add_argument('--lora-header',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sys.path.insert(0, str(app.parent))
import host_test_toolchain as host
formatter_path=a.stock_header.parent.parent/'DisplayFormatters.cpp'
formatter=formatter_path.read_bytes()
assert hashlib.sha256(formatter).hexdigest()=='2cc4e601afba55213e765df074bedee0348ffe2b3c64034a87d5107158ab4349'
stock=a.stock_header.read_bytes();lora=a.lora_header.read_bytes()
assert hashlib.sha256(stock).hexdigest()=='f0e32287b665cabc08b9fb63719f3b3c56fac15e7bf037ed2021ad94cf4b279e'
assert hashlib.sha256(lora).hexdigest()=='b823326cb4b948d302ea8793436405ee544c8df9663bd11a8f6c6d5ea767416f'
files=[formatter_path,Path(__file__).resolve(),app/'tests/radio_settings_cases.cpp',*(app/'src'/n for n in ('Config.h','LoRaPresets.h','USChannel.h','RadioSettings.h','main.cpp'))]
files.extend([Path(host.__file__), *(app/'src/stock/position').glob('*.[ch]*')])
pins={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in files}
def extract(source,decl):
 b=source.index(decl);i=source.index('{',b);depth=1;j=i+1
 while depth:depth+=(source[j]=='{')-(source[j]=='}');j+=1
 return source[b:j]
(out/'stock-modem-function.h').write_text(extract(stock.decode(),'static inline void modemPresetToParams('))
(out/'stock-preset-names.h').write_text('struct DisplayFormatters {static const char *getModemPresetDisplayName(meshtastic_Config_LoRaConfig_ModemPreset,bool,bool);};\n'+extract(formatter.decode(),'const char *DisplayFormatters::getModemPresetDisplayName(')+'\n')
enums='\n'.join(extract(lora.decode(),'enum '+n+' {')+';'for n in ('lora_signal_bandwidth','lora_datarate','lora_coding_rate'))
main=(app/'src/main.cpp').read_text();methods=extract(main,'int configure(const mt::Config &config) override')+'\n'+extract(main,'int start() override')
stub='''#include <errno.h>
ENUMS
#define IS_ENABLED(x) (x)
enum {LORA_CAD_MODE_LBT=1,LORA_CAD_SYMB_4=4};
struct lora_modem_config {uint32_t frequency; lora_signal_bandwidth bandwidth;lora_datarate datarate;lora_coding_rate coding_rate;uint16_t preamble_len;uint8_t sync_word;int8_t tx_power;struct {int mode,symbol_num;} cad;};
static lora_modem_config applied{};static unsigned configCalls=0,restoreCalls=0,purges=0;static int radioDevice=0,rxQueue=0;
static void k_msgq_purge(int *){++purges;}
static int lora_config(int,lora_modem_config *p){applied=*p;++configCalls;return 0;}
struct RadioBase {virtual int configure(const mt::Config&)=0;virtual int start()=0;};
struct NativeAdapter:RadioBase {lora_modem_config modem_{};bool configured_=false;int restore(){++restoreCalls;return 0;}
METHODS
};
'''.replace('ENUMS',enums).replace('METHODS',methods)
(out/'native-modem-methods.h').write_text(stub)
position_policy=host.position_policy_link(app,out)
commands=[];results=[]
for board in (0,1):
 cmd=[*shlex.split(os.environ.get('CXX','clang++')),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',*(['-fshort-enums']if a.short_enums else[]),'-DCONFIG_BOARD_HELTEC_T096='+str(board),'-I'+str(app/'src'),'-I'+str(app.parent/'zephcore/adapters/bench/vendor'),'-I'+str(app.parent/'zephcore/lib/monocypher'),*position_policy,str(app/'tests/radio_settings_cases.cpp'),'-o',str(out/f'case-{board}')]
 subprocess.run(cmd,check=True);commands.append(cmd);r=subprocess.run([str(out/f'case-{board}')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/f'case-{board}.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();results.append({'board':127 if board else 81,'groups':len([s for s in r.stdout.splitlines()if s.startswith('PASS ')])})
assert all(hashlib.sha256(Path(n).read_bytes()).hexdigest()==pin for n,pin in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','source_pins':pins,'short_enums':a.short_enums,'ASAN_UBSAN':True,'boards':results,'commands':commands,'scope':'Exact stock preset function/real Config and NativeRadio configure,start bodies; synthetic driver and no physical RF/register readback.','hardware':False},indent=2)+'\n')
