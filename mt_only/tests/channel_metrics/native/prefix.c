/* Host HAL/RTOS simulation; actual production functions are appended by runner. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#ifdef NDEBUG
#error assertions required
#endif
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(ms) (ms)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define BIT(n) (1U<<(n))
#define SX126X_IRQ_TX_DONE BIT(0)
#define SX126X_IRQ_RX_DONE BIT(1)
#define SX126X_IRQ_PREAMBLE_DETECTED BIT(2)
#define SX126X_IRQ_SYNC_WORD_VALID BIT(3)
#define SX126X_IRQ_HEADER_VALID BIT(4)
#define SX126X_IRQ_HEADER_ERR BIT(5)
#define SX126X_IRQ_CRC_ERR BIT(6)
#define SX126X_IRQ_CAD_DONE BIT(7)
#define SX126X_IRQ_CAD_ACTIVITY_DETECTED BIT(8)
#define SX126X_IRQ_RX_TX_TIMEOUT BIT(9)
#define SX126X_STANDBY_RC 0
#define SX126X_CMD_SET_RX 0x82
#define SX126X_CMD_SET_RX_DUTY_CYCLE 0x94
#define SX126X_CMD_SET_TX 0x83
#define SX126X_MS_TO_TIMEOUT(ms) ((uint32_t)((ms)*64))
#define SX126X_CMD_SET_CAD 0xc5
#define SX126X_CMD_SET_TX_CONTINUOUS_WAVE 0xd1
#define SX126X_CMD_SET_SLEEP 0x84
#define SX126X_BUSY_DEFAULT_TIMEOUT 1000
#define SX126X_REST_STATE 0
#define SX126X_STATE_IDLE 1
#define SX126X_STATE_TX 2
#define SX126X_STATE_RX 3
#define SX126X_STATE_RX_DUTY_CYCLE 4
#define SX126X_CAD_RX_PACKET 2
#define SX126X_CAD_RX_TMOUT 3
#define SX126X_CAD_RX_ARMED 1
#define SF_6 6
#define SX126X_IRQ_ALL 0xffff
#define SX126X_RX_TIMEOUT_CONTINUOUS 0xffffff
#define SX126X_REG_RX_GAIN 0x08ac
#define SX126X_REG_LORA_SYNC_WORD_MSB 0x0740
#define SX126X_REG_TX_CLAMP_CFG 0x08d8
#define SX126X_LORA_HEADER_EXPLICIT 0
#define SX126X_MAX_PAYLOAD_LEN 255
#define SX126X_LORA_CRC_OFF 0
#define SX126X_LORA_CRC_ON 1
#define SX126X_LORA_IQ_INVERTED 1
#define SX126X_LORA_IQ_STANDARD 0
#define SX126X_TX_RAMP_TIME 4
#define SX126X_SF56_MIN_PREAMBLE_LEN 12
#ifndef IS_ENABLED
#define IS_ENABLED(x) 0
#endif
#define LORA_CAD_MODE_LBT 1
#define __ASSERT_NO_MSG(x) assert(x)
#define CONTAINER_OF(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
typedef _Atomic int32_t atomic_t;
typedef int32_t atomic_val_t;
typedef int32_t k_timeout_t;
struct k_poll_signal {unsigned synthetic;};
static int32_t atomic_get(const atomic_t *p){return atomic_load(p);}
static int32_t atomic_set(atomic_t *p,int32_t v){return atomic_exchange(p,v);}
static void atomic_inc(atomic_t *p){atomic_fetch_add(p,1);}
static bool atomic_cas(atomic_t *p,int32_t before,int32_t after){return atomic_compare_exchange_strong(p,&before,after);}
struct k_work {int busy;};
struct k_work_sync {unsigned synthetic;};
/* Explicit synthetic spinlock seam. The pthread mutex serializes this host
 * model only: it does not model Zephyr IRQ masking or prove target races. */
