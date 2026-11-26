// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Display.h"

namespace mt::ui {
// The same recursive service mutex serializes local USB/BLE input and physical
// button/render polling.  Keeping this boundary in production code makes a
// future caller safe even if it is no longer reached from the locked main loop.
class Controller {
  void *context_;
  void (*lock_)(void *);
  void (*unlock_)(void *);
  struct Guard {
    Controller &owner;
    explicit Guard(Controller &value):owner(value){owner.lock_(owner.context_);}
    ~Guard(){owner.unlock_(owner.context_);}
  };
public:
  Controller(void *context,void (*lock)(void *),void (*unlock)(void *)):
    context_(context),lock_(lock),unlock_(unlock) {}
  int input(uint32_t code,uint32_t now) {
    Guard guard(*this);return mt::ui::input(code,now);
  }
  bool selectedPeerForTelemetry(SelectedPeerForTelemetry &out) {
    Guard guard(*this);return mt::ui::selectedPeerForTelemetry(out);
  }
  void peerTelemetry(const PeerTelemetryView &snapshot) {
    Guard guard(*this);mt::ui::peerTelemetry(snapshot);
  }
  void setCannedMessages(const char *list,bool ready,bool enabled,uint32_t destination=0xffffffffu,uint8_t channel=0) {
    Guard guard(*this);mt::ui::setCannedMessages(list,ready,enabled,destination,channel);
  }
  void setCannedRoutes(uint8_t validChannelMask,uint32_t configurationGeneration) {
    Guard guard(*this);mt::ui::setCannedRoutes(validChannelMask,configurationGeneration);
  }
  void setCannedContent(const char *list,bool ready,bool enabled) {
    Guard guard(*this);mt::ui::setCannedContent(list,ready,enabled);
  }
  bool takePendingCannedSend(PendingCannedSend &out) {
    Guard guard(*this);return mt::ui::takePendingCannedSend(out);
  }
  void reportCannedResult(int result) {
    Guard guard(*this);mt::ui::reportCannedResult(result);
  }
  void reportCannedCompletion(bool relayed,bool success) {
    Guard guard(*this);mt::ui::reportCannedCompletion(relayed,success);
  }
  void preview(const Service &service,uint32_t from,uint32_t to,uint32_t id,unsigned port,
               const uint8_t *payload,size_t n,int16_t rssi,int8_t snr,uint32_t now,bool local,const stock::text::Settings &settings={}) {
    Guard guard(*this);mt::ui::preview(service,from,to,id,port,payload,n,rssi,snr,now,local,settings);
  }
  void poll(const Service &service,bool radioReady,bool bleReady,bool bleConnected,
            bool usbConnected,uint32_t now,bool settingsReady=true,const GnssView &gnss={}) {
    Guard guard(*this);
    mt::ui::poll(service,radioReady,bleReady,bleConnected,usbConnected,now,settingsReady,gnss);
  }
};
}
