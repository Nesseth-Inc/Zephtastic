/* Common deterministic activity scenarios; no production classifier replica. */
#define P SX126X_IRQ_PREAMBLE_DETECTED
#define H SX126X_IRQ_HEADER_VALID
#define E SX126X_IRQ_HEADER_ERR
#define R SX126X_IRQ_RX_DONE
#define C SX126X_IRQ_CRC_ERR
#define S SX126X_IRQ_SYNC_WORD_VALID
#define FULL_STALE (P|S|H|E)
enum stale_mode {ORDINARY,ACTIVE,STATUS_ERROR,RECHECK_ERROR,CLEAR_ERROR,
  LATE_ACTIVE,WORK_BUSY,RECHECK_WORK,CHIP_BUSY,GPIO_ERROR,MUTEX_BUSY,NOT_READY,
  PREAMBLE_FIRST,PREAMBLE_GRACE,HEADER_FIRST,ACTIVE_FIRST,SUBMIT_ERROR,TIME_ZERO};
struct stale_case {const char *name;uint16_t initial,latest;enum stale_mode mode;
  int result;uint16_t reason;unsigned events;uint16_t event0,event1,clear_mask;
  unsigned dispatches;bool loss;};
static const struct stale_case stale_cases[]={
  {"pre-expired-zero",P,0,ORDINARY,0,0,0,0,0,P,0,false},
  {"pre-expired-pre",P,P,ORDINARY,0,0,0,0,0,P,0,false},
  {"pre-late-crc",P,C,ORDINARY,0,0,1,C,0,P,0,false},
  {"pre-late-header-error",P,E,ORDINARY,0,0,1,E,0,P,0,false},
  {"pre-late-sync",P,S,ORDINARY,0,0,1,S,0,P,0,false},
  {"pre-late-pre-crc",P,P|C,ORDINARY,0,0,1,P|C,0,P,0,false},
  {"pre-late-unknown-bit",P,0x8000,ORDINARY,0,0,1,0x8000,0,P,0,false},
  {"header-expired",H,0,ORDINARY,0,0,1,0,0,FULL_STALE,0,false},
  {"active-expired",0,0,ACTIVE,0,0,1,0,0,FULL_STALE,0,false},
  {"initial-pre-crc",P|C,P,ORDINARY,0,0,1,P|C,0,P,0,false},
  {"initial-pre-header-error",P|E,P,ORDINARY,0,0,1,P|E,0,P,0,false},
  {"initial-crc",C,0,ORDINARY,0,0,1,C,0,0,0,false},
  {"initial-header-error",E,0,ORDINARY,0,0,1,E,0,0,0,false},
  {"first-status-error",P,0,STATUS_ERROR,-EIO,SX126X_RX_BUSY_STATUS_ERROR,1,0,0,0,0,false},
  {"late-status-error",P,0,RECHECK_ERROR,-EIO,SX126X_RX_BUSY_RECHECK_ERROR,1,0,0,0,0,false},
  {"clear-pre-zero-error",P,0,CLEAR_ERROR,-EIO,SX126X_RX_BUSY_CLEAR_ERROR,1,0,0,P,0,false},
  {"clear-pre-pre-error",P,P,CLEAR_ERROR,-EIO,SX126X_RX_BUSY_CLEAR_ERROR,1,P,0,P,0,false},
  {"clear-header-error",H,0,CLEAR_ERROR,-EIO,SX126X_RX_BUSY_CLEAR_ERROR,1,0,0,FULL_STALE,0,false},
  {"late-rxdone-preserved",P,R,ORDINARY,1,SX126X_RX_BUSY_LATE_DONE,0,0,0,0,0,false},
  {"late-header-preserved",P,P|H,ORDINARY,1,SX126X_RX_BUSY_LATE_HEADER,0,0,0,0,0,false},
  {"late-worker-active-preserved",P,0,LATE_ACTIVE,1,SX126X_RX_BUSY_LATE_HEADER,0,0,0,0,0,false},
  {"worker-first-busy",P,0,WORK_BUSY,1,SX126X_RX_BUSY_WORK,0,0,0,0,0,false},
  {"worker-recheck-busy",P,0,RECHECK_WORK,1,SX126X_RX_BUSY_RECHECK_WORK,0,0,0,0,0,false},
  {"chip-busy",P,0,CHIP_BUSY,1,SX126X_RX_BUSY_CHIP,0,0,0,0,0,false},
  {"gpio-error-gate",P,0,GPIO_ERROR,-EIO,SX126X_RX_BUSY_GPIO_ERROR,0,0,0,0,0,false},
  {"mutex-busy",P,0,MUTEX_BUSY,1,SX126X_RX_BUSY_MUTEX,0,0,0,0,0,false},
  {"not-ready",P,0,NOT_READY,-ENODEV,SX126X_RX_BUSY_NOT_READY,0,0,0,0,0,false},
  {"first-preamble-busy",P,0,PREAMBLE_FIRST,1,SX126X_RX_BUSY_PREAMBLE_FIRST,0,0,0,0,0,false},
  {"preamble-grace-busy",P,0,PREAMBLE_GRACE,1,SX126X_RX_BUSY_PREAMBLE_WAIT,0,0,0,0,0,false},
  {"first-header-busy",H,0,HEADER_FIRST,1,SX126X_RX_BUSY_HEADER_FIRST,0,0,0,0,0,false},
  {"first-active-busy",0,0,ACTIVE_FIRST,1,SX126X_RX_BUSY_ACTIVE_FIRST,0,0,0,0,0,false},
  {"initial-rxdone-dispatch",R,0,ORDINARY,1,SX126X_RX_BUSY_RX_DONE,0,0,0,0,1,false},
  {"initial-rxdone-dispatch-error",R,0,SUBMIT_ERROR,-EIO,SX126X_RX_BUSY_DISPATCH_ERROR,0,0,0,0,1,true},
  {"header-late-rxdone-preserved",H,R,ORDINARY,1,SX126X_RX_BUSY_LATE_DONE,0,0,0,0,0,false},
  {"initial-and-late-crc",P|C,C,ORDINARY,0,0,2,P|C,C,P,0,false},
  {"no-activity",0,0,ORDINARY,0,0,0,0,0,0,0,false},
  {"preamble-time-zero-grace",P,0,TIME_ZERO,1,SX126X_RX_BUSY_PREAMBLE_WAIT,0,0,0,0,0,false},
  {"clear-late-crc-error",P,C,CLEAR_ERROR,-EIO,SX126X_RX_BUSY_CLEAR_ERROR,1,C,0,P,0,false},
  {"header-late-header-error",H,E,ORDINARY,0,0,1,E,0,FULL_STALE,0,false},
  {"late-all-bits-rxdone-preserved",P,0xffff,ORDINARY,1,SX126X_RX_BUSY_LATE_DONE,0,0,0,0,0,false},
};
static unsigned stale_case_count(void){return sizeof(stale_cases)/sizeof(stale_cases[0]);}
static const struct stale_case *stale_prepare(unsigned index,bool observer){
  const struct stale_case *c=&stale_cases[index];boot();if(observer)enable();
  irq_reads=work_reads=clear_calls=0;last_clear_mask=0;now_ms=5000;
  irq_sequence_count=2;irq_sequence[0]=c->initial;irq_sequence[1]=c->latest;
  atomic_set(&data.preamble_seen_at_ms,1000);atomic_set(&data.raw_header_seen_at_ms,1000);
  atomic_set(&data.header_seen_at_ms,1000);
  switch(c->mode){
    case ORDINARY:break;
    case ACTIVE:data.rx_packet_active=true;break;
    case STATUS_ERROR:irq_error_sequence[0]=-EIO;break;
    case RECHECK_ERROR:irq_error_sequence[1]=-EIO;break;
    case CLEAR_ERROR:clear_error=-EIO;break;
    case LATE_ACTIVE:active_on_recheck=true;break;
    case WORK_BUSY:data.irq_work.busy=1;break;
    case RECHECK_WORK:work_busy_on_recheck=true;break;
    case CHIP_BUSY:chip_busy=1;break;
    case GPIO_ERROR:chip_busy=-EIO;break;
    case MUTEX_BUSY:mutex_error=-EBUSY;break;
    case NOT_READY:dev.ready=false;break;
    case PREAMBLE_FIRST:atomic_set(&data.preamble_seen_at_ms,0);break;
    case PREAMBLE_GRACE:atomic_set(&data.preamble_seen_at_ms,4951);break;
    case HEADER_FIRST:atomic_set(&data.raw_header_seen_at_ms,0);break;
    case ACTIVE_FIRST:data.rx_packet_active=true;atomic_set(&data.header_seen_at_ms,0);break;
    case SUBMIT_ERROR:submit_result=-EIO;break;
    case TIME_ZERO:now_ms=0;atomic_set(&data.preamble_seen_at_ms,1);break;
  }
  used=0;trace[0]=0;return c;
}
