// SPDX-License-Identifier: GPL-3.0-only
#include "AudioCodec.h"
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
extern "C" {
#include <codec2.h>
}
#endif
namespace mt::audio {
constexpr uint8_t Codec2FrameCodec::Magic[3];
Codec2FrameCodec::Codec2FrameCodec()=default;
Codec2FrameCodec::~Codec2FrameCodec(){close();}
bool Codec2FrameCodec::open(uint8_t mode){
  close();
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  if(!modeAvailable(mode))return false;
  const int codecMode=mode==9?10:mode; // stock AudioModule wire mode 9 identifies Codec2 450.
  auto *c=codec2_create(codecMode);if(!c)return false;
  codec2_set_lpc_post_filter(c,1,0,0.8,0.2);
  const int bits=codec2_bits_per_frame(c),samples=codec2_samples_per_frame(c);
  if(bits<=0||samples<=0||((bits+7)/8)>MaxPayload-HeaderSize){codec2_destroy(c);return false;}
  codec_=c;mode_=mode;samples_=size_t(samples);bytes_=size_t((bits+7)/8);return true;
#else
  (void)mode;return false;
#endif
}
bool Codec2FrameCodec::modeAvailable(uint8_t mode){
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  return mode<=5||mode==8||mode==9;
#else
  (void)mode;return false;
#endif
}
void Codec2FrameCodec::close(){
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  if(codec_)codec2_destroy(static_cast<struct CODEC2*>(codec_));
#endif
  codec_=nullptr;samples_=bytes_=0;mode_=0;
}
bool Codec2FrameCodec::ready()const{return codec_!=nullptr;}
size_t Codec2FrameCodec::samplesPerFrame()const{return samples_;}
size_t Codec2FrameCodec::bytesPerFrame()const{return bytes_;}
uint8_t Codec2FrameCodec::wireMode()const{return mode_;}
bool Codec2FrameCodec::encode(const int16_t *in,uint8_t *out,size_t cap,size_t &written){
  written=0;if(!ready()||!in||!out||cap<bytes_)return false;
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  codec2_encode(static_cast<struct CODEC2*>(codec_),out,const_cast<int16_t*>(in));written=bytes_;return true;
#else
  return false;
#endif
}
bool Codec2FrameCodec::decode(const uint8_t *in,size_t length,int16_t *out,size_t cap,size_t &written){
  written=0;if(!ready()||!in||!out||length!=bytes_||cap<samples_)return false;
#if defined(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  codec2_decode(static_cast<struct CODEC2*>(codec_),out,const_cast<uint8_t*>(in));written=samples_;return true;
#else
  return false;
#endif
}
}
