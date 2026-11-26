/* Run the production joined reset and ordinary RX arm against a checked SPI bus. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_MT_NORMAL_ENDPOINT 1
#define LOG_ERR(...)
#define bench_record(...)
#define sx126x_observe_rx(...)
#define sx126x_reset_rx_busy_signals(...)
#define sx126x_set_rf_path(...)
#define K_FOREVER 0
#define SX126X_IRQ_ALL 0x3ff
#define SX126X_SLEEP_WARM_START 4
#define SX126X_CMD_SET_SLEEP 0x84
#define SX126X_CALIBRATE_ALL 127
#define SX126X_REG_RX_GAIN 0x8ac
#define SX126X_REG_TX_MODULATION 0x889
enum {SX126X_REST_STATE,SX126X_STATE_IDLE,SX126X_STATE_RX,SX126X_STANDBY_RC};
enum {SX126X_LORA_CRC_OFF,SX126X_LORA_CRC_ON,SX126X_LORA_HEADER_EXPLICIT,
      SX126X_MAX_PAYLOAD_LEN,SX126X_LORA_IQ_INVERTED,SX126X_LORA_IQ_STANDARD};
typedef void (*lora_recv_cb)(void);
typedef void (*sx126x_recv_packet_cb)(void);
struct sx126x_data {
    bool config_valid,rx_boost_enabled,rx_duty_cycle_enabled,cad_active;
    int lock,state,irq_work,rx_cb_gen;struct {int work;} dc_watchdog_work;
    struct {int frequency,preamble_len;bool iq_inverted,packet_crc_disable;} config;
    lora_recv_cb rx_cb;sx126x_recv_packet_cb rx_packet_cb;void *rx_cb_user_data,*tx_async_signal;
};
struct sx126x_hal_config {bool dio2_tx_enable;};
struct device {struct sx126x_data *data;struct sx126x_hal_config *config;};
static struct sx126x_data data;
static struct sx126x_hal_config config={true};
static const struct device dev={&data,&config};
static unsigned clock_ms,cal_at,attempts,wakes,rx_arms,delivered,clear_irq,operation,fail_at;
static uint8_t patch;
static bool calibration,wrong_read,persistent_wake_error,arm_error,reading_verify;
static void reset(void){
    memset(&data,0,sizeof(data));data.config_valid=true;data.config.frequency=906875000;
    clock_ms=cal_at=attempts=wakes=rx_arms=delivered=clear_irq=operation=fail_at=0;
    calibration=wrong_read=persistent_wake_error=arm_error=reading_verify=false;patch=0xa4;
}
static bool device_is_ready(const struct device *d){return d==&dev;}
static int k_work_busy_get(const int *w){return *w;}
static void k_mutex_lock(int *l,int mode){(void)mode;assert(*l==0);*l=1;}
static void k_mutex_unlock(int *l){assert(*l==1);*l=0;}
static int atomic_get(int *v){return *v;}
static void atomic_inc(int *v){++*v;}
static void atomic_set(int *v,int n){*v=n;}
static bool atomic_cas(int *v,int expect,int n){if(*v!=expect)return false;*v=n;return true;}
static void k_msleep(unsigned n){clock_ms+=n;}
static int step(void){assert(data.lock==1);return ++operation==fail_at?-EIO:0;}
static int sx126x_set_standby(const struct device*d,int mode){(void)d;(void)mode;return step();}
static int sx126x_hal_write_cmd(const struct device*d,int cmd,const uint8_t *v,size_t n){
    (void)d;assert(cmd==SX126X_CMD_SET_SLEEP&&n==1&&*v==4);++attempts;return step();
}
static int sx126x_hal_wakeup(const struct device*d){(void)d;++wakes;return persistent_wake_error?-EIO:step();}
static int sx126x_calibrate(const struct device*d,int bits){
    (void)d;assert(bits==127);cal_at=clock_ms;calibration=true;return step();
}
static int sx126x_hal_wait_busy(const struct device*d,int limit){
    (void)d;assert(limit==50 && clock_ms>=cal_at+5);return step();
}
static int sx126x_calibrate_image(const struct device*d,int hz){
    (void)d;assert(hz==906875000);cal_at=clock_ms;return step();
}
static int sx126x_set_dio2_as_rf_switch(const struct device*d,bool on){
    (void)d;assert(on&&clock_ms>=cal_at+50);return step();
}
static int sx126x_add_reg_to_retention(const struct device*d,unsigned reg){
    (void)d;assert(reg==SX126X_REG_RX_GAIN||reg==SX126X_REG_TX_MODULATION);return step();
}
static int sx126x_set_rx_gain(const struct device*d,bool boost){(void)d;(void)boost;return step();}
static int sx126x_hal_read_regs(const struct device*d,unsigned reg,uint8_t *p,size_t n){
    (void)d;assert(reg==0x8b5&&n==1);*p=patch^(reading_verify&&wrong_read?1:0);return step();
}
static int sx126x_hal_write_regs(const struct device*d,unsigned reg,uint8_t*p,size_t n){
    (void)d;assert(n==1);
    if(reg==0x8b5){assert(*p==0xa5);patch=*p;reading_verify=true;return step();}
    assert(reg==SX126X_REG_RX_GAIN);return 0;
}
static int sx126x_set_sleep(const struct device*d){(void)d;data.state=SX126X_REST_STATE;return 0;}
static int sx126x_ensure_ready(const struct device*d){(void)d;return 0;}
static int sx126x_clear_irq_status(const struct device*d,int bits){(void)d;assert(bits==SX126X_IRQ_ALL);++clear_irq;return 0;}
static int sx126x_set_packet_params(const struct device*d,int p,int a,int n,int c,int i){
    (void)d;(void)p;(void)a;(void)n;(void)c;(void)i;return 0;
}
static int sx126x_set_rx(const struct device*d,int t){(void)d;assert(!t);++rx_arms;return arm_error?-EIO:0;}
static uint8_t sx126x_rx_gain_byte(const struct device*d,bool boosted){(void)d;return boosted?3:0;}
static void sx126x_packet_callback_marker(void){}
static void receive(void){++delivered;}
#include "agc.inc"
int main(void){
    bool done;
    reset();assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==0&&done);
    assert(attempts==1&&clock_ms==55&&data.rx_packet_cb==receive&&rx_arms==1&&clear_irq==1);
    unsigned steps=operation;data.rx_packet_cb();assert(delivered==1);
    /* Each definite calibration/SPI refusal still independently restores RX.
     * No failure credits the donor's daily success timestamp. */
    for(unsigned i=1;i<=steps;i++){
        reset();fail_at=i;
        assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==0&&!done);
        assert(data.state==SX126X_STATE_RX&&rx_arms==1&&data.rx_packet_cb==receive);
    }
    reset();wrong_read=true;
    assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==0&&!done&&rx_arms==1);
    reset();persistent_wake_error=true;
    assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==-EIO&&!done&&!rx_arms);
    reset();arm_error=true;
    assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==-EIO&&rx_arms==1&&data.rx_cb==NULL);
    reset();data.state=SX126X_STATE_RX;data.rx_packet_cb=receive;
    assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==0&&!done&&!operation&&!clear_irq);
    data.rx_packet_cb();assert(delivered==1);
    for(int k=0;k<5;k++){
        reset();
        if(k==0)data.irq_work=1;
        if(k==1)data.dc_watchdog_work.work=1;
        if(k==2)data.rx_cb=receive;
        if(k==3)data.tx_async_signal=&data;
        if(k==4)data.cad_active=true;
        assert(sx126x_recv_after_agc(&dev,receive,NULL,&done)==-EBUSY&&!done&&!operation&&!clear_irq);
    }
    return 0;
}
