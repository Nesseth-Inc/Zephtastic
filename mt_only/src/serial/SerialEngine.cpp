// SPDX-License-Identifier: GPL-3.0-only
#include "SerialEngine.h"

#include "../stock/peripherals/SerialModule.h"

namespace mt_serial {
uint32_t baudRateFromStockEnum(uint32_t baudEnum) {
  static constexpr uint32_t rates[]={38400,110,300,600,1200,2400,4800,9600,
                                     19200,38400,57600,115200,230400,460800,
                                     576000,921600};
  return baudEnum<sizeof(rates)/sizeof(rates[0]) ? rates[baudEnum] : 0;
}
Engine::Engine(Config config) : config_(config) {}

uint32_t Engine::timeout_ms() const { return config_.timeout_ms ? config_.timeout_ms : 250; }
Port Engine::port() const { return config_.mode == Mode::TextMessage ? Port::TextMessageApp : Port::SerialApp; }

bool Engine::flush(Send send, void *context) {
  if (!used_) return true;
  if (config_.mode != Mode::Simple && config_.mode != Mode::TextMessage) {
    used_ = 0;
    have_byte_ = false;
    return true; // PROTO/NMEA/CalTopo are output/API modes, never text-to-mesh input.
  }
  const bool ok = send && send(context, port(), buffer_, used_,
                               config_.mode == Mode::Simple ? kChannelNamedSerial : kChannelPrimary,
                               true);
  used_ = 0;
  have_byte_ = false;
  return ok;
}

bool Engine::poll(uint32_t now_ms, Send send, void *context) {
  if (used_ && have_byte_ && static_cast<uint32_t>(now_ms - last_byte_ms_) >= timeout_ms())
    return flush(send, context);
  return true;
}

bool Engine::receive(const uint8_t *bytes, size_t size, uint32_t now_ms,
                     Send send, void *context) {
  if (size && !bytes) return false;
  if (config_.mode != Mode::Simple && config_.mode != Mode::TextMessage) return true;
  if (!poll(now_ms, send, context)) return false;
  for (size_t i = 0; i < size; ++i) {
    buffer_[used_++] = bytes[i];
    last_byte_ms_ = now_ms;
    have_byte_ = true;
    if (used_ == sizeof(buffer_) && !flush(send, context)) return false;
  }
  return true;
}

void Engine::mesh_receive(bool from_us, uint32_t packet_id, const char *short_name,
                          const uint8_t *payload, size_t size, Write write,
                          void *context) {
  if (!write || (size && !payload)) return;
  mt::stock::serial::handleReceived(config_.mode, config_.echo, last_echo_id_, from_us,
                                   packet_id, short_name, payload, size, write, context);
}
}  // namespace mt_serial
