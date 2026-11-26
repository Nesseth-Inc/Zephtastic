#define sx126x_observe_loss(...) ((void)0)
#define sx126x_observe_rx(...) ((void)0)
/* Synthetic HAL/RTOS fixture for exact production rx_activity extraction. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef NDEBUG
#error Native activity tests require assertions
#endif
#define K_NO_WAIT 0
#define BIT(n) (1U<<(n))
#define SX126X_IRQ_TX_DONE BIT(0)
#define SX126X_IRQ_RX_DONE BIT(1)
#define SX126X_IRQ_PREAMBLE_DETECTED BIT(2)
#define SX126X_IRQ_SYNC_WORD_VALID BIT(3)
#define SX126X_IRQ_HEADER_VALID BIT(4)
#define SX126X_IRQ_HEADER_ERR BIT(5)
#define SX126X_IRQ_CRC_ERR BIT(6)
#define SX126X_IRQ_RX_TX_TIMEOUT BIT(9)
typedef int32_t atomic_t;
typedef int32_t atomic_val_t;
struct k_work {int unused;};
struct sx126x_data {int lock,dio1_wq;struct k_work irq_work;bool rx_packet_active;
    atomic_t preamble_seen_at_ms,header_seen_at_ms,raw_header_seen_at_ms,rx_stale_releases;};
struct device {struct sx126x_data *data;bool ready;};
static struct sx126x_data data;
static struct device dev={&data,true};
static uint32_t now_ms;
static uint16_t irq,late_irq,last_clear;
static int chip_busy,mutex_result,work_busy,read_error,clear_error,submit_result,late_read_error;
static bool late_work_busy;static unsigned work_checks;
static unsigned reads,clears,submits;
static bool inject_late;
static char trace[1024];static unsigned trace_used;
static void mark(char c){assert(trace_used+1<sizeof(trace));trace[trace_used++]=c;trace[trace_used]=0;}
static bool device_is_ready(const struct device *d){mark('D');return d->ready;}
static int sx126x_hal_is_busy(const struct device*d){(void)d;mark('B');return chip_busy;}
static int k_mutex_lock(int*l,int wait){(void)l;(void)wait;mark('L');return mutex_result;}
static void k_mutex_unlock(int*l){(void)l;mark('U');}
static int k_work_busy_get(struct k_work*w){(void)w;mark('W');++work_checks;return late_work_busy&&work_checks==2?1:work_busy;}
static int sx126x_get_irq_status(const struct device*d,uint16_t*out){(void)d;mark('I');++reads;if(read_error)return read_error;if(reads==2&&late_read_error)return late_read_error;if(inject_late&&reads==2)irq=late_irq;*out=irq;return 0;}
static int sx126x_clear_irq_status(const struct device*d,uint16_t mask){(void)d;mark('C');assert(!(mask&(SX126X_IRQ_RX_DONE|SX126X_IRQ_CRC_ERR|SX126X_IRQ_RX_TX_TIMEOUT)));++clears;last_clear=mask;if(clear_error)return clear_error;irq&=(uint16_t)~mask;return 0;}
static int k_work_submit_to_queue(int*q,struct k_work*w){(void)q;(void)w;mark('Q');++submits;return submit_result;}
static uint32_t k_uptime_get_32(void){mark('T');return now_ms;}
static uint32_t sx126x_preamble_grace_ms(struct sx126x_data*d){(void)d;return 50;}
static uint32_t sx126x_max_payload_ms(struct sx126x_data*d){(void)d;return 200;}
static int32_t atomic_get(const atomic_t*p){return*p;}
static void atomic_set(atomic_t*p,atomic_val_t v){*p=v;}
static void atomic_inc(atomic_t*p){++*p;}
static void sx126x_reset_rx_busy_signals(struct sx126x_data*d){d->rx_packet_active=false;d->preamble_seen_at_ms=d->header_seen_at_ms=d->raw_header_seen_at_ms=0;}
static void boot(uint16_t bits){memset(&data,0,sizeof(data));dev.ready=true;now_ms=1000;irq=bits;late_irq=last_clear=0;chip_busy=mutex_result=work_busy=read_error=clear_error=submit_result=0;reads=clears=submits=0;inject_late=false;late_read_error=0;late_work_busy=false;work_checks=0;trace_used=0;trace[0]=0;}
static void case_done(const char*name,int result){printf("%s ret=%d irq=%u active=%d pre=%u head=%u raw=%u clears=%u mask=%u submits=%u trace=%s\n",name,result,irq,data.rx_packet_active,(uint32_t)data.preamble_seen_at_ms,(uint32_t)data.header_seen_at_ms,(uint32_t)data.raw_header_seen_at_ms,clears,last_clear,submits,trace);}
