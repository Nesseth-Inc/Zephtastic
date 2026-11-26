// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include "meshtastic/module_config.pb.h"

namespace mt::input {
// Owner-loop source for stock CannedMessageConfig rotary1/updown1 inputs.
// GPIO callbacks only enqueue captured edges; poll() owns all gesture state
// and invokes the event sink from caller context.
class CannedGpioInput {
public:
  enum class Result : uint8_t { Ok, Disabled, InvalidConfig, HardwareAbsent, HardwareError };
  struct Event { uint8_t code=0; const char *source=nullptr; uint32_t uptimeMs=0; };
  using EventSink=void(*)(void *context,const Event &event);
private:
  enum Pin : uint8_t { A=0, B=1, Press=2 };
  struct Edge { uint32_t at=0; uint8_t pin=0,raw=0; };
  struct PinSlot {
    CannedGpioInput *owner=nullptr;
    gpio_dt_spec spec{};
    gpio_callback callback{};
    uint8_t index=0;
    bool registered=false;
  };
  static constexpr size_t QueueDepth=24;
  alignas(4) char queueStorage_[sizeof(Edge)*QueueDepth]{};
  struct k_msgq queue_{};
  PinSlot pins_[3]{};
  meshtastic_ModuleConfig_CannedMessageConfig config_{};
  uint8_t raw_[3]{};
  bool configured_=false,enabledRotary_=false,enabledUpDown_=false;
  bool rotaryCwSet_=false,rotaryCcwSet_=false;
  bool down_[3]{}; // up, down, select active state
  uint32_t downAt_[3]{},lastShortAt_[3]{},lastLongAt_[3]{};
  bool longSent_[3]{};
  atomic_t overflow_=0;
  atomic_t dropped_=0;

  static void gpioHandler(const device *port,gpio_callback *callback,uint32_t pins);
  static bool validEvent(const meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar &value);
  static bool validBool(const bool &value);
  static bool validSource(const char *value,size_t capacity);
  static bool pinsAvailable();
  static gpio_dt_spec specFor(unsigned pin);
  bool pinMatches(unsigned pin,uint32_t configuredPin)const;
  Result initPins(const meshtastic_ModuleConfig_CannedMessageConfig &config);
  void stopPins();
  bool queueEdge(const Edge &edge);
  void emit(EventSink sink,void *context,uint8_t code,const char *source,uint32_t at);
  void process(const Edge &edge,EventSink sink,void *context);
  void tick(uint32_t now,EventSink sink,void *context);
public:
  CannedGpioInput();
  ~CannedGpioInput();
  CannedGpioInput(const CannedGpioInput &)=delete;
  CannedGpioInput &operator=(const CannedGpioInput &)=delete;
  Result configure(const meshtastic_ModuleConfig_CannedMessageConfig &config);
  // Pure capability checks for owner config/UI admission. These do not claim
  // GPIO ownership, configure pins, or register interrupt callbacks.
  static bool hardwareAvailable();
  bool configSupported(const meshtastic_ModuleConfig_CannedMessageConfig &config)const;
  void disable();
  bool ready()const{return configured_;}
  bool available()const{return configured_&&(enabledRotary_||enabledUpDown_);}
  // Bounded owner-loop drain; no application/UI work runs in GPIO ISR context.
  size_t poll(uint32_t now,EventSink sink,void *context,size_t maxEvents=16);
  uint32_t droppedEdges()const{return static_cast<uint32_t>(atomic_get(&dropped_));}
};
} // namespace mt::input
