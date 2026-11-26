// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MqttGatewayJournal.h"
#include "../Service.h"
#include <errno.h>
#include <string.h>

namespace mt { namespace network {
// Independent MQTT journal edit state. It never mutates the core Config or
// radio configuration; its storage pause is restored before returning.
class MqttGatewayEdit {
  Service &service_;
  MqttGatewayJournal &journal_;
  SettingsBackend &backend_;
  MqttGatewaySettings current_;
  bool dirty_=false,runtimeSupported_=false,trustReady_=false;
  static bool same(const MqttGatewaySettings&a,const MqttGatewaySettings&b){
    return memcmp(a.bytes,b.bytes,MqttGatewaySettings::Size)==0;
  }
  int persist(const MqttGatewaySettings &candidate,uint32_t now){
    if(!journal_.ready())return -EACCES;
    const int status=candidate.admissionStatus(runtimeSupported_,trustReady_);if(status)return status;
    const bool receiving=service_.receiving(),armed=service_.endpointArmed();
    const int paused=service_.pauseForStorage();if(paused)return paused;
    const int saved=journal_.save(backend_,candidate,runtimeSupported_,trustReady_);if(saved)return saved;
    current_=candidate;dirty_=false;
    if(receiving&&service_.config.provisioned()){
      if(!service_.start(false,now))return -EIO;
      if(armed&&!service_.armEndpoint(true))return -EIO;
    }
    return 0;
  }
public:
  MqttGatewayEdit(Service&s,MqttGatewayJournal&j,SettingsBackend&b):service_(s),journal_(j),backend_(b){}
  bool ready()const{return journal_.ready();}
  bool recovered()const{return journal_.state()==MqttGatewayJournal::State::Recovered;}
  bool ioError()const{return journal_.state()==MqttGatewayJournal::State::IoError;}
  bool dirty()const{return dirty_;}
  const MqttGatewaySettings&config()const{return current_;}
  bool runtimeSupported()const{return runtimeSupported_;}
  bool trustReady()const{return trustReady_;}
  void runtimePolicy(bool supported,bool trust){runtimeSupported_=supported;trustReady_=trust;}
  bool load(){dirty_=false;if(!ready())return false;current_=journal_.config();return true;}
  int save(const MqttGatewaySettings &candidate,uint32_t now,bool editing){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    const int status=candidate.admissionStatus(runtimeSupported_,trustReady_);if(status)return status;
    if(same(candidate,current_))return 0;
    if(editing){current_=candidate;dirty_=!same(candidate,journal_.config());return 0;}
    return persist(candidate,now);
  }
  int commit(uint32_t now){if(!dirty_)return 0;return persist(current_,now);}
  // An immediate journal I/O failure poisons the live journal by design. A
  // later explicit region reconciliation may re-read both slots and retry
  // only when that read selects an unambiguous, non-recovered state. Never
  // discard a staged edit or auto-repair a damaged slot here.
  int retryIoError(){
    if(dirty_||journal_.state()!=MqttGatewayJournal::State::IoError)return -EACCES;
    const int loaded=journal_.load(backend_);if(loaded)return loaded;
    if(journal_.state()==MqttGatewayJournal::State::Recovered||
       journal_.state()==MqttGatewayJournal::State::Invalid)return -EIO;
    current_=journal_.config();dirty_=false;return 0;
  }
};
}}
