// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../src/UsbSerialJtagTx.h"

#define ARG_UNUSED(x) (void)(x)
#define DT_NODE_HAS_COMPAT(node,compat) USB_CHOSEN
struct device {};
static device controller;
static const device *const usb=&controller;
using Callback=void (*)(const device *,void *);
static Callback callback;
struct Endpoint {
    bool ready=true, locked=false, injectOnEnable=false, injectAfterRead=false, closed=false, measured=false;
    int registerError=0, reads=0, interrupts=0, drops=0;
    uint32_t enabled=0, raw=0;
    std::vector<uint8_t> out, accepted, queued, delivered;
    std::vector<unsigned> txPackets;
    unsigned writable=0;
};
static Endpoint bus;
enum { USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT=4, USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY=8 };
static bool hostOut(const std::vector<uint8_t> &data){
    if(!bus.out.empty())return false;
    assert(!data.empty() && data.size()<=64);
    bus.out=data;bus.raw|=USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT;return true;
}
static void runPending(){
    if(bus.raw & bus.enabled){
        assert(!bus.locked && callback);
        ++bus.interrupts;callback(usb,nullptr);
    }
}
static void usb_serial_jtag_ll_clr_intsts_mask(unsigned bits){bus.raw&=~bits;}
static void usb_serial_jtag_ll_ena_intr_mask(unsigned bits){
    if(bus.injectOnEnable){assert(hostOut({7,8}));bus.injectOnEnable=false;}
    bus.enabled|=bits;
}
static bool usb_serial_jtag_ll_rxfifo_data_available(){return !bus.out.empty();}
static int usb_serial_jtag_ll_read_rxfifo(uint8_t *to,int space){
    assert(space>=int(bus.out.size()));
    const size_t n=bus.out.size();std::copy(bus.out.begin(),bus.out.end(),to);bus.out.clear();++bus.reads;
    if(bus.injectAfterRead){bus.injectAfterRead=false;assert(hostOut({9}));}
    return int(n);
}
static bool usb_serial_jtag_ll_txfifo_writable(){return bus.writable!=0;}
static unsigned usb_serial_jtag_ll_get_intr_ena_status(){return bus.enabled;}
static int usb_serial_jtag_ll_write_txfifo(const uint8_t *data,int size){
    int count=std::min(size,int(bus.writable));bus.delivered.insert(bus.delivered.end(),data,data+count);
    bus.txPackets.push_back(unsigned(count));
    bus.writable-=count;return count;
}
static void usb_serial_jtag_ll_txfifo_flush(){}
#include "driver.inc"
#define uart_irq_rx_enable serial_esp32_usb_irq_rx_enable
#define uart_irq_update serial_esp32_usb_irq_update
#define uart_irq_is_pending serial_esp32_usb_irq_is_pending
#define uart_irq_rx_ready serial_esp32_usb_irq_rx_ready
#define uart_fifo_read serial_esp32_usb_fifo_read
#define uart_irq_tx_ready serial_esp32_usb_irq_tx_ready
static unsigned irq_lock(){assert(!bus.locked);bus.locked=true;return 0;}
static void irq_unlock(unsigned){assert(bus.locked);bus.locked=false;}
static bool device_is_ready(const device *){return bus.ready;}
static int uart_irq_callback_user_data_set(const device *,Callback next,void *){
    if(bus.registerError)return bus.registerError;
    callback=next;return 0;
}
static int usbRx,usbTx,usbDrops,shutdownAdmissionClosed;
struct Line{bool measured,connected;};
static Line observeUsbLineLocked(const device *){return {bus.measured,false};}
static int atomic_get(int *){return bus.closed;}
static void atomic_inc(int *){++bus.drops;}
static size_t ring_buf_put(int *,uint8_t *p,size_t n){bus.accepted.insert(bus.accepted.end(),p,p+n);return n;}
static void notifyPowerActivity(){}
namespace bench {static void wipeKeyBuffer(void *p,size_t n){memset(p,0,n);}}
static void ring_buf_reset(int *){bus.queued.clear();}
static void uart_irq_tx_disable(const device *){bus.enabled&=~USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY;}
static uint32_t ring_buf_peek(int *,uint8_t *out,unsigned size){
    unsigned count=std::min(size,unsigned(bus.queued.size()));std::copy_n(bus.queued.begin(),count,out);return count;
}
static size_t ring_buf_size_get(int *){return bus.queued.size();}
#define uart_fifo_fill serial_esp32_usb_fifo_fill
static void ring_buf_consume(int *,unsigned count){bus.queued.erase(bus.queued.begin(),bus.queued.begin()+count);}
#include "app.inc"

