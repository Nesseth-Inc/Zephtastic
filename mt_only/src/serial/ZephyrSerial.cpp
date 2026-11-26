// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrSerial.h"

#include <errno.h>

#if MT_SERIAL_HAS_UART
#include <zephyr/kernel.h>

#define MT_SERIAL_NODE DT_ALIAS(mt_serial)
#define MT_SERIAL_RX_PIN DT_PROP_OR(MT_SERIAL_NODE, zephcore_rx_pin, 0)
#define MT_SERIAL_TX_PIN DT_PROP_OR(MT_SERIAL_NODE, zephcore_tx_pin, 0)
#if DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_console), okay)
BUILD_ASSERT(!DT_SAME_NODE(MT_SERIAL_NODE, DT_CHOSEN(zephyr_console)),
             "mt-serial UART must not be the Zephyr console");
#endif
#if DT_HAS_CHOSEN(zephyr_gnss)
BUILD_ASSERT(!DT_SAME_NODE(MT_SERIAL_NODE, DT_CHOSEN(zephyr_gnss)),
             "mt-serial UART must not be the GNSS device");
#endif
#if defined(CONFIG_MT_GNSS_T096) || defined(CONFIG_MT_GNSS_XIAO_L76K)
BUILD_ASSERT(!DT_SAME_NODE(MT_SERIAL_NODE, DT_NODELABEL(uart0)),
             "mt-serial UART must not be the GNSS UART");
#endif
#if !defined(CONFIG_UART_USE_RUNTIME_CONFIGURE) || !CONFIG_UART_USE_RUNTIME_CONFIGURE
BUILD_ASSERT(DT_NODE_HAS_PROP(MT_SERIAL_NODE, current_speed),
             "mt-serial UART must declare its fixed current-speed");
#endif

