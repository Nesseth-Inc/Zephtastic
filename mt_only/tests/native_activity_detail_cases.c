// SPDX-License-Identifier: GPL-3.0-only
// Same-observation numeric detail, using actual extracted production bodies.
static uint16_t detail;
static bool observe(void){reads=0;work_checks=0;return sx126x_is_receiving_diagnostic(&dev,&detail);}
static void expect(unsigned reason,unsigned error){assert((detail&31U)==reason&&(detail>>5)==error&&detail<=4095);}
int main(void){
 unsigned groups=0;
 boot(0);assert(!observe());expect(0,0);++groups;
 boot(0);dev.ready=false;assert(observe());expect(1,ENODEV);++groups;
 boot(0);chip_busy=1;assert(observe());expect(2,0);++groups;
 boot(0);chip_busy=-EIO;assert(observe());expect(3,EIO);++groups;
 boot(0);mutex_result=-EBUSY;assert(observe());expect(4,0);++groups;
 boot(0);work_busy=1;assert(observe());expect(5,0);++groups;
 boot(0);read_error=-EIO;assert(observe());expect(6,EIO);++groups;
 boot(SX126X_IRQ_RX_DONE);assert(observe());expect(7,0);assert(submits==1&&clears==0);++groups;
 boot(SX126X_IRQ_RX_DONE);submit_result=-EAGAIN;assert(observe());expect(8,EAGAIN);++groups;
 boot(0);data.rx_packet_active=true;assert(observe());expect(9,0);++groups;
 boot(SX126X_IRQ_HEADER_VALID);assert(observe());expect(10,0);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);assert(observe());expect(11,0);++groups;
 boot(0);data.rx_packet_active=true;data.header_seen_at_ms=1000;now_ms=1100;assert(observe());expect(12,0);++groups;
 boot(SX126X_IRQ_HEADER_VALID);data.raw_header_seen_at_ms=1000;now_ms=1100;assert(observe());expect(13,0);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1020;assert(observe());expect(14,0);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1050;late_work_busy=true;assert(observe());expect(15,0);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1050;late_read_error=-EIO;assert(observe());expect(16,EIO);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1050;inject_late=true;late_irq=SX126X_IRQ_RX_DONE;assert(observe());expect(17,0);assert(clears==0);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1050;inject_late=true;late_irq=SX126X_IRQ_HEADER_VALID;assert(observe());expect(18,0);assert(clears==0);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1050;clear_error=-EIO;assert(observe());expect(19,EIO);++groups;
 boot(0);chip_busy=(-2147483647-1);assert(observe());expect(3,127);++groups;
 boot(SX126X_IRQ_PREAMBLE_DETECTED);data.preamble_seen_at_ms=1000;now_ms=1050;assert(!observe());expect(0,0);assert(clears==1);++groups;
 printf("PASS same-observation busy detail: %u groups; all20codes, errorclamp, no extra native I/O\n",groups);
}
