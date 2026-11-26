// SPDX-License-Identifier: GPL-3.0-only
#include "UsbBootloader.h"
#if defined(CONFIG_BOARD_HELTEC_T096)
#include "RebootSchedule.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>

namespace mt::usbBootloader {
static RebootSchedule schedule;
static bool requested=false;
// Adafruit nRF52 UF2 selector, also used by the T096 donor board adapter.
// Bootloader acceptance still requires verification on the installed loader.
static constexpr uint8_t Uf2Selector=0x57;
bool poll(const device *usb,uint64_t now,void (*stop)(),void (*beforeReset)()) {
  if(!requested) {
    uint32_t baud=0;
    const int result=uart_line_ctrl_get(usb,UART_LINE_CTRL_BAUD_RATE,&baud);
    if(result!=0||baud!=1200)return false;
    requested=true;
    schedule.request(0,now); // Bounded100ms grace for the control transfer/close.
    stop(); // Prevent pending RF/client jobs during the grace period.
  }
  if(schedule.takeDue(now)) {
    beforeReset(); // Best effort cache flush; caller checks storage quiescence.
    nrf_power_gpregret_set(NRF_POWER,0,Uf2Selector);
    sys_reboot(SYS_REBOOT_COLD);
  }
  return true;
}
}
#else
namespace mt::usbBootloader {
bool poll(const device *,uint64_t,void (*)(),void (*)()) {return false;}
}
#endif
