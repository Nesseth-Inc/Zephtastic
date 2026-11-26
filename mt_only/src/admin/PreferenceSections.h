// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../SettingsJournal.h"
#include "PreferenceArchive.h"
#include "../Config.h"
#include "../ModuleSettings.h"
#include "../ModuleJournal.h"
#include "../modules/CompletionSettings.h"
#include "../modules/CompletionJournal.h"
#include "../modules/ExtendedModuleSettings.h"
#include "../modules/ExtendedModuleJournal.h"
#include "../notifications/NotificationSettings.h"
#include "../notifications/NotificationJournal.h"
#include "../network/WifiStationSettings.h"
#include "../network/WifiStationJournal.h"
#include "../network/MqttGatewaySettings.h"
#include "../network/MqttGatewayJournal.h"
#include "../BluetoothSettings.h"
#include "../BluetoothJournal.h"
#include "../modules/CannedMessageSettings.h"
#include "../modules/CannedMessageJournal.h"
#include "../notifications/RingtoneSettings.h"
#include "../notifications/RingtoneJournal.h"
#include "../ui/UiConfigSettings.h"
#include "../ui/UiConfigJournal.h"
#include "../admin/AdminKeySettings.h"
#include "../admin/AdminKeyJournal.h"
#if defined(CONFIG_MT_GPIO_MODULES)
#include "../modules/GpioModuleSettings.h"
#include "../modules/GpioModuleJournal.h"
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
#include "../modules/StoreForwardSettings.h"
#include "../modules/StoreForwardJournal.h"
#endif
#include <errno.h>
#include <string.h>

namespace mt::admin {

// Stable section identifiers are part of the local archive format. A source
// pointer is present only when that family is compiled and admitted by owner.
class PreferenceSections {
public:
  enum : uint8_t {
    Core=0, Modules=1, Completion=2, Extended=3, Notifications=4, Wifi=5,
    Mqtt=6, Bluetooth=7, Canned=8, Ringtone=9, Ui=10, Gpio=11,
    StoreForward=12, AdminKeys=13
  };
  static constexpr size_t Maximum=14;
  using Admit = int (*)(void *, uint8_t, const uint8_t *, size_t);
  using MqttAdmission = bool (*)(void *, bool &, bool &);

