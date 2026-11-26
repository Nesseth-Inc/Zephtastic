// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/esp32/AudioModule.cpp
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::stock::audio {
// The Zephyr capture/playback adapters own hardware transitions. A full RF
// queue can defer release until the last partial Codec2 packet is accepted.
template<class Begin, class End>
bool pollPtt(bool pressed, bool transmitting, Begin begin, End end)
{
  if (pressed && !transmitting) return begin();
  if (!pressed && transmitting) return end();
  return true;
}

struct Frames {
  static constexpr size_t HeaderSize=4,MaxPayload=233;
  static constexpr uint8_t Magic[3]={0xc0,0xde,0xc2};
  // AudioModule's packet sizing and run_codec2 receive loop, adapted from
  // Meshtastic 8e6a88d0, src/modules/esp32/AudioModule.cpp. The callback
  // owns the Codec2 decoder and the platform's bounded PCM output.
  static constexpr size_t packetSize(size_t frameSize) {
    if (!frameSize || frameSize > MaxPayload - HeaderSize) return 0;
    const size_t encodeFrameNum = (MaxPayload - HeaderSize) / frameSize;
    return HeaderSize + encodeFrameNum * frameSize;
  }

  template<typename Decode>
  static bool decodeFrames(const uint8_t *input, size_t length, uint8_t mode,
                           size_t frameSize, Decode decode) {
    const uint8_t header[] = {Magic[0], Magic[1], Magic[2], mode};
    if (!input || !frameSize || length < sizeof(header) || length > MaxPayload ||
        memcmp(input, header, sizeof(header)) != 0) return false;
    for (size_t offset = sizeof(header); frameSize <= length - offset; offset += frameSize) {
      if (!decode(input + offset, frameSize)) return false;
    }
    return true;
  }

};
}
