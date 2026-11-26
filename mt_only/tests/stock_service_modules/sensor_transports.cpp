// SPDX-License-Identifier: GPL-3.0-only
// Exercise main.cpp's USB and optional serial TX methods with capture-only owners.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
namespace {
bool held=false;
int usb=0;
size_t usbTx=0;
struct Line {unsigned generation=5; bool measured=true,connected=true;} line;
unsigned irq_lock(){assert(!held);held=true;return 0;}
void irq_unlock(unsigned){assert(held);held=false;}
Line observeUsbLineLocked(int){assert(held);return line;}
size_t ring_buf_space_get(size_t *q){assert(held);return 128-*q;}
size_t ring_buf_size_get(size_t *q){assert(held);return *q;}
size_t ring_buf_put(size_t *q,const uint8_t *,size_t n){assert(held&&n<=128-*q);*q+=n;return n;}
void uart_irq_tx_enable(int){assert(held);}
namespace mt_security { bool serialConsoleAllowed(){return true;} }
namespace mt_serial { constexpr size_t kMaxPhoneApiPayload=516; }
struct {bool ready(){return true;}} serialModule;
struct ClientHost {
    unsigned usbAcceptedGeneration_=5;bool usbAccepted_=true;
#include "usb_send.inc"
    virtual uint8_t keyVerificationOwner()const{return 0;}
    virtual int restoreExistingIdentity(const uint8_t *,size_t,uint32_t){return -ENOTSUP;}
    virtual bool sendIfQueueEmpty(const uint8_t *p,size_t n){return sendUsb(p,n,false,true);}
    virtual bool send(const uint8_t *p,size_t n){return sendUsb(p,n,false);}
};
#include "serial_send.inc"
}
void testSensorTransports()
{
    ClientHost host;
    SerialHost serial;
    const uint8_t first[]={1,2,3},second[]={4,5};
    assert(host.sendIfQueueEmpty(first,sizeof(first)) && !host.sendIfQueueEmpty(second,sizeof(second)));
    assert(serial.sendIfQueueEmpty(first,sizeof(first)) && !serial.sendIfQueueEmpty(second,sizeof(second)));
    usbTx=0;
    assert(host.sendIfQueueEmpty(second,sizeof(second)) && !serial.sendIfQueueEmpty(second,sizeof(second)));
    assert(host.send(first,sizeof(first)) && serial.send(second,sizeof(second)));
    uint8_t bytes[516]={};
    assert(serial.take(bytes,sizeof(bytes))==sizeof(first) && !memcmp(bytes,first,sizeof(first)));
    assert(serial.take(bytes,sizeof(bytes))==sizeof(second) && !memcmp(bytes,second,sizeof(second)));
    assert(serial.sendIfQueueEmpty(first,sizeof(first)) && !host.sendIfQueueEmpty(first,sizeof(first)));
    usbTx=0;++line.generation;
    assert(!host.sendIfQueueEmpty(first,sizeof(first)) && usbTx==0 && !held);
}
