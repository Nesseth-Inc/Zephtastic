// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "XModem.h"
#include <zephyr/kernel.h>
#include <errno.h>

namespace mt::stock::files {
// The I/O loop is the only protocol/file owner. The transport callbacks copy
// one packet under a spinlock and never call the filesystem while holding it.
template<class File> class PhoneFiles {
    enum class Phase { Idle, Waiting, Working, Reply, Ready };
    k_spinlock lock_{};
    Phase phase_=Phase::Idle;
    uint8_t owner_=0;
    bool revoked_=false,listed_=false;
    uint8_t listPending_=0,listReady_=0;
    uint32_t listedSize_=0;
    uint32_t last_=0;
    Packet slot_{};
    File &file_;
    XModem protocol_;
public:
    explicit PhoneFiles(File &file):file_(file){}
    bool submit(uint8_t owner,const Packet &packet,uint32_t now){
        const auto key=k_spin_lock(&lock_);
        const bool ok=phase_==Phase::Idle || (phase_==Phase::Ready&&owner==owner_&&!revoked_);
        if(ok){owner_=owner;last_=now;slot_=packet;phase_=Phase::Waiting;revoked_=false;}
        k_spin_unlock(&lock_,key);return ok;
    }
    // One bounded public-file stat per handshake, performed outside ServiceLock.
    int manifest(uint8_t owner,uint32_t &size){
        if(owner>1)return 0;
        const auto bit=uint8_t(1u<<owner);
        const auto key=k_spin_lock(&lock_);
        int result=-EAGAIN;
        if(listReady_&bit){size=listedSize_;result=listed_?1:0;}
        else listPending_|=bit;
        k_spin_unlock(&lock_,key);return result;
    }
    void manifestSent(uint8_t owner){
        if(owner>1)return;
        const auto key=k_spin_lock(&lock_);listReady_&=uint8_t(~(1u<<owner));k_spin_unlock(&lock_,key);
    }
    void disconnect(uint8_t owner){
        const auto key=k_spin_lock(&lock_);
        if(owner<=1){const auto keep=uint8_t(~(1u<<owner));listReady_&=keep;listPending_&=keep;}
        if(phase_!=Phase::Idle&&owner_==owner)revoked_=true;
        k_spin_unlock(&lock_,key);
    }
    bool response(uint8_t owner,Packet &out){
        const auto key=k_spin_lock(&lock_);
        const bool valid=phase_==Phase::Reply&&owner_==owner&&!revoked_;
        if(valid)out=slot_;
        k_spin_unlock(&lock_,key);return valid;
    }
    void sent(uint8_t owner){
        const auto key=k_spin_lock(&lock_);
        if(phase_==Phase::Reply&&owner_==owner){
            slot_={};phase_=protocol_.active()?Phase::Ready:Phase::Idle;
        }
        k_spin_unlock(&lock_,key);
    }
    void poll(uint32_t now){
        auto key=k_spin_lock(&lock_);
        if(listPending_){
            const uint8_t requested=listPending_;k_spin_unlock(&lock_,key);
            uint32_t size=0;const bool exists=file_.info(size)==0;
            key=k_spin_lock(&lock_);listReady_|=uint8_t(requested&listPending_);listPending_&=uint8_t(~requested);
            listed_=exists;listedSize_=size;
        }
        if(phase_==Phase::Idle){k_spin_unlock(&lock_,key);return;}
        if(revoked_||uint32_t(now-last_)>=30000){
            phase_=Phase::Working;
            k_spin_unlock(&lock_,key);
            protocol_.reset(file_);
            key=k_spin_lock(&lock_);slot_={};phase_=Phase::Idle;revoked_=false;
            k_spin_unlock(&lock_,key);return;
        }
        if(phase_!=Phase::Waiting){k_spin_unlock(&lock_,key);return;}
        phase_=Phase::Working;
        k_spin_unlock(&lock_,key);
        Packet answer{};
        protocol_.handle(file_,slot_,answer);
        key=k_spin_lock(&lock_);slot_=answer;phase_=Phase::Reply;k_spin_unlock(&lock_,key);
    }
};
}