  struct Sources {
    const Config *core=nullptr;
    const ModuleSettings *modules=nullptr;
    const modules::CompletionSettings *completion=nullptr;
    const modules::ExtendedModuleSettings *extended=nullptr;
    const NotificationSettings *notifications=nullptr;
    const network::WifiStationSettings *wifi=nullptr;
    const network::MqttGatewaySettings *mqtt=nullptr;
    const BluetoothSettings *bluetooth=nullptr;
    const modules::CannedMessageSettings *canned=nullptr;
    const notification::RingtoneSettings *ringtone=nullptr;
    const meshtastic_DeviceUIConfig *ui=nullptr;
#if defined(CONFIG_MT_GPIO_MODULES)
    const modules::GpioModuleSettings *gpio=nullptr;
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    const modules::StoreForwardSettings *storeForward=nullptr;
#endif
    const AdminKeySettings *adminKeys=nullptr;
  };
  struct Owner {
    SettingsBackend *backend=nullptr;
    Sources sources{};
    void *context=nullptr;
    bool (*ready)(void *)=nullptr;
    bool (*quiescent)(void *)=nullptr;
    Admit admit=nullptr;
    MqttAdmission mqttAdmission=nullptr;
  };

private:
  struct Wipe { uint8_t *p; size_t n; ~Wipe(){ volatile uint8_t *q=p; while(n--)*q++=0; } };
  struct WipeAny { void *p; size_t n; ~WipeAny(){ volatile uint8_t *q=static_cast<volatile uint8_t *>(p); while(n--)*q++=0; } };
  static bool goodOwner(const Owner &o) {
    return o.backend && o.ready && o.quiescent && o.admit &&
           o.ready(o.context) && o.quiescent(o.context);
  }
  static bool source(const Sources &s,uint8_t id) {
    switch(id) {
      case Core:return s.core; case Modules:return s.modules; case Completion:return s.completion;
      case Extended:return s.extended; case Notifications:return s.notifications; case Wifi:return s.wifi;
      case Mqtt:return s.mqtt; case Bluetooth:return s.bluetooth; case Canned:return s.canned;
      case Ringtone:return s.ringtone; case Ui:return s.ui;
#if defined(CONFIG_MT_GPIO_MODULES)
      case Gpio:return s.gpio;
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
      case StoreForward:return s.storeForward;
#endif
      case AdminKeys:return s.adminKeys; default:return false;
    }
  }
  static int encode(const Sources &s,uint8_t id,uint8_t *out,size_t cap,size_t &n) {
    n=0; if(!out||!source(s,id))return -EINVAL;
#define MT_COPY(ptr,sz) do { if(cap<(sz))return -ENOSPC; memcpy(out,(ptr),(sz)); n=(sz); } while(0)
    switch(id) {
      case Core: MT_COPY(s.core->bytes,Config::Size); break;
      case Modules: MT_COPY(s.modules->bytes,ModuleSettings::Size); break;
      case Completion: MT_COPY(s.completion->bytes,modules::CompletionSettings::Size); break;
      case Extended: MT_COPY(s.extended->bytes,modules::ExtendedModuleSettings::Size); break;
      case Notifications: MT_COPY(s.notifications->bytes,NotificationSettings::Size); break;
      case Wifi: MT_COPY(s.wifi->bytes,s.wifi->encodedSize()); break;
      case Mqtt: MT_COPY(s.mqtt->bytes,network::MqttGatewaySettings::Size); break;
      case Bluetooth: {
        if(cap<BluetoothSettings::Size||!BluetoothSettings::encode(*s.bluetooth,out,cap,n))return -EINVAL; break;
      }
      case Canned: MT_COPY(s.canned->messages,modules::CannedMessageSettings::Capacity); break;
      case Ringtone: MT_COPY(s.ringtone->ringtone,notification::RingtoneSettings::Capacity); break;
      case Ui: if(!ui::UiConfigSettings::encode(*s.ui,out,cap,n))return -EINVAL; break;
#if defined(CONFIG_MT_GPIO_MODULES)
      case Gpio: if(cap<modules::GpioModuleSettings::Size||!s.gpio->encode(out,cap))return -EINVAL; n=modules::GpioModuleSettings::Size; break;
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
      case StoreForward: if(cap<modules::StoreForwardSettings::Size||!s.storeForward->encode(out,cap))return -EINVAL; n=modules::StoreForwardSettings::Size; break;
#endif
      case AdminKeys: {
        constexpr size_t z=1+AdminKeySettings::PackedBytes;
        if(cap<z||!s.adminKeys->valid())return -EINVAL;
        memset(out,0,z);out[0]=s.adminKeys->count();
        memcpy(out+1,s.adminKeys->packedKeys(),size_t(out[0])*AdminKeySettings::KeyBytes);n=z;break;
      }
      default:return -EINVAL;
    }
#undef MT_COPY
    return n<=PreferenceArchive::MaxSectionBytes?0:-E2BIG;
  }
  static int capture(void *ctx,uint8_t id,size_t offset,uint8_t *out,size_t cap,size_t &total) {
    auto &o=*static_cast<Owner *>(ctx); if(!goodOwner(o))return -EACCES;
    uint8_t bytes[PreferenceArchive::MaxSectionBytes]{};Wipe wipe{bytes,sizeof(bytes)};size_t length=0;
    const int rc=encode(o.sources,id,bytes,sizeof(bytes),length);if(rc)return rc;
    total=length;if(offset>length)return -EINVAL;const size_t count=(length-offset)<cap?(length-offset):cap;
    if(count&&!out)return -EINVAL;if(count)memcpy(out,bytes+offset,count);return 0;
  }
  static int validate(void *ctx,uint8_t id,const uint8_t *p,size_t n) {
    auto &o=*static_cast<Owner *>(ctx); if(!goodOwner(o)||!source(o.sources,id)||!p)return -EACCES;
    bool valid=false;
    switch(id) {
      case Core:{Config v;WipeAny w{&v,sizeof(v)};valid=Config::decode(p,n,v);break;}
      case Modules:{ModuleSettings v;if(n==sizeof(v)){memcpy(v.bytes,p,n);valid=v.valid();}break;}
      case Completion:{modules::CompletionSettings v;if(n==sizeof(v)){memcpy(v.bytes,p,n);valid=v.valid();}break;}
      case Extended:{modules::ExtendedModuleSettings v;Wipe w{v.bytes,sizeof(v.bytes)};if(n==sizeof(v)){memcpy(v.bytes,p,n);valid=v.loadStored();}break;}
      case Notifications:{NotificationSettings v;if(n==sizeof(v)){memcpy(v.bytes,p,n);valid=v.valid();}break;}
      case Wifi:{network::WifiStationSettings v;Wipe w{v.bytes,sizeof(v.bytes)};valid=network::WifiStationSettings::decode(p,n,v);break;}
      case Mqtt:{network::MqttGatewaySettings v;Wipe w{v.bytes,sizeof(v.bytes)};if(n==sizeof(v)){memcpy(v.bytes,p,n);valid=v.valid();}break;}
      case Bluetooth:{BluetoothSettings v;uint8_t canonical[BluetoothSettings::Size]{};size_t z=0;Wipe w{canonical,sizeof(canonical)};valid=BluetoothSettings::decode(p,n,v)&&BluetoothSettings::encode(v,canonical,sizeof(canonical),z)&&z==n&&!memcmp(canonical,p,n);break;}
      case Canned:{modules::CannedMessageSettings v;if(n==sizeof(v)){memcpy(v.messages,p,n);valid=v.valid();}break;}
      case Ringtone:{notification::RingtoneSettings v;if(n==sizeof(v)){memcpy(v.ringtone,p,n);valid=v.valid();}break;}
      case Ui:{meshtastic_DeviceUIConfig v{};valid=ui::UiConfigSettings::decodeCanonical(p,n,v);break;}
#if defined(CONFIG_MT_GPIO_MODULES)
      case Gpio:{modules::GpioModuleSettings v;valid=modules::GpioModuleSettings::decode(p,n,v);break;}
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
      case StoreForward:{modules::StoreForwardSettings v;valid=modules::StoreForwardSettings::decode(p,n,v);break;}
#endif
      case AdminKeys:{AdminKeySettings v;valid=n==1+AdminKeySettings::PackedBytes&&p[0]<=AdminKeySettings::MaximumKeys&&v.assign(p[0],p+1,size_t(p[0])*AdminKeySettings::KeyBytes);if(valid)for(size_t i=1+size_t(p[0])*AdminKeySettings::KeyBytes;i<n;++i)if(p[i])valid=false;break;}
      default:return -EINVAL;
    }
    if(!valid)return -EINVAL;
    if(id==Mqtt) {
      bool runtimeSupported=false,trustReady=false;
      if(!o.mqttAdmission||!o.mqttAdmission(o.context,runtimeSupported,trustReady))return -EACCES;
      network::MqttGatewaySettings value;Wipe w{value.bytes,sizeof(value.bytes)};memcpy(value.bytes,p,n);
      const int admitted=value.admissionStatus(runtimeSupported,trustReady);
      if(admitted)return admitted;
    }
    return o.admit?o.admit(o.context,id,p,n):0;
  }
  static int apply(void *ctx,uint8_t id,const uint8_t *p,size_t n) {
    auto &o=*static_cast<Owner *>(ctx);if(!goodOwner(o)||!p)return -EACCES;
    int rc=-EINVAL;
#define MT_LOADSAVE(Journal, value) do { Journal j; rc=j.load(*o.backend); if(rc||!j.ready())return rc?rc:-EIO; rc=j.save(*o.backend,(value)); } while(0)
    switch(id) {
      case Core:{Config v;WipeAny w{&v,sizeof(v)};if(!Config::decode(p,n,v))return -EINVAL;SettingsJournal j;rc=j.load(*o.backend);if(rc||!j.ready())return rc?rc:-EIO;rc=j.save(*o.backend,v);break;}
      case Modules:{ModuleSettings v;if(n!=sizeof(v))return -EINVAL;memcpy(v.bytes,p,n);if(!v.valid())return -EINVAL;MT_LOADSAVE(ModuleJournal,v);break;}
      case Completion:{modules::CompletionSettings v;if(n!=sizeof(v))return -EINVAL;memcpy(v.bytes,p,n);if(!v.valid())return -EINVAL;MT_LOADSAVE(modules::CompletionJournal,v);break;}
      case Extended:{modules::ExtendedModuleSettings v;Wipe w{v.bytes,sizeof(v.bytes)};if(n!=sizeof(v))return -EINVAL;memcpy(v.bytes,p,n);if(!v.loadStored())return -EINVAL;MT_LOADSAVE(mt::modules::ExtendedModuleJournal,v);break;}
      case Notifications:{NotificationSettings v;if(n!=sizeof(v))return -EINVAL;memcpy(v.bytes,p,n);if(!v.valid())return -EINVAL;MT_LOADSAVE(NotificationJournal,v);break;}
      case Wifi:{network::WifiStationSettings v;Wipe w{v.bytes,sizeof(v.bytes)};if(!network::WifiStationSettings::decode(p,n,v))return -EINVAL;MT_LOADSAVE(mt::network::WifiStationJournal,v);break;}
      case Mqtt:{network::MqttGatewaySettings v;Wipe w{v.bytes,sizeof(v.bytes)};if(n!=sizeof(v))return -EINVAL;memcpy(v.bytes,p,n);if(!v.valid())return -EINVAL;network::MqttGatewayJournal j;rc=j.load(*o.backend);if(rc||!j.ready())return rc?rc:-EIO;bool runtime=false,trust=false;if(!o.mqttAdmission||!o.mqttAdmission(o.context,runtime,trust))return -EACCES;rc=j.save(*o.backend,v,runtime,trust);break;}
      case Bluetooth:{BluetoothSettings v;if(!BluetoothSettings::decode(p,n,v))return -EINVAL;MT_LOADSAVE(BluetoothJournal,v);break;}
      case Canned:{modules::CannedMessageSettings v;if(n!=sizeof(v))return -EINVAL;memcpy(v.messages,p,n);if(!v.valid())return -EINVAL;MT_LOADSAVE(modules::CannedMessageJournal,v);break;}
      case Ringtone:{notification::RingtoneSettings v;if(n!=sizeof(v))return -EINVAL;memcpy(v.ringtone,p,n);if(!v.valid())return -EINVAL;MT_LOADSAVE(notification::RingtoneJournal,v);break;}
      case Ui:{meshtastic_DeviceUIConfig v{};if(!ui::UiConfigSettings::decodeCanonical(p,n,v))return -EINVAL;MT_LOADSAVE(ui::UiConfigJournal,v);break;}
#if defined(CONFIG_MT_GPIO_MODULES)
      case Gpio:{modules::GpioModuleSettings v;if(!modules::GpioModuleSettings::decode(p,n,v))return -EINVAL;MT_LOADSAVE(modules::GpioModuleJournal,v);break;}
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
      case StoreForward:{modules::StoreForwardSettings v;if(!modules::StoreForwardSettings::decode(p,n,v))return -EINVAL;MT_LOADSAVE(modules::StoreForwardJournal,v);break;}
#endif
      case AdminKeys:{AdminKeySettings v;if(n!=1+AdminKeySettings::PackedBytes||p[0]>AdminKeySettings::MaximumKeys||!v.assign(p[0],p+1,size_t(p[0])*AdminKeySettings::KeyBytes))return -EINVAL;for(size_t i=1+size_t(p[0])*AdminKeySettings::KeyBytes;i<n;++i)if(p[i])return -EINVAL;MT_LOADSAVE(AdminKeyJournal,v);break;}
      default:return -EINVAL;
    }
#undef MT_LOADSAVE
    return rc;
  }

public:
  static bool ids(const Sources &sources,uint8_t out[PreferenceArchive::MaxSections],size_t &count) {
    count=0;if(!out)return false;
    // These baseline settings stores are always part of the companion image;
    // omission would turn a restore into an accidental partial profile.
    for(uint8_t id=Core;id<=Ui;++id)if(!source(sources,id))return false;
#if defined(CONFIG_MT_GPIO_MODULES)
    if(!sources.gpio)return false;
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(!sources.storeForward)return false;
#endif
    for(uint8_t id=0;id<Maximum;++id)if(source(sources,id))out[count++]=id;
    return count!=0;
  }
  static PreferenceArchive::Callbacks callbacks(Owner &owner) {
    return {&owner,
      [](void *p)->bool{return goodOwner(*static_cast<Owner *>(p));},
      [](void *p)->bool{auto &o=*static_cast<Owner *>(p);return o.quiescent&&o.quiescent(o.context);},
      &capture,&validate,&apply};
  }
};

} // namespace mt::admin
