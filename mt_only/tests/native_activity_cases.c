/* m60 native busy cases: 14 named cases; parent owns driver, fixture, build. */
#include <errno.h>
#define PREAMBLE BIT(2)
#define RX_DONE  BIT(1)
#define HEADER   BIT(4)
int main(void)
{
	int ret;
	/* 1 idle: no IRQ bits -> ret 0, no clear. */
	boot(0);
	ret = sx126x_rx_activity(&dev);
	assert(ret == 0 && clears == 0);
	case_done("1-idle", ret);
	/* 2 first PREAMBLE observation: defers only, stamps now. */
	boot(PREAMBLE);
	now_ms = 500000;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && data.preamble_seen_at_ms == 500000 && clears == 0);
	case_done("2-first-obs", ret);
	/* 3 maintained at 1000; stale at 1050: PREAMBLE-only clear. */
	boot(PREAMBLE);
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && data.preamble_seen_at_ms == 1000);
	now_ms = 1050; reads = 0;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 0 && clears == 1 && last_clear == PREAMBLE && irq == 0);
	case_done("3-maintained", ret);
	/* 4 genuine PREAMBLE within grace: retained, no clear. */
	boot(PREAMBLE);
	data.preamble_seen_at_ms = 1000;
	now_ms = 1049;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && clears == 0 && data.preamble_seen_at_ms == 1000);
	case_done("4-genuine", ret);
	/* 5 active header within payload bound: retained. */
	boot(0);
	data.rx_packet_active = true;
	data.header_seen_at_ms = 1000;
	now_ms = 1199;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && data.rx_packet_active && clears == 0);
	case_done("5-active", ret);
	/* 6 expired active header: full reset, one clear. */
	boot(0);
	data.rx_packet_active = true;
	data.header_seen_at_ms = 1000;
	now_ms = 1200;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 0 && !data.rx_packet_active && clears == 1 && data.header_seen_at_ms == 0);
	case_done("6-expired-active", ret);
	/* 7 raw HEADER_VALID: first stamps, expiry clears. */
	boot(HEADER);
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && data.raw_header_seen_at_ms == 1000);
	now_ms = 1200; reads = 0;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 0 && clears == 1 && data.raw_header_seen_at_ms == 0);
	case_done("7-raw-header", ret);
	/* 8 RX_DONE at long now: submit once, preserved, no clear. */
	boot(RX_DONE);
	now_ms = 300000;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && submits == 1 && clears == 0 && (irq & RX_DONE) != 0);
	case_done("8-rx-done", ret);
	/* 9 late RX_DONE on 2nd snapshot: no clear; preserved for worker. */
	boot(PREAMBLE);
	data.preamble_seen_at_ms = 1000;
	now_ms = 1050;
	inject_late = true; late_irq = RX_DONE;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && clears == 0 && (irq & RX_DONE) != 0);
	case_done("9-late-rx-done", ret);
	/* 10 late HEADER_VALID on 2nd snapshot: retained, no clear. */
	boot(PREAMBLE);
	data.preamble_seen_at_ms = 1000;
	now_ms = 1050;
	inject_late = true; late_irq = HEADER;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && clears == 0 && (irq & HEADER) != 0);
	case_done("10-late-header", ret);
	/* 11 chip busy 1 -> ret 1; -EIO -> ret -EIO; no reads. */
	boot(0);
	chip_busy = 1;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && reads == 0 && clears == 0);
	case_done("11-chip-busy", ret);
	boot(0);
	chip_busy = -EIO;
	ret = sx126x_rx_activity(&dev);
	assert(ret == -EIO && reads == 0 && clears == 0);
	case_done("11-chip-busy-eio", ret);
	/* 12 mutex -EBUSY -> ret 1; work busy -> ret 1; no reads. */
	boot(0);
	mutex_result = -EBUSY;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && reads == 0 && clears == 0);
	case_done("12-mutex-busy", ret);
	boot(0);
	work_busy = 1;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1 && reads == 0 && clears == 0);
	case_done("12-work-busy", ret);
	/* 13 read error -> -EIO; clear error -> -EIO, stamp kept. */
	boot(PREAMBLE);
	read_error = -EIO;
	ret = sx126x_rx_activity(&dev);
	assert(ret == -EIO && clears == 0);
	case_done("13-read-error", ret);
	boot(PREAMBLE);
	data.preamble_seen_at_ms = 1000;
	now_ms = 1050;
	clear_error = -EIO;
	ret = sx126x_rx_activity(&dev);
	assert(ret == -EIO && data.preamble_seen_at_ms == 1000);
	case_done("13-clear-error", ret);
	/* 14 uint32 wrap: elapsed 36 kept, 56 cleared. */
	boot(PREAMBLE);
	data.preamble_seen_at_ms = (int32_t)0xfffffff0U;
	now_ms = 20;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 1);
	now_ms = 40; reads = 0;
	ret = sx126x_rx_activity(&dev);
	assert(ret == 0 && clears == 1);
	case_done("14-uint32-wrap", ret);
	return 0;
}
