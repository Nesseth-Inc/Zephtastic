// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SerialEngine.h"
#include "PositionOutput.h"
#include "ProtoEngine.h"
#include "Ws85Engine.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#if DT_HAS_ALIAS(mt_serial)
#include <zephyr/drivers/uart.h>
#define MT_SERIAL_HAS_UART 1
#else
#define MT_SERIAL_HAS_UART 0
#endif

namespace mt_serial {

// Optional dedicated UART. `available` means the build has an alias and the
// device is ready; `ready` additionally requires enabled module configuration.
class ZephyrSerial {
 public:
  ZephyrSerial() = default;
  int configure(const Config &config, bool enabled, uint32_t baud);
  int configure(const Config &config, bool enabled, uint32_t baud,
                uint32_t rxPin, uint32_t txPin);
  bool available() const;
  bool configSupported(const Config &config, uint32_t baud,
                       uint32_t rxPin, uint32_t txPin) const;
  bool ready() const { return enabled_ && available(); }
  bool poll(uint32_t now_ms, Send send, void *send_context);
  bool pollProto(ToRadio toRadio, FromRadio fromRadio, void *phoneApiContext);
  bool emitProto(const uint8_t *protobuf, size_t size);
  bool pollNmea(uint32_t now_ms, const Position &localPosition);
  size_t pollCalTopo(uint32_t now_ms, const Waypoint *waypoints, size_t count);
  bool meshPosition(const Position &position, const char *sourceName);
  bool pollWs85(uint32_t now_ms, WeatherEmit emit, void *context);
  void mesh_receive(bool from_us, uint32_t id, const char *name,
                    const uint8_t *bytes, size_t size);
 private:
  Engine engine_{};
  ProtoEngine proto_{};
  PositionOutput positionOutput_{};
  Ws85Engine ws85_{};
  Config config_{};
  bool configured_ = false;
  bool enabled_ = false;
  uint32_t baud_ = 38400;
  uint32_t rxPin_=0, txPin_=0;
#if MT_SERIAL_HAS_UART && defined(CONFIG_UART_USE_RUNTIME_CONFIGURE) && CONFIG_UART_USE_RUNTIME_CONFIGURE
  bool uart_configured_ = false;
#endif
};

}  // namespace mt_serial
