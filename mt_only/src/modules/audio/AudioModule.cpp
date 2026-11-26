// SPDX-License-Identifier: GPL-3.0-only
#include "AudioModule.h"
#include "AudioWire.h"
#include "../../RegionPolicy.h"
#include <string.h>
#include "meshtastic/portnums.pb.h"

namespace mt::audio {
bool AudioModule::regionPermits(uint32_t regionCode) {
  const auto *region = radio::RegionPolicy::lookup(regionCode);
  return regionCode && region && region->profile && region->profile->audioPermitted;
}

bool AudioModule::configure(const AudioConfig &c,ZephyrI2sAudio &io){
  stop();io_=&io;
  if(!c.enabled)return true;
  // Official enum is 1-based on the packet and maps to Codec2 mode enum - 1.
  const auto wire=static_cast<uint8_t>(c.bitrate)-1;
  if(!codec_.open(wire))return false;
  if(!io.configure(c,codec_.samplesPerFrame())){io.stop();codec_.close();return false;}
  enabled_=true;return true;
}
AudioCapabilities AudioModule::availableCapabilities(const AudioConfig &c,const ZephyrI2sAudio &io){
  if(!c.enabled||!Codec2FrameCodec::modeAvailable(static_cast<uint8_t>(c.bitrate)-1))return {};
  const auto p=io.availableCapabilities(c);
  return {true,p.i2sCapture,p.i2sPlayback,p.ptt};
}
AudioCapabilities AudioModule::capabilities()const{
  if(!enabled_||!io_)return {};
  const auto c=io_->capabilities();return {codec_.ready(),c.i2sCapture,c.i2sPlayback,c.ptt};
}
bool AudioModule::receive(uint32_t from,uint32_t channel,uint32_t port,const uint8_t *p,size_t n){
  if(!enabled_||transmitting_||port!=meshtastic_PortNum_AUDIO_APP||!AudioWire::validPayload(p,n)||n>PayloadCapacity)return false;
  if(p[3]!=codec_.wireMode())return false;
  const size_t frameBytes=codec_.bytesPerFrame(),samples=codec_.samplesPerFrame();
  if(!frameBytes||!samples||(n-Codec2FrameCodec::HeaderSize)%frameBytes)return false;
  if(samples>320)return false;
  {
    QueueGuard guard(queueLock_);
    if(transmitting_||inCount_>=2)return false;
    const uint8_t slot=uint8_t((inHead_+inCount_)%2);incoming_[slot].size=uint16_t(n);memcpy(incoming_[slot].bytes,p,n);++inCount_;
  }
  (void)from;(void)channel;return true;
}
bool AudioModule::beginPtt(){if(!enabled_||!capabilities().capture)return false;{
  QueueGuard guard(queueLock_);inHead_=inCount_=0;transmitting_=true;
}txSize_=Codec2FrameCodec::HeaderSize;memcpy(tx_,Codec2FrameCodec::Magic,3);tx_[3]=codec_.wireMode();return true;}
bool AudioModule::pushCaptureSamples(const int16_t *s,size_t n){
  if(!transmitting_||!s||n!=codec_.samplesPerFrame())return false;
  size_t bytes=codec_.bytesPerFrame(),written=0;
  const size_t fullPacket = AudioWire::packetSize(bytes);
  if (!fullPacket || (txSize_ == fullPacket && !flush())) return false;
  if(!codec_.encode(s,tx_+txSize_,sizeof(tx_)-txSize_,written)||written!=bytes)return false;
  txSize_+=written;
  // Keep packet air time bounded and under the standard 233-byte data limit.
  if(txSize_==fullPacket)return flush();
  return true;
}
bool AudioModule::flush(){
  if(txSize_==Codec2FrameCodec::HeaderSize)return true;
  {
    QueueGuard guard(queueLock_);
    if(outCount_>=2)return false;
    const uint8_t slot=uint8_t((outHead_+outCount_)%2);outgoing_[slot].size=uint16_t(txSize_);memcpy(outgoing_[slot].bytes,tx_,txSize_);++outCount_;
  }
  txSize_=Codec2FrameCodec::HeaderSize;memcpy(tx_,Codec2FrameCodec::Magic,3);tx_[3]=codec_.wireMode();return true;
}
bool AudioModule::endPtt(){if(!transmitting_)return false;if(!flush())return false;transmitting_=false;return true;}
bool AudioModule::peekOutgoing(uint8_t *out,size_t cap,size_t &size,uint32_t &port){
  size=0;if(!out)return false;
  {
    QueueGuard guard(queueLock_);
    if(!outCount_)return false;
    auto &packet=outgoing_[outHead_];if(cap<packet.size)return false;
    size=packet.size;memcpy(out,packet.bytes,size);
  }
  port=meshtastic_PortNum_AUDIO_APP;return true;
}
void AudioModule::outgoingAccepted(){
  QueueGuard guard(queueLock_);
  if(!outCount_)return;
  outgoing_[outHead_].size=0;outHead_=uint8_t((outHead_+1)%2);--outCount_;
}
bool AudioModule::popOutgoing(uint8_t *out,size_t cap,size_t &size,uint32_t &port){
  if(!peekOutgoing(out,cap,size,port))return false;
  outgoingAccepted();return true;
}
bool AudioModule::playQueuedPacket(const QueuedPacket &packet){
  const size_t frameBytes=codec_.bytesPerFrame(),samples=codec_.samplesPerFrame();if(!frameBytes||!samples||samples>320)return false;
  int16_t decoded[320];
  return AudioWire::decodeFrames(packet.bytes, packet.size, codec_.wireMode(), frameBytes,
      [&](const uint8_t *encoded, size_t length) {
        size_t count = 0;
        return codec_.decode(encoded, length, decoded, 320, count) && count == samples &&
               io_->playback(decoded, count, 100);
      });
}
void AudioModule::poll(uint32_t){
  if(!enabled_||!io_)return;
  if(io_->capabilities().ptt){
    const bool pressed=io_->pttPressed();
    if(!stock::audio::pollPtt(pressed,transmitting_,[&]{return beginPtt();},[&]{return endPtt();}))return;
  }
  if(!transmitting_){
    QueuedPacket packet{};bool havePacket=false;
    {
      QueueGuard guard(queueLock_);
      if(inCount_){packet=incoming_[inHead_];incoming_[inHead_].size=0;inHead_=uint8_t((inHead_+1)%2);--inCount_;havePacket=true;}
    }
    if(havePacket)playQueuedPacket(packet);
    return;
  }
  int16_t samples[320];const size_t n=codec_.samplesPerFrame();if(n<=320&&io_->capture(samples,n,40))pushCaptureSamples(samples,n);
}
void AudioModule::stop(){enabled_=transmitting_=false;if(io_)io_->stop();codec_.close();io_=nullptr;txSize_=Codec2FrameCodec::HeaderSize;
  QueueGuard guard(queueLock_);inHead_=inCount_=outHead_=outCount_=0;
}
}
