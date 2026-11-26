#!/usr/bin/env python3
"""Compile exact driver health/RX and BUSY/opcode bodies against synthetic I/O."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--zephyr',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=Path(__file__).resolve().parents[1]
# Existing RX oracle extracts exact bodies from the isolated Zephyr checkout.
subprocess.run(['python3',str(app.parent/'zephcore/bench/run-native-rx-rejection-tests.py'),'--zephyr',str(a.zephyr.resolve()),'--output',str(out/'rx')],check=True)
pth=out/'rx/native-rx-rejection.c';s=pth.read_text().replace('#define CONFIG_ZEPHCORE_DUAL_RX_BENCH 1','#define CONFIG_MT_ONLY 1');pth.write_text(s)
def run(path,name):
 cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(path),'-o',str(out/name)]
 subprocess.run(cmd,check=True);r=subprocess.run([str(out/name)],capture_output=True,text=True,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});print(r.stdout,end='');return r.stdout
rx=run(pth,'rx-mt-only')
native=a.zephyr/'drivers/lora/native/sx126x/sx126x_hal_common.c';source=native.read_text()
def extract(name):
 begin=source.index('int '+name+'(');brace=source.index('{',begin);i=brace+1;depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[begin:i]
names=['sx126x_hal_wait_busy','sx126x_hal_write_cmd','sx126x_hal_read_cmd'];bodies={n:extract(n) for n in names}
header=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define CONFIG_MT_ONLY 1
#define CONFIG_MT_ALLOW_TX 0
#define CONFIG_ZEPHCORE_DUAL_RX_BENCH 0
#define IS_ENABLED(x) (x)
#define WAIT_FOR(expr,delay,step) ((void)(delay),(expr))
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define SX126X_BUSY_DEFAULT_TIMEOUT 1000
#define SX126X_CMD_SET_TX 0x83
#define SX126X_CMD_SET_RX 0x82
#define SX126X_CMD_SET_RX_DUTY_CYCLE 0x94
#define SX126X_CMD_SET_CAD 0xc5
#define SX126X_CMD_SET_TX_CONTINUOUS_WAVE 0xd1
#define SX126X_CMD_SET_SLEEP 0x84
struct sx126x_hal_config {int spi;};
struct device {const struct sx126x_hal_config *config;};
static bool busy;static unsigned faults,wakes,spiCalls,invalidations;
static void sx126x_airtime_observer_reconfigure_invalidate(const struct device *d){(void)d;++invalidations;}
static int sx126x_hal_is_busy(const struct device *d){(void)d;return busy;}
static int sx126x_bench_fault(const struct device *d,int r){(void)d;if(r<0)++faults;return r;}
static int sx126x_hal_wakeup(const struct device *d){(void)d;++wakes;assert(false);return 0;}
static bool sx126x_bench_tx_ready(const struct device *d){(void)d;return false;}
static bool sx126x_bench_tx_consume(const struct device *d,const uint8_t *p,size_t n){(void)d;(void)p;(void)n;return false;}
static int spi_transfer(const int *p,const uint8_t *h,size_t hn,uint8_t *b,size_t n,bool read){(void)p;(void)h;(void)hn;(void)b;(void)n;(void)read;++spiCalls;return 0;}
static void k_busy_wait(unsigned us){(void)us;}
'''
footer=r'''
int main(void){struct sx126x_hal_config cfg={0};struct device d={&cfg};
 busy=true;assert(sx126x_hal_wait_busy(&d,1000)==-ETIMEDOUT);assert(faults==1&&!wakes&&!spiCalls);
 busy=false;assert(sx126x_hal_wait_busy(&d,1000)==0);
 const uint8_t prohibited[]={0x83,0xd1,0xd2};
 for(unsigned i=0;i<3;++i){assert(sx126x_hal_write_cmd(&d,prohibited[i],NULL,0)==-EPERM);assert(sx126x_hal_read_cmd(&d,prohibited[i],NULL,0)==-EPERM);}
 assert(!spiCalls&&!invalidations);assert(sx126x_hal_write_cmd(&d,0x80,NULL,0)==0&&spiCalls==1&&!invalidations);
 busy=true;assert(sx126x_hal_write_cmd(&d,0x80,NULL,0)==-ETIMEDOUT&&spiCalls==1&&!wakes);
 busy=false;
 const uint8_t starts[]={SX126X_CMD_SET_RX,SX126X_CMD_SET_RX_DUTY_CYCLE,SX126X_CMD_SET_CAD};
 for(unsigned i=0;i<3;++i){assert(sx126x_hal_write_cmd(&d,starts[i],NULL,0)==0);assert(invalidations==i+1&&spiCalls==i+2);}
 puts("PASS exact MT_ONLY native BUSY finite timeout and six final SPI TX opcode barriers");return 0;}
'''
f=out/'native-boundaries.c';f.write_text(header+'\n'+'\n'.join(bodies.values())+'\n'+footer);result=run(f,'native-boundaries')
enabled_footer=r'''
int main(void){struct sx126x_hal_config cfg={0};struct device d={&cfg};
 assert(sx126x_hal_write_cmd(&d,0x83,NULL,0)==0&&spiCalls==1&&invalidations==1);
 for(unsigned op=0xd1;op<=0xd2;++op)assert(sx126x_hal_write_cmd(&d,op,NULL,0)==-EPERM);
 assert(sx126x_hal_read_cmd(&d,0x83,NULL,0)==-EPERM&&spiCalls==1&&invalidations==1);
 puts("PASS enabled packet TX permits SetTx but still blocks continuous wave/preamble and read-command bypass");return 0;}
'''
enabled=out/'native-enabled-boundaries.c';enabled.write_text(header.replace('#define CONFIG_MT_ALLOW_TX 0','#define CONFIG_MT_ALLOW_TX 1')+'\n'+'\n'.join(bodies.values())+'\n'+enabled_footer)
enabled_result=run(enabled,'native-enabled-boundaries')

# Extract and execute the exact driver-owned storage pause against a synthetic
# modem/RTOS.  The standby stub can latch RX_DONE at the precise point the old
# application-only barrier missed: after DIO submission is closed but before
# the final chip status read.
driver=(a.zephyr/'drivers/lora/native/sx126x/sx126x.c').read_text()
def driver_extract(declaration):
 begin=driver.index(declaration);brace=driver.index('{',begin);i=brace+1;depth=1
 while depth:
  depth+=(driver[i]=='{')-(driver[i]=='}');i+=1
 return driver[begin:i]
storage_functions=[driver_extract(x) for x in (
 'static void sx126x_observe_reconfigure_quiescent(',
 'static void sx126x_storage_standby_state(',
 'static int sx126x_storage_settle_after_standby(',
 'int sx126x_pause_for_storage(')]
pause='\n\n'.join(storage_functions)
pause_header=r'''
#define sx126x_observe_rx(...) ((void)0)
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define K_FOREVER 0
#define SX126X_STATE_RX 3
#define SX126X_REST_STATE 1
#define SX126X_STANDBY_RC 0
#define SX126X_IRQ_RX_DONE 2U
#define SX126X_IRQ_HEADER_VALID 16U
#define SX126X_IRQ_ALL 0xffffU
#define ARG_UNUSED(x) (void)(x)
typedef int atomic_t;
typedef void (*lora_recv_cb)(void);
struct k_work {int value;};struct k_work_delayable {int value;};
struct k_work_sync {int value;};struct k_mutex {int value;};
struct k_spinlock {bool held;};typedef unsigned k_spinlock_key_t;
struct sx126x_data {struct k_work irq_work;struct k_work_delayable dc_watchdog_work;
 struct k_mutex lock;atomic_t state,rx_cb_gen;void *rx_cb,*rx_cb_user_data;
 bool rx_duty_cycle_enabled,cad_active;void *tx_async_signal,*cad_cb;
 atomic_t observation_enabled,observation_reconfigure_permit;struct k_spinlock observation_lock;};
struct device {struct sx126x_data *data;bool ready;};
static struct sx126x_data data;static struct device dev={&data,true};
static uint16_t chip_irq;static bool callback_enabled,frozen,slept;
static bool inject_done,inject_header,running_work,inject_work_on_enable,late_work_runs,late_work;
static int delivered,restarts,joined_work,standby_calls,standby_fail_call;
static int callback_disable_error,callback_enable_error,standby_error,status_error,restart_error;
static int clear_error,sleep_error,error_delivered,journal_writes;
static bool device_is_ready(const struct device *d){return d->ready;}
static int atomic_get(const atomic_t *v){return *v;}static void atomic_set(atomic_t *v,int n){*v=n;}
static void atomic_inc(atomic_t *v){++*v;}
static k_spinlock_key_t k_spin_lock(struct k_spinlock *s){assert(!s->held);s->held=true;return 0x5a;}
static void k_spin_unlock(struct k_spinlock *s,k_spinlock_key_t k){assert(s->held&&k==0x5a);s->held=false;}
static void k_mutex_lock(struct k_mutex *m,int timeout){(void)timeout;++m->value;}
static void k_mutex_unlock(struct k_mutex *m){assert(m->value==1);--m->value;}
static int sx126x_hal_set_dio1_callback(const struct device *d,void (*cb)(const struct device *)){
 (void)d;if(cb&&callback_enable_error)return callback_enable_error;
 if(!cb&&callback_disable_error)return callback_disable_error;
 callback_enabled=cb!=NULL;if(cb&&inject_work_on_enable){running_work=true;late_work=true;}return 0;}
static void sx126x_hal_retire_dio1_callback(const struct device *d){(void)d;callback_enabled=false;}
static bool k_work_cancel_sync(struct k_work *w,struct k_work_sync *s){(void)w;(void)s;
 assert(!callback_enabled);if(running_work){if(late_work){++joined_work;if(late_work_runs){if(chip_irq&SX126X_IRQ_RX_DONE)++delivered;chip_irq=0;frozen=false;}}else{++delivered;chip_irq=0;}running_work=late_work=false;}return true;}
static bool k_work_cancel_delayable_sync(struct k_work_delayable *w,struct k_work_sync *s){(void)w;(void)s;return true;}
static int sx126x_set_standby(const struct device *d,int mode){(void)d;(void)mode;
 ++standby_calls;
 if(inject_done){chip_irq|=SX126X_IRQ_RX_DONE;inject_done=false;}
 if(inject_header){chip_irq|=SX126X_IRQ_HEADER_VALID;inject_header=false;}
 if(standby_error&&(standby_fail_call==0||standby_calls==standby_fail_call))return standby_error;
 frozen=true;
 return 0;}
static int sx126x_get_irq_status(const struct device *d,uint16_t *irq){(void)d;
 if(status_error)return status_error;
 *irq=chip_irq;return 0;}
static int sx126x_restart_rx(const struct device *d,struct sx126x_data *s){(void)d;(void)s;
 assert(callback_enabled);if(restart_error)return restart_error;++restarts;chip_irq=0;frozen=false;return 0;}
static int sx126x_handle_irq_status(const struct device *d,uint16_t irq){(void)d;
 assert(irq&SX126X_IRQ_RX_DONE);chip_irq=0;
 if(restart_error){++error_delivered;return restart_error;}
 ++delivered;
 ++restarts;frozen=false;return 0;}
static int sx126x_clear_irq_status(const struct device *d,uint16_t irq){(void)d;if(clear_error)return clear_error;chip_irq&=~irq;return 0;}
static int sx126x_set_sleep(const struct device *d){if(sleep_error)return sleep_error;slept=true;frozen=false;d->data->state=SX126X_REST_STATE;return 0;}
static void sx126x_reset_rx_busy_signals(struct sx126x_data *s){(void)s;}
static void sx126x_set_rf_path(const struct device *d,bool enable,bool tx){(void)d;(void)enable;(void)tx;}
static void sx126x_dio1_callback(const struct device *d){(void)d;}
'''
pause_footer=r'''
static void boot(void){data=(struct sx126x_data){0};data.state=SX126X_STATE_RX;data.rx_cb=(void *)1;
 dev.ready=true;chip_irq=0;callback_enabled=true;frozen=slept=inject_done=inject_header=running_work=false;
 inject_work_on_enable=late_work=false;late_work_runs=true;joined_work=0;
 standby_calls=standby_fail_call=0;
 delivered=restarts=callback_disable_error=callback_enable_error=standby_error=status_error=restart_error=0;
 clear_error=sleep_error=error_delivered=journal_writes=0;}
int main(void){
 boot();data.observation_enabled=1;
 assert(sx126x_pause_for_storage(&dev)==0&&slept&&!callback_enabled&&!data.rx_cb&&data.state==SX126X_REST_STATE);
 assert(data.observation_reconfigure_permit==1&&!data.observation_lock.held);
 boot();data.observation_enabled=1;inject_done=true;
 assert(sx126x_pause_for_storage(&dev)==-EAGAIN&&!data.observation_reconfigure_permit&&!data.observation_lock.held);
 boot();inject_done=true;assert(sx126x_pause_for_storage(&dev)==-EAGAIN&&delivered==1&&restarts==1&&callback_enabled&&!slept);
 boot();inject_header=true;assert(sx126x_pause_for_storage(&dev)==-EAGAIN&&!delivered&&restarts==1&&callback_enabled&&!slept);
 boot();running_work=true;assert(sx126x_pause_for_storage(&dev)==0&&delivered==1&&slept);
 boot();standby_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-EAGAIN&&restarts==1&&callback_enabled&&!slept&&data.rx_cb);
 boot();standby_error=-EIO;inject_done=true;assert(sx126x_pause_for_storage(&dev)==-EAGAIN&&delivered==1&&restarts==1&&callback_enabled&&!slept&&data.rx_cb);
 boot();standby_error=-EIO;status_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-EIO&&!restarts&&callback_enabled&&!slept&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();callback_disable_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-EIO&&callback_enabled&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();data.state=SX126X_REST_STATE;assert(sx126x_pause_for_storage(&dev)==-EIO&&!callback_enabled&&!restarts&&!slept&&data.state==SX126X_REST_STATE&&data.rx_cb);
 boot();data.rx_cb=NULL;assert(sx126x_pause_for_storage(&dev)==-EIO&&!callback_enabled&&!restarts&&!slept&&data.state==SX126X_STATE_RX&&!data.rx_cb);
 boot();status_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-EIO&&!callback_enabled&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();inject_header=true;callback_enable_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-EBUSY&&!callback_enabled&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();inject_done=true;callback_enable_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-ESHUTDOWN&&delivered==1&&!callback_enabled&&data.state==SX126X_REST_STATE);
 boot();inject_header=true;restart_error=-EFAULT;assert(sx126x_pause_for_storage(&dev)==-EBUSY&&!callback_enabled&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();inject_done=true;restart_error=-EFAULT;assert(sx126x_pause_for_storage(&dev)==-ESHUTDOWN&&error_delivered==1&&!callback_enabled&&data.state==SX126X_REST_STATE);
 boot();inject_header=true;restart_error=-EFAULT;inject_work_on_enable=true;assert(sx126x_pause_for_storage(&dev)==-ESHUTDOWN&&joined_work==1&&frozen==false&&!callback_enabled&&data.state==SX126X_REST_STATE);
 boot();inject_header=true;restart_error=-EFAULT;inject_work_on_enable=true;late_work_runs=false;assert(sx126x_pause_for_storage(&dev)==-EBUSY&&joined_work==1&&!callback_enabled&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();inject_header=true;callback_enable_error=-EIO;standby_error=-ENODEV;standby_fail_call=2;assert(sx126x_pause_for_storage(&dev)==-ENODEV&&standby_calls==2&&!callback_enabled&&data.state==SX126X_STATE_RX&&data.rx_cb);
 boot();clear_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-ESHUTDOWN&&!callback_enabled&&data.state==SX126X_REST_STATE);
 boot();sleep_error=-EIO;assert(sx126x_pause_for_storage(&dev)==-ESHUTDOWN&&!callback_enabled&&data.state==SX126X_REST_STATE);
 assert(journal_writes==0);
 puts("PASS exact driver storage pause: transactional callback failures, explicit RX/standby/unknown outcomes, preserved captured RX_DONE, and zero storage writes");return 0;}
'''
pause_file=out/'native-storage-pause.c';pause_file.write_text(pause_header+'\n'+pause+'\n'+pause_footer)
pause_result=run(pause_file,'native-storage-pause')

# The standalone GPIO HAL must roll its callback pointer back on a failed
# interrupt configuration.  Compile the exact production function separately.
hal=(a.zephyr/'drivers/lora/native/sx126x/sx126x_hal.c').read_text()
def source_extract(source,declaration):
 begin=source.index(declaration);brace=source.index('{',begin);i=brace+1;depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[begin:i]
hal_callback=source_extract(hal,'int sx126x_hal_set_dio1_callback(')
hal_header=r'''
#include <assert.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define GPIO_INT_EDGE_TO_ACTIVE 1
#define GPIO_INT_DISABLE 2
struct gpio_dt_spec {int pin;};
struct device;
struct sx126x_hal_config {struct gpio_dt_spec dio1;};
struct sx126x_hal_data {void (*dio1_callback)(const struct device *dev);};
struct device {const struct sx126x_hal_config *config;struct sx126x_hal_data *data;};
static int gpio_error,configured,first_calls,second_calls;
static struct sx126x_hal_data *observed_data;
static struct sx126x_hal_data *get_hal_data(const struct device *dev){return dev->data;}
static int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *gpio,int mode){(void)gpio;configured=mode;if(observed_data->dio1_callback)observed_data->dio1_callback(NULL);return gpio_error;}
static void first(const struct device *dev){(void)dev;++first_calls;}
static void second(const struct device *dev){(void)dev;++second_calls;}
'''
hal_footer=r'''
int main(void){struct sx126x_hal_config cfg={0};struct sx126x_hal_data data={.dio1_callback=first};struct device dev={&cfg,&data};observed_data=&data;
 gpio_error=-EIO;assert(sx126x_hal_set_dio1_callback(&dev,NULL)==-EIO&&data.dio1_callback==first&&configured==GPIO_INT_DISABLE&&first_calls==1&&!second_calls);
 gpio_error=0;assert(sx126x_hal_set_dio1_callback(&dev,NULL)==0&&data.dio1_callback==NULL&&first_calls==2&&!second_calls);
 gpio_error=-EIO;assert(sx126x_hal_set_dio1_callback(&dev,second)==-EIO&&data.dio1_callback==NULL&&configured==GPIO_INT_EDGE_TO_ACTIVE&&!second_calls);
 gpio_error=0;assert(sx126x_hal_set_dio1_callback(&dev,second)==0&&data.dio1_callback==second&&!second_calls);
 puts("PASS exact standalone GPIO callback replacement keeps the old handler through disable and publishes a new handler only after enable succeeds");return 0;}
'''
hal_file=out/'native-callback-transaction.c';hal_file.write_text(hal_header+'\n'+hal_callback+'\n'+hal_footer)
hal_result=run(hal_file,'native-callback-transaction')

# The application adapter must not relabel a partially mutating failed restore
# as verified standby. Compile its exact pure outcome mapper as C.
main_source=(app/'src/main.cpp').read_text()
resume_outcome=source_extract(main_source,'static int storage_resume_outcome(')
resume_file=out/'native-storage-resume-outcome.c'
resume_file.write_text('#include <assert.h>\n#include <errno.h>\n#include <stdio.h>\n'+resume_outcome+r'''
int main(void){assert(storage_resume_outcome(0)==-EAGAIN);assert(storage_resume_outcome(-EIO)==-EIO);assert(storage_resume_outcome(-ENODEV)==-ENODEV);puts("PASS exact application storage-resume outcome propagates partially mutating restore failures as unknown hard faults");return 0;}
''')
resume_result=run(resume_file,'native-storage-resume-outcome')

# Terminal sleep/abort deliberately retire software callback admission even if
# the hardware disable reports failure. Abort then joins both work sources,
# closes admission again, and exposes REST only after SetStandby succeeds.
terminal='\n\n'.join(driver_extract(x) for x in (
 'static int sx126x_set_sleep(', 'int sx126x_bench_abort('))
terminal_header=r'''
#define sx126x_observe_rx(...) ((void)0)
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define CONFIG_LORA_SX126X_NATIVE_SLEEP 1
#define IS_ENABLED(x) (x)
#define K_FOREVER 0
#define SX126X_SLEEP_WARM_START 1U
#define SX126X_STATE_IDLE 0
#define SX126X_REST_STATE 1
#define SX126X_STATE_SLEEP 2
#define SX126X_STATE_RX 3
#define SX126X_STANDBY_RC 0
#define SX126X_CMD_SET_SLEEP 0x84
typedef int atomic_t;
struct k_work {int value;};struct k_work_delayable {int value;};
struct k_work_sync {int value;};struct k_mutex {int value;};
struct sx126x_data {struct k_work irq_work;struct k_work_delayable dc_watchdog_work;
 struct k_mutex lock;atomic_t state,rx_cb_gen;void *tx_async_signal,*cad_cb,*rx_cb,*rx_cb_user_data;
 bool cad_active;};
struct device {struct sx126x_data *data;bool ready;};
static struct sx126x_data data;static struct device dev={&data,true};
static bool callback_enabled;static int callback_disable_error,retire_calls,disable_calls;
static int joined_irq,joined_watchdog,standby_calls,standby_error,sleep_commands,revoke_calls;
static int atomic_get(const atomic_t *v){return *v;}static void atomic_set(atomic_t *v,int n){*v=n;}
static void atomic_inc(atomic_t *v){++*v;}
static int sx126x_hal_set_dio1_callback(const struct device *d,void (*cb)(const struct device *)){
 (void)d;assert(cb==NULL);++disable_calls;if(callback_disable_error)return callback_disable_error;callback_enabled=false;return 0;}
static void sx126x_hal_retire_dio1_callback(const struct device *d){(void)d;callback_enabled=false;++retire_calls;}
static void sx126x_set_rf_path(const struct device *d,bool enable,bool tx){(void)d;assert(!enable&&!tx);}
static int sx126x_hal_write_cmd(const struct device *d,uint8_t cmd,const uint8_t *p,size_t n){
 (void)d;assert(cmd==SX126X_CMD_SET_SLEEP&&p&&n==1);++sleep_commands;return 0;}
static void sx126x_bench_revoke(void){++revoke_calls;}
static bool device_is_ready(const struct device *d){return d->ready;}
static bool k_work_cancel_sync(struct k_work *w,struct k_work_sync *s){(void)w;(void)s;assert(!callback_enabled);++joined_irq;return true;}
static bool k_work_cancel_delayable_sync(struct k_work_delayable *w,struct k_work_sync *s){(void)w;(void)s;assert(!callback_enabled);++joined_watchdog;return true;}
static void k_mutex_lock(struct k_mutex *m,int timeout){(void)timeout;++m->value;}
static void k_mutex_unlock(struct k_mutex *m){assert(m->value==1);--m->value;}
static int sx126x_set_standby(const struct device *d,int mode){(void)d;(void)mode;++standby_calls;return standby_error;}
static void sx126x_reset_rx_busy_signals(struct sx126x_data *s){(void)s;}
'''
terminal_footer=r'''
static void boot(void){data=(struct sx126x_data){0};data.state=SX126X_STATE_RX;data.rx_cb=(void *)1;
 dev.ready=true;callback_enabled=true;callback_disable_error=-EIO;retire_calls=disable_calls=0;
 joined_irq=joined_watchdog=standby_calls=standby_error=sleep_commands=revoke_calls=0;}
int main(void){
 boot();assert(sx126x_set_sleep(&dev)==0&&!callback_enabled&&retire_calls==1&&disable_calls==1&&sleep_commands==1&&data.state==SX126X_STATE_SLEEP);
 boot();assert(sx126x_bench_abort(&dev)==0&&!callback_enabled&&retire_calls==2&&disable_calls==2&&joined_irq==1&&joined_watchdog==1&&standby_calls==1&&data.state==SX126X_REST_STATE&&revoke_calls==1);
 boot();standby_error=-EIO;assert(sx126x_bench_abort(&dev)==-EIO&&!callback_enabled&&joined_irq==1&&joined_watchdog==1&&data.state==SX126X_STATE_RX);
 puts("PASS exact terminal sleep/abort callback retirement, joined work barrier, and honest standby failure state");return 0;}
'''
terminal_file=out/'native-terminal-cleanup.c';terminal_file.write_text(terminal_header+'\n'+terminal+'\n'+terminal_footer)
terminal_result=run(terminal_file,'native-terminal-cleanup')
terminal_disabled_footer=r'''
static void boot(void){data=(struct sx126x_data){0};data.state=SX126X_STATE_RX;data.rx_cb=(void *)1;
 dev.ready=true;callback_enabled=true;callback_disable_error=-EIO;retire_calls=disable_calls=0;
 joined_irq=joined_watchdog=standby_calls=standby_error=sleep_commands=revoke_calls=0;}
int main(void){boot();assert(sx126x_set_sleep(&dev)==0&&!callback_enabled&&retire_calls==1&&disable_calls==1&&sleep_commands==0&&data.state==SX126X_STATE_IDLE);
 puts("PASS exact configured standby substitute retires callback admission without native sleep");return 0;}
'''
terminal_disabled_file=out/'native-terminal-no-sleep.c'
terminal_disabled_file.write_text(terminal_header.replace('#define CONFIG_LORA_SX126X_NATIVE_SLEEP 1','#define CONFIG_LORA_SX126X_NATIVE_SLEEP 0')+'\n'+terminal+'\n'+terminal_disabled_footer)
terminal_disabled_result=run(terminal_disabled_file,'native-terminal-no-sleep')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'hardware':False,'driver_sha256':hashlib.sha256(driver.encode()).hexdigest(),'hal_sha256':hashlib.sha256(hal.encode()).hexdigest(),'main_sha256':hashlib.sha256(main_source.encode()).hexdigest(),'unchanged_extracted_functions':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in bodies.items()},'storage_pause_functions_sha256':hashlib.sha256(pause.encode()).hexdigest(),'hal_callback_function_sha256':hashlib.sha256(hal_callback.encode()).hexdigest(),'storage_resume_outcome_sha256':hashlib.sha256(resume_outcome.encode()).hexdigest(),'terminal_cleanup_functions_sha256':hashlib.sha256(terminal.encode()).hexdigest(),'rx':rx,'result':result,'enabled_test':enabled_result,'storage_pause_test':pause_result,'callback_transaction_test':hal_result,'storage_resume_outcome_test':resume_result,'terminal_cleanup_test':terminal_result,'terminal_no_sleep_test':terminal_disabled_result},indent=2)+'\n')
