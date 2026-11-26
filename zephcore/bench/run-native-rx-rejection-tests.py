#!/usr/bin/env python3
"""Exact pinned native RX_DONE and atomic health bodies; synthetic HAL/RTOS, no RF."""
import argparse,hashlib,json,os,re,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--zephyr',type=Path);a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
zephyr=a.zephyr if a.zephyr else root/'zephyr'
s=(zephyr/'drivers/lora/native/sx126x/sx126x.c').read_text();regs=(zephyr/'drivers/lora/native/sx126x/sx126x_regs.h').read_text()
def extract(name):
 m=re.search(r'(?:static )?(?:int|void) '+name+r'\(',s);assert m;brace=s.index('{',m.start());i=brace+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[m.start():i]
header=r'''
#define sx126x_observe_rx(...) ((void)0)
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1U << (n))
#define CONFIG_ZEPHCORE_DUAL_RX_BENCH 1
#define SX126X_STATE_RX 3
#define SX126X_STANDBY_RC 0
#define K_FOREVER 0
#define K_NO_WAIT 0
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)
typedef int atomic_val_t;
struct device;
typedef void (*lora_recv_cb)(const struct device *,uint8_t *,uint16_t,int16_t,int8_t,void *);
struct sx126x_packet_status {uint8_t snr_raw;bool snr_known;};
typedef void (*sx126x_recv_packet_cb)(const struct device *,uint8_t *,uint16_t,int16_t,int8_t,const struct sx126x_packet_status *,void *);
struct sx126x_data {int bench_faults,state,lock,rx_cb_gen,rx_msgq;bool config_valid,rx_duty_cycle_enabled;uint8_t rx_buf[255];lora_recv_cb rx_cb;sx126x_recv_packet_cb rx_packet_cb;void *rx_cb_user_data;};
struct sx126x_rx_result {int status;uint16_t len;int16_t rssi;int8_t snr;struct sx126x_packet_status packet_status;};
struct device {struct sx126x_data *data;bool ready;};
static struct sx126x_data data;static struct device dev={&data,true};
static int failAt,calls,restarts,callbacks,callbackHealth,callbackLen;static uint8_t length=12;static bool nullCallback;
static bool device_is_ready(const struct device *d){return d->ready;}
static int atomic_get(const int *v){return *v;}
static void k_mutex_lock(int *v,int wait){(void)v;(void)wait;}
static void k_mutex_unlock(int *v){(void)v;}
static int io(void){if(++calls==failAt){++data.bench_faults;return -EIO;}return 0;}
static void sx126x_reset_rx_busy_signals(struct sx126x_data *d){(void)d;}
static int sx126x_get_rx_buffer_status(const struct device *d,uint8_t *n,uint8_t *o){(void)d;*n=length;*o=0;return io();}
static int sx126x_get_packet_status(const struct device *d,int16_t *r,int8_t *s,struct sx126x_packet_status *p){(void)d;(void)p;*r=-70;*s=5;return io();}
static int sx126x_hal_read_buffer(const struct device *d,uint8_t o,uint8_t *b,size_t n){(void)d;(void)o;memset(b,0x55,n);return io();}
static void sx126x_retention_debug_check(const struct device *d,const char *c,bool b){(void)d;(void)c;(void)b;}
static int sx126x_restart_rx(const struct device *d,struct sx126x_data *r){(void)d;++restarts;int ret=io();if(!ret)r->state=SX126X_STATE_RX;return ret;}
static void sx126x_set_standby(const struct device *d,int mode){(void)d;(void)mode;}
static void sx126x_set_sleep(const struct device *d){(void)d;}
static void k_msgq_put(int *q,const void *v,int wait){(void)q;(void)v;(void)wait;}
'''
footer=r'''
static void callback(const struct device *d,uint8_t *p,uint16_t n,int16_t r,int8_t s,void *ctx){(void)r;(void)s;(void)ctx;assert(restarts==1);++callbacks;nullCallback=p==NULL;callbackLen=n;callbackHealth=sx126x_bench_health(d);}
static void boot(void){memset(&data,0,sizeof(data));data.config_valid=true;data.state=SX126X_STATE_RX;data.rx_cb=callback;dev.ready=true;failAt=calls=restarts=callbacks=callbackLen=0;length=12;nullCallback=false;}
int main(void){unsigned tests=0;
 const uint16_t rejects[]={SX126X_IRQ_CRC_ERR,SX126X_IRQ_HEADER_ERR,SX126X_IRQ_HEADER_ERR|SX126X_IRQ_CRC_ERR};
 for(unsigned i=0;i<3;++i){boot();sx126x_handle_irq_rx_done(&dev,SX126X_IRQ_RX_DONE|rejects[i]);assert(callbacks==1&&nullCallback&&!callbackLen&&callbackHealth==1&&!data.bench_faults);++tests;}
 boot();length=0;sx126x_handle_irq_rx_done(&dev,SX126X_IRQ_RX_DONE);assert(callbacks==1&&nullCallback&&callbackHealth==1);++tests;
 boot();sx126x_handle_irq_rx_done(&dev,SX126X_IRQ_RX_DONE|SX126X_IRQ_HEADER_VALID|SX126X_IRQ_HEADER_ERR);assert(callbacks==1&&!nullCallback&&callbackLen==12&&callbackHealth==1);++tests;
 for(int stage=1;stage<=4;++stage){boot();failAt=stage;sx126x_handle_irq_rx_done(&dev,SX126X_IRQ_RX_DONE);assert(callbacks==1&&callbackHealth==-EIO&&data.bench_faults);++tests;}
 boot();dev.ready=false;assert(sx126x_bench_health(&dev)==-ENODEV);++tests;
 printf("PASS exact native RX rejection classification: %u cases; restart-before-callback;HAL failures synthetic\n",tests);return 0;}
'''
bodies={n:extract(n) for n in ['sx126x_bench_health','sx126x_handle_irq_rx_done']};defs='\n'.join(re.findall(r'^#define SX126X_IRQ_.*$',regs,re.M));f=out/'native-rx-rejection.c';f.write_text(header+'\n'+defs+'\n'+'\n'.join(bodies.values())+'\n'+footer)
cmd=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(f),'-o',str(out/'tests')];subprocess.run(cmd,check=True);r=subprocess.run([str(out/'tests')],capture_output=True,text=True,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});print(r.stdout,end='');(out/'result.json').write_text(json.dumps({'functions_unmodified':True,'native_sha256':hashlib.sha256(s.encode()).hexdigest(),'functions':{n:hashlib.sha256(b.encode()).hexdigest() for n,b in bodies.items()},'stdout':r.stdout,'hardware_proven':False,'compile':cmd},indent=2)+'\n')
