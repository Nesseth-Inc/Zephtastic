// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NodeJournal.h"
#include "SettingsEdit.h"
#include "ModuleEdit.h"
#include "notifications/NotificationEdit.h"
#include "network/WifiStationEdit.h"
#include "modules/ExtendedModuleEdit.h"
#include "modules/CompletionEdit.h"
#include "network/MqttGatewayAdmin.h"
#if defined(CONFIG_MT_GPIO_MODULES)
#include "modules/GpioModuleEdit.h"
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
#include "modules/StoreForwardEdit.h"
#endif

namespace mt {
// Stock commit_edit_settings saves its NodeDB segment together with settings.
// This bounded coordinator does the same for our separate verified journals.
// The writes are intentionally not claimed atomic across journals: each must
// verify independently, and any uncertainty fails closed without a retry.
class SettingsCommit {
  Service &service_;
  Radio &radio_;
  SettingsEdit &edit_;
  NodeJournal &nodes_;
  SettingsBackend &backend_;
  ModuleEdit *modules_;
  NotificationEdit *notifications_;
  network::WifiStationEdit *wifi_;
  modules::ExtendedModuleEdit *extended_;
  network::MqttGatewayAdmin *mqtt_=nullptr;
  modules::CompletionEdit *completion_=nullptr;
#if defined(CONFIG_MT_GPIO_MODULES)
  modules::GpioModuleEdit *gpioModules_=nullptr;
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  modules::StoreForwardEdit *storeForward_=nullptr;
#endif

  int restore(bool wasReceiving,uint32_t now) {
    if(!wasReceiving)return 0;
    if(!service_.config.provisioned()||!service_.start(false,now))return -EIO;
    const bool enabled=service_.normalMode()&&service_.txAllowed()&&service_.config.txEnabled();
    if(!service_.armEndpoint(enabled)){service_.stop();return -EIO;}
    return service_.faulted()?-EIO:0;
  }
public:
  void mqtt(network::MqttGatewayAdmin &admin){mqtt_=&admin;}
  void completion(modules::CompletionEdit &edit){completion_=&edit;}
#if defined(CONFIG_MT_GPIO_MODULES)
  void gpioModules(modules::GpioModuleEdit &edit){gpioModules_=&edit;}
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  void storeForward(modules::StoreForwardEdit &edit){storeForward_=&edit;}
#endif
  SettingsCommit(Service &service,Radio &radio,SettingsEdit &edit,
                 NodeJournal &nodes,SettingsBackend &backend,ModuleEdit *modules=nullptr,NotificationEdit *notifications=nullptr,
                 network::WifiStationEdit *wifi=nullptr,modules::ExtendedModuleEdit *extended=nullptr):
    service_(service),radio_(radio),edit_(edit),nodes_(nodes),backend_(backend),modules_(modules),notifications_(notifications),wifi_(wifi),extended_(extended){}

  int commit(uint32_t now) {
    // A truly clean/no-open commit remains an inert stock-compatible success.
    if(!edit_.open()&&!service_.nodes.dirty()&&(!modules_||!modules_->dirty())&&
       (!notifications_||!notifications_->dirty())&&(!wifi_||!wifi_->dirty())&&(!extended_||!extended_->dirty())
       &&(!mqtt_||!mqtt_->dirty())&&(!completion_||!completion_->dirty())
#if defined(CONFIG_MT_GPIO_MODULES)
       &&(!gpioModules_||!gpioModules_->dirty())
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
       &&(!storeForward_||!storeForward_->dirty())
#endif
       )return edit_.commit(now);
    // Never discard or interrupt accepted client/RF work to make storage safe.
    if(!service_.storageStopSafe()||radio_.busy())return -EBUSY;
    const bool wasReceiving=service_.receiving();
    const int paused=service_.pauseForStorage();
    if(paused)return paused;
    if(service_.faulted()||!service_.storageQuiescent()||!radio_.ready()||
       radio_.health()!=0)return -EIO;
    if(modules_){const int result=modules_->commit(now);if(result)return result;}
    if(notifications_){const int result=notifications_->commit(now);if(result)return result;}
    if(wifi_){const int result=wifi_->commit(now);if(result)return result;}
    if(extended_){const int result=extended_->commit(now);if(result)return result;}
    if(completion_){const int result=completion_->commit(now);if(result)return result;}
#if defined(CONFIG_MT_GPIO_MODULES)
    if(gpioModules_){const int result=gpioModules_->commit(now);if(result)return result;}
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(storeForward_){const int result=storeForward_->commit(now);if(result)return result;}
#endif
    const int settingsResult=edit_.commit(now);
    if(settingsResult)return settingsResult;
    // A consent grant becomes durable only after its core channel commits.
    if(mqtt_){const int result=mqtt_->commit(now);if(result)return result;}
    // Force bypasses only the cache wear timer. The quiescence proof above is
    // still mandatory and NodeJournal checks it again through this argument.
    const int nodeResult=nodes_.flush(backend_,service_.nodes,now,true,true);
    if(nodeResult<0)return nodeResult;
    return restore(wasReceiving,now);
  }
};
}
