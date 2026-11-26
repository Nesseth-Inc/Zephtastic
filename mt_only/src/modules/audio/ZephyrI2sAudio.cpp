// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrI2sAudio.h"
#include "AudioConfig.h"

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#if defined(CONFIG_I2S) && DT_NODE_EXISTS(DT_NODELABEL(audio_i2s))
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <string.h>
#define MT_HAS_AUDIO_I2S_NODE 1
#else
#define MT_HAS_AUDIO_I2S_NODE 0
#endif
#if MT_HAS_AUDIO_I2S_NODE
K_MEM_SLAB_DEFINE(mtAudioRxSlab,640,4,4);
K_MEM_SLAB_DEFINE(mtAudioTxSlab,640,4,4);
#endif

namespace mt::audio {
#if DT_NODE_EXISTS(DT_ALIAS(mt_audio_ptt))
static const gpio_dt_spec pttSpec=GPIO_DT_SPEC_GET(DT_ALIAS(mt_audio_ptt),gpios);
#define MT_HAS_AUDIO_PTT 1
#else
#define MT_HAS_AUDIO_PTT 0
#endif
ZephyrI2sAudio::ZephyrI2sAudio()=default;
ZephyrI2sAudio::~ZephyrI2sAudio(){stop();}
I2sCapabilities ZephyrI2sAudio::availableCapabilities(const AudioConfig &cfg){
  I2sCapabilities caps{};
#if MT_HAS_AUDIO_PTT
  caps.ptt=(!cfg.pttPin||cfg.pttPin==pttSpec.pin)&&gpio_is_ready_dt(&pttSpec);
#endif
#if MT_HAS_AUDIO_I2S_NODE
  if(!cfg.ws&&!cfg.dataIn&&!cfg.dataOut&&!cfg.clock){const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));caps.i2sCapture=caps.i2sPlayback=device_is_ready(dev);}
#endif
  return caps;
}
I2sCapabilities ZephyrI2sAudio::availablePlaybackCapabilities(){
  I2sCapabilities caps{};
#if MT_HAS_AUDIO_I2S_NODE
  const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));caps.i2sPlayback=device_is_ready(dev);
#endif
  return caps;
}
bool ZephyrI2sAudio::configure(const AudioConfig &cfg,size_t frameSamples){
  stop();
#if MT_HAS_AUDIO_PTT
  if(availableCapabilities(cfg).ptt&&gpio_pin_configure_dt(&pttSpec,GPIO_INPUT)==0)pttReady_=true;
#endif
#if MT_HAS_AUDIO_I2S_NODE
  // Board DTS must explicitly provide this node and its pinctrl. User supplied
  // protobuf pin numbers are never muxed dynamically. On initial T096/XIAO
  // builds no such audio node exists, therefore no phantom capability appears.
  const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));
  if(!availableCapabilities(cfg).i2sCapture||!device_is_ready(dev)||frameSamples==0||frameSamples>320)return false;
  static struct i2s_config cfgI2s;
  cfgI2s={};cfgI2s.word_size=16;cfgI2s.channels=1;cfgI2s.format=I2S_FMT_DATA_FORMAT_I2S;
  cfgI2s.options=I2S_OPT_BIT_CLK_MASTER|I2S_OPT_FRAME_CLK_MASTER;
  cfgI2s.frame_clk_freq=8000;cfgI2s.mem_slab=&mtAudioRxSlab;cfgI2s.block_size=frameSamples*sizeof(int16_t);
  cfgI2s.timeout=40;
  if(i2s_configure(dev,I2S_DIR_RX,&cfgI2s)!=0)return false;
  cfgI2s.mem_slab=&mtAudioTxSlab;
  if(i2s_configure(dev,I2S_DIR_TX,&cfgI2s)!=0){i2s_trigger(dev,I2S_DIR_RX,I2S_TRIGGER_DROP);return false;}
  if(i2s_trigger(dev,I2S_DIR_RX,I2S_TRIGGER_START)!=0){i2s_trigger(dev,I2S_DIR_RX,I2S_TRIGGER_DROP);return false;}
  if(i2s_trigger(dev,I2S_DIR_TX,I2S_TRIGGER_START)!=0){i2s_trigger(dev,I2S_DIR_RX,I2S_TRIGGER_DROP);i2s_trigger(dev,I2S_DIR_TX,I2S_TRIGGER_DROP);return false;}
  captureReady_=true;playbackReady_=true;frameSamples_=frameSamples;return true;
