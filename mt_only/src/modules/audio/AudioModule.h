// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "AudioCodec.h"
#include "AudioConfig.h"
#include "ZephyrI2sAudio.h"
#include <cstddef>
#include <cstdint>
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <atomic>
#endif

namespace mt::audio {
struct AudioCapabilities { bool codec2=false,capture=false,playback=false,ptt=false; };
class AudioModule {
public:
  static bool regionPermits(uint32_t regionCode);
  bool configure(const AudioConfig&,ZephyrI2sAudio&);
  static AudioCapabilities availableCapabilities(const AudioConfig&,const ZephyrI2sAudio&);
  AudioCapabilities capabilities()const;
  bool receive(uint32_t from,uint32_t channel,uint32_t port,const uint8_t*,size_t);
  bool beginPtt();
  bool pushCaptureSamples(const int16_t*,size_t);
  bool endPtt();
  // The main owner is the sole consumer. No queue spinlock is held over RF submission.
  template<class Send> bool sendPayload(uint8_t *out,size_t capacity,Send sendToMesh) {
    size_t size=0;uint32_t port=0;
    if (!peekOutgoing(out,capacity,size,port) || !sendToMesh(port,out,size)) return false;
    outgoingAccepted();
    return true;
  }
  bool popOutgoing(uint8_t *payload,size_t capacity,size_t &size,uint32_t &port);
  void poll(uint32_t nowMs);
  void stop();
private:
  static constexpr size_t PayloadCapacity=233;
  Codec2FrameCodec codec_{}; ZephyrI2sAudio *io_=nullptr;
  bool enabled_=false,transmitting_=false;
  size_t txSize_=Codec2FrameCodec::HeaderSize;
  uint8_t tx_[PayloadCapacity]{};
  struct QueuedPacket {uint16_t size=0;uint8_t bytes[PayloadCapacity]{};};
  QueuedPacket incoming_[2]{},outgoing_[2]{};uint8_t inHead_=0,inCount_=0,outHead_=0,outCount_=0;
#if defined(__ZEPHYR__)
  struct QueueGuard {
    struct k_spinlock &lock;
    k_spinlock_key_t key;
    explicit QueueGuard(struct k_spinlock &value):lock(value),key(k_spin_lock(&lock)){}
    ~QueueGuard(){k_spin_unlock(&lock,key);}
    QueueGuard(const QueueGuard &)=delete;
    QueueGuard &operator=(const QueueGuard &)=delete;
  };
  struct k_spinlock queueLock_{};
#else
  struct QueueGuard {
    std::atomic_flag &lock;
    explicit QueueGuard(std::atomic_flag &value):lock(value){
      while(lock.test_and_set(std::memory_order_acquire)){}
    }
    ~QueueGuard(){lock.clear(std::memory_order_release);}
    QueueGuard(const QueueGuard &)=delete;
    QueueGuard &operator=(const QueueGuard &)=delete;
  };
  std::atomic_flag queueLock_=ATOMIC_FLAG_INIT;
#endif
  bool playQueuedPacket(const QueuedPacket&);
  bool peekOutgoing(uint8_t *,size_t,size_t &,uint32_t &);
  void outgoingAccepted();
  bool flush();
};
}
