// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

namespace mt::stock::files {class RangeFile;}
namespace mt::modules {
class RangeTestLog {
public:
  static constexpr size_t QueueCapacity=12;
  static constexpr size_t PayloadCapacity=233;
  static constexpr size_t SenderNameCapacity=48;
  static constexpr size_t CsvCapacity=1400;

  struct Config { bool enabled=false, save=false, clearOnReboot=false; };
  struct Record {
    uint32_t from=0;
    char senderName[SenderNameCapacity]{};
    int32_t senderLatitudeI=0, senderLongitudeI=0, senderAltitudeM=0;
    int32_t receiverLatitudeI=0, receiverLongitudeI=0, receiverAltitudeM=0;
    float distanceMeters=0;
    float rxSnr=0;
    int32_t rxRssi=0;
    uint16_t payloadSize=0;
    bool senderPositionValid=false, receiverPositionValid=false;
    bool distanceValid=false;
    uint8_t hopLimit=0;
    bool timeValid=false;
    uint8_t utcHour=0, utcMinute=0, utcSecond=0;
    uint8_t payload[PayloadCapacity]{};
  };
  enum class EnqueueResult : uint8_t { Queued, Invalid, Full };
  enum class StartResult : uint8_t { Ready, Disabled, Unavailable, ClearFailed };
  enum class PollResult : uint8_t { Disabled, Unavailable, Empty, Written, IoError, LowSpace };

  // Pure, bounded packet-owner enqueue. Caller invokes only for an admitted
  // foreign RANGE_TEST_APP packet when enabled && save are true.
  EnqueueResult enqueue(const Record &record);
  // Called once at boot outside ServiceLock. clearOnReboot unlinks only the
  // configured RangeTest.csv path; this class never mounts, formats, or mkdirs.
  StartResult start(const Config &config);
  // Filesystem worker. Call outside ServiceLock; retries the queue head on I/O
  // failure before writing. Disabled saving or low space drops unrecorded samples.
  PollResult poll(const Config &config);
  // Remove the stock app-visible CSV with the same serialized FS owner as append.
  int removeFile(const char *name);
  static bool available(); // side-effect-free build/path capability only
  uint32_t dropped() const;
  size_t queued() const;

private:
  struct Entry { Record record{}; uint32_t sequence=0; };
  Entry queue_[QueueCapacity]{};
  size_t read_=0, write_=0, count_=0;
  uint32_t lastSequence_=0, dropped_=0;
  bool fileBusy_=false,startCalled_=false,appendBlocked_=false,csvRepairNeeded_=false;
  friend class ::mt::stock::files::RangeFile;
  class FileLease {
  public:
    explicit FileLease(RangeTestLog &owner):owner_(owner){
      auto key=k_spin_lock(&owner_.lock_);held_=!owner_.fileBusy_;
      if(held_)owner_.fileBusy_=true;
      k_spin_unlock(&owner_.lock_,key);
    }
    ~FileLease(){
      if(held_){auto key=k_spin_lock(&owner_.lock_);owner_.fileBusy_=false;k_spin_unlock(&owner_.lock_,key);}
    }
    bool held()const{return held_;}
  private:
    RangeTestLog &owner_;bool held_;
  };
  // Read only after startCalled_. Keep the empty queue entirely in BSS.
  StartResult startResult_{};
  mutable struct k_spinlock lock_{};

  bool peek(Entry &entry);
  void consume(uint32_t sequence);
  void discard(size_t count);
  PollResult blocked(PollResult result){appendBlocked_=true;return result;}
  static bool validRecord(const Record &record);
  static size_t formatCsv(const Record &record, char *out, size_t capacity);
};
} // namespace mt::modules