#else
  (void)cfg;(void)frameSamples;return false;
#endif
}
bool ZephyrI2sAudio::configurePlayback(const AudioConfig &,size_t frameSamples){
  if(playbackReady_)return true;
  if(captureReady_&&frameSamples_)frameSamples=frameSamples_;
#if MT_HAS_AUDIO_I2S_NODE
  if(!frameSamples||frameSamples>320)return false;
  const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));if(!device_is_ready(dev))return false;
  struct i2s_config cfg{};cfg.word_size=16;cfg.channels=1;cfg.format=I2S_FMT_DATA_FORMAT_I2S;
  cfg.options=I2S_OPT_BIT_CLK_MASTER|I2S_OPT_FRAME_CLK_MASTER;cfg.frame_clk_freq=8000;cfg.mem_slab=&mtAudioTxSlab;
  cfg.block_size=frameSamples*sizeof(int16_t);cfg.timeout=40;
  if(i2s_configure(dev,I2S_DIR_TX,&cfg)!=0)return false;
  if(i2s_trigger(dev,I2S_DIR_TX,I2S_TRIGGER_START)!=0){i2s_trigger(dev,I2S_DIR_TX,I2S_TRIGGER_DROP);return false;}
  frameSamples_=frameSamples;playbackReady_=true;return true;
#else
  (void)frameSamples;return false;
#endif
}
I2sCapabilities ZephyrI2sAudio::capabilities()const{return {captureReady_,playbackReady_,pttReady_};}
bool ZephyrI2sAudio::capture(int16_t *out,size_t count,uint32_t timeout){
#if MT_HAS_AUDIO_I2S_NODE
  if(!captureReady_||!out||!count)return false;const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));void *block=nullptr;size_t size=0;
  if(i2s_read(dev,&block,&size)!=0||!block)return false;
  if(size<count*sizeof(int16_t)){i2s_buf_read_release(dev,block);return false;}
  memcpy(out,block,count*sizeof(int16_t));i2s_buf_read_release(dev,block);(void)timeout;return true;
#else
  (void)out;(void)count;(void)timeout;return false;
#endif
}
bool ZephyrI2sAudio::playback(const int16_t *in,size_t count,uint32_t timeout){
#if MT_HAS_AUDIO_I2S_NODE
  if(!playbackReady_||!in||!count||count>320)return false;const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));
  void *block=nullptr;const size_t bytes=count*sizeof(int16_t);
  if(k_mem_slab_alloc(&mtAudioTxSlab,&block,K_MSEC(timeout))!=0)return false;
  memcpy(block,in,bytes);
  if(i2s_write(dev,block,bytes)!=0){k_mem_slab_free(&mtAudioTxSlab,block);return false;}
  return true;
#else
  (void)in;(void)count;(void)timeout;return false;
#endif
}
bool ZephyrI2sAudio::playTone(uint16_t frequency,uint8_t volume){
  if(!playbackReady_)return false;
  int16_t samples[320]{};const size_t count=frameSamples_?frameSamples_:160;
  if(frequency){
    const uint32_t step=(uint32_t(frequency)*65536u)/8000u;
    uint32_t phase=0;const int16_t amplitude=int16_t((uint16_t(volume)*20000u)/100u);
    for(size_t i=0;i<count;++i){samples[i]=(phase&0x8000u)?amplitude:int16_t(-amplitude);phase+=step;}
  }
  return playback(samples,count,30);
}
bool ZephyrI2sAudio::pttPressed()const{
#if MT_HAS_AUDIO_PTT
  return pttReady_&&gpio_pin_get_dt(&pttSpec)>0;
#else
  return false;
#endif
}
void ZephyrI2sAudio::stop(){
#if MT_HAS_AUDIO_I2S_NODE
  if(captureReady_||playbackReady_){const auto *dev=DEVICE_DT_GET(DT_NODELABEL(audio_i2s));
    if(captureReady_)i2s_trigger(dev,I2S_DIR_RX,I2S_TRIGGER_DROP);
    if(playbackReady_)i2s_trigger(dev,I2S_DIR_TX,I2S_TRIGGER_DROP);
  }
#endif
  captureReady_=playbackReady_=pttReady_=false;frameSamples_=0;
}
}
