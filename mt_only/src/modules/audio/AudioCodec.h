// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace mt::audio {
// The packet wire contract follows pinned Meshtastic AudioModule: C0 DE C2,
// then one Codec2 mode byte, then whole codec frames; voice is AUDIO_APP.
struct Codec2FrameCodec {
  static constexpr uint8_t Magic[3]={0xc0,0xde,0xc2};
  static constexpr size_t HeaderSize=4, MaxPayload=233;
  Codec2FrameCodec();
  ~Codec2FrameCodec();
  Codec2FrameCodec(const Codec2FrameCodec&)=delete;
  Codec2FrameCodec& operator=(const Codec2FrameCodec&)=delete;
  bool open(uint8_t wireMode);
  static bool modeAvailable(uint8_t wireMode);
  void close();
  bool ready()const;
  size_t samplesPerFrame()const;
  size_t bytesPerFrame()const;
  uint8_t wireMode()const;
  bool encode(const int16_t *samples,uint8_t *out,size_t capacity,size_t &written);
  bool decode(const uint8_t *frame,size_t length,int16_t *samples,size_t capacity,size_t &written);
private:
  void *codec_=nullptr; uint8_t mode_=0; size_t samples_=0,bytes_=0;
};
}
