// SPDX-License-Identifier: GPL-3.0-only
#include "UsbBootloader.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static device usb;
NRF_POWER_Type test_power{{0x12,0x34}};
static uint32_t baud=115200;
static int getResult=0;
static unsigned reads=0;
static std::vector<std::string> order;
struct Reset {};
int uart_line_ctrl_get(const device *d,uint32_t cmd,uint32_t *value) {
  assert(d==&usb&&cmd==UART_LINE_CTRL_BAUD_RATE);++reads;*value=baud;return getResult;
}
void nrf_power_gpregret_set(NRF_POWER_Type *p,uint32_t index,uint32_t value) {
  assert(p==&test_power&&index==0&&value==0x57);
  assert((order==std::vector<std::string>{"stop","flush"}));
  p->gpregret[index]=value;order.push_back("selector");
}
void sys_reboot(int type) {
  assert(type==SYS_REBOOT_COLD&&test_power.gpregret[0]==0x57&&test_power.gpregret[1]==0x34);
  assert((order==std::vector<std::string>{"stop","flush","selector"}));
  order.push_back("reset");throw Reset{};
}
static void stop(){order.push_back("stop");}
static void flush(){order.push_back("flush");}
static bool poll(uint64_t now){return mt::usbBootloader::poll(&usb,now,stop,flush);}
int main(int argc,char **argv) {
  assert(argc==2);const std::string mode=argv[1];
#if defined(CONFIG_BOARD_HELTEC_T096)
  if(mode=="ordinary") {
    for(uint32_t value:{0u,300u,1199u,1201u,9600u,115200u,1000000u}) {baud=value;assert(!poll(1000));}
    assert(order.empty()&&test_power.gpregret[0]==0x12);
  } else if(mode=="read_error") {
    baud=1200;getResult=-5;assert(!poll(1000));assert(!poll(3000));assert(order.empty());
  } else {
    assert(mode=="touch"||mode=="error_then_touch"||mode=="latched"||mode=="wrap"||mode=="long_uptime");
    const uint64_t start=mode=="wrap"?UINT64_MAX-50:mode=="long_uptime"?(uint64_t(1)<<32)+300:1000;
    baud=1200;
    const unsigned expectedReads=mode=="error_then_touch"?2:1;
    if(mode=="error_then_touch"){getResult=-5;assert(!poll(start-1));assert(order.empty());getResult=0;}
    assert(poll(start));assert((order==std::vector<std::string>{"stop"}));
    // Closing CDC does not clear its line coding. A new host changing the baud
    // after observation must not cancel the already latched bootloader request.
    if(mode=="latched"){baud=115200;getResult=-1;}
    assert(poll(start));assert(poll(start+99));assert(reads==expectedReads);
    assert(test_power.gpregret[0]==0x12&&test_power.gpregret[1]==0x34);
    bool reset=false;try{(void)poll(start+100);}catch(const Reset &){reset=true;}
    assert(reset&&order.size()==4&&reads==expectedReads);
  }
#else
  assert(mode=="non_t096");baud=1200;
  for(uint64_t now:{0u,100u,1000u})assert(!poll(now));
  assert(reads==0&&order.empty()&&test_power.gpregret[0]==0x12&&test_power.gpregret[1]==0x34);
#endif
  std::printf("PASS %s\n",mode.c_str());
}