struct k_spinlock {unsigned synthetic;};
typedef unsigned k_spinlock_key_t;
static pthread_mutex_t observation_spin_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned observation_spin_held;
static atomic_uint observation_spin_locks,observation_spin_unlocks;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock){
 assert(lock&&observation_spin_held==0);
 assert(!pthread_mutex_lock(&observation_spin_mutex));observation_spin_held=1;
 atomic_fetch_add(&observation_spin_locks,1);return 0x5a;
}
static void k_spin_unlock(struct k_spinlock *lock,k_spinlock_key_t key){
 assert(lock&&key==0x5a&&observation_spin_held==1);observation_spin_held=0;
 atomic_fetch_add(&observation_spin_unlocks,1);assert(!pthread_mutex_unlock(&observation_spin_mutex));
}
struct k_msgq {pthread_mutex_t mutex;char *buf;size_t size;unsigned max,head,count;bool init;};
struct sx126x_airtime_input {uint32_t bandwidth_hz;uint16_t preamble_len;uint8_t sf,cr;bool ldro,crc;};
struct sx126x_airtime_value {uint32_t ms;bool trustworthy;};
/* AIRTIME_FUNCTION */
/* OBSERVATION_STRUCT */
enum lora_datarate {SF_5=5,SF_11=11,SF_12=12};
enum lora_signal_bandwidth {BW_250_KHZ=250,BW_125_KHZ=125};
struct lora_modem_config {uint32_t frequency;enum lora_datarate datarate;enum lora_signal_bandwidth bandwidth;uint16_t preamble_len;uint8_t coding_rate,sync_word;bool packet_crc_disable,iq_inverted,public_network;int8_t tx_power;struct {int mode;} cad;};
struct sx126x_hal_config {bool force_ldro,rx_boosted,is_sx1261;int spi;};
struct device;
/* PACKET_STATUS_DECLS */
typedef void (*lora_recv_cb)(const struct device*,uint8_t*,uint16_t,int16_t,int8_t,void*);
typedef void (*lora_cad_cb)(const struct device*,bool,void*);
struct sx126x_tx_result {int status;};
struct sx126x_rx_result {int16_t rssi;int8_t snr;struct sx126x_packet_status packet_status;uint8_t len;int status;};
/* STAGE_INFO_STRUCT */
struct sx126x_data {
  atomic_t state,rx_cb_gen,preamble_seen_at_ms,header_seen_at_ms,dc_timeout_restarts,cad_rx_state,raw_header_seen_at_ms,rx_stale_releases;
  atomic_t observation_enabled,observation_loss,observation_config_state,observation_reconfigure_permit;
  struct k_spinlock observation_lock;
  uint32_t observation_tx_started,observation_tx_done,observation_tx_timeouts,observation_last_tx_irq;
 struct sx126x_stage_info send_stage,config_stage,restart_stage;
  struct sx126x_airtime_input observation_config;
  uint32_t observation_frequency;uint8_t observation_sync_word;bool observation_iq;
  struct k_msgq observation_queue,rx_msgq,tx_msgq;
  struct sx126x_airtime_observation observation_buffer[16];
  struct lora_modem_config config;bool config_valid;
  int lock,dio1_wq;struct k_work irq_work,dc_watchdog_work;const struct device *dev;
  uint8_t rx_buf[255];lora_recv_cb rx_cb;sx126x_recv_packet_cb rx_packet_cb;void *rx_cb_user_data,*tx_async_signal;
  bool rx_duty_cycle_enabled,cad_active,cad_exit_rx,rx_packet_active,rx_boost_enabled;
  struct {uint32_t rx_period,sleep_period;} duty_cycle;
  lora_cad_cb cad_cb;void *cad_user_data;int cad_result,cad_sem;
};
struct device {struct sx126x_data *data;const struct sx126x_hal_config *config;bool ready;};
static struct sx126x_data data;
static struct sx126x_hal_config hal;
static struct device dev={&data,&hal,true};
static int64_t now_ms=1000;
static uint8_t input_length=16;
static int buffer_error,packet_error,fifo_error,restart_error,clear_error,irq_error;
static int gain_error,params_error,ready_error,set_rx_error,config_fail_step,config_step;
/* Stage-only seams default to0 and do not alter historical HAL trace text. */
static int validate_error,duty_error,write_buffer_error,pa_error,workaround_error,read_regs_error,clamp_write_error;
static int ready_fail_at,ready_fail_result,cad_result;static unsigned ready_calls,cad_calls,write_buffer_calls,read_regs_calls,queue_purges;
static bool cad_post_busy,sync_mismatch,policy_mt,policy_tx,policy_bench,bench_admitted;
static int standby_error,sleep_error,callback_error;
static int tx_cmd_error,signal_result;static unsigned tx_commands,signal_calls;static uint32_t tx_timeout_ticks;static void *last_signal;
static unsigned callback_closes,callback_opens,callback_retires,irq_joins,watchdog_joins;
static void (*config_step_hook)(void);
static int hal_busy_error,hal_spi_error;
static unsigned hal_spi_calls,hal_sleep_waits;
static int hal_permit_at_spi;
void sx126x_airtime_observer_reconfigure_invalidate(const struct device *dev);
static int submit_result,chip_busy;static unsigned submits;
static uint16_t irq_sequence[2],last_clear_mask;
static int irq_error_sequence[2],mutex_error;
static unsigned irq_sequence_count,irq_reads,work_reads,clear_calls;
static bool work_busy_on_recheck,active_on_recheck;
static bool mutate_generation;
static unsigned callbacks,callback_length,msgs;static int msg_status;
static uint16_t input_irq;
static char trace[8192];static size_t used;
static void mark(char c){assert(used+1<sizeof(trace));trace[used++]=c;trace[used]=0;}
static bool device_is_ready(const struct device *d){return d->ready;}
static int k_mutex_lock(int *p,int w){(void)p;(void)w;mark('L');return mutex_error;}
static void k_mutex_unlock(int *p){(void)p;mark('U');}
static int k_work_busy_get(struct k_work *w){++work_reads;return w->busy||(work_busy_on_recheck&&work_reads==2);}
static int k_work_cancel_sync(struct k_work *w,struct k_work_sync *sync){(void)sync;++irq_joins;w->busy=0;return 0;}
static int k_work_cancel_delayable_sync(struct k_work *w,struct k_work_sync *sync){(void)sync;++watchdog_joins;w->busy=0;return 0;}
static int sx126x_hal_set_dio1_callback(const struct device *d,void (*cb)(const struct device*)){(void)d;if(cb)++callback_opens;else ++callback_closes;return callback_error;}
static void sx126x_hal_retire_dio1_callback(const struct device *d){(void)d;++callback_retires;}
static int k_work_submit_to_queue(int *q,struct k_work *w){(void)q;(void)w;mark('Q');++submits;return submit_result;}
static int sx126x_hal_is_busy(const struct device *d){(void)d;mark('B');return chip_busy;}
static int64_t k_uptime_get(void){return now_ms;}
static uint32_t k_uptime_get_32(void){mark('t');return (uint32_t)now_ms;}
static void k_msgq_init(struct k_msgq *q,char *buf,size_t size,unsigned max){q->buf=buf;q->size=size;q->max=max;q->head=q->count=0;assert(pthread_mutex_init(&q->mutex,NULL)==0);q->init=true;}
static int k_msgq_put(struct k_msgq *q,const void *p,int wait){assert(wait==K_NO_WAIT);assert(observation_spin_held==0);if(!q->init){mark('m');++msgs;msg_status=q==&data.tx_msgq?((const struct sx126x_tx_result*)p)->status:((const struct sx126x_rx_result*)p)->status;return 0;}assert(pthread_mutex_lock(&q->mutex)==0);int r=0;if(q->count==q->max)r=-ENOMSG;else{memcpy(q->buf+((q->head+q->count)%q->max)*q->size,p,q->size);++q->count;}assert(pthread_mutex_unlock(&q->mutex)==0);return r;}
static int k_msgq_get(struct k_msgq *q,void *p,int wait){assert(wait==K_NO_WAIT);assert(q->init);assert(pthread_mutex_lock(&q->mutex)==0);int r=0;if(!q->count)r=-ENOMSG;else{memcpy(p,q->buf+q->head*q->size,q->size);q->head=(q->head+1)%q->max;--q->count;}assert(pthread_mutex_unlock(&q->mutex)==0);return r;}
static void k_msgq_purge(struct k_msgq *q){assert(observation_spin_held==0);++queue_purges;q->head=q->count=0;}
static int bandwidth_to_hz(enum lora_signal_bandwidth bw,uint32_t *out){if(bw!=BW_250_KHZ&&bw!=BW_125_KHZ)return -EINVAL;*out=(uint32_t)bw*1000;return 0;}
static bool should_enable_ldro(enum lora_datarate sf,enum lora_signal_bandwidth bw,const struct sx126x_hal_config *cfg){return cfg->force_ldro||(((UINT64_C(1)<<sf)*1000000)/(bw*1000)>16380);}
static void sx126x_reset_rx_busy_signals(struct sx126x_data *d){mark('z');d->rx_packet_active=false;atomic_set(&d->preamble_seen_at_ms,0);atomic_set(&d->header_seen_at_ms,0);}
static int sx126x_get_rx_buffer_status(const struct device *d,uint8_t *n,uint8_t *off){(void)d;mark('b');*n=input_length;*off=7;return buffer_error;}
/* Both historical three-argument and packet-metadata call sites use the same
 * legacy trace stub. The M64 packet suite separately extracts the real read. */
