// SPDX-License-Identifier: GPL-3.0-only
#include "SerialEngine.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

using namespace mt_serial;
struct Capture { std::vector<Frame> frames; Port port{}; int channel = 0; bool ack = false; std::string output; };
static bool send(void *p, Port port, const uint8_t *b, size_t n, int ch, bool ack) {
  auto &c = *static_cast<Capture *>(p); Frame f{}; f.size = n; std::memcpy(f.bytes, b, n);
  c.frames.push_back(f); c.port = port; c.channel = ch; c.ack = ack; return true;
}
static void write(void *p, const uint8_t *b, size_t n) { static_cast<Capture *>(p)->output.append(reinterpret_cast<const char *>(b), n); }

int main() {
  Capture c;
  Engine simple({Mode::Simple, 0, true});
  const uint8_t a[] = {'a','b'}; const uint8_t b[] = {'c'};
  assert(simple.receive(a, sizeof(a), 10, send, &c));
  assert(simple.receive(b, sizeof(b), 200, send, &c));
  assert(c.frames.empty());
  assert(simple.poll(449, send, &c) && c.frames.empty());
  assert(simple.poll(450, send, &c) && c.frames.size() == 1);
  assert(c.frames[0].size == 3 && std::memcmp(c.frames[0].bytes, "abc", 3) == 0);
  assert(c.port == Port::SerialApp && c.channel == kChannelNamedSerial && c.ack);

  Engine text({Mode::TextMessage, 40, false});
  assert(text.receive(a, sizeof(a), 1000, send, &c));
  assert(text.poll(1039, send, &c) && c.frames.size() == 1);
  assert(text.poll(1040, send, &c) && c.frames.size() == 2);
  assert(c.port == Port::TextMessageApp && c.channel == kChannelPrimary && c.ack);

  Engine simple_rx({Mode::Simple, 0, true});
  simple_rx.mesh_receive(false, 7, "XY", a, sizeof(a), write, &c);
  assert(c.output == "ab");
  c.output.clear();
  simple_rx.mesh_receive(true, 8, "XY", a, sizeof(a), write, &c);
  simple_rx.mesh_receive(true, 8, "XY", a, sizeof(a), write, &c);
  assert(c.output == "ab");
  c.output.clear();
  Engine text_rx({Mode::TextMessage, 0, false});
  text_rx.mesh_receive(false, 9, "XY", a, sizeof(a), write, &c);
  assert(c.output == "\r\nXY: ab\r\n");
  c.output.clear();
  text_rx.mesh_receive(false, 9, nullptr, a, sizeof(a), write, &c);
  assert(c.output == "\r\n???: ab\r\n");
  const uint8_t binary[]={'a',0,'b'};
  c.output.clear();
  simple_rx.mesh_receive(false,10,"XY",binary,sizeof(binary),write,&c);
  assert(c.output==std::string("a\0b",3));
  c.output.clear();
  simple_rx.mesh_receive(true,11,"XY",binary,sizeof(binary),write,&c);
  assert(c.output=="a");
  c.output.clear();
  text_rx.mesh_receive(false,12,"XY",binary,sizeof(binary),write,&c);
  assert(c.output=="\r\nXY: a\r\n");

  uint8_t full[kMaxPayload]{};
  Capture large;
  assert(simple.receive(full, sizeof(full), 2000, send, &large));
  assert(large.frames.size() == 1 && large.frames[0].size == kMaxPayload);
  return 0;
}
