// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrUpdateBackend.h"
#include "../BoardIdentity.h"
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
#include "SignedOta.h"
#include "ZephyrWiFiStation.h"
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
#include "SignedBleOta.h"
#endif
#endif
#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>

#if defined(CONFIG_MT_NORDIC_DFU) && defined(CONFIG_SOC_SERIES_NRF52)
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#define MT_NORDIC_DFU_BACKEND 1
#if DT_NODE_EXISTS(DT_NODELABEL(uf2_partition))
#define MT_HAS_UF2_PARTITION 1
static constexpr uint32_t Uf2Start=DT_REG_ADDR(DT_NODELABEL(uf2_partition));
static constexpr uint32_t Uf2Size=DT_REG_SIZE(DT_NODELABEL(uf2_partition));
#else
#define MT_HAS_UF2_PARTITION 0
#endif
#else
#define MT_NORDIC_DFU_BACKEND 0
#define MT_HAS_UF2_PARTITION 0
#endif

#if defined(CONFIG_MT_ESP_STOCK_OTA_LOADER) && defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <zephyr/sys/reboot.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#define MT_ESP_OTA_BACKEND 1
#else
#define MT_ESP_OTA_BACKEND 0
#endif

namespace mt::update {
namespace {
Mode preparedMode=Mode::None;
bool prepared=false;
int unsupported(){return -ENOTSUP;}

#if MT_NORDIC_DFU_BACKEND && MT_HAS_UF2_PARTITION
// Adafruit enterOTADfu() cold-resets with the SoftDevice uninitialized.
// 0xB1 belongs to a direct application jump with a running SoftDevice.
constexpr uint8_t NordicOtaReset=0xA8u;
bool nordicBootloaderPresent(){
  if(Uf2Size<0x1000u||Uf2Start>UINT32_MAX-Uf2Size)return false;
  const uint32_t start=NRF_UICR->NRFFW[0];
  const uint64_t end=uint64_t(Uf2Start)+Uf2Size;
  if(start<Uf2Start||uint64_t(start)+8u>end||(start&3u))return false;
  const auto *vectors=reinterpret_cast<const volatile uint32_t *>(uintptr_t(start));
  const uint32_t initialSp=vectors[0],resetVector=vectors[1];
  const uint32_t pc=resetVector&~uint32_t(1);
  constexpr uint32_t RamStart=0x20000000u,RamEnd=0x20040000u;
  return initialSp>RamStart&&initialSp<=RamEnd&&(resetVector&1u)&&
    pc>=start&&uint64_t(pc)<end;
}
#endif

#if MT_ESP_OTA_BACKEND
constexpr char NvsNamespace[]="MeshtasticOTA";
constexpr char CombinedLoader[]="MeshtasticOTA";
constexpr char BleLoader[]="MeshtasticOTA-BLE";
constexpr char WifiLoader[]="MeshtasticOTA-WiFi";

bool overlaps(uint32_t a,uint32_t an,uint32_t b,uint32_t bn){
  if(!an||!bn)return false;
  const uint64_t ae=uint64_t(a)+an,be=uint64_t(b)+bn;
  return uint64_t(a)<be&&uint64_t(b)<ae;
}
bool samePartition(const esp_partition_t *a,const esp_partition_t *b){
  return a&&b&&a->address==b->address&&a->size==b->size&&
    a->type==b->type&&a->subtype==b->subtype;
}
bool restoreBootSelector(const esp_partition_t *running){
  if(!running||running->type!=ESP_PARTITION_TYPE_APP)return false;
  const esp_partition_t *selected=esp_ota_get_boot_partition();
  if(!samePartition(selected,running)){
    // Exactly one rollback attempt to the captured running application. Never
    // reapply the loader selector from a recovery path.
    (void)esp_ota_set_boot_partition(running);
  }
  selected=esp_ota_get_boot_partition();
  return samePartition(selected,running);
}
bool safeFromZephyrSettings(const esp_partition_t *part){
  if(!part)return false;
#if DT_HAS_CHOSEN(zephyr_settings_partition)
  constexpr uint32_t settingsAddress=DT_REG_ADDR(DT_CHOSEN(zephyr_settings_partition));
  constexpr uint32_t settingsSize=DT_REG_SIZE(DT_CHOSEN(zephyr_settings_partition));
  if(overlaps(part->address,part->size,settingsAddress,settingsSize))return false;
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(mt_pki_history))
  constexpr uint32_t historyAddress=DT_REG_ADDR(DT_NODELABEL(mt_pki_history));
  constexpr uint32_t historySize=DT_REG_SIZE(DT_NODELABEL(mt_pki_history));
  if(overlaps(part->address,part->size,historyAddress,historySize))return false;
#endif
  return true;
}
const esp_partition_t *findPartition(esp_partition_type_t type,esp_partition_subtype_t subtype,const char *label){
  return esp_partition_find_first(type,subtype,label);
}
bool validLoader(Mode mode,const esp_partition_t *loader){
  if(!loader||loader->type!=ESP_PARTITION_TYPE_APP||loader->subtype!=ESP_PARTITION_SUBTYPE_APP_OTA_1)return false;
  esp_app_desc_t desc{};
  if(esp_ota_get_partition_description(loader,&desc)!=ESP_OK)return false;
  const bool ble=mode==Mode::EspBleLoader;
  return strncmp(desc.project_name,CombinedLoader,sizeof(CombinedLoader))==0||
    (ble&&strncmp(desc.project_name,BleLoader,sizeof(BleLoader))==0)||
    (!ble&&strncmp(desc.project_name,WifiLoader,sizeof(WifiLoader))==0);
}
bool validEspLayout(Mode mode,const esp_partition_t **loaderOut,const esp_partition_t **nvsOut){
#if !DT_HAS_CHOSEN(zephyr_settings_partition)
  (void)mode;(void)loaderOut;(void)nvsOut;return false;
#else
  if(!loaderOut||!nvsOut)return false;
  const esp_partition_t *running=esp_ota_get_running_partition();
  const esp_partition_t *loader=findPartition(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,nullptr);
  const esp_partition_t *nvs=findPartition(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,"nvs");
  const esp_partition_t *otaData=findPartition(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,"otadata");
  const esp_partition_t *ota0=findPartition(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
  const esp_partition_t *boot=esp_ota_get_boot_partition();
  if(!running||!ota0||running->address!=ota0->address||running->subtype!=ESP_PARTITION_SUBTYPE_APP_OTA_0||
     !loader||!nvs||!otaData||!samePartition(boot,running)||!validLoader(mode,loader)||
     nvs->size<0x3000u||otaData->size<0x2000u)return false;
  if(!safeFromZephyrSettings(running)||!safeFromZephyrSettings(ota0)||!safeFromZephyrSettings(loader)||
     !safeFromZephyrSettings(nvs)||!safeFromZephyrSettings(otaData)||
     overlaps(nvs->address,nvs->size,loader->address,loader->size)||
     overlaps(nvs->address,nvs->size,otaData->address,otaData->size)||
     overlaps(loader->address,loader->size,otaData->address,otaData->size)||
     overlaps(loader->address,loader->size,running->address,running->size)||
     overlaps(nvs->address,nvs->size,running->address,running->size)||
     overlaps(otaData->address,otaData->size,running->address,running->size))return false;
  *loaderOut=loader;*nvsOut=nvs;return true;
#endif
}
bool cString(const char *s,size_t capacity,size_t &length){
  if(!s||!capacity)return false;length=strnlen(s,capacity);return length<capacity;
}
int stageEsp(Mode mode,const uint8_t hash[32],const meshtastic_Config_NetworkConfig &network){
  size_t ssidLength=0,pskLength=0;
  if(!hash||!cString(network.wifi_ssid,sizeof(network.wifi_ssid),ssidLength)||
     !cString(network.wifi_psk,sizeof(network.wifi_psk),pskLength))return -EINVAL;
  if(mode==Mode::EspWifiLoader&&(!network.wifi_enabled||!ssidLength))return -EINVAL;
  const esp_partition_t *loader=nullptr,*nvsPart=nullptr;
  if(!validEspLayout(mode,&loader,&nvsPart))return -ENOTSUP;
  // nvs_open_from_partition does not initialize or erase a missing NVS store.
  nvs_handle_t handle=0;esp_err_t result=nvs_open_from_partition(nvsPart->label,NvsNamespace,NVS_READWRITE,&handle);
  if(result!=ESP_OK)return -EIO;
  const uint8_t method=mode==Mode::EspBleLoader?1u:2u;
  result=nvs_set_u8(handle,"method",method);
  if(result==ESP_OK)result=nvs_set_blob(handle,"ota_hash",hash,32);
  if(result==ESP_OK)result=nvs_set_str(handle,"ssid",network.wifi_ssid);
  if(result==ESP_OK)result=nvs_set_str(handle,"psk",network.wifi_psk);
  if(result==ESP_OK)result=nvs_set_u8(handle,"updated",0u);
  if(result==ESP_OK)result=nvs_commit(handle);
  uint8_t gotMethod=0,updated=0,gotHash[32]{};size_t hashLength=sizeof(gotHash);
  char gotSsid[sizeof(network.wifi_ssid)]{},gotPsk[sizeof(network.wifi_psk)]{};
  struct WipeReadback {
    char *ssid,*psk;
    ~WipeReadback(){
      volatile char *a=ssid,*b=psk;
      for(size_t i=0;i<sizeof(meshtastic_Config_NetworkConfig{}.wifi_ssid);++i)a[i]=0;
      for(size_t i=0;i<sizeof(meshtastic_Config_NetworkConfig{}.wifi_psk);++i)b[i]=0;
    }
  } wipeReadback{gotSsid,gotPsk};
  size_t gotSsidLength=sizeof(gotSsid),gotPskLength=sizeof(gotPsk);
  if(result==ESP_OK)result=nvs_get_u8(handle,"method",&gotMethod);
  if(result==ESP_OK)result=nvs_get_blob(handle,"ota_hash",gotHash,&hashLength);
  if(result==ESP_OK)result=nvs_get_str(handle,"ssid",gotSsid,&gotSsidLength);
  if(result==ESP_OK)result=nvs_get_str(handle,"psk",gotPsk,&gotPskLength);
  if(result==ESP_OK)result=nvs_get_u8(handle,"updated",&updated);
  const bool readback=result==ESP_OK&&gotMethod==method&&hashLength==32&&
    memcmp(gotHash,hash,32)==0&&gotSsidLength==ssidLength+1&&
    gotPskLength==pskLength+1&&strcmp(gotSsid,network.wifi_ssid)==0&&
    strcmp(gotPsk,network.wifi_psk)==0&&updated==0;
  nvs_close(handle);
  if(!readback)return -EIO;
  // The target is selected only after the existing loader and all saved
  // handoff data have been verified. No partition is created or erased.
  const esp_partition_t *running=esp_ota_get_running_partition();
  if(!running||running->type!=ESP_PARTITION_TYPE_APP||
     running->subtype!=ESP_PARTITION_SUBTYPE_APP_OTA_0)return -EIO;
  if(esp_ota_set_boot_partition(loader)!=ESP_OK){
    (void)restoreBootSelector(running);
    return -EIO;
  }
  const esp_partition_t *selected=esp_ota_get_boot_partition();
  if(!samePartition(selected,loader)){
    // Restore the already verified running app as the boot target if the
    // selection readback is uncertain. Never reboot after an uncertain switch.
    (void)restoreBootSelector(running);
    return -EIO;
  }
  return 0;
}
#endif
}

bool available(Mode mode){
  if(!board::HasUpdateTarget)return false;
  switch(mode){
  case Mode::None:return false;
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
  case Mode::EspWifiLoader:{char ip[16];return zc_wifi_station_ip(ip,sizeof(ip))&&signedOta::available();}
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
  case Mode::EspBleLoader:return signedBleAvailable()&&signedOta::available();
#endif
#endif
#if MT_NORDIC_DFU_BACKEND && MT_HAS_UF2_PARTITION
  case Mode::NordicUf2:case Mode::NordicBleDfu:return nordicBootloaderPresent();
#endif
#if MT_ESP_OTA_BACKEND
  case Mode::EspBleLoader:case Mode::EspWifiLoader:{
    const esp_partition_t *loader=nullptr,*nvs=nullptr;
    return validEspLayout(mode,&loader,&nvs);
  }
#endif
  default:return false;
  }
}

int prepare(Mode mode,const uint8_t hash[32],const meshtastic_Config_NetworkConfig &network){
  if(prepared)return -EBUSY;
  const bool hashMode=mode==Mode::EspBleLoader||mode==Mode::EspWifiLoader;
  if(mode==Mode::None||(hashMode&&!hash)||(!hashMode&&hash))return -EINVAL;
  if(!available(mode))return -ENOTSUP;
  int result=-ENOTSUP;
  switch(mode){
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
  case Mode::EspWifiLoader:
#if defined(CONFIG_MT_SIGNED_BLE_OTA)
  case Mode::EspBleLoader:
#endif
    signedOta::activate(hash);result=0;break;
#endif
#if MT_NORDIC_DFU_BACKEND && MT_HAS_UF2_PARTITION
  case Mode::NordicUf2:nrf_power_gpregret_set(NRF_POWER,0,0x57u);result=NRF_POWER->GPREGRET==0x57u?0:-EIO;break;
  case Mode::NordicBleDfu:nrf_power_gpregret_set(NRF_POWER,0,NordicOtaReset);result=NRF_POWER->GPREGRET==NordicOtaReset?0:-EIO;break;
#endif
#if MT_ESP_OTA_BACKEND
  case Mode::EspBleLoader:case Mode::EspWifiLoader:result=stageEsp(mode,hash,network);break;
#endif
  default:return -ENOTSUP;
  }
  if(result==0){prepared=true;preparedMode=mode;}
  return result;
}

void rebootPrepared(Mode mode){
  if(!prepared||mode!=preparedMode)return;
  prepared=false;preparedMode=Mode::None;
#if MT_NORDIC_DFU_BACKEND || MT_ESP_OTA_BACKEND
  sys_reboot(SYS_REBOOT_COLD);
#endif
}
}