static int status_base(const struct device *d,int16_t *r,int8_t *s){(void)d;mark('p');*r=-90;*s=5;return packet_error;}
static int status_meta(const struct device *d,int16_t *r,int8_t *s,struct sx126x_packet_status *v){*v=(struct sx126x_packet_status){0};return status_base(d,r,s);}
#define STATUS_SELECT(_1,_2,_3,_4,NAME,...) NAME
#define sx126x_get_packet_status(...) STATUS_SELECT(__VA_ARGS__,status_meta,status_base)(__VA_ARGS__)
static int sx126x_hal_read_buffer(const struct device *d,uint8_t off,uint8_t *buf,uint8_t n){(void)d;(void)off;mark('f');memset(buf,0x25,n);return fifo_error;}
static int sx126x_restart_rx(const struct device *d,struct sx126x_data *v){(void)d;mark('r');if(mutate_generation)atomic_inc(&v->rx_cb_gen);return restart_error;}
static int sx126x_set_sleep(const struct device *d){mark('s');atomic_set(&d->data->state,SX126X_REST_STATE);return sleep_error;}
static int sx126x_set_standby(const struct device *d,uint8_t v){(void)d;(void)v;mark('S');return standby_error;}
static void sx126x_retention_debug_check(const struct device *d,const char *s,bool b){(void)d;(void)s;(void)b;mark('d');}
static void k_poll_signal_raise(void *p,int v){last_signal=p;signal_result=v;++signal_calls;mark('P');}
static bool sx126x_cad_rx_resolve(struct sx126x_data *d,int result){(void)d;(void)result;mark('c');return false;}
static int step(char c){mark(c);if(c=='5'&&config_step_hook)config_step_hook();return ++config_step==config_fail_step?-EIO:0;}
static int sx126x_ensure_ready(const struct device *d){(void)d;assert(observation_spin_held==0);++ready_calls;if(config_fail_step)return step('e');mark('e');return ready_fail_at==(int)ready_calls?ready_fail_result:ready_error;}
static int sx126x_clear_irq_status(const struct device *d,uint16_t irq){(void)d;mark('C');last_clear_mask=irq;++clear_calls;return clear_error;}
static int sx126x_get_irq_status(const struct device *d,uint16_t *irq){(void)d;mark('I');unsigned i=irq_reads++;if(i==1&&active_on_recheck)data.rx_packet_active=true;if(irq_sequence_count){assert(i<irq_sequence_count);*irq=irq_sequence[i];return irq_error_sequence[i];}*irq=input_irq;return irq_error;}
static uint32_t sx126x_max_payload_ms(struct sx126x_data *d){(void)d;return 1200;}
static uint32_t sx126x_preamble_grace_ms(struct sx126x_data *d){(void)d;return 50;}
static uint32_t zc_lora_ms_to_steps24(uint32_t ms,uint32_t multiplier){return ms*multiplier/1000;}
static void sys_put_be24(uint32_t n,uint8_t *out){out[0]=n>>16;out[1]=n>>8;out[2]=n;}
static int sx126x_hal_write_cmd(const struct device *d,uint8_t cmd,const uint8_t *buf,size_t size){(void)d;mark('w');if(cmd==SX126X_CMD_SET_TX){assert(size==3);++tx_commands;tx_timeout_ticks=((uint32_t)buf[0]<<16)|((uint32_t)buf[1]<<8)|buf[2];return tx_cmd_error;}return restart_error;}
static int sx126x_hal_wait_busy(const struct device *d,unsigned timeout){(void)d;(void)timeout;return hal_busy_error;}
static int sx126x_bench_fault(const struct device *d,int result){(void)d;return result;}
static bool sx126x_bench_tx_ready(const struct device *d){(void)d;return true;}
static bool sx126x_bench_tx_consume(const struct device *d,const uint8_t *buf,size_t n){(void)d;(void)buf;(void)n;return true;}
static int spi_transfer(const int *spi,uint8_t *header,size_t header_len,uint8_t *buf,size_t len,bool read){(void)spi;(void)header;(void)buf;(void)len;assert(header_len==1&&!read&&observation_spin_held==0);++hal_spi_calls;hal_permit_at_spi=atomic_get(&data.observation_reconfigure_permit);return hal_spi_error;}
static void k_busy_wait(unsigned us){assert(us==500);++hal_sleep_waits;}
static int sx126x_set_rx_duty_cycle(const struct device *d,uint32_t rx,uint32_t sleep){(void)d;(void)rx;(void)sleep;mark('u');return duty_error;}
static void sx126x_dc_watchdog_kick(struct sx126x_data *d){(void)d;mark('k');}
static uint8_t sx126x_rx_gain_byte(const struct device *d,bool b){(void)d;(void)b;mark('v');return 0x96;}
static int sx126x_hal_write_regs(const struct device *d,uint16_t reg,const uint8_t *buf,size_t n){(void)d;(void)buf;(void)n;assert(observation_spin_held==0);mark('G');return reg==SX126X_REG_TX_CLAMP_CFG?clamp_write_error:gain_error;}
static void bench_record(const struct device *d,const char *name,int ret,uint32_t v){(void)d;(void)name;(void)ret;(void)v;}
static int sx126x_set_packet_params(const struct device *d,uint16_t preamble,int hdr,uint8_t max,int crc,int iq){(void)d;(void)preamble;(void)hdr;(void)max;(void)crc;(void)iq;mark('q');return params_error;}
static void sx126x_set_rf_path(const struct device *d,bool rx,bool tx){(void)d;(void)rx;(void)tx;mark('R');}
static int sx126x_set_rx(const struct device *d,uint32_t n){(void)d;(void)n;mark('x');return set_rx_error;}
static int sx126x_validate_config(const struct lora_modem_config *c){(void)c;mark('V');return validate_error;}
static int sx126x_calibrate_image(const struct device *d,uint32_t n){(void)d;(void)n;return step('1');}
static int sx126x_set_rf_frequency(const struct device *d,uint32_t n){(void)d;(void)n;return step('2');}
static int sx126x_apply_agc_band_cal(const struct device *d,uint32_t n){(void)d;(void)n;return step('3');}
static int sx126x_hal_configure_tx_params(const struct device *d,int n,uint32_t freq,int ramp){(void)d;(void)n;(void)freq;(void)ramp;assert(observation_spin_held==0);int r=step('4');return r?r:pa_error;}
static int bandwidth_to_reg(enum lora_signal_bandwidth bw,uint8_t *reg){(void)bw;*reg=0x05;return 0;}
static int sx126x_set_modulation_params(const struct device *d,int sf,int bw,int cr,bool ldro){(void)d;(void)sf;(void)bw;(void)cr;(void)ldro;return step('5');}
static int sx126x_apply_tx_modulation_workaround(const struct device *d,int bw){(void)d;(void)bw;int r=step('6');return r?r:workaround_error;}
static int sx126x_set_sync_word(const struct device *d,const struct lora_modem_config *c){(void)d;(void)c;return step('7');}
static int sx126x_hal_read_regs(const struct device *d,uint16_t r,uint8_t *buf,size_t n){assert(observation_spin_held==0);++read_regs_calls;memset(buf,0,n);if(r==SX126X_REG_LORA_SYNC_WORD_MSB&&n==2&&!sync_mismatch){uint8_t word=d->data->config.sync_word?d->data->config.sync_word:(d->data->config.public_network?0x34:0x12);buf[0]=0x04|(word&0xf0);buf[1]=0x04|((word&0x0f)<<4);}return read_regs_error;}
static int sx126x_hal_write_buffer(const struct device *d,uint8_t offset,uint8_t *buf,uint32_t n){(void)d;assert(offset==0&&buf&&n<=255&&observation_spin_held==0);++write_buffer_calls;mark('F');return write_buffer_error;}
static bool bench_admit(const struct device *d,const uint8_t *buf,size_t n){(void)d;(void)buf;(void)n;return bench_admitted;}
static uint32_t sx126x_bench_tx_timeout(void){return 10000;}
static uint32_t sx126x_cad_timeout_ms(struct sx126x_data *v){(void)v;return 100;}
static int sx126x_lora_cad(const struct device *d,k_timeout_t timeout){assert(timeout==100&&observation_spin_held==0);++cad_calls;mark('A');if(cad_post_busy)atomic_set(&d->data->state,SX126X_STATE_IDLE);return cad_result;}
static uint16_t sys_get_be16(const uint8_t *b){return ((uint16_t)b[0]<<8)|b[1];}
static int sx126x_set_rx_gain(const struct device *d,bool boost){(void)d;(void)boost;return step('8');}
static void k_sem_give(int *p){(void)p;mark('g');}
static void sx126x_hal_dio1_irq_enable(const struct device *d){(void)d;mark('D');}
static void receive(const struct device *d,uint8_t *p,uint16_t n,int16_t r,int8_t s,void *u){(void)d;(void)r;(void)s;(void)u;assert((p!=NULL)==(n!=0));mark('a');++callbacks;callback_length=n;}
static void boot(void){if(data.observation_queue.init)assert(pthread_mutex_destroy(&data.observation_queue.mutex)==0);memset(&data,0,sizeof(data));memset(&hal,0,sizeof(hal));dev.ready=true;data.dev=&dev;atomic_set(&data.state,SX126X_STATE_RX);data.rx_cb=receive;input_length=16;buffer_error=packet_error=fifo_error=restart_error=clear_error=irq_error=0;gain_error=params_error=ready_error=set_rx_error=config_fail_step=config_step=0;validate_error=duty_error=write_buffer_error=pa_error=workaround_error=read_regs_error=clamp_write_error=0;ready_fail_at=ready_fail_result=cad_result=0;ready_calls=cad_calls=write_buffer_calls=read_regs_calls=queue_purges=0;cad_post_busy=sync_mismatch=policy_mt=policy_bench=false;policy_tx=bench_admitted=true;submit_result=chip_busy=mutex_error=0;standby_error=sleep_error=callback_error=0;tx_cmd_error=signal_result=0;tx_commands=signal_calls=tx_timeout_ticks=0;last_signal=NULL;callback_closes=callback_opens=callback_retires=irq_joins=watchdog_joins=0;config_step_hook=NULL;hal_busy_error=hal_spi_error=hal_permit_at_spi=0;hal_spi_calls=hal_sleep_waits=0;submits=irq_sequence_count=irq_reads=work_reads=clear_calls=0;last_clear_mask=0;memset(irq_sequence,0,sizeof(irq_sequence));memset(irq_error_sequence,0,sizeof(irq_error_sequence));work_busy_on_recheck=active_on_recheck=false;mutate_generation=false;callbacks=callback_length=msgs=0;msg_status=0;now_ms=1000;input_irq=0;used=0;trace[0]=0;}
