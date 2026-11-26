// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace mt::audio {
struct AudioConfig;
struct I2sCapabilities { bool i2sCapture=false,i2sPlayback=false,ptt=false; };

// A narrow adapter boundary: compile-time I2S presence is not reported as an
// available microphone/speaker until DT devices are ready and configured.
class ZephyrI2sAudio {
public:
  ZephyrI2sAudio();
  ~ZephyrI2sAudio();
  ZephyrI2sAudio(const ZephyrI2sAudio&)=delete;
  ZephyrI2sAudio& operator=(const ZephyrI2sAudio&)=delete;
  bool configure(const AudioConfig &config,size_t frameSamples);
  bool configurePlayback(const AudioConfig &config,size_t frameSamples=160);
  I2sCapabilities capabilities()const;
  static I2sCapabilities availableCapabilities(const AudioConfig &config);
  static I2sCapabilities availablePlaybackCapabilities();
  bool capture(int16_t *samples,size_t count,uint32_t timeoutMs);
  bool playback(const int16_t *samples,size_t count,uint32_t timeoutMs);
  bool playTone(uint16_t frequencyHz,uint8_t volume=32);
  bool pttPressed()const;
  void stop();
private:
  bool captureReady_=false,playbackReady_=false,pttReady_=false;size_t frameSamples_=0;
};
}
