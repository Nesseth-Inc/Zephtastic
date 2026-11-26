#!/usr/bin/env python3
"""Exercise exact native PA configuration at T096's fixed request; synthetic SPI.
This proves command bytes/error handling, not RF output or FEM calibration.
"""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--zephyr',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(Path(__file__).resolve().parents[2]));import host_test_toolchain as host
native=a.zephyr.resolve()/'drivers/lora/native/sx126x';source=(native/'sx126x_hal.c').read_text()
start=source.index('int sx126x_hal_configure_tx_params(');brace=source.index('{',start);i=brace+1;depth=1
while depth:depth+=(source[i]=='{')-(source[i]=='}');i+=1
body=source[start:i]
util=out/'zephyr/sys';util.mkdir(parents=True,exist_ok=True);(util/'util.h').write_text('#pragma once\n#define BIT(n) (1u<<(n))\n#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))\n')
header=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "sx126x_regs.h"
#define SX126X_FREQ_400MHZ 400000000
struct sx126x_hal_config {bool is_sx1261;};struct device {const struct sx126x_hal_config *config;};
static unsigned paCalls,ocpCalls,txParamCalls;static int paError,ocpError;
static uint8_t pa[4],ocp,params[2];
static int sx126x_hal_set_pa_config(const struct device *d,uint8_t duty,uint8_t hp,uint8_t chip,uint8_t lut){(void)d;++paCalls;pa[0]=duty;pa[1]=hp;pa[2]=chip;pa[3]=lut;return paError;}
static int sx126x_hal_write_regs(const struct device *d,uint16_t addr,const uint8_t *p,size_t n){(void)d;assert(addr==SX126X_REG_OCP&&n==1);++ocpCalls;ocp=p[0];return ocpError;}
static int sx126x_hal_write_cmd(const struct device *d,uint8_t op,const uint8_t *p,size_t n){(void)d;assert(op==SX126X_CMD_SET_TX_PARAMS&&n==2);++txParamCalls;params[0]=p[0];params[1]=p[1];return 0;}
'''
footer=r'''
int main(void){struct sx126x_hal_config c={false};struct device d={&c};
assert(!sx126x_hal_configure_tx_params(&d,-9,906875000,SX126X_RAMP_800_US));
assert(paCalls==1&&ocpCalls==1&&txParamCalls==1&&pa[0]==2&&pa[1]==2&&pa[2]==0&&pa[3]==1&&ocp==0x38&&params[0]==(uint8_t)-5&&params[1]==SX126X_RAMP_800_US);
paError=-5;assert(sx126x_hal_configure_tx_params(&d,-9,906875000,SX126X_RAMP_800_US)==-5);assert(ocpCalls==1&&txParamCalls==1);
paError=0;ocpError=-6;assert(sx126x_hal_configure_tx_params(&d,-9,906875000,SX126X_RAMP_800_US)==-6);assert(ocpCalls==2&&txParamCalls==1);
puts("PASS exact T096 lowest-drive PA bytes and failure propagation; no RF calibration claim");}
'''
f=out/'power.c';f.write_text(header+'\n'+body+'\n'+footer)
cmd=[*host.cc(),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(out),'-I'+str(native),str(f),'-o',str(out/'power-tests')]
subprocess.run(cmd,check=True);r=subprocess.run([str(out/'power-tests')],check=True,text=True,capture_output=True);print(r.stdout,end='');(out/'power.log').write_text(r.stdout)
(out/'results.json').write_text(json.dumps({'PASS':True,'hardware':False,'ASAN_UBSAN':True,'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'extracted_function_sha256':hashlib.sha256(body.encode()).hexdigest(),'requested_chip_drive':-9,'SetPaConfig':[2,2,0,1],'SetTxParams_power':-5,'OCP':56,'antenna_output_calibrated':False,'command':cmd},indent=2)+'\n')
