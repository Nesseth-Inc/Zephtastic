// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_MT_NORMAL_ENDPOINT 1
#define K_FOREVER (-1)
#define SX126X_CAD_PEAK_MIN 12
#define SX126X_CAD_PEAK_MAX 48
#define SX126X_STANDBY_RC 0
enum {LBT_IDLE,LBT_SCAN,LBT_RX};
enum {SX126X_REST_STATE,SX126X_STATE_IDLE,SX126X_STATE_RX};
enum {SX126X_IRQ_CAD_DONE=128,SX126X_IRQ_CAD_ACTIVITY_DETECTED=256,SX126X_IRQ_RX_DONE=2,
      SX126X_IRQ_RX_TX_TIMEOUT=512,SX126X_IRQ_CRC_ERR=64,SX126X_IRQ_HEADER_ERR=32};
enum {SX126X_CMD_SET_CAD_PARAMS=0x88,SX126X_CMD_SET_CAD=0xc5};
struct lora_modem_config {struct {int symbol_num,detection_peak,detection_minimum;} cad;};
struct sx126x_data {
    int state,lock,cad_sem,cad_result,lbt_handoff,header_seen_at_ms,cad_peak_offset,cad_probe_peak;
    bool config_valid,rx_packet_active,cad_active,cad_exit_rx;
    void *cad_cb;
    struct lora_modem_config config;
};
struct device {struct sx126x_data *data;};
typedef int k_timeout_t;
static struct sx126x_data d;
static struct device dev={&d};
static uint8_t params[7];static int writes,rearms,sleeps,air,wait_error,rearm_error;static uint16_t irq_flags;
static void sx126x_lbt_irq(const struct device *,uint16_t);
static bool atomic_cas(int *p,int expect,int next){if(*p!=expect)return false;*p=next;return true;}
static void atomic_set(int *p,int val){*p=val;}
static int atomic_get(int *p){return *p;}
static unsigned k_uptime_get_32(void){return 50;}
static int sx126x_restart_rx(const struct device *dev,struct sx126x_data *s){assert(dev->data==s);++rearms;return rearm_error;}
static int sx126x_set_sleep(const struct device *dev){++sleeps;dev->data->state=SX126X_REST_STATE;return 0;}
static int sx126x_cad_base_peak_cfg(struct lora_modem_config *c){(void)c;return 21;}
static int sx126x_set_standby(const struct device *dev,int mode){assert(dev->data==&d&&mode==0);return 0;}
static unsigned sx126x_lora_airtime(const struct device *dev,unsigned len){assert(dev->data==&d&&len==249);return air;}
static unsigned zc_lora_ms_to_steps24(unsigned ms,unsigned scale){uint64_t v=(uint64_t)ms*scale/1000;return v>0xffffff?0xffffff:(unsigned)v;}
static void sys_put_be24(unsigned value,uint8_t *out){out[0]=value>>16;out[1]=value>>8;out[2]=value;}
static int sx126x_hal_write_cmd(const struct device *dev,int op,uint8_t *buf,int len){
    assert(dev->data==&d);++writes;
    if(op==SX126X_CMD_SET_CAD_PARAMS){assert(len==7);memcpy(params,buf,7);}
    else assert(op==SX126X_CMD_SET_CAD&&!buf&&!len);
    return 0;
}
static int sx126x_clear_irq_status(const struct device *dev,int mask){assert(dev->data==&d&&mask);return 0;}
static void sx126x_set_rf_path(const struct device *dev,bool rx,bool tx){assert(dev->data==&d&&rx&&!tx);}
static void k_mutex_lock(int *m,int t){assert(!*m&&t==K_FOREVER);*m=1;}
static void k_mutex_unlock(int *m){assert(*m);*m=0;}
static int sx126x_ensure_ready(const struct device *dev){assert(dev->data==&d);return 0;}
static void k_sem_reset(int *s){*s=0;}
static int k_sem_take(int *s,int timeout){
    assert(s==&d.cad_sem&&timeout==100&&!d.lock);
    if(wait_error)return wait_error;
    sx126x_lbt_irq(&dev,irq_flags);
    d.cad_result=!!(irq_flags&SX126X_IRQ_CAD_ACTIVITY_DETECTED);
    if(irq_flags&(SX126X_IRQ_RX_DONE|SX126X_IRQ_RX_TX_TIMEOUT)){++rearms;d.rx_packet_active=false;}
    return 0;
}
#include "driver.inc"
static void reset(void){
    memset(&d,0,sizeof(d));d.config_valid=true;d.state=SX126X_REST_STATE;d.config.cad.symbol_num=4;
    d.config.cad.detection_peak=24;d.config.cad.detection_minimum=10;
    writes=rearms=sleeps=wait_error=rearm_error=0;air=1910;irq_flags=SX126X_IRQ_CAD_DONE;
}
int main(void){
    const unsigned int count[]={1,2,4,8,16};
    for(unsigned i=0;i<5;i++){
        reset();d.config.cad.symbol_num=count[i];assert(sx126x_lora_cad_mode(&dev,100,true)==0);
        assert(params[0]==i&&params[1]==24&&params[2]==10&&params[3]==1);
        assert((((unsigned)params[4]<<16)|(params[5]<<8)|params[6])==1910*64);
        assert(d.lbt_handoff==LBT_IDLE&&d.state==SX126X_REST_STATE&&writes==2&&!rearms);
    }
    reset();d.config.cad.symbol_num=3;assert(sx126x_lora_cad_mode(&dev,100,true)==-EINVAL&&!writes);
    reset();air=0;assert(sx126x_lora_cad_mode(&dev,100,true)==-EINVAL&&!writes);
    reset();air=300000;assert(!sx126x_lora_cad_mode(&dev,100,true)&&params[4]==255&&params[5]==255&&params[6]==255);
    reset();wait_error=-EAGAIN;assert(sx126x_lora_cad_mode(&dev,100,true)==-ETIMEDOUT&&d.lbt_handoff==LBT_IDLE);
    reset();irq_flags|=SX126X_IRQ_CAD_ACTIVITY_DETECTED;assert(sx126x_lora_cad_mode(&dev,100,true)==1);
    assert(d.state==SX126X_STATE_RX&&d.lbt_handoff==LBT_RX&&d.rx_packet_active&&!rearms);
    sx126x_lbt_irq(&dev,SX126X_IRQ_HEADER_ERR);assert(rearms==1&&d.lbt_handoff==LBT_IDLE);
    sx126x_lbt_irq(&dev,SX126X_IRQ_HEADER_ERR);assert(rearms==1);
    const unsigned terminal[]={SX126X_IRQ_HEADER_ERR,SX126X_IRQ_CRC_ERR,SX126X_IRQ_RX_DONE,SX126X_IRQ_RX_TX_TIMEOUT};
    for(unsigned i=0;i<4;i++){
        reset();irq_flags|=SX126X_IRQ_CAD_ACTIVITY_DETECTED|terminal[i];
        assert(sx126x_lora_cad_mode(&dev,100,true)==1&&d.state==SX126X_STATE_RX&&d.lbt_handoff==LBT_IDLE&&rearms==1);
    }
    reset();irq_flags|=SX126X_IRQ_CAD_ACTIVITY_DETECTED|SX126X_IRQ_HEADER_ERR;rearm_error=-EIO;
    assert(sx126x_lora_cad_mode(&dev,100,true)==1&&sleeps==1&&d.state==SX126X_REST_STATE);
    reset();irq_flags|=SX126X_IRQ_CAD_ACTIVITY_DETECTED;assert(sx126x_lora_cad_mode(&dev,100,false)==1);
    assert(params[3]==0&&params[4]==0&&params[5]==0&&params[6]==0&&d.state==SX126X_REST_STATE&&!rearms);
}