int main(){
    // Start with a framed response queued, its interrupt enabled, and no raw
    // IN event. Polling while the host stalls must consume nothing.
    bus.queued={0x94,0xc3,0,2,4,5};const auto frame=bus.queued;
    bus.enabled=USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY;callback=usbInterrupt;
    pollUsbTxLocked(usb);runPending();assert(bus.queued==frame&&bus.interrupts==0);
    bus.writable=3;pollUsbTxLocked(usb);
#if defined(CONFIG_SOC_ESP32S3) && USB_CHOSEN
    assert(bus.delivered==std::vector<uint8_t>(frame.begin(),frame.begin()+3));
    bus.writable=64;pollUsbTxLocked(usb);assert(bus.queued.empty()&&bus.delivered==frame);
    bus.queued=frame;bus.measured=true;pollUsbTxLocked(usb);
    assert(bus.queued.empty()&&bus.delivered==frame); // stale epoch cannot disclose a frame
    for (unsigned size : {64u, 128u, 129u}) {
        bus={};bus.queued.resize(size);
        for (unsigned i=0;i<size;++i) bus.queued[i]=uint8_t(i);
        const auto expected=bus.queued;
        bus.enabled=USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY;
        while(!bus.queued.empty()) {
            pollUsbTxLocked(usb); // busy endpoint holds all remaining bytes
            bus.writable=64;
            pollUsbTxLocked(usb);
            bus.writable=0;
        }
        assert(bus.delivered==expected);
        const std::vector<unsigned> packets=size==64?std::vector<unsigned>{63,1}:
            size==128?std::vector<unsigned>{64,63,1}:std::vector<unsigned>{64,64,1};
        assert(bus.txPackets==packets);
    }
#else
    assert(bus.queued==frame&&bus.delivered.empty());
#endif
    bus={};callback=nullptr;
    // Existing driver clears any old OUT edge on enable; a full endpoint cannot
    // accept the next packet until its owner drains the first.
    assert(hostOut({1,2}));callback=usbInterrupt;uart_irq_rx_enable(usb);runPending();
    assert(bus.interrupts==0 && !hostOut({3}) && bus.accepted.empty());
    bus={};callback=nullptr;bus.ready=false;
    assert(startApplicationUsbRx()==-ENODEV && !callback && !bus.enabled);
    bus={};bus.registerError=-EIO;assert(startApplicationUsbRx()==-EIO && !callback && !bus.enabled);
    for(bool shutdown:{false,true}){
        bus={};callback=nullptr;bus.closed=shutdown;
        std::vector<uint8_t> packet(64,42);
        assert(hostOut(packet));assert(startApplicationUsbRx()==0);
#if defined(CONFIG_SOC_ESP32S3) && USB_CHOSEN
        assert(bus.out.empty() && bus.reads==1);
        assert(shutdown ? bus.accepted.empty() : bus.accepted==packet);
        runPending();assert(!bus.interrupts);
        assert(hostOut({0x94,0xc3}));runPending();assert(bus.interrupts==1 && bus.reads==2);
#else
        runPending();assert(bus.reads==0 && !bus.out.empty());
#endif
    }
#if defined(CONFIG_SOC_ESP32S3) && USB_CHOSEN
    bus={};bus.injectOnEnable=true;assert(startApplicationUsbRx()==0);
    assert(bus.reads==1 && bus.accepted==std::vector<uint8_t>({7,8}));
    bus={};bus.injectAfterRead=true;assert(hostOut({3}));assert(startApplicationUsbRx()==0);
    assert(bus.reads==1 && bus.accepted==std::vector<uint8_t>({3}));
    runPending();assert(bus.reads==2 && bus.accepted==std::vector<uint8_t>({3,9}));
    bus={};bus.measured=true;assert(hostOut({4}));assert(startApplicationUsbRx()==0);
    assert(bus.out.empty() && bus.accepted.empty()); // existing measured-low gate
#endif
    bus={};assert(startApplicationUsbRx()==0 && bus.reads==0);
    assert(hostOut({10}));runPending();assert(bus.accepted==std::vector<uint8_t>({10}));
}