namespace mt_serial {
namespace {
const struct device *uart_device() { return DEVICE_DT_GET(MT_SERIAL_NODE); }
void uart_write(void *context, const uint8_t *bytes, size_t size) {
  const auto *dev = static_cast<const struct device *>(context);
  for (size_t i = 0; i < size; ++i) uart_poll_out(dev, bytes[i]);
}
}

int ZephyrSerial::configure(const Config &config, bool enabled, uint32_t baud) {
  return configure(config,enabled,baud,0,0);
}

int ZephyrSerial::configure(const Config &config, bool enabled, uint32_t baud,
                            uint32_t rxPin, uint32_t txPin) {
  const auto *dev = uart_device();
  if (!device_is_ready(dev)) return -ENODEV;
  if(!configSupported(config,baud,rxPin,txPin)) return -ENOTSUP;
  // Keep untouched settings writes from disturbing pending bytes or UART state.
  if (configured_ && config_.mode == config.mode && config_.timeout_ms == config.timeout_ms &&
      config_.echo == config.echo && enabled_ == enabled && baud_ == baud &&
      rxPin_==rxPin && txPin_==txPin)
    return 0;
  if (enabled) {
#if defined(CONFIG_UART_USE_RUNTIME_CONFIGURE) && CONFIG_UART_USE_RUNTIME_CONFIGURE
    if (!uart_configured_ || baud_ != baud) {
      struct uart_config uart_cfg{};
      uart_cfg.baudrate = baud;
      uart_cfg.parity = UART_CFG_PARITY_NONE;
      uart_cfg.stop_bits = UART_CFG_STOP_BITS_1;
      uart_cfg.data_bits = UART_CFG_DATA_BITS_8;
      uart_cfg.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
      const int err = uart_configure(dev, &uart_cfg);
      if (err) return err;
      uart_configured_ = true;
    }
#else
    if (baud != DT_PROP(MT_SERIAL_NODE, current_speed)) return -ENOTSUP;
#endif
  }
  // Reset parser/cadence state only for an actual serial behavior or enable change.
  const bool behaviorChanged=!configured_ || config_.mode != config.mode ||
      config_.timeout_ms != config.timeout_ms || config_.echo != config.echo ||
      enabled_!=enabled;
  if (behaviorChanged) {
    engine_ = Engine(config);
    proto_.reset();
    positionOutput_=PositionOutput{};
  }
  config_ = config;
  enabled_ = enabled;
  baud_ = baud;
  rxPin_=rxPin; txPin_=txPin;
  configured_ = true;
  return 0;
}

bool ZephyrSerial::available() const {
  const auto *dev = uart_device();
  return device_is_ready(dev);
}

bool ZephyrSerial::configSupported(const Config &config,uint32_t baud,
                                   uint32_t rxPin,uint32_t txPin) const {
  if(!available() || !baud || ((rxPin==0)!=(txPin==0))) return false;
  switch(config.mode) {
  case Mode::Simple: case Mode::TextMessage: case Mode::Proto:
  case Mode::Nmea: case Mode::CalTopo: case Mode::Ws85: break;
  default: return false;
  }
  if(rxPin && (rxPin!=MT_SERIAL_RX_PIN || txPin!=MT_SERIAL_TX_PIN)) return false;
#if !DT_NODE_HAS_PROP(MT_SERIAL_NODE, zephcore_rx_pin) || !DT_NODE_HAS_PROP(MT_SERIAL_NODE, zephcore_tx_pin)
  if(rxPin) return false;
#endif
#if !defined(CONFIG_UART_USE_RUNTIME_CONFIGURE) || !CONFIG_UART_USE_RUNTIME_CONFIGURE
  if(baud!=DT_PROP(MT_SERIAL_NODE,current_speed)) return false;
#endif
  return true;
}

bool ZephyrSerial::poll(uint32_t now_ms, Send send, void *send_context) {
  if (!ready()) return true;
  if(config_.mode!=Mode::Simple && config_.mode!=Mode::TextMessage) return true;
  const auto *dev = uart_device();
  if (!device_is_ready(dev)) return false;
  uint8_t chunk[kMaxPayload];
  size_t count = 0;
  unsigned char byte = 0;
  while (count < sizeof(chunk) && uart_poll_in(dev, &byte) == 0)
    chunk[count++] = static_cast<uint8_t>(byte);
  if (count && !engine_.receive(chunk, count, now_ms, send, send_context)) return false;
  if (!engine_.poll(now_ms, send, send_context)) return false;
  return true;
}

bool ZephyrSerial::pollProto(ToRadio toRadio, FromRadio fromRadio,
                             void *phoneApiContext) {
  if(!ready() || config_.mode!=Mode::Proto) return false;
  const auto *dev=uart_device();
  if(!device_is_ready(dev)) return false;
  uint8_t chunk[128];
  size_t count=0;
  unsigned char byte=0;
  while(count<sizeof(chunk) && uart_poll_in(dev,&byte)==0)
    chunk[count++]=static_cast<uint8_t>(byte);
  if(count && !proto_.receive(chunk,count,toRadio,phoneApiContext)) return false;
  return proto_.poll(fromRadio,phoneApiContext,uart_write,
                     const_cast<struct device *>(dev));
}

bool ZephyrSerial::emitProto(const uint8_t *protobuf, size_t size) {
  if(!ready() || config_.mode!=Mode::Proto) return false;
  const auto *dev=uart_device();
  if(!device_is_ready(dev)) return false;
  return ProtoEngine::emit(protobuf,size,uart_write,const_cast<struct device *>(dev));
}

bool ZephyrSerial::pollNmea(uint32_t nowMs, const Position &localPosition) {
  if(!ready() || config_.mode!=Mode::Nmea) return false;
  const auto *dev=uart_device();
  if(!device_is_ready(dev)) return false;
  return positionOutput_.pollNmea(nowMs,localPosition,uart_write,
                                  const_cast<struct device *>(dev));
}

size_t ZephyrSerial::pollCalTopo(uint32_t nowMs, const Waypoint *waypoints,
                                 size_t count) {
  if(!ready() || config_.mode!=Mode::CalTopo) return 0;
  const auto *dev=uart_device();
  if(!device_is_ready(dev)) return 0;
  return positionOutput_.pollCalTopo(nowMs,waypoints,count,uart_write,
                                     const_cast<struct device *>(dev));
}

bool ZephyrSerial::meshPosition(const Position &position,const char *sourceName) {
  if(!ready() || (config_.mode!=Mode::Nmea && config_.mode!=Mode::CalTopo)) return false;
  const auto *dev=uart_device();
  if(!device_is_ready(dev)) return false;
  return positionOutput_.meshWaypoint(position,sourceName,config_.mode,uart_write,
                                      const_cast<struct device *>(dev));
}

bool ZephyrSerial::pollWs85(uint32_t nowMs,WeatherEmit emit,void *context) {
  if(!ready() || config_.mode!=Mode::Ws85) return false;
  const auto *dev=uart_device();
  if(!device_is_ready(dev)) return false;
  uint8_t chunk[128];
  size_t count=0;
  unsigned char byte=0;
  while(count<sizeof(chunk) && uart_poll_in(dev,&byte)==0)
    chunk[count++]=static_cast<uint8_t>(byte);
  if(count && !ws85_.feed(chunk,count)) return false;
  return ws85_.poll(nowMs,emit,context);
}

void ZephyrSerial::mesh_receive(bool from_us, uint32_t id, const char *name,
                               const uint8_t *bytes, size_t size) {
  if (!ready() || config_.mode==Mode::Proto) return;
  if(!from_us && config_.mode!=Mode::Simple && config_.mode!=Mode::TextMessage) return;
  const auto *dev = uart_device();
  if (!device_is_ready(dev)) return;
  engine_.mesh_receive(from_us, id, name, bytes, size, uart_write,
                       const_cast<struct device *>(dev));
}
}  // namespace mt_serial
#else
namespace mt_serial {
int ZephyrSerial::configure(const Config &config, bool enabled, uint32_t baud) {
  return configure(config,enabled,baud,0,0);
}
int ZephyrSerial::configure(const Config &config, bool enabled, uint32_t baud,
                            uint32_t rxPin, uint32_t txPin) {
  config_ = config;
  baud_ = baud;
  rxPin_=rxPin; txPin_=txPin;
  configured_ = true;
  enabled_ = enabled;
  return enabled ? -ENODEV : 0;
}
bool ZephyrSerial::available() const { return false; }
bool ZephyrSerial::configSupported(const Config &,uint32_t,uint32_t,uint32_t) const { return false; }
bool ZephyrSerial::poll(uint32_t, Send, void *) { return true; }
bool ZephyrSerial::pollProto(ToRadio, FromRadio, void *) { return false; }
bool ZephyrSerial::emitProto(const uint8_t *, size_t) { return false; }
bool ZephyrSerial::pollNmea(uint32_t,const Position &) { return false; }
size_t ZephyrSerial::pollCalTopo(uint32_t,const Waypoint *,size_t) { return 0; }
bool ZephyrSerial::meshPosition(const Position &,const char *) { return false; }
bool ZephyrSerial::pollWs85(uint32_t,WeatherEmit,void *) { return false; }
void ZephyrSerial::mesh_receive(bool, uint32_t, const char *, const uint8_t *, size_t) {}
}  // namespace mt_serial
#endif
