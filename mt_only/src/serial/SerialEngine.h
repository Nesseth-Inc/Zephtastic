// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mt_serial {

// Stock Meshtastic's DATA_PAYLOAD_LEN is 233 bytes. Keep this extraction
// bounded independently of the larger historical Arduino read buffer.
constexpr size_t kMaxPayload = 233;
constexpr int kChannelNamedSerial = -2;  // caller resolves channel whose name is "serial"
constexpr int kChannelPrimary = 0;
enum class Mode : uint8_t { Simple, TextMessage, Proto, Nmea, CalTopo, Ws85 };
// Stock SerialConfig baud enum ordinal -> physical line rate.
uint32_t baudRateFromStockEnum(uint32_t baudEnum);
enum class Port : uint8_t { SerialApp, TextMessageApp };

struct Config {
  Mode mode = Mode::Simple;
  uint32_t timeout_ms = 0;  // zero selects stock's 250 ms default
  bool echo = false;
};
struct Frame { uint8_t bytes[kMaxPayload]; size_t size; };
// Stock sets want_ack=1 for both modes. Named-serial is resolved by the caller;
// TEXTMSG uses ordinary channel zero, matching SinglePortModule's default.
using Send = bool (*)(void *, Port, const uint8_t *, size_t, int channel, bool want_ack);
using Write = void (*)(void *, const uint8_t *, size_t);

// The caller supplies time from a monotonic clock. Each receive call is the
// bytes available during one UART service pass; timeout is inter-byte.
class Engine {
 public:
  explicit Engine(Config config = {});
  bool receive(const uint8_t *bytes, size_t size, uint32_t now_ms,
               Send send, void *context);
  bool poll(uint32_t now_ms, Send send, void *context);
  void mesh_receive(bool from_us, uint32_t packet_id, const char *short_name,
                    const uint8_t *payload, size_t size, Write write,
                    void *context);
  size_t pending() const { return used_; }
  uint32_t timeout_ms() const;
  Port port() const;

 private:
  bool flush(Send send, void *context);
  Config config_;
  uint8_t buffer_[kMaxPayload]{};
  size_t used_ = 0;
  uint32_t last_byte_ms_ = 0;
  bool have_byte_ = false;
  uint32_t last_echo_id_ = 0;
};

}  // namespace mt_serial
