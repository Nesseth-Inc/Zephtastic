// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrNotification.h"
#include "../stock/peripherals/StatusLEDModule.h"
#include "HeartbeatController.h"
#include "led_gate.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
namespace mt::notification {
#if defined(CONFIG_BOARD_HELTEC_T096) && DT_NODE_HAS_STATUS(DT_ALIAS(led0),okay)
static const gpio_dt_spec led=GPIO_DT_SPEC_GET(DT_ALIAS(led0),gpios);
BUILD_ASSERT(DT_GPIO_FLAGS(DT_ALIAS(led0),gpios)==GPIO_ACTIVE_HIGH);
BUILD_ASSERT(DT_GPIO_PIN(DT_ALIAS(led0),gpios)==28);
static bool ready=false;
static bool last=false;
static HeartbeatController heartbeatController;
static stock::StatusLedRadio rxIndicator;
void radioReceived(uint32_t now){
  const auto mode=zephcore_leds_radio_mode();
  if(ready&&!zephcore_leds_disabled()&&(mode==LEDS_RADIO_RX||mode==LEDS_RADIO_ALL))
    rxIndicator.handleLoRaRx(now);
}
void init(){ready=gpio_is_ready_dt(&led)&&gpio_pin_configure_dt(&led,GPIO_OUTPUT_INACTIVE)==0;last=false;heartbeatController=HeartbeatController{};rxIndicator={};}
bool available(){return ready;}
uint32_t pin(){return led.pin;}
void render(NotificationController::Output output,bool heartbeatEnabled,uint32_t now){
  if(!ready)return;
  // Gate-off always means the DT-declared inactive level. No configurable GPIO
  // reaches this driver; active affects only the explicitly enabled LED mode.
  bool level=false;
  heartbeatController.configure(heartbeatEnabled&&!zephcore_leds_disabled(),now);
  // There is no durable unread/read model in this composition. Reuse donor
  // liveness pulses without treating message-history length as unread state.
  heartbeatController.poll(static_cast<heartbeat::Mode>(zephcore_leds_hb_mode()),false,now);
  const auto mode=zephcore_leds_radio_mode();
  const bool received=rxIndicator.runOnce(now)&&(mode==LEDS_RADIO_RX||mode==LEDS_RADIO_ALL);
  if(!zephcore_leds_disabled()){
    if(received||(zephcore_led_radio_holds_pin()&&mode!=LEDS_RADIO_OFF))level=true;
    else if(output.enabled)level=output.active?output.on:!output.on;
    else level=heartbeatController.light();
  }
  if(level!=last){if(gpio_pin_set_dt(&led,level)==0)last=level;else ready=false;}
}
#else
void radioReceived(uint32_t){}
void init(){}
bool available(){return false;}
uint32_t pin(){return 0;}
void render(NotificationController::Output,bool,uint32_t){}
#endif
void radioActive(bool active){zephcore_led_radio_hold_pin(active);}
}
