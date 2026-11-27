// SPDX-License-Identifier: GPL-3.0-only
#include "stock/telemetry/NoiseFloor.h"
#include "companion/BootCounter.h"
#include "Companion.h"
#include "BoardIdentity.h"
#include "stock/admin/FactoryConfigReset.h"
#include "stock/admin/ZephyrResetFloor.h"
#if MT_HAS_DEVICE_RESET
#include "stock/admin/DeviceReset.h"
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
#include "admin/AdminKeyCommit.h"
#include "admin/RemoteHardwarePinCache.h"
#include "admin/RemoteHardwarePinJournal.h"
#include "admin/RemoteFavoriteCommit.h"
#include "admin/LegacyAdminProtocol.h"
#include "admin/PendingRemoteConfig.h"
#include "admin/RemoteDeviceTransaction.h"
#endif
#include "monocypher.h"
#include "mqtt/OnlineNodeWindow.h"
#include "mqtt/HotNodeMetadataJournal.h"
#include "mqtt/HotNodeMetadataStartup.h"
#include "IdleRx.h"
#include "AgcMaintenance.h"
#if defined(CONFIG_MT_RUNTIME_RESOURCES)
#include "RuntimeResources.h"
static mt::RuntimeResources runtimeResources;
#endif
#include "BleTransport.h"
// BEGIN M104 native Settings admission.
#include "BleSettingsAdmission.h"
#include "BleSettingsServiceWindow.h"
// END M104 native Settings admission.
#include "WallClock.h"
#if defined(CONFIG_MT_GPIO_MODULES)
#include "modules/GpioModuleEdit.h"
#include "modules/GpioModuleRuntime.h"
#include "modules/GpioModuleChannels.h"
#include "modules/GpioModuleService.h"
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
#include "modules/StoreForwardEdit.h"
#include "modules/StoreForwardRuntime.h"
#include <esp_psram.h>
#endif
#include "ZephyrSettings.h"
#include "RebootSchedule.h"
#include "ui/UiConfigJournal.h"
#include "ui/UiConfigCommit.h"
#include "admin/HardwarePinInventory.h"
#include "admin/PreferenceArchive.h"
#include "admin/PreferenceSections.h"
#include "pki/verification/KeyVerificationService.h"
#include "update/FirmwareUpdateController.h"
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
#include "update/SignedOta.h"
#endif
#include "UsbBootloader.h"
#include "NodeJournal.h"
#include "NodeAdmin.h"
#include "SettingsEdit.h"
#include "SettingsCommit.h"
#include "network/WifiStationAdmin.h"
#include "network/WifiRuntime.h"
#include "network/TcpTransport.h"
#include "network/MqttGatewayAdmin.h"
#include "network/MqttChannelUplink.h"
#include "network/MqttPkiUplink.h"
#include "network/MqttMapReporter.h"
#include "network/MqttBootSnapshot.h"
#if defined(CONFIG_MT_MQTT_GATEWAY)
#include "network/MqttRuntime.h"
#if defined(CONFIG_MT_M119_TLS_TEST)
#include "network/M119TlsTestCa.h"
#elif defined(CONFIG_MQTT_LIB_TLS)
#include "network/MqttRootCa.h"
#endif
#include "MqttServiceEnvelopeDecoder.h"
#endif
#include "DeviceSettingsCommit.h"
#include "BluetoothJournal.h"
#include "BluetoothSettingsCommit.h"
#include "PendingBluetoothAdmin.h"
#include "ModuleController.h"
#include "modules/RouterLateRoleDefaults.h"
#ifdef CONFIG_MT_T096_ROUTER_ONLY
#include "modules/RouterInitialSettings.h"
#endif
#include "modules/RouterRoleDefaults.h"
#include "modules/OperatingRole.h"
#include "modules/CompletionEdit.h"
#include "modules/CompletionRuntime.h"
#include "modules/RangeTestLog.h"
#if defined(CONFIG_MT_PHONE_FILES)
#include "stock/files/PhoneFiles.h"
#include "stock/files/RangeFile.h"
#endif
#include "input/CannedGpioInput.h"
#include "notifications/RingtoneCommit.h"
#if defined(CONFIG_MT_TAK_TRACKER)
#include "modules/TakRoleDefaults.h"
#endif
#include "modules/BeaconController.h"
#include "modules/CannedMessageCommit.h"
#include "stock/waypoint/WaypointHistory.h"
#if defined(CONFIG_MT_MAX30102_OPTICAL)
#include "stock/telemetry/max30102/OpticalWindow.h"
#endif
#if defined(CONFIG_MT_SCD4X_ADMIN)
#include "stock/telemetry/ZephyrScd4xAdmin.h"
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
#include "stock/telemetry/accessory/Accessories.h"
#endif
#if defined(CONFIG_MT_SHTXX_ADMIN)
#include "stock/telemetry/ZephyrShtAdmin.h"
#endif
#if defined(CONFIG_MT_GENERIC_THREAD) || defined(CONFIG_MT_DROPZONE)
#include "stock/optional/OptionalModules.h"
#endif
#if defined(CONFIG_MT_DROPZONE)
#include "stock/optional/DropzoneBoard.h"
#endif
#if defined(CONFIG_MT_SERIAL_MODULE)
#include "serial/ZephyrSerial.h"
#include "stock/peripherals/SerialPosition.h"
#include "stock/peripherals/SerialWeather.h"
#endif
#include "notifications/NotificationEdit.h"
#include "notifications/ZephyrNotification.h"
#include "NormalTxStartup.h"
#include "UsbSerialJtagTx.h"
#include "UsbSessionEpoch.h"
#include "security/ConsoleSecurity.h"
#include "security/ApiLogBuffer.h"
#if defined(CONFIG_MT_BLE_LOG_RADIO)
#include "security/LogRadioWire.h"
#endif
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
#include "pki/ZephyrEntropy.h"
#include "pki/ZephyrPkiStorage.h"
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
#include "pki/ZephyrExpandedContacts.h"
#include "pki/UsbStorageCommand.h"
#endif
#include "pki/PkiPeerAdministration.h"
#include "pki/PsaPkiBackend.h"
#include "pki/PkiStartup.h"
#include "pki/NvsProvisioningSettings.h"
#include "pki/PkiFreshUsbIngress.h"
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
#include "pki/provisioning/StockImportConsent.h"
#endif
#include "pki/psa-threading/ZephyrPsaThreading.h"
#endif
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
#include "pki/LegacyTailTools.h"
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
#include "pki/NvsProvisioningSettings.h"
#endif
#include "pki/PkiUsbIngress.h"
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
#include "pki/UsbLifecycleCommand.h"
#include "storage/ZephyrSparePartitionStatus.h"
#if defined(CONFIG_BOARD_HELTEC_T096) && CONFIG_BOARD_HELTEC_T096
#include "storage/CapacityProbe.h"
#endif
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
#include "pki/UsbContactsCommand.h"
#endif
#endif
#if defined(CONFIG_MT_DISPLAY)
#include "ui/Display.h"
#include "ui/Controller.h"
#include "ui/CannedMenu.h"
#else
#include "stock/ui/HeadlessButton.h"
#include <zephyr/drivers/gpio.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/lora/sx126x_ext.h>
#include <zephyr/drivers/lora/sx126x_bench.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/settings/settings.h>
#include "ZephyrHardware.h"
#include "stock/ui/ZephyrBuzzerFeedback.h"
#include "gnss/ZephyrGnss.h"
#include "gnss/PositionController.h"
#include "environment/ZephyrEnvironment.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(semtech_sx1262)==1);
BUILD_ASSERT(!IS_ENABLED(CONFIG_NETWORKING)||(IS_ENABLED(CONFIG_BOARD_XIAO_ESP32S3)&&
  IS_ENABLED(CONFIG_MT_WIFI_STATION)&&IS_ENABLED(CONFIG_MT_TCP_COMPANION)),
  "networking requires the explicit XIAO WiFi/TCP profile");
BUILD_ASSERT(!IS_ENABLED(CONFIG_MT_LTO_RUNTIME)||(IS_ENABLED(CONFIG_BOARD_HELTEC_T096)&&
  IS_ENABLED(CONFIG_LTO)&&IS_ENABLED(CONFIG_ISR_TABLES_LOCAL_DECLARATION)&&
  IS_ENABLED(CONFIG_MT_PKI_STOCK_TRIAL)&&IS_ENABLED(CONFIG_MT_BLE_NATIVE_STORE)));
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BOARD_HELTEC_T096)||!IS_ENABLED(CONFIG_MT_ALLOW_TX)||IS_ENABLED(CONFIG_MT_T096_OWNER_TX),
             "T096 TX requires the explicit owner-test profile");
BUILD_ASSERT(!IS_ENABLED(CONFIG_MT_NORMAL_TX_DEFAULT)||(IS_ENABLED(CONFIG_MT_ALLOW_TX)&&IS_ENABLED(CONFIG_MT_NORMAL_ENDPOINT)),
             "normal TX default requires the normal endpoint and hardware TX gate");
BUILD_ASSERT(!IS_ENABLED(CONFIG_MT_NORMAL_TX_DEFAULT)||!IS_ENABLED(CONFIG_MT_PKI_TARGET_RUNTIME)||
             IS_ENABLED(CONFIG_MT_PKI_STOCK_TRIAL)||IS_ENABLED(CONFIG_MT_STORE_FORWARD_PSRAM),
             "private TX requires an explicit stock-PKI trial or StoreForward server profile");
static_assert(mt::NodeAdmin::CommittedRxFault==mt::Companion::Host::EditCommittedDegraded,
              "NodeDB committed/degraded outcome must cross the companion host boundary unchanged");
static const device *const radioDevice=DEVICE_DT_GET(DT_ALIAS(lora0));
#if (defined(CONFIG_MT_PKI_STOCK_TRIAL) && MT_PKI_LEGACY_TAIL_TOOLS_ENABLED) || defined(CONFIG_MT_XIAO_LARGE_NODEDB)
static const device *preparationWatchdog=nullptr;
static int preparationWatchdogChannel=-1;
#endif
struct Rx {uint8_t data[255];uint16_t length;int16_t rssi;int8_t snr;uint8_t snrRaw;bool snrKnown;};
K_MSGQ_DEFINE(rxQueue,sizeof(Rx),8,4);
K_MUTEX_DEFINE(serviceLock);
static atomic_t beaconOffProfileRx;
static mt::stock::telemetry::NoiseFloor radioNoiseFloor;
static atomic_t radioRxGood,radioRxBad;
#ifdef CONFIG_MT_NORMAL_ENDPOINT
static atomic_t agcLastGood;
static mt::AgcMaintenance agcSchedule;
#endif
static void notifyPowerActivity();
#if defined(CONFIG_MT_DISPLAY)
static void lockUi(void *) {k_mutex_lock(&serviceLock,K_FOREVER);}
static void unlockUi(void *) {k_mutex_unlock(&serviceLock);}
static mt::ui::Controller uiController(nullptr,lockUi,unlockUi);
static uint32_t cannedPacketId=0;
#endif
#if !defined(CONFIG_MT_M119_TLS_TEST)
static atomic_t drops;
#endif
static void receive(const device *dev,uint8_t *p,uint16_t n,int16_t rssi,int8_t snr,
                    const sx126x_packet_status *status,void *) {
#if defined(CONFIG_MT_M119_TLS_TEST)
  // Connection-only image: discard radio callbacks before they enter RX queues.
  (void)dev;(void)p;(void)n;(void)rssi;(void)snr;(void)status;
  return;
#else
  // An excursion enables this only after the home RX stop/drain boundary.
  // Off-profile native frames never enter the home application admission path.
  if(atomic_get(&beaconOffProfileRx))return;
  // Count terminal home-radio receives before admission or software queue loss.
  // A NULL callback with failed RX restoration signals a modem fault.
#ifdef CONFIG_MT_NORMAL_ENDPOINT
  if(p&&n>=16&&n<=255)atomic_set(&agcLastGood,k_uptime_get_32());
#endif
  if(p&&n<=255)atomic_inc(n>=16?&radioRxGood:&radioRxBad);
  else if(!p&&!n&&sx126x_bench_health(dev)==1)atomic_inc(&radioRxBad);
  notifyPowerActivity();
  Rx event{};event.snr=snr;event.rssi=rssi;
  // Driver's NULL/0 notification is classified against atomic health in main.
  if((!p&&n)||n>255){atomic_inc(&drops);return;}
  // Metadata belongs only to this successful payload; copy before returning.
  event.snrKnown=p&&n&&status&&status->snr_known;
  if(event.snrKnown)event.snrRaw=status->snr_raw;
  event.length=n;if(n)memcpy(event.data,p,n);
  if(k_msgq_put(&rxQueue,&event,K_NO_WAIT)!=0)atomic_inc(&drops);
#endif
}
/* Successful restoration is the only clean retry.  A failed restore may have
 * partially reconfigured the modem before returning, so propagate its hard
 * error; only the driver's settlement path may report verified standby. */
static int storage_resume_outcome(int restore_result) {
  return restore_result==0?-EAGAIN:restore_result;
}
static void recordApiEvent(mt::api::ApiLogBuffer::Event event,int32_t value);
static void observeRemoteNativeTx(uint32_t wireId,int result);
// CPU-only protocol/private state uses the initialized PSRAM BSS window in
// this combined profile. Service is owner-thread state; NativeRadio stages TX
// into internal bytes so radio/DMA never borrows a Service PSRAM frame.
// Radio, storage, ISR state and stacks stay internal.
#if defined(CONFIG_SOC_ESP32S3) && defined(CONFIG_ESP_SPIRAM) && \
    defined(CONFIG_MT_MQTT_GATEWAY) && defined(CONFIG_MT_PKI_TARGET_RUNTIME)
#define MT_SERVICE_EXTERNAL_RAM 1
#define MT_WIFI_PSRAM_BSS(name) \
  __attribute__((section(".ext_ram.bss.mt_wifi_" #name), aligned(16)))
#else
#define MT_SERVICE_EXTERNAL_RAM 0
#define MT_WIFI_PSRAM_BSS(name)
#endif
class NativeRadio final:public mt::Radio {
  k_poll_signal signal_{};
#if MT_SERVICE_EXTERNAL_RAM
  // One serialized active transmission, retained through native completion or
  // a verified abort. The NativeRadio object itself stays in internal RAM.
  uint8_t txInternal_[255]{};
#endif
  lora_modem_config modem_{};
  bool configured_=false;
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
  bool stagedReceiveOnly_=true,appliedReceiveOnly_=true;
#endif
  uint16_t busyDiagnostic_=0;
  uint32_t stopCalls_=0,storageCalls_=0;
#ifdef CONFIG_MT_NORMAL_ENDPOINT
  bool joinedForAgc_=false;
#endif
  uint16_t loggedTxBytes_=0;uint32_t loggedTxWireId_=0;bool loggedTxPending_=false;
public:
  uint32_t diagnosticStopCalls()const{return stopCalls_;}
  uint32_t diagnosticStorageCalls()const{return storageCalls_;}
  void init(){k_poll_signal_init(&signal_);}
  bool diagnostic(mt::NativeStages &out)const override {
    out={};struct sx126x_airtime_observer_info value{};
    if(sx126x_airtime_observer_info(radioDevice,&value))return false;
    auto copy=[](const sx126x_stage_info &s){return mt::NativeStage{s.calls,s.failed_call,s.stage,s.result};};
    out.send=copy(value.send_stage);out.config=copy(value.config_stage);out.restart=copy(value.restart_stage);return true;
  }
  bool beaconIsolationSupported()const override{return true;}
  void beaconRxIsolation(bool active)override{atomic_set(&beaconOffProfileRx,active?1:0);}
  bool ready() override {return device_is_ready(radioDevice);}
  int configure(const mt::Config &config) override {
    mt::LoRaPresets::Profile p{};mt::USChannelSelection s{};
    if(!config.valid()||!config.resolveRadio(p,s))return -EINVAL;
    lora_modem_config c{};c.frequency=s.frequencyHz;
    switch(p.bandwidthHz){case 7800:c.bandwidth=BW_7_KHZ;break;case 10400:c.bandwidth=BW_10_KHZ;break;
      case 15600:c.bandwidth=BW_15_KHZ;break;case 20800:c.bandwidth=BW_20_KHZ;break;case 31250:c.bandwidth=BW_31_KHZ;break;
      case 41700:c.bandwidth=BW_41_KHZ;break;case 62500:c.bandwidth=BW_62_KHZ;break;case 125000:c.bandwidth=BW_125_KHZ;break;
      case 250000:c.bandwidth=BW_250_KHZ;break;case 500000:c.bandwidth=BW_500_KHZ;break;default:return -ENOTSUP;}
    c.datarate=static_cast<lora_datarate>(p.spreadingFactor);
    c.coding_rate=static_cast<lora_coding_rate>(p.codingDenominator-4);c.preamble_len=16;c.sync_word=0x2b;
    c.tx_power=int8_t(config.effectivePower(IS_ENABLED(CONFIG_BOARD_HELTEC_T096)));
    c.cad.mode=LORA_CAD_MODE_LBT;c.cad.symbol_num=LORA_CAD_SYMB_4;
#if defined(CONFIG_MT_NORMAL_ENDPOINT)
    // Stock SX126x/RadioLib defaults at the actual four-symbol scan.
    c.cad.detection_peak=uint8_t(p.spreadingFactor+13);c.cad.detection_minimum=10;
#endif
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
    stagedReceiveOnly_=mt::radio::RegionPolicy::fixedReceiveOnly(config.regionCode());
    if(stagedReceiveOnly_)appliedReceiveOnly_=true;
#endif
    modem_=c;configured_=true;return 0;
  }
  int start() override {
    if(!configured_)return -EINVAL;
    k_msgq_purge(&rxQueue);
    int ret=lora_config(radioDevice,&modem_);if(ret)return ret;
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
    ret=restore();
    if(!ret)appliedReceiveOnly_=stagedReceiveOnly_;
    return ret;
#else
    return restore();
#endif
  }
  int restore() override {
#ifdef CONFIG_MT_NORMAL_ENDPOINT
    const bool joined=joinedForAgc_;
    joinedForAgc_=false;
    if(joined&&!receiveEventPending()&&agcSchedule.take(k_uptime_get_32(),uint32_t(atomic_get(&agcLastGood)))) {
      bool calibrated=false;
      const int result=sx126x_recv_after_agc(radioDevice,receive,nullptr,&calibrated);
      if(!result&&calibrated)agcSchedule.succeeded(k_uptime_get_32());
      return result;
    }
#endif
    return sx126x_recv_async_packet(radioDevice,receive,nullptr);
  }
  int stop() override {
#ifdef CONFIG_MT_NORMAL_ENDPOINT
    joinedForAgc_=false;
#endif
    loggedTxPending_=false; // Abort has no proved successful TX completion.
    if(stopCalls_!=UINT32_MAX)++stopCalls_;
    const int ret=ready()?sx126x_bench_abort(radioDevice):-ENODEV;
    if(!ret){
      mt::notification::radioActive(false);
#if MT_SERVICE_EXTERNAL_RAM
      bench::wipeKeyBuffer(txInternal_,sizeof(txInternal_));
#endif
    }
    k_msgq_purge(&rxQueue);return ret;
  }
  bool receiveEventPending()const override{return k_msgq_num_used_get(&rxQueue)!=0;}
  int stopForStorage() override {
#ifdef CONFIG_MT_NORMAL_ENDPOINT
    joinedForAgc_=false;
#endif
    if(storageCalls_!=UINT32_MAX)++storageCalls_;
    // A pre-existing event belongs to the service and must be drained first.
    if(receiveEventPending())return -EAGAIN;
    const int ret=ready()?sx126x_pause_for_storage(radioDevice):-ENODEV;
    if(ret)return ret;
    // The driver froze RX and joined DIO work. A callback that completed
    // before that barrier has published its event; preserve it and resume RX.
    if(k_msgq_num_used_get(&rxQueue)) {
      return storage_resume_outcome(restore());
    }
#ifdef CONFIG_MT_NORMAL_ENDPOINT
    // The driver joined both IRQ work sources and proved an empty final IRQ.
    joinedForAgc_=true;
#endif
    return 0;
  }
  int health() override {return sx126x_bench_health(radioDevice);}
  bool busy() override {return sx126x_is_receiving_diagnostic(radioDevice,&busyDiagnostic_);}
  uint16_t busyDiagnostic()const override {return busyDiagnostic_;}
  uint32_t airtime(size_t n) override {return lora_airtime(radioDevice,n);}
  int send(uint8_t *p,size_t n) override {
#ifdef CONFIG_MT_NORMAL_ENDPOINT
    joinedForAgc_=false;
#endif
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
    if(appliedReceiveOnly_)return -EACCES;
#endif
#if MT_SERVICE_EXTERNAL_RAM
    if(!p||!n||n>sizeof(txInternal_))return -EINVAL;
    memcpy(txInternal_,p,n);p=txInternal_;
#endif
    k_poll_signal_reset(&signal_);
    loggedTxWireId_=p&&n>=16?bench::le32(p+8):0;
    const int result=lora_send_async(radioDevice,p,n,&signal_);
#if MT_SERVICE_EXTERNAL_RAM
    // A rejected native call owns no pending frame/completion. Clear the
    // staging copy on this path too, including a busy/retry outcome.
    if(result)bench::wipeKeyBuffer(txInternal_,sizeof(txInternal_));
#endif
    loggedTxPending_=false;
    if(!result) {
      mt::notification::radioActive(true);
      if(n<=255){loggedTxBytes_=uint16_t(n);loggedTxPending_=true;
        recordApiEvent(mt::api::ApiLogBuffer::Event::RadioTxQueuedBytes,int32_t(n));}
    } else {recordApiEvent(mt::api::ApiLogBuffer::Event::RadioTxRejectedErrno,result);
      observeRemoteNativeTx(loggedTxWireId_,result);}
    return result;
  }
  bool completion(int &result) override {
    unsigned signalled=0;k_poll_signal_check(&signal_,&signalled,&result);
    if(signalled) {
#if MT_SERVICE_EXTERNAL_RAM
      bench::wipeKeyBuffer(txInternal_,sizeof(txInternal_));
#endif
      mt::notification::radioActive(false);
      if(loggedTxPending_) {
        loggedTxPending_=false;
        observeRemoteNativeTx(loggedTxWireId_,result);
        recordApiEvent(result==0?mt::api::ApiLogBuffer::Event::RadioTxCompleteBytes:
          mt::api::ApiLogBuffer::Event::RadioTxFailureErrno,result==0?loggedTxBytes_:result);
      }
    }
    return signalled!=0;
  }
};
static NativeRadio radio;
static mt::Service service MT_WIFI_PSRAM_BSS(service)(radio,IS_ENABLED(CONFIG_MT_ALLOW_TX),0,
                           mt::board::Model,IS_ENABLED(CONFIG_MT_NORMAL_ENDPOINT));
// Both local transports are dispatched with serviceLock held and share this
// RAM-only UTC anchor. Uptime-based protocol and service deadlines stay separate.
// Called only by the Service owner, including synchronous telemetry replies.
// Drain-time attribution follows stock log-call accounting; callback delay can
// shift a boundary bucket. No payload bytes, IRQ reads, SPI or application locks.
static void drainChannelObservations(mt::ChannelMeter &meter,void *) {
  meter.supported(service.receiving()&&sx126x_airtime_observer_supported(radioDevice));
  if(sx126x_airtime_observer_take_loss(radioDevice))meter.uncertain(zt::ChannelEvent::NativeLoss);
  sx126x_airtime_observation event{};
  unsigned count=0;
  for(;count<16&&sx126x_airtime_observer_take(radioDevice,&event)==0;++count)
    meter.observeRx(event.airtime_ms,event.known&&event.observed_at_ms!=UINT64_MAX,event.irq,event.length);
  // Preserve the original short-circuit: a full drain does not consume loss.
  if(count==16)meter.uncertain(zt::ChannelEvent::DrainLimit);
  else if(sx126x_airtime_observer_take_loss(radioDevice))meter.uncertain(zt::ChannelEvent::NativeLoss);
}
static mt::WallClock wallClock;
// Serialized by serviceLock; no logging/ISR callback borrows Companion scratch.
static mt::api::ApiLogBuffer apiLogs;
#if defined(CONFIG_MT_BLE_LOG_RADIO)
static void publishSafeRadioLog(uint32_t token) {
  // recordApiEvent can run deep in a radio or admin handler. The owner lock
  // serializes this dedicated fixed scratch independently of PhoneAPI's workspace.
  static meshtastic_LogRecord scratch MT_WIFI_PSRAM_BSS(log_radio_scratch);
  mt::ble::LogRadioValue value;
  if(mt::api::encodeLogRadio(apiLogs,scratch,value,[](uint32_t utc){
#if defined(CONFIG_MT_LOCAL_CLOCK)
    return service.config.localClock(utc);
#else
    return utc;
#endif
  }))mt::ble::publishLogRadio(token,value);
}
#endif
static void recordApiEvent(mt::api::ApiLogBuffer::Event event,int32_t value=0) {
  if(serviceLock.owner!=k_current_get()||!serviceLock.lock_count)return;
  const uint32_t utc=wallClock.at(uint64_t(k_uptime_get()));
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  const uint32_t token=apiLogs.enabled()?mt::ble::logRadioSession():0;
  if(apiLogs.record(event,value,utc!=0,utc)&&token)publishSafeRadioLog(token);
#else
  (void)apiLogs.record(event,value,utc!=0,utc);
#endif
}
// Scratch is shared only while the current thread owns serviceLock. Framing,
// replay and session ownership stay in each transport's Companion instance.
static mt::companion::CompanionWorkspace companionWorkspace MT_WIFI_PSRAM_BSS(companion_workspace);
static mt::pki::verification::KeyVerificationService keyVerification MT_WIFI_PSRAM_BSS(key_verification)(service);
static __attribute__((noinline)) mt::Companion::WorkspaceProof companionWorkspaceProof() {
  return mt::Companion::WorkspaceProof(serviceLock.owner==k_current_get()&&serviceLock.lock_count!=0);
}
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
static mt::pki::ZephyrEntropy privateEntropy;
static mt::pki::ZephyrPkiStorage privateStorage(serviceLock,radio,privateEntropy);
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
static mt::pki::ZephyrExpandedContactFlash expandedFlash;
static mt::pki::ExpandedContacts expandedContacts MT_WIFI_PSRAM_BSS(expanded_contacts)(expandedFlash);
static const device *expandedWatchdog;static int expandedChannel=-1;
static void feedExpandedWatchdog(){if(expandedWatchdog&&expandedChannel>=0)(void)wdt_feed(expandedWatchdog,expandedChannel);}
#endif
static mt::pki::PsaPkiBackend privateBackend;
static mt::experimental::PkiCrypto privateCrypto(privateBackend);
static mt::pki::PkiStartup privateStartup MT_WIFI_PSRAM_BSS(pki_startup)(privateStorage.storage(),privateCrypto,privateBackend,privateEntropy);
#endif
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
static mt::ble::BleSettingsServiceWindow<mt::Service,NativeRadio> bleStorageWindow(service,radio);
#endif
static mt::SettingsJournal journal;
#if defined(CONFIG_MT_RF_ADMIN_GET)
static mt::admin::PendingRemoteConfig pendingRemoteConfig MT_WIFI_PSRAM_BSS(pending_remote_config);
static bool remoteConfigHeld=false,remoteResponseObserverReady=false;
enum class RemoteAdminLane:uint8_t {None,Private,Shared};
struct RemoteMutationContext {RemoteAdminLane lane=RemoteAdminLane::None;uint32_t peer=0,request=0;};
static RemoteMutationContext remoteMutation{},pendingRemoteOwner{};
struct RemoteMutationScope {
  RemoteMutationContext previous;
  RemoteMutationScope(RemoteAdminLane lane,uint32_t peer,uint32_t request):previous(remoteMutation){remoteMutation={lane,peer,request};}
  ~RemoteMutationScope(){remoteMutation=previous;}
};
static mt::admin::ConfigFingerprint configFingerprint(const mt::Config &config) {
  mt::admin::ConfigFingerprint result{};
  crypto_blake2b(result.bytes,sizeof(result.bytes),config.bytes,sizeof(config.bytes));return result;
}
#endif
static bool remoteConfigPending(){
#if defined(CONFIG_MT_RF_ADMIN_GET)
  return pendingRemoteConfig.pending()||remoteConfigHeld;
#else
  return false;
#endif
}
static void observeRemoteNativeTx(uint32_t wireId,int result) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(serviceLock.owner==k_current_get()&&serviceLock.lock_count&&pendingRemoteConfig.pending())
    (void)pendingRemoteConfig.actualNativeTxResult(wireId,result,k_uptime_get_32());
#else
  (void)wireId;(void)result;
#endif
}
static mt::admin::PreferenceArchive preferenceArchive;
static mt::admin::PreferenceArchive::SnapshotDomain preferenceDomain;
static bool preferenceDomainReady=false,preferenceRecoveryHeld=false;
static int preferenceCommand(pb_size_t operation,uint32_t now,mt::UsbSessionEpoch::RestoreReceipt *receipt);
static int resumePreferenceRestore(uint32_t now);
static mt::ZephyrSettings settings;
#if defined(CONFIG_SOC_ESP32S3)
static uint32_t myBootCount=0;
#endif
static mt::ui::UiConfigJournal uiConfigJournal;
static mt::update::FirmwareUpdateController firmwareUpdate;
using mt::ScheduledOwnerAction;
static bool updateFailureReported=false;
static bool explicitShutdownDue=false;
static atomic_t shutdownAdmissionClosed;
#if defined(CONFIG_MT_GPIO_MODULES)
static mt::modules::GpioModuleJournal gpioModuleJournal;
static mt::modules::GpioModuleEdit gpioModuleEdit(service,gpioModuleJournal,settings);
static mt::modules::GpioModuleRuntime gpioModuleRuntime(mt::modules::zephyrGpioModuleBackend());
static void gpioLocalPacket(void *,const uint8_t *,size_t,uint32_t,unsigned);
static mt::modules::GpioModuleService gpioModuleService(service,gpioModuleRuntime,gpioLocalPacket,nullptr);
static void gpioModuleFrame(void *,const uint8_t *,size_t,const bench::RouteMetadata &,unsigned,uint32_t,uint32_t);
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
static mt::modules::StoreForwardJournal storeForwardJournal;
static mt::modules::StoreForwardEdit storeForwardEdit(service,storeForwardJournal,settings);
static mt::modules::StoreForwardRuntime storeForwardRuntime;
BUILD_ASSERT(DT_NODE_EXISTS(DT_NODELABEL(psram0)), "StoreForward requires PSRAM");
BUILD_ASSERT(DT_PROP(DT_NODELABEL(psram0), size)>=8*1024*1024, "StoreForward requires the 8 MiB XIAO profile");
static bool storeForwardPsramReady=false;
static void storeForwardFrame(void *,const uint8_t *frame,size_t size,const bench::RouteMetadata &metadata,unsigned channel){
  const int64_t monotonicMs=k_uptime_get();
  const uint64_t now=monotonicMs>=0?uint64_t(monotonicMs):0;
  const bool uptimeValid=monotonicMs>=0&&now/1000u<=UINT32_MAX;
  const uint32_t uptime=uptimeValid?uint32_t(now/1000u):0;
  storeForwardRuntime.receive(service,frame,size,metadata,uint8_t(channel),uint32_t(now),wallClock.at(now),uptime);
}
static const mt::modules::StoreForwardArchive *storeForwardArchive(void *){
  return storeForwardRuntime.localArchive();
}
static bool storeForwardLocalText(void *,const mt::modules::StoreForwardArchive::MessageView &message){
  return storeForwardRuntime.archiveLocalText(service,message);
}
#endif
static mt::BluetoothJournal bluetoothJournal;
static mt::ble::PendingBluetoothAdmin pendingBluetooth;
static mt::ble::PendingBluetoothAdmin::Request bluetoothWork;
static bool bluetoothWorkReady=false;
static bool bluetoothConfigReady=false;
static mt::SettingsEdit settingsEdit(service,journal,settings);
static mt::RebootSchedule rebootSchedule;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
static mt::NodeJournal nodeJournal MT_WIFI_PSRAM_BSS(node_journal);
#else
static mt::NodeJournal nodeJournal;
#endif
static mt::mqtt::OnlineNodeWindow hotNodeWindow;
static mt::mqtt::HotNodeMetadataJournal hotNodeJournal;
static uint8_t hotNodePublicKey[32]{};
static bool hotNodeCountAvailable=false,hotNodeDirty=false,hotNodeClockSidecarPending=false;
static uint32_t hotNodeDirtyAt=0,hotNodeLastWrite=0;
static bool syncHotNodeFlags();
static void markHotNodeDirty(uint32_t now);
static mt::ModuleJournal moduleJournal;
static mt::ModuleEdit moduleEdit(service,moduleJournal,settings);
static mt::NotificationJournal notificationJournal;
static mt::NotificationEdit notificationEdit(service,notificationJournal,settings);
static mt::NotificationController notifications;
static mt::network::WifiStationJournal wifiJournal;
static mt::network::WifiStationEdit wifiEdit(service,wifiJournal,settings);
static mt::network::WifiStationAdmin wifiAdmin(wifiEdit,settingsEdit,rebootSchedule);
#if defined(CONFIG_MT_POWER_STRESS)
#include "stock/platform/PowerStressModule.h"
static mt::stock::powerstress::Module powerStressModule;
#endif
static mt::ModuleController modules;
static mt::modules::CompletionJournal completionJournal MT_WIFI_PSRAM_BSS(completion_journal);
static mt::modules::CompletionEdit completionEdit MT_WIFI_PSRAM_BSS(completion_edit)(service,completionJournal,settings);
static mt::stock::waypoint::WaypointModule waypoints MT_WIFI_PSRAM_BSS(waypoint_history);
static mt::stock::waypoint::WaypointHistory waypointHistory;
static mt::stock::waypoint::WaypointHistory::Domain waypointHistoryDomain(){
  mt::stock::waypoint::WaypointHistory::Domain domain{};
  domain.node=service.route.identity();domain.channelGeneration=journal.channelGeneration();
  crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,sizeof(domain.channelHash));
  const auto flags=service.config.channelDomainFlags();crypto_blake2b_update(&hash,&flags,sizeof(flags));
  for(unsigned i=0;i<8;++i){
    mt::Config::DomainChannel channel{};service.config.readDomainChannel(i,channel);
    crypto_blake2b_update(&hash,reinterpret_cast<const uint8_t*>(&channel),sizeof(channel));
  }
  crypto_blake2b_final(&hash,domain.channelHash);
  if(preferenceDomainReady)memcpy(domain.publicKey,preferenceDomain.publicKey,sizeof(domain.publicKey));
  return domain;
}
static mt::stock::waypoint::WaypointHistory::Contacts waypointHistoryContacts(){
  mt::stock::waypoint::WaypointHistory::Contacts contacts{};
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  contacts.known=preferenceDomainReady&&privateStorage.storage().contactRevision(contacts.generation);
#endif
  return contacts;
}
#if defined(CONFIG_MT_SATELLITE_HISTORY)
#include "companion/SatelliteHistory.h"
#include "companion/SatelliteVolume.h"
static mt::companion::SatelliteVolume satelliteVolume;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
#include "companion/LargeNodeVolume.h"
static mt::companion::LargeNodeVolume extendedNodes(satelliteVolume);
#endif
static mt::companion::SatelliteHistory satelliteHistory;
static uint8_t satelliteEpoch[16]{};
static bool satelliteMayInitialize=false;
static bool directoryDomain(mt::companion::SatelliteHistory::Domain &d,uint8_t &channels){
  // Only the verified local identity and channel lifetime bind this file.
  // An owner-contact prewrite can already hold the independent PKI lease.
  if(!preferenceDomainReady||!nodeJournal.ready()||!service.nodes.ready())return false;
  auto cfg=waypointHistoryDomain();
  d.node=cfg.node;d.channelGeneration=cfg.channelGeneration;
  memcpy(d.publicKey,cfg.publicKey,sizeof(d.publicKey));
  memcpy(d.channelDigest,cfg.channelHash,sizeof(d.channelDigest));memcpy(d.epoch,satelliteEpoch,sizeof(d.epoch));
  channels=0;
  for(unsigned i=0;i<8;i++){mt::PrimaryChannel::Selection channel{};
    if(service.config.channelSelection(i,channel))channels|=uint8_t(1u<<i);
    mt::PrimaryChannel::clear(channel);}
  return true;
}
static bool satelliteDomain(mt::companion::SatelliteHistory::Domain &d,uint8_t &channels){
  return waypointHistoryContacts().known&&directoryDomain(d,channels);
}
#if defined(CONFIG_MT_PEER_DIRECTORY)
#include "companion/PeerDirectoryOwner.h"
#include "companion/PeerDirectoryVolume.h"
#if defined(CONFIG_BOARD_HELTEC_T096)
static constexpr unsigned DirectoryLimit=119;
#else
static constexpr unsigned DirectoryLimit=199;
#endif
using DirectoryOwner=mt::companion::PeerDirectoryOwner<DirectoryLimit,mt::LocalClientCount>;
static DirectoryOwner peerDirectory;
static mt::companion::PeerDirectoryVolume directoryVolume;
#if defined(CONFIG_MT_WARM_PEERS)
#include "companion/WarmPeerOwner.h"
#include "companion/WarmPeerVolume.h"
using WarmOwner=mt::companion::WarmPeerOwner<DirectoryLimit==119?100:2000>;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
static WarmOwner warmPeers MT_WIFI_PSRAM_BSS(warm_peers);
#else
static WarmOwner warmPeers;
#endif
static mt::companion::WarmPeerVolume warmVolume;
#if defined(CONFIG_MT_PAGED_SATELLITES)
#include "companion/SatellitePageOwner.h"
#include "companion/SatellitePageVolume.h"
using ReportPages=mt::companion::SatellitePageOwner<DirectoryLimit==119?40:199,mt::LocalClientCount>;
static ReportPages reportPages;
static mt::companion::SatellitePageVolume reportVolume;
static bool reportFilesCompatible=false;
static void bindReports(){
  if(peerDirectory.ready()&&reportFilesCompatible)reportPages.bind(peerDirectory.warmDomain().directory);
  else reportPages.close();
}
#endif
static void bindWarm(){
    if(peerDirectory.ready())warmPeers.bind(peerDirectory.warmDomain(),peerDirectory.warmWithdrawals());
    else warmPeers.suspend();
#if defined(CONFIG_MT_PAGED_SATELLITES)
  bindReports();
#endif
}
#endif
K_MUTEX_DEFINE(directoryLock);
static uint32_t directorySettingsGen=UINT32_MAX,directoryNodeGen=0;
static unsigned directoryScan=0;
static bool directoryBound=false;
static DirectoryOwner::Domain bindDirectory(const mt::companion::SatelliteHistory::Domain &d){
  DirectoryOwner::Domain result{};result.node=d.node;memcpy(result.key,d.publicKey,sizeof(result.key));
  d.directoryChannels(result.channels);memcpy(result.epoch,d.epoch,sizeof(result.epoch));return result;
}
static void openDirectory(const mt::companion::SatelliteHistory::Domain &d){
  directoryScan=0;directoryNodeGen=nodeJournal.generation();directorySettingsGen=journal.generation();
#if defined(CONFIG_MT_PAGED_SATELLITES)
  const int result=reportPages.openDirectory(peerDirectory,directoryVolume,bindDirectory(d),directoryNodeGen,
                                            reportVolume,warmVolume,reportFilesCompatible,satelliteHistory);
  if(result==-EXDEV)printk("Peer archive layout differs, existing files preserved\n");
#else
  const int result=peerDirectory.open(directoryVolume,bindDirectory(d),directoryNodeGen);
#endif
  directoryBound=!result;
#if defined(CONFIG_MT_WARM_PEERS)
  service.warmIdentityVeto([](uint32_t id){
    if(settingsEdit.open()||directorySettingsGen!=journal.generation()||k_mutex_lock(&directoryLock,K_NO_WAIT))return true;
    const bool refused=peerDirectory.refusesUnsignedIdentity(id)||warmPeers.refusesUnsignedIdentity(id);
    k_mutex_unlock(&directoryLock);return refused;
  });
  service.warmPositionRole([](uint32_t id,uint8_t &role){
    if(settingsEdit.open()||directorySettingsGen!=journal.generation()||k_mutex_lock(&directoryLock,K_NO_WAIT))return false;
    const bool found=peerDirectory.ready()&&warmPeers.positionRole(id,role);
    k_mutex_unlock(&directoryLock);return found;
  });
#endif
  nodeJournal.directoryFence([](uint32_t previous,uint32_t next){
    k_mutex_lock(&directoryLock,K_FOREVER);
    const int result=peerDirectory.fenceJournal(directoryVolume,previous,next);
    k_mutex_unlock(&directoryLock);return result;
  });
}
static void captureDirectory(){
  if(!satelliteVolume.mounted()||settingsEdit.open()||!nodeJournal.ready()||!preferenceDomainReady)return;
  if(k_mutex_lock(&directoryLock,K_NO_WAIT))return;
  if(directoryBound&&directorySettingsGen!=journal.generation()){
    mt::companion::SatelliteHistory::Domain d{};uint8_t channels=0;
    if(satelliteDomain(d,channels)){
      if(peerDirectory.domain(bindDirectory(d)))directorySettingsGen=journal.generation();
      else openDirectory(d);
    }else peerDirectory.close();
  }
#if defined(CONFIG_MT_PAGED_SATELLITES)
  if(directoryBound&&!peerDirectory.ready())satelliteHistory.pagedReady(false);
#endif
  directoryNodeGen=nodeJournal.generation();
#if defined(CONFIG_MT_PAGED_SATELLITES)
  peerDirectory.capture(service.nodes,directoryNodeGen,directoryScan,[](const mt::NodeCache::Entry &e){
    peerDirectory.observe(e,service.satellites.publicHeader(e.id()));
  });
#else
  peerDirectory.capture(service.nodes,directoryNodeGen,directoryScan);
#endif
#if defined(CONFIG_MT_WARM_PEERS)
  bindWarm();warmPeers.capture(service.nodes);
#if defined(CONFIG_MT_PAGED_SATELLITES)
  reportPages.reconcile(service.nodes,service.satellites,peerDirectory);
#endif
#endif
  k_mutex_unlock(&directoryLock);
}
static int forgetDirectory(uint32_t id,bool all=false){
  k_mutex_lock(&directoryLock,K_FOREVER);
  const int rc=mt::companion::withExistingHistory(satelliteVolume,[&](){
    if(!directoryBound||!peerDirectory.ready()){
      // Mount or epoch reads may have failed during startup. Rebind before
      // acknowledging any edit that could otherwise resurrect archived metadata.
      mt::companion::SatelliteHistory::Domain d{};uint8_t channels=0;
      if(!directoryDomain(d,channels))return -EAGAIN;
      if(settings.read("mtsat/epoch",d.epoch,sizeof(d.epoch))!=int(sizeof(d.epoch)))return -EAGAIN;
      memcpy(satelliteEpoch,d.epoch,sizeof(d.epoch));
      openDirectory(d);if(!peerDirectory.ready())return -EAGAIN;
    }
    const int result=all?peerDirectory.clear(directoryVolume):peerDirectory.forget(directoryVolume,id);
#if defined(CONFIG_MT_PAGED_SATELLITES)
    reportPages.withdraw(all?0:id);
#endif
#if defined(CONFIG_MT_WARM_PEERS)
    bindWarm();
#endif
    return result;
  });
  k_mutex_unlock(&directoryLock);return rc;
}
static int eraseDirectory(){
  k_mutex_lock(&directoryLock,K_FOREVER);peerDirectory.close();directoryBound=false;
  int rc=satelliteVolume.mounted()?directoryVolume.erase(DirectoryOwner::Directory::ResetPages):0;
#if defined(CONFIG_MT_WARM_PEERS)
  warmPeers.suspend();if(!rc&&satelliteVolume.mounted())rc=warmVolume.erase(WarmOwner::ResetPages);
#if defined(CONFIG_MT_PAGED_SATELLITES)
  reportPages.close();if(!rc&&satelliteVolume.mounted())rc=reportVolume.eraseAll();
#endif
#endif
  k_mutex_unlock(&directoryLock);return rc;
}
#endif
static void loadSatelliteHistory(bool allowInitialize,bool createEpoch=false){
  satelliteMayInitialize=allowInitialize;
  mt::companion::SatelliteHistory::Domain domain{};uint8_t channels=0;
  if(!satelliteDomain(domain,channels)||satelliteVolume.mount(allowInitialize))return;
  nodeJournal.enableReplayEpochs(service.nodes);
  int rc=settings.read("mtsat/epoch",domain.epoch,sizeof(domain.epoch));
  // Re-read after writing. A failed read must never replace an existing lifetime.
  if(rc==-ENOENT&&createEpoch&&!sys_csrand_get(domain.epoch,sizeof(domain.epoch))&&
     !settings.write("mtsat/epoch",domain.epoch,sizeof(domain.epoch)))
    rc=settings.read("mtsat/epoch",domain.epoch,sizeof(domain.epoch));
  if(rc!=int(sizeof(domain.epoch)))return;
  memcpy(satelliteEpoch,domain.epoch,sizeof(satelliteEpoch));
#if defined(CONFIG_MT_PAGED_SATELLITES)
  reportFilesCompatible=!reportVolume.legacySnapshot();
  if(!reportFilesCompatible)printk("Public report archive deferred, legacy private history preserved\n");
#else
  (void)satelliteHistory.load(satelliteVolume,service.nodes,service.satellites,domain,channels);
#endif
#if defined(CONFIG_MT_PEER_DIRECTORY)
  k_mutex_lock(&directoryLock,K_FOREVER);
  if(!directoryBound||!peerDirectory.ready())openDirectory(domain);
#if defined(CONFIG_MT_PAGED_SATELLITES)
  bindReports();satelliteHistory.pagedReady(reportPages.bound());
#endif
  k_mutex_unlock(&directoryLock);
#endif
}
#endif
static mt::modules::CompletionRuntime completionRuntime MT_WIFI_PSRAM_BSS(completion_runtime);
#if defined(CONFIG_MT_DISPLAY)
static bool dismissLocalNotificationInput(){
  const bool basic=mt::stock::notification::handleInputEvent(notifications.armed(),[]{notifications.stop();});
  const bool extended=completionRuntime.requestNoticeDismissal();
  return basic||extended;
}
#endif
static void notifyPowerActivity(){mt::power::ZephyrPowerBackend::notifyGlobalActivity();}
static mt::input::CannedGpioInput cannedGpio;
static mt::notification::RingtoneJournal ringtoneJournal MT_WIFI_PSRAM_BSS(ringtone_journal);
#if defined(CONFIG_MT_RANGE_TEST_LOG)
static mt::modules::RangeTestLog rangeLog;
#if defined(CONFIG_MT_PHONE_FILES)
static mt::stock::files::RangeFile phoneFileBackend(rangeLog);
static mt::stock::files::PhoneFiles phoneFiles(phoneFileBackend);
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
static mt::stock::files::TreeManifest publicManifest;
#endif
#endif
#endif
static uint8_t cannedGpioWire[128]{};static size_t cannedGpioWireSize=0;
static mt::modules::ExtendedModuleJournal extendedJournal;
static mt::modules::CannedMessageJournal cannedJournal;
#if defined(CONFIG_MT_RF_ADMIN_GET)
static mt::admin::AdminKeyJournal adminKeyJournal;
static mt::admin::RemoteHardwarePinCache remoteHardwarePinCache;
static mt::admin::RemoteHardwarePinJournal remoteHardwarePinJournal;
static bool remoteHardwarePinJournalReady=false;
static bool remoteHardwarePinDirty=false;
#endif
static mt::modules::ExtendedModuleEdit extendedEdit(service,extendedJournal,settings);
using FactoryPhase=mt::stock::admin::factory::Phase;
static bool factoryConfigResetScheduled=false;
#if MT_HAS_DEVICE_RESET
namespace deviceReset=mt::stock::admin::device_reset;
static deviceReset::ZephyrFloor resetFloor;
static deviceReset::Floor::Head resetHead;
static bool resetRecoveryBoot=false;
static mt::stock::admin::factory::EraseRetry resetBootRetry;
#endif
static mt::stock::admin::factory::EraseRetry factoryEraseRetry;
static uint32_t factoryEraseNode;
#if defined(CONFIG_MT_BME680_IAQ)
#include "stock/telemetry/bme680/ZephyrBme680.h"
#endif
static int eraseResetOwnedFiles(){
#if defined(CONFIG_MT_BME680_IAQ)
  const int bme=mt::stock::telemetry::bme680::retire();if(bme)return bme;
#endif
#if defined(CONFIG_MT_RANGE_TEST_LOG)
  auto eraseCsv=[](){return mt::modules::RangeTestLog::available()?rangeLog.removeFile("/static/rangetest.csv"):0;};
#endif
#if defined(CONFIG_MT_SATELLITE_HISTORY)
  const int rc=mt::companion::erasePrivateHistory(satelliteVolume,[&](){
#if defined(CONFIG_MT_PEER_DIRECTORY)
    const int dir=eraseDirectory();if(dir)return dir;
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
    const int nodes=extendedNodes.erase();if(nodes)return nodes;
#endif
#endif
#if defined(CONFIG_MT_RANGE_TEST_LOG) && defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    if(!strcmp(CONFIG_MT_RANGE_TEST_LOG_DIR,"/zt-history/public")){
      return mt::companion::eraseSharedRangeLog([](){
        fs_dirent entry{};return fs_stat("/zt-history/public",&entry);
      },eraseCsv);
    }
#endif
    return 0;
  });
  if(rc)return rc;
#if defined(CONFIG_MT_RANGE_TEST_LOG) && defined(CONFIG_MT_PHONE_PUBLIC_TREE)
  if(!strcmp(CONFIG_MT_RANGE_TEST_LOG_DIR,"/zt-history/public"))return 0;
#endif
#endif
#if defined(CONFIG_MT_RANGE_TEST_LOG)
  return eraseCsv();
#else
  return 0;
#endif
}
static void pollFactoryErase(uint64_t now){
  const int erased=factoryEraseRetry.poll(now,[](){
    if(!service.storageQuiescent()||!radio.ready()||radio.busy()||radio.health()!=0)return -EIO;
    return mt::stock::admin::factory::finish(settings,factoryEraseNode,preferenceDomain.publicKey,[](){
      return eraseResetOwnedFiles();
    });
  });
  if(!erased)sys_reboot(SYS_REBOOT_COLD);
}
static void scheduleOrdinaryReboot(int32_t seconds,uint64_t now){
  if(firmwareUpdate.pending()||factoryConfigResetScheduled)return;
  rebootSchedule.request(seconds,now);
}
static bool initialized=false,storageReady=false;
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
static mt::history::TransmitHistory broadcastHistory;
static mt::history::Clock broadcastClock(){
  const auto now=uint64_t(k_uptime_get());return {now,wallClock.at(now)};
}
// Main and software shutdown callers hold serviceLock. Timestamp reads use
// RAM. Native flash writes must stop RF and restore the previous admission.
static int checkpointBroadcasts(bool beacon){
  if(!service.broadcastHistory||!storageReady||settingsEdit.open()||service.faulted())return -EACCES;
  const auto now=uint64_t(k_uptime_get());
  if(!beacon&&!broadcastHistory.due(now))return 0;
  broadcastHistory.retry(now);
  const bool receiving=service.receiving(),relaying=service.relaying(),armed=service.endpointArmed();
  int rc=service.pauseForStorage();if(rc)return rc;
  rc=-EIO;
  if(service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0){
    if(beacon)broadcastHistory.record(mt::history::Kind::Beacon);
    // The main loop will retry a rejected beacon after its normal cadence.
    rc=broadcastHistory.save(settings,now);
  }
  if(receiving&&(!service.start(relaying,uint32_t(now))||(armed&&!service.armEndpoint(true))))return -EIO;
  return rc;
}
static void bindBroadcastHistory(uint32_t node){
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  if(!preferenceDomainReady||preferenceDomain.node!=node)return;
  const uint8_t *binding=preferenceDomain.publicKey;
#else
  const uint8_t *binding=nullptr;
#endif
  if(!broadcastHistory.load(settings,node,binding,broadcastClock)){
    service.broadcastHistory=&broadcastHistory;
    service.saveBeaconHistory=[](){return checkpointBroadcasts(true);};
  }
}
static void flushBroadcastsBeforeReset(){
  if(service.broadcastHistory&&storageReady&&service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0)
    (void)broadcastHistory.save(settings,uint64_t(k_uptime_get()));
}
#endif
static void saveWaypoints(uint64_t now){
  if(!storageReady||!waypointHistory.due(waypoints,journal.channelGeneration(),now))return;
  waypointHistory.retry(now);
  const bool receiving=service.receiving(),relaying=service.relaying(),armed=service.endpointArmed();
  if(service.pauseForStorage()!=0)return;
  if(service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0)
    (void)waypointHistory.save(settings,waypoints,waypointHistoryDomain(),waypointHistoryContacts(),now);
  if(receiving&&service.start(relaying,uint32_t(now))&&armed)(void)service.armEndpoint(true);
}
#if defined(CONFIG_MT_SATELLITE_HISTORY)
static void pollSatelliteHistory(uint64_t now){
  if(!storageReady||settingsEdit.open()||rebootSchedule.pending()||!journal.ready()||!nodeJournal.ready())return;
  const bool recover=!satelliteHistory.ready();
#if defined(CONFIG_MT_PAGED_SATELLITES)
  if(!recover)return;
#endif
  if((!recover&&service.nodes.dirty())||!satelliteHistory.due(now))return;
  satelliteHistory.attempted(now,false);
  const bool receiving=service.receiving(),relaying=service.relaying(),armed=service.endpointArmed();
  if(service.pauseForStorage())return;
  if(service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0){
    if(recover){
      loadSatelliteHistory(satelliteMayInitialize,true);
      if(satelliteHistory.ready())satelliteHistory.attempted(now,true);
    }
#if !defined(CONFIG_MT_PAGED_SATELLITES)
    else{
      mt::companion::SatelliteHistory::Domain domain{};uint8_t channels=0;
      const bool saved=satelliteDomain(domain,channels)&&
        !satelliteHistory.save(satelliteVolume,service.nodes,service.satellites,domain,channels);
      satelliteHistory.attempted(now,saved);
    }
#endif
  }
  if(receiving&&service.start(relaying,uint32_t(now))&&armed)(void)service.armEndpoint(true);
}
#endif
static int settingsStartupResult=-EAGAIN;
#if defined(CONFIG_MT_SERIAL_MODULE)
static mt_serial::ZephyrSerial serialModule;
struct SerialSeen {uint32_t from=0,id=0,at=0;unsigned channel=0;};
static SerialSeen serialSeen[8];
static unsigned nextSerialSeen=0;
#endif
static bool cannedInputAvailable() {
#if defined(CONFIG_MT_DISPLAY)
  return mt::ui::ready()&&cannedJournal.ready()&&extendedEdit.ready();
#else
  return false;
#endif
}
static bool serialInputAvailable() {
#if defined(CONFIG_MT_SERIAL_MODULE)
  return serialModule.available()&&extendedEdit.ready();
#else
  return false;
#endif
}
static void refreshCannedInput() {
#if defined(CONFIG_MT_DISPLAY)
  const bool ready=initialized&&storageReady&&journal.ready()&&cannedInputAvailable()&&
    cannedJournal.config().valid()&&extendedEdit.trusted()&&!settingsEdit.open()&&!rebootSchedule.pending();
  uint8_t channels=0;
  if(ready)for(unsigned i=0;i<8;++i) {
    mt::PrimaryChannel::Selection selection;
    if(service.config.channelSelection(i,selection))channels|=uint8_t(1u<<i);
    mt::PrimaryChannel::clear(selection);
  }
  uiController.setCannedRoutes(channels,journal.generation());
  uiController.setCannedContent(ready?cannedJournal.config().messages:nullptr,ready,
    ready&&extendedEdit.config().cannedEnabled());
#endif
}
#if defined(CONFIG_MT_SERIAL_MODULE)
static bool serialConfig(meshtastic_ModuleConfig_SerialConfig &out){
  meshtastic_ModuleConfig c{};
  if(!extendedEdit.ready()||!completionEdit.ready()||!mt::modules::completionConfig(completionEdit.config(),moduleEdit.config(),notificationEdit.config(),extendedEdit.config(),c,meshtastic_ModuleConfig_serial_tag))return false;
  out=c.payload_variant.serial;return true;
}
static bool serialRuntimeConfig(const meshtastic_ModuleConfig_SerialConfig &value,mt_serial::Config &config,uint32_t &baud){
  const auto mode=mt::detail::enumNumber(value.mode),baudEnum=mt::detail::enumNumber(value.baud);
  switch(mode){
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_DEFAULT:
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE:config.mode=mt_serial::Mode::Simple;break;
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG:config.mode=mt_serial::Mode::TextMessage;break;
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_PROTO:config.mode=mt_serial::Mode::Proto;break;
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_NMEA:config.mode=mt_serial::Mode::Nmea;break;
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_CALTOPO:config.mode=mt_serial::Mode::CalTopo;break;
  case meshtastic_ModuleConfig_SerialConfig_Serial_Mode_WS85:config.mode=mt_serial::Mode::Ws85;break;
  default:return false;
  }
  baud=mt_serial::baudRateFromStockEnum(baudEnum);config.timeout_ms=value.timeout;config.echo=value.echo;
  return baud&&value.timeout<=INT32_MAX&&!value.override_console_serial_port;
}
#endif
static void refreshSerialInput() {
#if defined(CONFIG_MT_SERIAL_MODULE)
  meshtastic_ModuleConfig_SerialConfig value{};mt_serial::Config config{};uint32_t baud=0;
  const bool valid=serialConfig(value)&&serialRuntimeConfig(value,config,baud);
  const bool ready=valid&&initialized&&storageReady&&journal.ready()&&extendedEdit.trusted()&&completionEdit.trusted()&&
    !settingsEdit.open()&&!rebootSchedule.pending();
  (void)serialModule.configure(config,ready&&value.enabled,baud?baud:38400,value.rxd,value.txd);
#endif
}
#if defined(CONFIG_MT_SERIAL_MODULE)
static int namedSerialChannel() {
  for(unsigned index=0;index<8;++index) {
    mt::ChannelRecord channel;
    if(!service.config.channelRecord(index,channel)||!channel.role())continue;
    const char *name=channel.name();
    constexpr char expected[]="serial";
    unsigned at=0;
    for(;at<sizeof(expected);++at) {
      const char value=name[at]>='A'&&name[at]<='Z'?char(name[at]-'A'+'a'):name[at];
      if(value!=expected[at])break;
      if(!value)return int(index);
    }
  }
  return -1;
}
#endif
static mt::modules::TrafficManagementWindow trafficWindow;
static mt::modules::TrafficNodeInfoResponder trafficNodeInfo MT_WIFI_PSRAM_BSS(traffic_nodeinfo);
static mt::modules::BeaconBroadcaster beacons;
static mt::modules::BeaconTargetRuntime beaconTargets MT_WIFI_PSRAM_BSS(beacon_targets)(service);
static uint32_t beaconSettingsEpoch(){
  static uint32_t epoch=1,coreGeneration=UINT32_MAX;static uint8_t beaconConfig[244]{};static bool captured=false;
  const auto generation=journal.generation();const auto *config=extendedEdit.config().bytes+24;
  // The persistent send marker is outside this exact beacon-config span.
  if(!captured||generation!=coreGeneration||memcmp(beaconConfig,config,sizeof(beaconConfig))){
    captured=true;coreGeneration=generation;memcpy(beaconConfig,config,sizeof(beaconConfig));if(!++epoch)++epoch;
  }
  return epoch;
}

static mt::SettingsCommit settingsCommit(service,radio,settingsEdit,nodeJournal,settings,&moduleEdit,&notificationEdit,&wifiEdit,&extendedEdit);
static mt::network::MqttGatewayJournal mqttJournal;
static mt::network::MqttGatewayEdit mqttEdit(service,mqttJournal,settings);
static mt::network::MqttGatewayAdmin mqttAdmin(mqttEdit,settingsEdit);
static mt::mqtt::MqttMapReporter mqttMapReporter;
static mt::mqtt::UplinkWindow mqttWindow;
static bool mqttSettingsUsable() {
  return mqttAdmin.ready()&&mqttJournal.state()!=mt::network::MqttGatewayJournal::State::Recovered&&
    mqttJournal.state()!=mt::network::MqttGatewayJournal::State::Invalid&&
    mqttJournal.state()!=mt::network::MqttGatewayJournal::State::IoError;
}
static const mt::network::MqttGatewaySettings &committedMqttSettings() { return mqttJournal.config(); }
static bool persistentWorkPending(){
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  if(mt::stock::telemetry::accessory::pending())return true;
#endif
  bool pending=settingsEdit.open()||moduleEdit.dirty()||completionEdit.dirty()||notificationEdit.dirty()||
    wifiEdit.dirty()||extendedEdit.dirty();
#if defined(CONFIG_MT_MQTT_GATEWAY)
  pending=pending||mqttEdit.dirty();
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  pending=pending||storeForwardEdit.dirty();
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
  pending=pending||gpioModuleEdit.dirty();
#endif
  return pending;
}
static bool administrationWorkPending(){
#if defined(CONFIG_MT_RF_ADMIN_GET)
  return service.privateAdminWorkQueued()||service.privateAdminQueriesPending()||
    service.legacyAdminWorkQueued();
#else
  return false;
#endif
}
static bool mqttProxyConfigured() {
#if defined(CONFIG_MT_M119_TLS_TEST)
  return false; // The canary tests device-owned TLS only, never phone-delegated proxy TLS.
#else
  if(!mqttSettingsUsable())return false;
  const auto &config=committedMqttSettings();
  // An enabled config with an empty address uses the stock default broker.
  return config.enabled()&&config.proxyEnabled();
#endif
}
static void refreshMqttProxyBridge() { service.setMqttProxyBridgeEnabled(mqttProxyConfigured()); }
static const char *mqttRoot() {
  return reinterpret_cast<const char *>(committedMqttSettings().bytes+mt::network::MqttGatewaySettings::Root);
}
#if defined(CONFIG_MT_MQTT_GATEWAY)
static mt::mqtt::BootSnapshot mqttBoot;
static bool mqttStarted=false,mqttTrustAvailable=false;
#if defined(CONFIG_MT_M119_TLS_TEST)
static bool mqttTestCaReady=false;
#endif
static mt::mqtt::RestartPolicy mqttRestart;
#endif
static mt::mqtt::PublishResult enqueueMqttResult(const char *topic,const meshtastic_MeshPacket *packet,
    const char *channel,size_t channelLength,const char *gateway,size_t gatewayLength) {
  if(!topic||!packet||!channel||!gateway)return mt::mqtt::PublishResult::Invalid;
  if(mqttProxyConfigured()) {
    refreshMqttProxyBridge();
    mt::mqtt::client_proxy::Message message{};
    size_t topicLength=0;
    while(topic[topicLength]&&topicLength<sizeof(message.topic))++topicLength;
    if(!topicLength||topicLength>=sizeof(message.topic))return mt::mqtt::PublishResult::Invalid;
    memcpy(message.topic,topic,topicLength);
    size_t payloadLength=0;
    if(!mt::mqtt::ServiceEnvelopeCodec::encode(packet,channel,channelLength,gateway,gatewayLength,
          message.payload,sizeof(message.payload),&payloadLength)||payloadLength>UINT16_MAX) {
      mt::mqtt::client_proxy::clear(message);return mt::mqtt::PublishResult::Invalid;
    }
    message.length=uint16_t(payloadLength);message.kind=mt::mqtt::client_proxy::PayloadKind::Binary;
    message.retained=false;
    const auto pushed=service.enqueueMqttProxyPublish(message);
    mt::mqtt::client_proxy::clear(message);
    return pushed==mt::mqtt::client_proxy::Queue<1>::PushResult::Accepted||
      pushed==mt::mqtt::client_proxy::Queue<1>::PushResult::AcceptedDroppedOldest?
      mt::mqtt::PublishResult::Accepted:mt::mqtt::PublishResult::Disabled;
  }
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(mqttStarted)return mt::mqtt::enqueuePacket(topic,packet,channel,channelLength,gateway,gatewayLength);
#endif
  return mt::mqtt::PublishResult::Disabled;
}
static bool enqueueMqtt(void *,const char *topic,const meshtastic_MeshPacket *packet,const char *channel,const char *gateway) {
  return enqueueMqttResult(topic,packet,channel,channel?strlen(channel):0,gateway,gateway?strlen(gateway):0)==
    mt::mqtt::PublishResult::Accepted;
}
static mt::mqtt::PublishResult enqueueMapMqtt(void *,const char *topic,const meshtastic_MeshPacket *packet,
    const char *channel,size_t channelLength,const char *gateway,size_t gatewayLength) {
  return enqueueMqttResult(topic,packet,channel,channelLength,gateway,gatewayLength);
}
// The PKI encoder already produced the stock binary ServiceEnvelope. Copy it
// into the existing bounded producer path without decoding or re-encoding it.
static bool enqueueMqttEnvelope(const char *topic,const uint8_t *payload,size_t payloadSize) {
  if(!topic||!payload||!payloadSize)return false;
  if(mqttProxyConfigured()) {
    refreshMqttProxyBridge();
    mt::mqtt::client_proxy::Message message{};
    size_t topicLength=0;
    while(topicLength<sizeof(message.topic)&&topic[topicLength])++topicLength;
    if(!topicLength||topicLength>=sizeof(message.topic)||
       payloadSize>sizeof(message.payload)||payloadSize>UINT16_MAX)return false;
    memcpy(message.topic,topic,topicLength);
    memcpy(message.payload,payload,payloadSize);
    message.length=uint16_t(payloadSize);
    message.kind=mt::mqtt::client_proxy::PayloadKind::Binary;
    message.retained=false;
    const auto pushed=service.enqueueMqttProxyPublish(message);
    mt::mqtt::client_proxy::clear(message);
    return pushed==mt::mqtt::client_proxy::Queue<1>::PushResult::Accepted||
      pushed==mt::mqtt::client_proxy::Queue<1>::PushResult::AcceptedDroppedOldest;
  }
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(mqttStarted)
    return mt::mqtt::enqueueEnvelope(topic,payload,payloadSize)==mt::mqtt::PublishResult::Accepted;
#endif
  return false;
}
#if defined(CONFIG_MT_MQTT_GATEWAY)
static bool mqttDownlinkTopicMatches(const mt::mqtt::DownlinkMessage &message,
                                     const mt::mqtt::MqttServiceEnvelopeDecoder::Decoded &envelope) {
  return mt::mqtt::MqttDownlinkAdmission::topicMatches(mqttBoot.root(),message.topic,
    message.topicLength,envelope);
}
#endif
static bool mqttGatewayIdentity(char out[10]) {
  const int size=snprintf(out,10,"!%08x",unsigned(service.route.identity()));
  return size==9;
}
static bool mqttTransportWanted() {
  if(!mqttSettingsUsable())return false;
  const auto &config=committedMqttSettings();
  if(!config.enabled())return false;
  if(config.proxyEnabled())return mqttProxyConfigured();
#if defined(CONFIG_MT_MQTT_GATEWAY)
  return mqttStarted;
#else
  return false;
#endif
}
static void mqttFrame(void *,const uint8_t *frame,size_t size,const bench::RouteMetadata &metadata,unsigned index) {
#if defined(CONFIG_MT_M119_TLS_TEST)
  (void)frame;(void)size;(void)metadata;(void)index;
  return; // TLS/MQTT connection test: never publish Mesh frames.
#else
  if(!mqttTransportWanted()||mqttEdit.dirty()||settingsEdit.open()||rebootSchedule.pending())return;
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(!mqttProxyConfigured()&&mqttRestart.pending())return;
#endif
  const uint64_t now=uint64_t(k_uptime_get());const uint32_t from=bench::le32(frame+4),id=bench::le32(frame+8);
  if(mqttWindow.contains(from,id,index,now))return;
  const auto &settings=committedMqttSettings();
  const bool uplink=mqttAdmin.channelUplinkEnabled(service.config,index,true);
  mt::ChannelRecord channel;if(!service.config.channelRecord(index,channel))return;
  char gateway[10]{};if(!mqttGatewayIdentity(gateway))return;
  mt::network::MqttPacketEligibility::Context context;
  context.mqttEnabled=settings.enabled();context.transportReady=true;
  context.encryptionEnabled=settings.encryptionEnabled();
  context.defaultServer=mt::network::MqttDefaultServerPolicy::isDefaultAddress(
    reinterpret_cast<const char *>(settings.bytes+mt::network::MqttGatewaySettings::Address),
    settings.bytes[mt::network::MqttGatewaySettings::AddressLength]);
  context.anyChannelUplinkEnabled=uplink;context.thisChannelUplinkEnabled=uplink;
  context.channelActive=channel.role()!=0;context.channelRole=uint8_t(channel.role());
  context.channelIndex=uint8_t(index);context.channelHash=service.codec.channelHash();
  context.localNode=service.route.identity();context.sourceIsLocal=from==context.localNode;
  context.localUploadConsent=mqttAdmin.configOkToMqtt(true);
  if(mt::network::MqttChannelUplink::publish(service.codec,service.config,index,frame,size,metadata,
     context,mqttRoot(),gateway,enqueueMqtt,nullptr))mqttWindow.accepted(from,id,index,now);
  mt::mqtt::client_proxy::wipe(gateway,sizeof(gateway));
#endif
}
#if !defined(CONFIG_MT_M119_TLS_TEST)
static bool mqttPkiUplinkAllowed(uint32_t localNode) {
  if(!localNode||localNode!=service.route.identity()||!mqttTransportWanted()||
     mqttEdit.dirty()||settingsEdit.open()||rebootSchedule.pending()||
     !mqttAdmin.configOkToMqtt(true))return false;
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(!mqttProxyConfigured()&&mqttRestart.pending())return false;
#endif
  const auto &settings=committedMqttSettings();
  if(!settings.valid()||!settings.enabled()||!settings.encryptionEnabled())return false;
  bool anyChannelUplink=false;
  for(unsigned i=0;i<mt::network::MqttGatewaySettings::ChannelSlots;++i)
    anyChannelUplink|=mqttAdmin.channelUplinkEnabled(service.config,i,true);
  return anyChannelUplink;
}
#endif

static bool mqttPkiPublish(const uint8_t *frame,size_t size,uint32_t localNode,
                           bool transit,uint32_t stamp=0,bool validTime=false) {
#if defined(CONFIG_MT_M119_TLS_TEST)
  (void)frame;(void)size;(void)localNode;(void)transit;(void)stamp;(void)validTime;
  return false;
#else
  if(!frame||!mqttPkiUplinkAllowed(localNode))return false;
  char gateway[10]{};
  if(!mqttGatewayIdentity(gateway))return false;
  char topic[mt::mqtt::client_proxy::kTopicStorageBytes]{};
  uint8_t envelope[mt::mqtt::client_proxy::kPayloadStorageBytes]{};
  size_t envelopeSize=0;
  const bool encoded=transit?
    mt::network::MqttPkiUplink::encodeTransit(localNode,frame,size,mqttRoot(),gateway,
      topic,sizeof(topic),envelope,sizeof(envelope),&envelopeSize,stamp,validTime):
    mt::network::MqttPkiUplink::encode(localNode,frame,size,mqttRoot(),gateway,
      topic,sizeof(topic),envelope,sizeof(envelope),&envelopeSize);
  if(encoded)(void)enqueueMqttEnvelope(topic,envelope,envelopeSize);
  mt::mqtt::client_proxy::wipe(envelope,sizeof(envelope));
  bench::wipeKeyBuffer(gateway,sizeof(gateway));
  return encoded;
#endif
}
// Service binds this only to a tagged private local TX after successful radio
// completion. No RX, relay, ordinary channel, or failed-TX path reaches it.
static void mqttPrivateTx(void *,const uint8_t *frame,size_t size,uint32_t localNode) {
  mqttPkiPublish(frame,size,localNode,false);
}
// A passive RF observation may be uplinked independently of RF retransmission.
static bool mqttPkiTransit(void *,const uint8_t *frame,size_t size,uint32_t localNode,uint32_t stamp,bool validTime) {
  return mqttPkiPublish(frame,size,localNode,true,stamp,validTime);
}
static uint8_t hotEvictionFlags(const mt::NodeCache::Entry *entry) {
  if(!entry)return 0;
  uint8_t flags=0;
  if(entry->favorite())flags|=mt::mqtt::OnlineNodeWindow::Favorite;
  if(entry->ignored())flags|=mt::mqtt::OnlineNodeWindow::Ignored;
  if(entry->manuallyVerified())flags|=mt::mqtt::OnlineNodeWindow::ManuallyVerified;
  if(entry->bytes[mt::NodeCache::Entry::KeyLength]==32)flags|=mt::mqtt::OnlineNodeWindow::HasPublicKey;
  return flags;
}
static void markHotNodeDirty(uint32_t now) {
  if(!hotNodeDirty)hotNodeDirtyAt=now;
  hotNodeDirty=true;
}
static bool syncHotNodeFlags() {
  bool changed=false;
  for(unsigned i=0;i<service.nodes.count();++i) {
    const auto *entry=service.nodes.get(i);if(!entry||entry->id()==service.route.identity())continue;
    for(size_t row=0;row<hotNodeWindow.hotCount();++row) {
      const auto *current=hotNodeWindow.hotAt(row);
      if(current&&current->node==entry->id()&&current->evictionFlags!=hotEvictionFlags(entry)) {
        changed=hotNodeWindow.mergeObservedEvictionFlags(entry->id(),hotEvictionFlags(entry))||changed;break;
      }
    }
  }
  if(changed)markHotNodeDirty(k_uptime_get_32());
  return changed;
}
static void observeHotNode(void *,uint32_t node,uint32_t utc,uint32_t uptime,bool viaMqtt) {
  if(!hotNodeCountAvailable||!node||node==service.route.identity())return;
  const int cacheSlot=service.nodes.find(node);
  const auto *entry=cacheSlot<0?nullptr:service.nodes.get(unsigned(cacheSlot));
  bool changed=false;
  const uint32_t stamp=utc?utc:uptime;
  if(!hotNodeWindow.observeStock(node,utc!=0,stamp,viaMqtt,false,k_uptime_get_32(),
                                &changed,entry!=nullptr,hotEvictionFlags(entry)))return;
  if(changed)markHotNodeDirty(k_uptime_get_32());
  if(!utc)hotNodeClockSidecarPending=true;
}
static bool seedHotNodeWindow(mt::mqtt::OnlineNodeWindow &window,uint32_t self) {
  window.clear();window.setSelf(self);
  if(!window.create(self))return false;
  for(unsigned i=0;i<service.nodes.count();++i) {
    const auto *entry=service.nodes.get(i);
    if(!entry||entry->id()==self||!window.create(entry->id()))continue;
    (void)window.setEvictionFlags(entry->id(),hotEvictionFlags(entry));
  }
  // New journal migration seeds only current membership and durable policy.
  // Legacy NodeJournal has no recoverable receive timestamps; lastHeard stays 0.
  return true;
}
static void loadHotNodeMetadata(uint32_t self) {
  hotNodeCountAvailable=false;hotNodeDirty=false;hotNodeClockSidecarPending=false;
  if(!service.nodes.ready())return;
  auto seed=[](mt::mqtt::OnlineNodeWindow &window,uint32_t node) {
    // Use the exact pre-existing NodeDB membership and policy seed path.
    return seedHotNodeWindow(window,node);
  };
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  mt::pki::IdentityBinding prepared{};
  const auto result=mt::mqtt::startHotNodeMetadata(
    privateStorage.storage(),privateStartup.state()==mt::pki::PkiStartup::State::Prepared,
    privateStartup.state()==mt::pki::PkiStartup::State::Empty,self,prepared,
    hotNodePublicKey,hotNodeJournal,settings,hotNodeWindow,seed);
  hotNodeCountAvailable=result.countAvailable();
  if(!hotNodeCountAvailable)return;
  if(result.state==mt::mqtt::HotNodeMetadataStartupState::JournalInitialized) {
    hotNodeLastWrite=k_uptime_get_32();return;
  }
  if(result.state==mt::mqtt::HotNodeMetadataStartupState::JournalLoaded||
     result.state==mt::mqtt::HotNodeMetadataStartupState::JournalRecovered)syncHotNodeFlags();
#else
  // Public-only builds keep a useful RAM count without opening or saving the
  // private-key-bound metadata journal.
  hotNodeCountAvailable=seed(hotNodeWindow,self);
#endif
}

static void persistHotNodeMetadata(uint32_t now) {
  if(!hotNodeCountAvailable||hotNodeJournal.state()!=mt::mqtt::HotNodeMetadataJournal::State::Loaded||!hotNodeDirty||
     uint32_t(now-hotNodeDirtyAt)<mt::NodeJournal::SettleMs||
     (hotNodeLastWrite&&uint32_t(now-hotNodeLastWrite)<mt::NodeJournal::IntervalMs))return;
  const bool wasReceiving=service.receiving(),wasRelaying=service.relaying(),wasArmed=service.endpointArmed();
  if(service.pauseForStorage()!=0)return;
  if(service.storageQuiescent()&&radio.ready()&&radio.health()==0) {
    const int result=hotNodeJournal.save(settings,hotNodeWindow,service.route.identity(),hotNodePublicKey);
    if(!result){hotNodeDirty=false;hotNodeLastWrite=now;}
  }
  if(wasReceiving&&service.start(wasRelaying,now)&&wasArmed)(void)service.armEndpoint(true);
}
static void backfillHotNodeClock(uint32_t utc,uint32_t uptime,uint32_t now) {
  if(utc&&hotNodeClockSidecarPending&&hotNodeCountAvailable) {
    hotNodeWindow.backfillHeardAt(utc,uptime);hotNodeClockSidecarPending=false;
    markHotNodeDirty(now);
  }
}
static void pollMqttMapReport(uint32_t now) {
#if defined(CONFIG_MT_M119_TLS_TEST)
  (void)now;
  return; // TLS/MQTT connection test: never publish map/location data.
#else
  refreshMqttProxyBridge();
  if(!mqttTransportWanted()||mqttEdit.dirty()||settingsEdit.open()||rebootSchedule.pending())return;
  if(!hotNodeCountAvailable)return;
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(!mqttProxyConfigured()&&mqttRestart.pending())return;
#endif
  mt::mqtt::MqttMapReport::Source source{};
  const auto local=service.config.node();
  char longName[25]{},shortName[5]{};
  memcpy(longName,local.bytes+bench::MtNodeRecord::Name,24);
  memcpy(shortName,local.bytes+bench::MtNodeRecord::Short,4);
  meshtastic_Position position{};
  if(service.localPosition(position)) {
    source.latitudeI=position.latitude_i;source.longitudeI=position.longitude_i;
    source.altitude=position.altitude;
  }
  source.longName=longName;source.shortName=shortName;source.firmwareVersion=mt::firmwareVersion();
  source.role=int32_t(service.config.role());
  source.hwModel=mt::board::Model;
  source.region=static_cast<meshtastic_Config_LoRaConfig_RegionCode>(service.config.regionCode());
  source.modemPreset=int32_t(service.config.modemPreset());
  source.hasDefaultChannel=mt::mqtt::MqttMapReporter::hasDefaultChannel(service.config);
  const uint64_t monotonic=k_uptime_get();
  const uint32_t nowUtc=wallClock.at(monotonic),nowUptime=uint32_t(monotonic/1000u);
  backfillHotNodeClock(nowUtc,nowUptime,uint32_t(monotonic));
  source.numOnlineLocalNodes=uint16_t(hotNodeWindow.countOnlineLocal(nowUtc?nowUtc:nowUptime));
  char gateway[10]{};if(!mqttGatewayIdentity(gateway))return;
  const char *channel=service.config.effectivePrimaryName();
  const size_t channelLength=strlen(channel),gatewayLength=strlen(gateway);
  mt::mqtt::MqttMapReporter::Identity identity{};
  identity.fromNode=service.route.identity();identity.packetId=0;
  identity.channelId=channel;identity.channelIdLength=channelLength;
  identity.gatewayId=gateway;identity.gatewayIdLength=gatewayLength;
  // Mirror stock's compile-time event-key predicate; the opt-in location
  // gates remain enforced independently by MqttMapReporter.
  const bool eventChannel=mt::mqtt::MqttMapReporter::isEventChannel(service.config,0);
  auto directState=mt::mqtt::State::Stopped;
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(mqttStarted)directState=mt::mqtt::status().state;
#endif
  (void)mqttMapReporter.poll(committedMqttSettings(),source,identity,mqttRoot(),now,
    mqttProxyConfigured(),directState,eventChannel,enqueueMapMqtt,nullptr);
  mt::mqtt::client_proxy::wipe(gateway,sizeof(gateway));
  mt::mqtt::client_proxy::wipe(longName,sizeof(longName));
  mt::mqtt::client_proxy::wipe(shortName,sizeof(shortName));
#endif
}
static int applicationAfterCommit(int result,uint64_t now) {
  result=wifiAdmin.afterCommit(result,now);
  refreshMqttProxyBridge();
#if defined(CONFIG_MT_MQTT_GATEWAY)
  mqttRestart.observed(mqttJournal.generation(),mqttEdit.dirty());
  if(mqttRestart.due(result,settingsEdit.open(),mqttEdit.dirty()))scheduleOrdinaryReboot(1,now);
  if(!result&&mqttStarted&&!rebootSchedule.pending()&&
     !mqttBoot.downlinkSubscriptionsMatch(service.config))scheduleOrdinaryReboot(1,now);
#endif
  return result;
}
static const device *const usb=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
RING_BUF_DECLARE(usbRx,4096);
#if defined(CONFIG_MT_T096_FULL_COMPANION)
// sendUsb reserves a whole frame before enqueue. The handshake resumes on the
// next poll when the host drains the ring. RX capacity remains independent.
RING_BUF_DECLARE(usbTx,2048);
#else
RING_BUF_DECLARE(usbTx,8192);
#endif
static atomic_t usbDrops;
static atomic_t usbEpochChanged;
static mt::UsbSessionEpoch usbSession;
static void wipeUsbRxLocked(){
  for(;;){
    uint8_t *claimed=nullptr;const uint32_t count=ring_buf_get_claim(&usbRx,&claimed,UINT32_MAX);
    if(!count)break;
    bench::wipeKeyBuffer(claimed,count);
    if(ring_buf_get_finish(&usbRx,count)!=0)break;
  }
  ring_buf_reset(&usbRx);
}
static void resetUsbBuffersLocked(bool discardRx,bool epoch) {
  if(discardRx)wipeUsbRxLocked();
  ring_buf_reset(&usbTx);atomic_set(&usbDrops,0);uart_irq_tx_disable(usb);
  if(epoch)atomic_set(&usbEpochChanged,1);
}
static mt::UsbSessionEpoch::Snapshot observeUsbLineLocked(const device *dev) {
  uint32_t dtr=0;const bool measured=uart_line_ctrl_get(dev,UART_LINE_CTRL_DTR,&dtr)==0;
  return usbSession.bind(measured,dtr!=0,[]{resetUsbBuffersLocked(true,true);},
    []{resetUsbBuffersLocked(false,true);},[]{resetUsbBuffersLocked(true,false);});
}
// Caller owns serviceLock. Use the last verified durable Config, never an
// open edit's speculative settings. Private RF readiness is intentionally not
// a prerequisite for a valid enabled local recovery console.
static void syncConsoleSecurityPolicy(mt::UsbSessionEpoch::RestoreReceipt *receipt=nullptr) {
  const bool trusted=settingsStartupResult==0&&journal.ready()&&!preferenceRecoveryHeld;
  const bool allowed=trusted&&journal.config().serialEnabled();
  const unsigned key=irq_lock();
  if(allowed!=mt_security::serialConsoleAllowed()) {
    mt::UsbSessionEpoch::Snapshot before{};
    if(receipt)before=observeUsbLineLocked(usb);
    mt_security::serialConsolePolicy(allowed,trusted);
    usbSession.fence();resetUsbBuffersLocked(true,true);
    if(receipt&&!allowed)receipt->bind(before,usbSession.generation());
    // Companion consumes usbEpochChanged at its next extraction boundary.
    // Do not reset its borrowed Admin/input scratch inside a save callback.
  }
  irq_unlock(key);
}
static void syncApiLogPolicy() {
  const bool trusted=settingsStartupResult==0&&journal.ready()&&!preferenceRecoveryHeld;
  const bool enabled=trusted&&journal.config().debugLogApiEnabled();
#if defined(CONFIG_MT_BLE_LOG_RADIO)
  if(enabled!=apiLogs.enabled())mt::ble::setLogRadioEnabled(enabled);
#endif
  apiLogs.setEnabled(enabled);
}
static bool legacyAdminChannelConfigured(const mt::Config &config) {
  if(!config.valid()||config.licensedOperation())return false;
  for(unsigned i=0;i<8;++i) {
    mt::ChannelRecord record;mt::PrimaryChannel::Selection key;
    const bool found=config.channelRecord(i,record)&&record.role()&&
      mt::Config::isAdminChannelName(record.name())&&config.channelSelection(i,key)&&
      !key.licensedPlaintext&&(key.length==16||key.length==32);
    mt::PrimaryChannel::clear(key);bench::wipeKeyBuffer(&record,sizeof(record));
    if(found)return true;
  }
  return false;
}
static void observeApiOwnerState() {
  static bool observed=false,wasStorageHeld=false,wasFaulted=false,wasRadioReady=false;
  static uint8_t previousPower=0,previousUpdate=0;
  if(!apiLogs.enabled()){observed=false;return;}
  const bool storageHeld=!storageReady,faulted=service.faulted(),radioReady=radio.ready();
  if(!observed||storageHeld!=wasStorageHeld)
    recordApiEvent(mt::api::ApiLogBuffer::Event::StorageHeld,storageHeld?1:0);
  if(!observed||faulted!=wasFaulted)
    recordApiEvent(mt::api::ApiLogBuffer::Event::RadioFault,faulted?1:0);
  if(radioReady&&(!observed||!wasRadioReady))recordApiEvent(mt::api::ApiLogBuffer::Event::RadioReady);
  if(!observed&&initialized&&storageReady&&!faulted)recordApiEvent(mt::api::ApiLogBuffer::Event::BootReady);
  const auto power=uint8_t(completionRuntime.powerState()),update=uint8_t(firmwareUpdate.state());
  if(!observed||power!=previousPower)recordApiEvent(mt::api::ApiLogBuffer::Event::PowerPolicyState,power);
  if(!observed||update!=previousUpdate)recordApiEvent(mt::api::ApiLogBuffer::Event::UpdateOwnerState,update);
  previousPower=power;previousUpdate=update;
  observed=true;wasStorageHeld=storageHeld;wasFaulted=faulted;wasRadioReady=radioReady;
}
static bool serviceUsbIrqOnce(const device *dev) {
  uart_irq_update(dev);
  if(!uart_irq_is_pending(dev))return false;
  bool progress=false;const auto line=observeUsbLineLocked(dev);
  if(uart_irq_rx_ready(dev)) {
    uint8_t bytes[64];int n=uart_fifo_read(dev,bytes,sizeof(bytes));
    if(n>0){
      if((!line.measured||line.connected)&&!atomic_get(&shutdownAdmissionClosed)){
        if(ring_buf_put(&usbRx,bytes,n)!=unsigned(n))atomic_inc(&usbDrops);
        notifyPowerActivity();
      }
      bench::wipeKeyBuffer(bytes,sizeof(bytes));progress=true;}
  }
  if(uart_irq_tx_ready(dev)) {
    if(line.measured&&!line.connected){ring_buf_reset(&usbTx);uart_irq_tx_disable(dev);progress=true;}
    else {uint8_t bytes[64];
#if defined(CONFIG_SOC_ESP32S3) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console),espressif_esp32_usb_serial)
      static_assert(sizeof(bytes)==mt::usb_serial_jtag::PacketSize);
      const uint32_t limit=mt::usb_serial_jtag::nextWriteSize(ring_buf_size_get(&usbTx));
#else
      const uint32_t limit=sizeof(bytes);
#endif
      uint32_t n=ring_buf_peek(&usbTx,bytes,limit);
      if(n){int sent=uart_fifo_fill(dev,bytes,n);ring_buf_consume(&usbTx,sent>0?sent:0);progress=progress||sent>0;}
      else uart_irq_tx_disable(dev);}
  }
  return progress;
}
// ESP32-S3 can lose the IN-empty edge while the host is applying backpressure.
// Called from the normal USB poll with IRQs locked. Drain only older queued
// bytes through the same FIFO path as the ISR, preserving framed protobufs.
static void pollUsbTxLocked(const device *dev) {
#if defined(CONFIG_SOC_ESP32S3) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console),espressif_esp32_usb_serial)
  if(ring_buf_size_get(&usbTx)&&uart_irq_tx_ready(dev))(void)serviceUsbIrqOnce(dev);
#else
  (void)dev;
#endif
}
static void usbInterrupt(const device *dev,void *) {
  while(serviceUsbIrqOnce(dev)){}
}
static int startApplicationUsbRx() {
  if(!device_is_ready(usb))return -ENODEV;
  const int result=uart_irq_callback_user_data_set(usb,usbInterrupt,nullptr);
  if(result)return result;
#if defined(CONFIG_SOC_ESP32S3) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console),espressif_esp32_usb_serial)
  // This driver clears the pending OUT edge when it arms RX. Recheck the
  // actual FIFO through the normal ISR before a preexisting packet can hold
  // the OUT endpoint indefinitely. A later packet uses the ordinary IRQ.
  const unsigned key=irq_lock();
  uart_irq_rx_enable(usb);
  (void)serviceUsbIrqOnce(usb);
  irq_unlock(key);
#else
  uart_irq_rx_enable(usb);
#endif
  return 0;
}
static void forgetCompanionPeer(uint32_t id,void *);
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME) || defined(CONFIG_MT_PKI_STOCK_TRIAL)
static bool cachedPeerKeyAllowed(uint32_t peer,const uint8_t publicKey[32]) {
  if(peer==service.route.identity()||!publicKey||!service.nodes.ready()||service.nodes.ignored(peer))return false;
  const int slot=service.nodes.find(peer);const auto *entry=slot<0?nullptr:service.nodes.get(unsigned(slot));
  return !entry||entry->bytes[mt::NodeCache::Entry::KeyLength]!=32||
    !memcmp(entry->bytes+mt::NodeCache::Entry::Key,publicKey,32);
}
#endif
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
// Recovery is a USB-local operation on both target-runtime boards. Keep this
// measured CDC lease separate from T096-only fresh-provision/legacy tools.
class UsbIdentityRecoverySession {
  bool bound_=false,unmeasuredAuthorized_=false;uint32_t epoch_=0,unmeasuredEpoch_=0;
public:
  bool current(uint32_t epoch)const {
    const unsigned key=irq_lock();const auto line=observeUsbLineLocked(usb);
    const bool valid=line.measured?
      (line.connected&&line.generation==epoch&&(!bound_||epoch_==epoch)):
      (IS_ENABLED(CONFIG_BOARD_XIAO_ESP32S3)&&unmeasuredAuthorized_&&epoch==unmeasuredEpoch_&&usbSession.generation()==epoch);
    irq_unlock(key);return valid;
  }
  bool bind(uint32_t epoch){if(!current(epoch))return false;bound_=true;epoch_=epoch;return true;}
  void authorizeUnmeasured(uint32_t epoch){if(IS_ENABLED(CONFIG_BOARD_XIAO_ESP32S3)){unmeasuredAuthorized_=true;unmeasuredEpoch_=epoch;}}
  void clear(){bound_=false;unmeasuredAuthorized_=false;epoch_=0;unmeasuredEpoch_=0;}
};
static UsbIdentityRecoverySession identityRecoverySession;
static mt::pki::PkiIdentityRecoveryIngress<UsbIdentityRecoverySession> identityRestore(privateStorage,identityRecoverySession);
// Normal-image setup is independent of the diagnostic legacy-tail tools.
// Incomplete fresh intent may be continued explicitly while startup is Held;
// the core independently rejects every non-genesis or existing identity.
class FreshUsbContext final:public mt::pki::ProvisioningContext {
  UsbIdentityRecoverySession session_;unsigned depth_=0;bool bound_=false;uint32_t epoch_=0;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  uint64_t deadline_=0;
#endif
public:
  bool bind(uint32_t epoch){if(!session_.bind(epoch))return false;bound_=true;epoch_=epoch;return true;}
  bool current(uint32_t epoch)const{
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    if(deadline_&&millis()>=deadline_)return false;
#endif
    return session_.current(epoch);
  }
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  void limitUntil(uint64_t deadline){deadline_=deadline;}
  bool prepareHeldImportEntropy()const{return mt::ble::prepareHeldImportEntropy();}
  bool currentImportBle(uint32_t token)const{return mt::ble::stockImportLink(token);}
#endif
  bool current()const{return bound_&&current(epoch_);}
  void clear(){session_.clear();bound_=false;epoch_=0;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    deadline_=0;
#endif
  }
  void authorizeUnmeasured(uint32_t epoch){session_.authorizeUnmeasured(epoch);}
  bool tryEnter()override{if(k_mutex_lock(&serviceLock,K_NO_WAIT))return false;++depth_;return true;}
  void leave()override{if(depth_){--depth_;k_mutex_unlock(&serviceLock);}}
  uint64_t millis()const override{const auto value=k_uptime_get();return value<0?UINT64_MAX:uint64_t(value);}
  bool quiescent()const override {
    const auto state=privateStartup.state();
    return depth_&&current()&&initialized&&settingsStartupResult==0&&
      (state==mt::pki::PkiStartup::State::Empty||state==mt::pki::PkiStartup::State::Held)&&
      service.identityRecoveryQuiescent()&&!settingsEdit.open()&&!rebootSchedule.pending()&&
      !firmwareUpdate.pending()&&k_msgq_num_used_get(&rxQueue)==0;
  }
  bool candidateAllowed(const mt::pki::IdentityBinding &candidate)const override {
    return candidate.node!=service.route.identity()&&
      (!service.nodes.ready()||(!service.nodes.ignored(candidate.node)&&service.nodes.find(candidate.node)<0));
  }
  bool peerAllowed(const mt::pki::IdentityBinding &,const mt::pki::ProvisionPeer &)const override{return false;}
};
static FreshUsbContext freshUsbContext;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
static FreshUsbContext importUsbContext;
#endif
static mt::pki::NvsProvisioningSettings freshSettings;
static mt::pki::PkiFreshProvisioning freshProvisioning(freshSettings,freshUsbContext,privateEntropy);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
static mt::pki::PkiFreshProvisioning importedProvisioning(freshSettings,importUsbContext,privateEntropy);
static mt::pki::StockImportConsent<FreshUsbContext> stockImport(importedProvisioning,importUsbContext,privateEntropy);
#endif
#if MT_HAS_DEVICE_RESET
class ResetProvisioningContext final:public mt::pki::ProvisioningContext{
public:
  bool tryEnter()override{return k_mutex_lock(&serviceLock,K_NO_WAIT)==0;}
  void leave()override{k_mutex_unlock(&serviceLock);}
  bool quiescent()const override{return resetRecoveryBoot&&resetHead.state==deviceReset::Floor::State::Selected&&
    serviceLock.owner==k_current_get()&&service.identityRecoveryQuiescent();}
  uint64_t millis()const override{return uint64_t(k_uptime_get());}
  bool candidateAllowed(const mt::pki::IdentityBinding &id)const override{return !memcmp(id.publicKey,resetHead.newKey,32);}
  bool peerAllowed(const mt::pki::IdentityBinding &,const mt::pki::ProvisionPeer &)const override{return false;}
};
static void pollDeviceResetBoot(){
  const int rc=resetBootRetry.poll(uint64_t(k_uptime_get()),[](){
    if(!resetRecoveryBoot||!service.identityRecoveryQuiescent())return -EACCES;
    return deviceReset::rebuild(settings,resetHead,[](){
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
      if(!mt::ble::store::beginDeviceResetErase(service.identityRecoveryQuiescent()))return -EBUSY;
#endif
      int rc=deviceReset::eraseSubtree(settings,"bt");
      if(!rc)rc=deviceReset::eraseSubtree(settings,"mtble/gatt");
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
      mt::ble::store::endBoot(rc==-EAGAIN?0:rc);
#endif
      return rc;
    },[](){
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
      const int retired=expandedContacts.retireDomain(settings,resetHead.oldKey);if(retired)return retired;
#endif
      return eraseResetOwnedFiles();
    },[](const mt::pki::IdentityBinding &expected){
      ResetProvisioningContext context;
      mt::pki::PkiFreshProvisioning owner(freshSettings,context,privateEntropy);
      mt::pki::IdentityBinding staged{};int rc=owner.stage(staged);if(rc)return rc;
      if(staged.node!=expected.node||memcmp(staged.publicKey,expected.publicKey,32))return -EXDEV;
      return owner.confirm(expected);
    });
  });
  if(!rc)sys_reboot(SYS_REBOOT_COLD);
}
#endif

static mt::pki::PkiFreshUsbIngress<FreshUsbContext> freshIngress(freshProvisioning,freshUsbContext);
#endif
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
// Fresh explicit CDC provisioning only. It cannot stop RF, arm TX, import a
// key, regenerate a terminal attempt, choose a store, or change public settings.
class UsbPkiContext final:public mt::pki::ProvisioningContext,public mt::pki::ProvisionUsbSession {
  unsigned depth_=0;bool bound_=false;uint32_t epoch_=0;
public:
  bool current(uint32_t epoch)const override {
    const unsigned key=irq_lock();const auto line=observeUsbLineLocked(usb);
    const bool valid=line.measured&&line.connected&&line.generation==epoch;
    irq_unlock(key);return valid&&(!bound_||epoch_==epoch);
  }
  bool current()const override{return bound_&&current(epoch_);}
  bool bind(uint32_t epoch)override {
    if(!current(epoch))return false;
    bound_=true;epoch_=epoch;return true;
  }
  void clear()override{bound_=false;epoch_=0;}
  bool tryEnter()override{if(k_mutex_lock(&serviceLock,K_NO_WAIT))return false;++depth_;return true;}
  void leave()override{if(depth_){--depth_;k_mutex_unlock(&serviceLock);}}
  uint64_t millis()const override {
    const int64_t value=k_uptime_get();return value<0?UINT64_MAX:uint64_t(value);
  }
  bool quiescent()const override {
    return depth_&&current()&&initialized&&storageReady&&journal.ready()&&
      k_uptime_get()>=0&&privateStartup.state()==mt::pki::PkiStartup::State::Empty&&
      !service.privateConfigured()&&service.storageQuiescent()&&!service.endpointArmed()&&
      !service.faulted()&&radio.ready()&&!radio.busy()&&radio.health()==0&&
      !settingsEdit.open()&&!rebootSchedule.pending()&&k_msgq_num_used_get(&rxQueue)==0;
  }
  bool candidateAllowed(const mt::pki::IdentityBinding &candidate)const override {
    return candidate.node!=service.route.identity()&&service.nodes.ready()&&
      service.nodes.find(candidate.node)<0;
  }
  bool peerAllowed(const mt::pki::IdentityBinding &,const mt::pki::ProvisionPeer &peer)const override {
    return cachedPeerKeyAllowed(peer.node,peer.publicKey);
  }
};
static UsbPkiContext provisionContext;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
static mt::pki::NvsProvisioningSettings provisionSettings;
static mt::pki::PkiProvisioning provisioning(provisionSettings,provisionContext,privateEntropy);
static mt::pki::PkiUsbIngress provisionIngress(provisioning,provisionContext);
#endif
#endif // CONFIG_MT_PKI_STOCK_TRIAL
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
// Shared local-owner storage barrier for target-runtime profiles. USB epoch
// authority remains unavailable unless the T096 stock-trial context exists.
class OwnerPeerContext final:public mt::pki::PeerAdministrationContext {
public:
  bool tryEnter()override{return k_mutex_lock(&serviceLock,K_NO_WAIT)==0;}
  void leave()override{k_mutex_unlock(&serviceLock);}
  bool current(uint32_t epoch)const override {
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
    return provisionContext.current(epoch);
#else
    // Target-only profiles have no USB provisioning or peer-admin epoch.
    (void)epoch;return false;
#endif
  }
  bool ready()const override {
    return initialized&&storageReady&&journal.ready()&&
      (privateStartup.state()==mt::pki::PkiStartup::State::Active||privateStartup.state()==mt::pki::PkiStartup::State::Dormant)&&
      service.privateConfigured()&&!service.faulted()&&!settingsEdit.open()&&
      !rebootSchedule.pending()&&k_msgq_num_used_get(&rxQueue)==0;
  }
  bool peerAllowed(const mt::pki::IdentityBinding &local,const mt::pki::PkiPeerPin &pin)const override {
    (void)local;return cachedPeerKeyAllowed(pin.node,pin.publicKey);
  }
  bool pause()override{return ready()&&service.beginPrivateAdministration();}
  bool resume()override{return service.endPrivateAdministration();}
  void hold()override{(void)privateStartup.hold(service);}
};
static OwnerPeerContext peerContext;
#if defined(CONFIG_MT_PKI_STOCK_TRIAL) && MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
static mt::pki::PkiPeerAdministration peerAdministration(privateStorage.storage(),peerContext);
#endif
// Drain only on the serialized owner loop. The Service candidate is evidence
// from a signature-verified NodeInfo, never a NodeCache promotion. Persistence
// completes under the existing RF/storage pause before the live directory is
// updated; every borrowed identity/key copy is wiped on all exits.
static void pollVerifiedPeerCandidate() {
  struct Scratch {
    mt::pki::IdentityBinding local{};
    mt::pki::KeyRef ref{};
    mt::pki::PkiPeerPin pin{};
    uint32_t node=0;
    ~Scratch(){
      bench::wipeKeyBuffer(&local,sizeof(local));
      bench::wipeKeyBuffer(&ref,sizeof(ref));
      bench::wipeKeyBuffer(&pin,sizeof(pin));
      node=0;
    }
  } scratch;
  auto &store=privateStorage.storage();
  if(!service.peekVerifiedPeerCandidate(scratch.node,scratch.pin.publicKey))return;
  scratch.pin.node=scratch.node;
  if(!initialized||!storageReady||!journal.ready()||settingsEdit.open()||
     rebootSchedule.pending()||k_msgq_num_used_get(&rxQueue)!=0||
     store.state()!=mt::pki::PkiStorage::State::Active||
     !store.compactContacts()||!peerContext.ready()||!service.nodes.ready())return;
  const auto startupState=privateStartup.state();
  if((startupState!=mt::pki::PkiStartup::State::Active&&
      startupState!=mt::pki::PkiStartup::State::Dormant)||!service.privateConfigured())return;
  auto *identity=store.runtimeIdentity();
  if(!identity||!identity->binding(scratch.local,scratch.ref)||
     scratch.local.node!=service.route.identity()) {peerContext.hold();return;}
  if(scratch.pin.node<=1||
     scratch.pin.node>=0xfffffffeu||scratch.pin.node==scratch.local.node) {
    service.clearVerifiedPeerCandidate();return;
  }
  if(!peerContext.peerAllowed(scratch.local,scratch.pin)||
     store.revoked(scratch.pin.node)) {
    service.clearVerifiedPeerCandidate();return;
  }

  uint8_t stored[32]{};
  const bool hasStored=store.lookup(scratch.pin.node,stored);
  const auto afterLookup=store.state();
  if(afterLookup!=mt::pki::PkiStorage::State::Active) {
    bench::wipeKeyBuffer(stored,sizeof(stored));peerContext.hold();return;
  }
  if(hasStored) {
    const bool exact=!memcmp(stored,scratch.pin.publicKey,sizeof(stored));
    const bool active=exact&&store.active(scratch.pin.node,stored);
    bench::wipeKeyBuffer(stored,sizeof(stored));
    if(exact&&!active){peerContext.hold();return;}
    // A different durable key is a terminal conflict. An exact active pin was
    // already published from the startup snapshot; do not write it twice.
    service.clearVerifiedPeerCandidate();
    return;
  }
  bench::wipeKeyBuffer(stored,sizeof(stored));

#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  // A signature proves possession. Expanded membership requires an explicit
  // owner pin. Consume the observation before pausing RF or requesting a write.
  if(store.expandedActive()){service.clearVerifiedPeerCandidate();return;}
#endif
  // A busy endpoint leaves the queued proof intact. Do not consume it until
  // the storage pause succeeds and the same candidate is still present.
  if(!peerContext.pause())return;
  uint32_t stillNode=0;uint8_t stillKey[32]{};
  const bool same=service.peekVerifiedPeerCandidate(stillNode,stillKey)&&
    stillNode==scratch.pin.node&&!memcmp(stillKey,scratch.pin.publicKey,sizeof(stillKey));
  bench::wipeKeyBuffer(stillKey,sizeof(stillKey));
  if(!store.compactContacts()||
     store.state()!=mt::pki::PkiStorage::State::Active) {
    peerContext.hold();return;
  }
  if(!same||!peerContext.peerAllowed(scratch.local,scratch.pin)||
     store.revoked(scratch.pin.node)) {
    if(!peerContext.resume())peerContext.hold();
    if(!same)service.clearVerifiedPeerCandidate();
    return;
  }

  const int committed=store.addTrustedPeer(scratch.local,scratch.pin);
  if(committed!=0) {
    const auto storageState=store.state();
    const bool ordinaryRefusal=committed==-ENOSPC||committed==-EEXIST||committed==-EINVAL;
    if(storageState!=mt::pki::PkiStorage::State::Active||
       (!ordinaryRefusal&&committed!=-EAGAIN&&committed!=-EBUSY)) {
      peerContext.hold();return;
    }
    if(ordinaryRefusal)service.clearVerifiedPeerCandidate();
    if(!peerContext.resume())peerContext.hold();
    return; // Keep a still-valid candidate for a later owner-loop attempt.
  }

  uint32_t consumedNode=0;uint8_t consumedKey[32]{};
  const bool consumed=service.takeVerifiedPeerCandidate(consumedNode,consumedKey)&&
    consumedNode==scratch.pin.node&&!memcmp(consumedKey,scratch.pin.publicKey,sizeof(consumedKey));
  bench::wipeKeyBuffer(consumedKey,sizeof(consumedKey));
  if(!consumed||!privateStartup.adoptPersistedPeer(scratch.pin)) {
    service.clearVerifiedPeerCandidate();peerContext.hold();return;
  }
  if(!peerContext.resume())peerContext.hold();
}
#endif // CONFIG_MT_PKI_TARGET_RUNTIME
#if defined(CONFIG_MT_PKI_STOCK_TRIAL) && MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
class UsbTailPreparationProgress final:public mt::pki::PreparationProgress {
  uint32_t epoch_;const int64_t start_;int64_t previous_;
public:
  explicit UsbTailPreparationProgress(uint32_t epoch):epoch_(epoch),start_(k_uptime_get()),previous_(start_){}
  int checkpoint()override{
    const int64_t now=k_uptime_get();
    if(!mt::pki::preparationDeadline(start_,now,20000)||now<previous_)return -ETIMEDOUT;
    previous_=now;
    if(!peerContext.current(epoch_)||!peerContext.ready())return -EACCES;
    if(!preparationWatchdog||preparationWatchdogChannel<0)return -ENODEV;
    const int rc=wdt_feed(preparationWatchdog,preparationWatchdogChannel);
    return rc>0?-EIO:rc;
  }
};
#endif
static uint64_t ztUptimeMsProvider(void *);
#if defined(CONFIG_MT_TAK_TRACKER)
static void refreshTakConfiguration() {
  meshtastic_ModuleConfig config{};
  const bool ready=storageReady&&extendedEdit.trusted()&&!settingsEdit.open()&&
    !rebootSchedule.pending()&&extendedEdit.config().get(config,meshtastic_ModuleConfig_tak_tag);
  if(ready)(void)service.takConfiguration(uint32_t(config.payload_variant.tak.team),
    uint32_t(config.payload_variant.tak.role));
  else (void)service.takConfiguration(0,0,false);
}
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
static bool gpioModuleAvailable() {
  const auto caps=mt::modules::zephyrGpioModuleBackend().capabilities();
  return caps.exported&&(caps.readable||caps.writable);
}
static void refreshGpioModules() {
  const bool ready=storageReady&&gpioModuleEdit.trusted()&&!settingsEdit.open()&&
    !rebootSchedule.pending()&&service.normalMode()&&service.receiving()&&!service.faulted();
  (void)gpioModuleService.configure(gpioModuleEdit.config(),ready,journal.generation());
}
static void gpioModuleFrame(void *,const uint8_t *frame,size_t size,const bench::RouteMetadata &metadata,
                            unsigned channel,uint32_t now,uint32_t random) {
  refreshGpioModules();gpioModuleService.receive(frame,size,metadata,channel,now,random);
}
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
static void clearRemoteAdminSessions();
#endif
// Preference archive operations use the device-local SettingsBackend only.
// Companion dispatch already owns serviceLock; never take a second lock here.
static bool preferenceLockOwned() {
  return serviceLock.owner==k_current_get()&&serviceLock.lock_count!=0;
}
static bool preferenceJournalsReady() {
  bool ready=journal.ready()&&moduleJournal.ready()&&completionJournal.ready()&&
    extendedJournal.ready()&&notificationJournal.ready()&&wifiJournal.ready()&&
    mqttJournal.ready()&&bluetoothJournal.ready()&&cannedJournal.ready()&&
    ringtoneJournal.ready()&&uiConfigJournal.ready();
#if defined(CONFIG_MT_GPIO_MODULES)
  ready=ready&&gpioModuleJournal.ready();
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  ready=ready&&storeForwardJournal.ready();
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
  ready=ready&&adminKeyJournal.ready();
#endif
  return ready;
}
static bool preferenceOwnerReady(void *) {
  if(!preferenceLockOwned()||!initialized||settingsStartupResult!=0||
     !preferenceDomainReady||settingsEdit.open()||rebootSchedule.pending()||
     firmwareUpdate.pending()||pendingBluetooth.busy()||persistentWorkPending()||
     k_msgq_num_used_get(&rxQueue)!=0)return false;
  if(preferenceRecoveryHeld)return service.identityRecoveryQuiescent();
  return storageReady&&preferenceJournalsReady()&&service.storageQuiescent()&&
    radio.ready()&&!radio.busy()&&radio.health()==0;
}
static bool preferenceOwnerQuiescent(void *) {
  if(!preferenceLockOwned())return false;
  if(preferenceRecoveryHeld)return service.identityRecoveryQuiescent();
  return service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0;
}
static int preferenceAdmit(void *,uint8_t id,const uint8_t *bytes,size_t length) {
  if(!bytes)return -EINVAL;
  if(id==mt::admin::PreferenceSections::Completion) {
    mt::modules::CompletionSettings value;
    if(length!=sizeof(value))return -EINVAL;
    memcpy(value.bytes,bytes,length);if(!value.valid())return -EINVAL;
    static constexpr unsigned tags[]={meshtastic_ModuleConfig_audio_tag,
      meshtastic_ModuleConfig_ambient_lighting_tag,meshtastic_ModuleConfig_paxcounter_tag,
      meshtastic_ModuleConfig_telemetry_tag,meshtastic_ModuleConfig_external_notification_tag,
      meshtastic_ModuleConfig_serial_tag,meshtastic_ModuleConfig_canned_message_tag,
      meshtastic_ModuleConfig_range_test_tag};
    for(unsigned tag:tags) {
      meshtastic_ModuleConfig config{};if(!value.get(config,tag))continue;
      if(mt::modules::CompletionRuntime::handles(tag)) {
        if(!completionRuntime.supports(config))return -ENOTSUP;
      } else if(tag==meshtastic_ModuleConfig_serial_tag) {
#if defined(CONFIG_MT_SERIAL_MODULE)
        mt_serial::Config runtime{};uint32_t baud=0;
        if(!serialRuntimeConfig(config.payload_variant.serial,runtime,baud)||
           (config.payload_variant.serial.enabled&&!serialModule.configSupported(
             runtime,baud,config.payload_variant.serial.rxd,config.payload_variant.serial.txd)))return -ENOTSUP;
#else
        if(config.payload_variant.serial.enabled)return -ENOTSUP;
#endif
      } else if(tag==meshtastic_ModuleConfig_canned_message_tag) {
        const auto &c=config.payload_variant.canned_message;
        if(c.rotary1_enabled||c.updown1_enabled||c.inputbroker_pin_a||c.inputbroker_pin_b||
           c.inputbroker_pin_press||c.inputbroker_event_cw||c.inputbroker_event_ccw||
           c.inputbroker_event_press||!memchr(c.allow_input_source,0,sizeof(c.allow_input_source)))return -ENOTSUP;
      } else if(tag==meshtastic_ModuleConfig_range_test_tag) {
        const auto &c=config.payload_variant.range_test;
        if(c.save||c.clear_on_reboot)return -ENOTSUP;
      }
    }
    return 0;
  }
#if defined(CONFIG_MT_GPIO_MODULES)
  if(id==mt::admin::PreferenceSections::Gpio) {
    mt::modules::GpioModuleSettings value;
    return mt::modules::GpioModuleSettings::decode(bytes,length,value)&&
      gpioModuleRuntime.validateConfig(value)?0:-ENOTSUP;
  }
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  if(id==mt::admin::PreferenceSections::StoreForward) {
    mt::modules::StoreForwardSettings value;
    if(!mt::modules::StoreForwardSettings::decode(bytes,length,value))return -EINVAL;
    meshtastic_ModuleConfig wire{};
    return value.get(wire.payload_variant.store_forward)&&storeForwardPsramReady?0:-ENOTSUP;
  }
#endif
  return 0;
}
static bool preferenceMqttAdmission(void *,bool &runtimeSupported,bool &trustReady) {
#if defined(CONFIG_MT_MQTT_GATEWAY)
  runtimeSupported=true;trustReady=mqttTrustAvailable;
#else
  runtimeSupported=false;trustReady=false;
#endif
  return true;
}
static mt::admin::PreferenceSections::Owner preferenceOwner() {
  using Sections=mt::admin::PreferenceSections;
  Sections::Owner owner{};owner.backend=&settings;owner.ready=preferenceOwnerReady;
  owner.quiescent=preferenceOwnerQuiescent;owner.admit=preferenceAdmit;
  owner.mqttAdmission=preferenceMqttAdmission;
  owner.sources.core=&journal.config();owner.sources.modules=&moduleJournal.config();
  owner.sources.completion=&completionJournal.config();owner.sources.extended=&extendedJournal.config();
  owner.sources.notifications=&notificationJournal.config();owner.sources.wifi=&wifiJournal.config();
  owner.sources.mqtt=&mqttJournal.config();owner.sources.bluetooth=&bluetoothJournal.config();
  owner.sources.canned=&cannedJournal.config();owner.sources.ringtone=&ringtoneJournal.config();
  owner.sources.ui=&uiConfigJournal.config();
#if defined(CONFIG_MT_GPIO_MODULES)
  owner.sources.gpio=&gpioModuleJournal.config();
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  owner.sources.storeForward=&storeForwardJournal.config();
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
  owner.sources.adminKeys=&adminKeyJournal.settings();
#endif
  return owner;
}
static int preferenceArchiveCall(pb_size_t operation) {
  if(!preferenceDomainReady)return -EACCES;
  auto owner=preferenceOwner();uint8_t ids[mt::admin::PreferenceArchive::MaxSections]{};
  size_t count=0;
  if(!mt::admin::PreferenceSections::ids(owner.sources,ids,count))return -EACCES;
  const auto callbacks=mt::admin::PreferenceSections::callbacks(owner);
  if(operation==meshtastic_AdminMessage_backup_preferences_tag)
    return preferenceArchive.backup(settings,preferenceDomain,callbacks,ids,count);
  if(operation==meshtastic_AdminMessage_remove_backup_preferences_tag)
    return preferenceArchive.remove(settings,preferenceDomain,callbacks);
  if(operation==meshtastic_AdminMessage_restore_preferences_tag)
    return preferenceArchive.restore(settings,preferenceDomain,callbacks,ids,count);
  return -ENOTSUP;
}
static int preferenceCommand(pb_size_t operation,uint32_t now,mt::UsbSessionEpoch::RestoreReceipt *receipt) {
  if(!preferenceLockOwned()||!storageReady||!preferenceJournalsReady()||!preferenceDomainReady||
     preferenceRecoveryHeld||persistentWorkPending()||pendingBluetooth.busy()||
     rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
  const bool wasReceiving=service.receiving(),wasRelaying=service.relaying(),wasArmed=service.endpointArmed();
  const int stopped=service.pauseForStorage();
  if(stopped)return stopped;
  const bool safe=service.storageQuiescent()&&!service.faulted()&&radio.ready()&&!radio.busy()&&radio.health()==0;
  const int result=safe?preferenceArchiveCall(operation):-EIO;
  if(operation==meshtastic_AdminMessage_restore_preferences_tag) {
    auto intent=mt::admin::PreferenceArchive::IntentState::Absent;
    const int inspected=preferenceArchive.inspectRestoreIntent(settings,intent);
    if(result==0||preferenceArchive.frozen()||inspected||intent==mt::admin::PreferenceArchive::IntentState::Pending||
       intent==mt::admin::PreferenceArchive::IntentState::Corrupt) {
      preferenceRecoveryHeld=true;storageReady=false;
      syncConsoleSecurityPolicy(receipt);
      syncApiLogPolicy();
      mt::ble::setIngressOpen(false);
      scheduleOrdinaryReboot(1,uint64_t(k_uptime_get()));
      if(!result&&(inspected||intent!=mt::admin::PreferenceArchive::IntentState::Completed))return -EIO;
      return result;
    }
  }
  bool restored=true;
  if(wasReceiving) {
    restored=!service.faulted()&&service.config.provisioned()&&service.start(wasRelaying,now);
    if(restored&&wasArmed)restored=service.normalMode()&&service.txAllowed()&&service.config.txEnabled();
    if(restored)restored=service.armEndpoint(wasArmed)&&!service.faulted();
  } else if(wasArmed)restored=false;
  if(!restored){service.stop();return -EIO;}
  return result;
}
static int resumePreferenceRestore(uint32_t now) {
  (void)now;
  if(!preferenceLockOwned()||!preferenceRecoveryHeld||settingsStartupResult!=0||
     !preferenceDomainReady||settingsEdit.open()||rebootSchedule.pending()||
     firmwareUpdate.pending()||persistentWorkPending()||pendingBluetooth.busy()||
     k_msgq_num_used_get(&rxQueue)!=0||
     !service.identityRecoveryQuiescent())return -EACCES;
  auto owner=preferenceOwner();uint8_t ids[mt::admin::PreferenceArchive::MaxSections]{};
  size_t count=0;
  if(!mt::admin::PreferenceSections::ids(owner.sources,ids,count))return -EACCES;
  const auto callbacks=mt::admin::PreferenceSections::callbacks(owner);
  const int result=preferenceArchive.resume(settings,preferenceDomain,callbacks,ids,count);
  if(result==0) {
    preferenceRecoveryHeld=true;storageReady=false;
    syncConsoleSecurityPolicy();
    syncApiLogPolicy();
    mt::ble::setIngressOpen(false);
    scheduleOrdinaryReboot(1,uint64_t(k_uptime_get()));
  }
  return result;
}
#if defined(CONFIG_MT_RF_ADMIN_GET)
static mt::admin::RemoteDeviceTransaction remoteDeviceTransaction MT_WIFI_PSRAM_BSS(remote_device_transaction);
static mt::admin::RemoteDeviceBundle remoteDeviceCandidate MT_WIFI_PSRAM_BSS(remote_device_candidate);
// The serialized role transaction validates and writes exactly one section at
// a time. The complete candidate has its own lifetime throughout the commit.
template<class T> struct RemoteSection: T {
  static void *operator new(size_t,void *location)noexcept{return location;}
};
union RemoteSectionScratch {
  RemoteSection<mt::Config> core;
  RemoteSection<mt::ModuleSettings> modules;
  RemoteSection<mt::modules::CompletionSettings> completion;
  RemoteSection<mt::NotificationSettings> notifications;
  RemoteSection<mt::network::MqttGatewaySettings> mqtt;
  RemoteSectionScratch(){}
  static void *operator new(size_t,void *location)noexcept{return location;}
  template<class T> T &load(T *slot,const uint8_t *bytes,size_t length){
    static_assert(__has_trivial_destructor(T),"scratch sections need no destruction");
    T *value=new(slot) T;memcpy(value->bytes,bytes,length);return *value;
  }
};
static bool remoteDevicePreparing=false,remoteDevicePending=false;
static uint32_t remoteDeviceMqttGeneration=0;
struct RemoteDeviceProjectionScratch {
  union {
    RemoteSection<mt::modules::RouterRoleDefaults::Projection> router;
    RemoteSection<mt::modules::RouterLateRoleDefaults::Projection> late;
  };
  meshtastic_ModuleConfig wire{};
  meshtastic_Config_PowerConfig power{};
  RemoteDeviceProjectionScratch(){}
  static void *operator new(size_t,void *location)noexcept{return location;}
};
// Under the preference lock, role projection finishes and is wiped before any
// journal callback starts. The persistent candidate is separate from both.
union RemoteDeviceWork {
  RemoteSectionScratch section;
  RemoteDeviceProjectionScratch projection;
  RemoteDeviceWork(){}
  RemoteSectionScratch &beginSection(){return *new(&section) RemoteSectionScratch;}
  RemoteDeviceProjectionScratch &beginProjection(){return *new(&projection) RemoteDeviceProjectionScratch;}
};
static_assert(__has_trivial_destructor(RemoteDeviceWork));
static RemoteDeviceWork remoteDeviceWork MT_WIFI_PSRAM_BSS(remote_device_work);
static bool roleBackendReady(void *) {
  return preferenceLockOwned()&&settingsStartupResult==0&&storageReady;
}
static bool roleJournalsReady(void *) {
  return roleBackendReady(nullptr)&&journal.ready()&&moduleJournal.ready()&&
    completionJournal.ready()&&notificationJournal.ready()&&mqttJournal.ready();
}
static bool roleStoragePaused(void *) {
  return roleBackendReady(nullptr)&&!settingsEdit.open()&&service.storageQuiescent()&&
    !service.faulted()&&radio.ready()&&!radio.busy()&&radio.health()==0;
}
static mt::admin::RemoteDeviceTransaction::Domain remoteDeviceDomain() {
  mt::admin::RemoteDeviceTransaction::Domain domain{};
  domain.node=preferenceDomain.node;
  memcpy(domain.publicKey,preferenceDomain.publicKey,sizeof(domain.publicKey));return domain;
}
static int roleCapture(void *,uint8_t section,uint8_t *out,size_t capacity,size_t &length) {
  length=0;if(!roleJournalsReady(nullptr)||!out)return -EACCES;
  const uint8_t *bytes=nullptr;
  switch(section) {
  case 0:bytes=journal.config().bytes;length=mt::Config::Size;break;
  case 1:bytes=moduleJournal.config().bytes;length=mt::ModuleSettings::Size;break;
  case 2:bytes=completionJournal.config().bytes;length=mt::modules::CompletionSettings::Size;break;
  case 3:bytes=notificationJournal.config().bytes;length=mt::NotificationSettings::Size;break;
  case 4:bytes=mqttJournal.config().bytes;length=mt::network::MqttGatewaySettings::Size;break;
  default:return -EINVAL;
  }
  if(capacity<length)return -E2BIG;
  memcpy(out,bytes,length);return 0;
}
static int roleDecodeScratch(RemoteSectionScratch &scratch,uint8_t section,const uint8_t *bytes,size_t length) {
  if(!bytes||length!=mt::admin::RemoteDeviceBundle::sectionSize(section))return -EINVAL;
  switch(section) {
  case 0:
    scratch.load(&scratch.core,bytes,length);
    return mt::admin::RemoteDeviceBundle::validIntentCore(scratch.core)?0:-EINVAL;
  case 1:return scratch.load(&scratch.modules,bytes,length).valid()?0:-EINVAL;
  case 2:return scratch.load(&scratch.completion,bytes,length).valid()?0:-EINVAL;
  case 3:return scratch.load(&scratch.notifications,bytes,length).valid()?0:-EINVAL;
  case 4:return scratch.load(&scratch.mqtt,bytes,length).valid()?0:-EINVAL;
  default:return -EINVAL;
  }
}
static int roleValidate(void *,uint8_t section,const uint8_t *bytes,size_t length) {
  auto &scratch=remoteDeviceWork.beginSection();
  const int result=roleDecodeScratch(scratch,section,bytes,length);
  bench::wipeKeyBuffer(&remoteDeviceWork,sizeof(remoteDeviceWork));return result;
}
static int roleSave(void *,uint8_t section,const uint8_t *bytes,size_t length) {
  if(!roleJournalsReady(nullptr)||!roleStoragePaused(nullptr))return -EACCES;
  auto &scratch=remoteDeviceWork.beginSection();
  mt::detail::VolatileWipe wipe{&remoteDeviceWork,sizeof(remoteDeviceWork)};
  const int valid=roleDecodeScratch(scratch,section,bytes,length);if(valid)return valid;
  switch(section) {
  case 0:
    if(scratch.core.bytes[2]!=mt::Config::Version) {
      // Migrate only this validated save scratch. The original intent remains byte-exact.
      if(!mt::Config::decode(scratch.core.bytes,mt::Config::Size,
                             scratch.core))return -EINVAL;
    }
    return journal.save(settings,scratch.core);
  case 1:return moduleJournal.save(settings,scratch.modules);
  case 2:return completionJournal.save(settings,scratch.completion);
  case 3:return notificationJournal.save(settings,scratch.notifications);
  case 4:
    // Choose a narrow policy-only journal entrypoint before any write; never
    // retry an ambiguous persistence error through the other path.
    if(!memcmp(mqttJournal.config().bytes,scratch.mqtt.bytes,
        mt::network::MqttGatewaySettings::ConsentBase))
      return mqttJournal.saveChannelConsentCandidate(settings,scratch.mqtt);
    return mqttJournal.saveLoRaPolicyCandidate(settings,scratch.mqtt);
  default:return -EINVAL;
  }
}
static bool roleDigest(void *,const uint8_t *bytes,size_t length,uint8_t out[32]) {
  if(!bytes||!length||!out)return false;
  crypto_blake2b(out,32,bytes,length);return true;
}
static mt::admin::RemoteDeviceTransaction::Callbacks roleCallbacks() {
  return {&settings,nullptr,roleBackendReady,roleJournalsReady,roleStoragePaused,
    roleCapture,roleValidate,roleSave,roleDigest};
}
static bool remoteDeviceJournalsMatch() {
  return roleJournalsReady(nullptr)&&journal.config().same(remoteDeviceCandidate.core)&&
    moduleJournal.config().same(remoteDeviceCandidate.modules)&&
    completionJournal.config().same(remoteDeviceCandidate.completion)&&
    notificationJournal.config().same(remoteDeviceCandidate.notifications)&&
    !memcmp(mqttJournal.config().bytes,remoteDeviceCandidate.mqtt.bytes,mt::network::MqttGatewaySettings::Size);
}
static int persistRemoteDevice(void *) {
  if(!remoteDevicePreparing||!preferenceDomainReady||!roleStoragePaused(nullptr))return -EACCES;
  remoteDevicePending=true;remoteDeviceMqttGeneration=mqttJournal.generation();
  mt::admin::RemoteDeviceTransaction::Outcome outcome{};
  const int result=remoteDeviceTransaction.beginAndCommit(remoteDeviceDomain(),remoteDeviceCandidate,
    roleCallbacks(),outcome);
  if(result||!outcome.completeVerified||!remoteDeviceJournalsMatch()) {
    remoteConfigHeld=true;return result?result:-EIO;
  }
  return 0;
}
static int recoverRemoteDeviceAtBoot() {
  if(!roleBackendReady(nullptr)||!roleStoragePaused(nullptr))return -EACCES;
  if(!preferenceDomainReady) {
    // A fresh unbound node may have no role intent. Any surviving value with
    // no established identity domain holds RF; do not guess its owner.
    uint8_t byte=0;
    const int a=settings.read("mtrole/intent0",&byte,1),b=settings.read("mtrole/intent1",&byte,1);
    return a==-ENOENT&&b==-ENOENT?0:-EXDEV;
  }
  mt::admin::RemoteDeviceTransaction::Outcome outcome{};
  int result=remoteDeviceTransaction.inspect(remoteDeviceDomain(),roleCallbacks(),outcome);
  if(result||outcome.state!=mt::admin::RemoteDeviceTransaction::State::Pending)return result;
  result=journal.load(settings);if(!result)result=moduleJournal.load(settings);
  if(!result)result=completionJournal.load(settings);
  if(!result)result=notificationJournal.load(settings);
  if(!result)result=mqttJournal.load(settings);
  if(result)return result;
  result=remoteDeviceTransaction.resume(remoteDeviceDomain(),roleCallbacks(),outcome);
  return result?result:outcome.completeVerified?0:-EIO;
}
#endif
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
static mt::ui::ReportFrames::Origin reportContact(uint32_t from){
  mt::ui::ReportFrames::Origin origin{};
  const int slot=service.nodes.find(from);
  const auto *peer=slot<0?nullptr:service.nodes.get(unsigned(slot));
  if(!peer||peer->ignored()||peer->placeholder())return origin;
  origin.node=from;origin.epoch=service.nodes.replayEpoch(unsigned(slot));origin.favorite=peer->favorite();
  for(unsigned i=0;i<4&&peer->bytes[mt::NodeCache::Entry::Short+i];++i){
    const char ch=char(peer->bytes[mt::NodeCache::Entry::Short+i]);origin.name[i]=(ch>=32&&ch<127)?ch:' ';
  }
  return origin;
}
static void validateDisplayReports(uint64_t now){
  mt::ui::telemetryReports().validate(storageReady&&completionEdit.trusted()?service.route.identity():0,
                                     service.waypointDomainEpoch(),now,reportContact);
}
static void observeDisplayReport(uint32_t from,uint32_t id,bool shared,const meshtastic_Telemetry &report){
  const auto now=uint64_t(k_uptime_get());
  validateDisplayReports(now);
  auto origin=reportContact(from);
  if(from==service.route.identity())origin.node=from;
  mt::ui::telemetryReports().observe(origin,shared,id,now,report);
}
#endif
class ClientHost:public mt::Companion::Host {
  uint32_t usbAcceptedGeneration_=0;
  bool usbAccepted_=false;
  bool sendUsb(const uint8_t *data,size_t size,bool recovery,bool idleOnly=false) {
    if(!data||!size)return false;
    const unsigned key=irq_lock();const auto line=observeUsbLineLocked(usb);
    const bool admitted=usbAccepted_&&line.generation==usbAcceptedGeneration_&&
      (!line.measured||line.connected)&&(recovery||mt_security::serialConsoleAllowed());
    if(!admitted||(idleOnly&&ring_buf_size_get(&usbTx))||ring_buf_space_get(&usbTx)<size){irq_unlock(key);return false;}
    const bool queued=ring_buf_put(&usbTx,data,size)==size;
    if(queued)uart_irq_tx_enable(usb);
    irq_unlock(key);return queued;
  }
public:
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  int sensorAdmin(const meshtastic_SensorConfig &cfg,uint32_t session,uint32_t id)override{
    if(!ready()||factoryConfigResetScheduled)return -EBUSY;
    return mt::stock::telemetry::accessory::submit(cfg,session,id);
  }
  int scaleAdmin(uint32_t weight,uint32_t session,uint32_t id)override{
    if(!ready()||factoryConfigResetScheduled)return -EBUSY;
    return mt::stock::telemetry::accessory::submitScale(weight,session,id);
  }
  bool sensorAdminResult(uint32_t session,uint32_t &id,int &result)override{
    return mt::stock::telemetry::accessory::take(session,id,result);
  }
#elif defined(CONFIG_MT_SCD4X_ADMIN)
  int sensorAdmin(const meshtastic_SensorConfig &cfg,uint32_t session,uint32_t id)override{
    return mt::stock::telemetry::scd4x::submit(cfg,session,id);
  }
  bool sensorAdminResult(uint32_t session,uint32_t &id,int &result)override{
    return mt::stock::telemetry::scd4x::take(session,id,result);
  }
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
  int oneWireAdmin(unsigned channel)override{return mt::stock::telemetry::oneWire::select(channel);}
#endif
#if defined(CONFIG_MT_SHTXX_ADMIN)
  int shtAccuracy(const meshtastic_SensorConfig &cfg)override{return mt::stock::telemetry::setShtAccuracy(cfg);}
#endif
  bool consoleAllowed()const override{return mt_security::serialConsoleAllowed();}
  bool sendIfQueueEmpty(const uint8_t *data,size_t size)override{return sendUsb(data,size,false,true);}
  bool sendRecovery(const uint8_t *data,size_t size)override{return sendUsb(data,size,true);}
  bool safeApiLogEnabled()const override{return apiLogs.enabled();}
  void safeApiLogSession(bool ble,bool active)override {
    const auto consumer=ble?mt::api::ApiLogBuffer::Consumer::Ble:mt::api::ApiLogBuffer::Consumer::Usb;
    if(active) {
      if((ble?!mt::ble::connected():!mt_security::serialConsoleAllowed())||!apiLogs.beginSession(consumer))return;
      recordApiEvent(ble?mt::api::ApiLogBuffer::Event::BleSessionOpened:mt::api::ApiLogBuffer::Event::UsbSessionOpened);
    } else {
      apiLogs.endSession(consumer);
      recordApiEvent(ble?mt::api::ApiLogBuffer::Event::BleSessionClosed:mt::api::ApiLogBuffer::Event::UsbSessionClosed);
    }
  }
  bool nextSafeApiLog(bool ble,meshtastic_LogRecord &out,uint32_t &dropped)override {
    out={};dropped=0;
    if(ble?!mt::ble::connected():!mt_security::serialConsoleAllowed())return false;
    return apiLogs.next(ble?mt::api::ApiLogBuffer::Consumer::Ble:mt::api::ApiLogBuffer::Consumer::Usb,out,dropped);
  }
  int saveRemoteConfig(const mt::Config &candidate,uint32_t now)override {
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!preferenceLockOwned()||!remoteResponseObserverReady||remoteMutation.lane==RemoteAdminLane::None||
       remoteMutation.peer<=1||remoteMutation.peer>=0xfffffffeu||!remoteMutation.request||
       !ready()||!candidate.valid()||!journal.ready())return -EACCES;
    if(settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending()||
       pendingBluetooth.busy()||persistentWorkPending()||service.privateAdminQueriesPending()||
       service.adminRequestsQueued())return -EBUSY;
    // Only the exact prepared Device owner may add role/default journals.
    if(remoteDevicePreparing&&!candidate.same(remoteDeviceCandidate.core))return -EINVAL;
    if((!remoteDevicePreparing&&(candidate.role()!=service.config.role()||
        candidate.rebroadcastMode()!=service.config.rebroadcastMode()))||
       candidate.licensedOperation()!=service.config.licensedOperation())return -ENOTSUP;
#if defined(CONFIG_LOG_BACKEND_UART) && CONFIG_LOG_BACKEND_UART
    if(!candidate.serialEnabled())return -ENOTSUP;
#endif
    if(candidate.adminChannelEnabled()&&!legacyAdminChannelConfigured(candidate))return -EACCES;
    if(candidate.isManaged()) {
      if(candidate.licensedOperation()||!adminKeyJournal.ready()||
         !adminKeyJournal.settings().valid()||!adminKeyJournal.settings().count())return -EACCES;
      bool currentAdmin=false;
      for(size_t i=0;i<adminKeyJournal.settings().count();++i) {
        const auto *key=adminKeyJournal.settings().key(i);
        if(service.privateAdminPeerCurrent(bench::MtSignature::nodeId(key),key)){currentAdmin=true;break;}
      }
      if(!currentAdmin)return -EACCES;
    }
    if(candidate.same(service.config)&&!remoteDevicePreparing)return 0;
    bool durable=false;uint32_t generation=0;
    const int result=settingsEdit.persistForRemoteReply(candidate,now,durable,generation,
      remoteDevicePreparing?persistRemoteDevice:nullptr,nullptr);
    if(result&&remoteDevicePreparing)remoteConfigHeld=true;
    if(durable) {
      auto fingerprint=configFingerprint(candidate);
      const bool staged=pendingRemoteConfig.stage(candidate,true,generation,fingerprint,
        remoteMutation.peer,remoteMutation.request,k_uptime_get_32());
      bench::wipeKeyBuffer(&fingerprint,sizeof(fingerprint));
      pendingRemoteOwner=remoteMutation;remoteConfigHeld=!staged||result!=0;
      service.configurationTransition(true);
      // A durable candidate is never discarded after an old-profile resume
      // error. Explicit recovery/reboot will load that same journal record.
      if(!staged){syncConsoleSecurityPolicy();syncApiLogPolicy();return -EIO;}
    }
    syncConsoleSecurityPolicy();syncApiLogPolicy();return result;
#else
    (void)candidate;(void)now;return -ENOTSUP;
#endif
  }
  int saveRemoteDevice(const mt::Config &candidate,bool disabled,uint32_t now)override {
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!preferenceLockOwned()||!ready()||remoteDevicePreparing||remoteDevicePending||
       !preferenceDomainReady||!candidate.valid()||!moduleEdit.ready()||!completionEdit.trusted()||
       !notificationEdit.ready()||!mqttJournal.ready()||settingsEdit.open()||persistentWorkPending())return -EACCES;
    const auto role=candidate.role();
    if(role!=0&&role!=1&&role!=2&&role!=11&&role!=12&&!mt::modules::OperatingRole::handles(role))return -ENOTSUP;
    if(!mt::notification::available()&&!disabled)return -ENOTSUP;
    if(candidate.role()==service.config.role()&&
       candidate.rebroadcastMode()==service.config.rebroadcastMode()&&disabled==heartbeatDisabled())
      return saveRemoteConfig(candidate,now);
    struct WipeUnusedDeviceCandidate {
      ~WipeUnusedDeviceCandidate(){if(!remoteDevicePending)
        bench::wipeKeyBuffer(&remoteDeviceCandidate,sizeof(remoteDeviceCandidate));}
    } wipeUnusedCandidate;
    remoteDeviceCandidate.core=candidate;
    remoteDeviceCandidate.modules=moduleJournal.config();
    remoteDeviceCandidate.completion=completionJournal.config();
    remoteDeviceCandidate.notifications=notificationJournal.config();
    remoteDeviceCandidate.mqtt=mqttJournal.config();
    {
      auto &scratch=remoteDeviceWork.beginProjection();
      mt::detail::VolatileWipe wipeProjection{&remoteDeviceWork,sizeof(remoteDeviceWork)};
      if(role==mt::DeviceRolePolicy::Role::ROUTER&&role!=service.config.role()) {
        new(&scratch.router) RemoteSection<mt::modules::RouterRoleDefaults::Projection>;
        if(!moduleConfig(scratch.wire,meshtastic_ModuleConfig_telemetry_tag)||!powerConfig(scratch.power))return -EACCES;
        if(mt::modules::RouterRoleDefaults::project(service.config.role(),candidate,moduleEdit.config(),
            scratch.router,&scratch.wire.payload_variant.telemetry,&scratch.power)!=
           mt::modules::RouterRoleDefaults::Result::Applied)return -EINVAL;
        scratch.wire.payload_variant.telemetry=scratch.router.telemetry;
        if(!completionRuntime.supports(scratch.wire)||!completionRuntime.supportsPower(scratch.router.power))return -ENOTSUP;
        if(!remoteDeviceCandidate.completion.set(scratch.wire))return -EINVAL;
        if(!moduleConfig(scratch.wire,meshtastic_ModuleConfig_paxcounter_tag))return -EACCES;
        scratch.wire.payload_variant.paxcounter.paxcounter_update_interval=scratch.router.paxcounterInterval;
        if(!remoteDeviceCandidate.completion.set(scratch.wire)||
           !remoteDeviceCandidate.completion.setPower(scratch.router.power))return -EINVAL;
        remoteDeviceCandidate.core=scratch.router.core;remoteDeviceCandidate.modules=scratch.router.modules;
      } else if(mt::modules::OperatingRole::handles(role)&&role!=service.config.role()) {
        const bool hidden=role==mt::DeviceRolePolicy::Role::CLIENT_HIDDEN;
        if(hidden&&!moduleConfig(scratch.wire,meshtastic_ModuleConfig_telemetry_tag))return -EACCES;
        if(!mt::modules::OperatingRole::project(remoteDeviceCandidate.core,remoteDeviceCandidate.modules,&scratch.wire))return -EINVAL;
        if(hidden&&!remoteDeviceCandidate.completion.set(scratch.wire))return -EINVAL;
      } else if(role==mt::DeviceRolePolicy::Role::ROUTER_LATE&&role!=service.config.role()) {
        new(&scratch.late) RemoteSection<mt::modules::RouterLateRoleDefaults::Projection>;
        if(mt::modules::RouterLateRoleDefaults::project(service.config.role(),role,service.config,
            moduleEdit.config(),scratch.late,candidate.rebroadcastMode())!=
           mt::modules::RouterLateRoleDefaults::Result::Applied)return -EINVAL;
        remoteDeviceCandidate.core=scratch.late.core;remoteDeviceCandidate.modules=scratch.late.modules;
        if(!remoteDeviceCandidate.core.setNodeInfoSeconds(candidate.node().interval()))return -EINVAL;
      }
    }
    if(!remoteDeviceCandidate.notifications.setHeartbeatDisabled(disabled)||!remoteDeviceCandidate.valid())return -EINVAL;
    struct Preparing {
      Preparing(){remoteDevicePreparing=true;}
      ~Preparing(){remoteDevicePreparing=false;}
    } preparing;
    return saveRemoteConfig(remoteDeviceCandidate.core,now);
#else
    (void)candidate;(void)disabled;(void)now;return -ENOTSUP;
#endif
  }
  int saveRemoteChannel(const mt::Config &candidate,unsigned consentIndex,bool enabled,uint32_t now)override {
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!preferenceLockOwned()||!ready()||!candidate.valid()||consentIndex>=8||
       remoteDevicePreparing||remoteDevicePending||settingsEdit.open()||persistentWorkPending())return -EACCES;
    if(candidate.role()!=service.config.role()||candidate.rebroadcastMode()!=service.config.rebroadcastMode()||
       candidate.licensedOperation()!=service.config.licensedOperation())return -EINVAL;
    if(!mqttAdmin.ready())return -EACCES;
    if(!preferenceDomainReady||!roleJournalsReady(nullptr))return -EACCES;
    struct WipeUnusedChannelCandidate {
      ~WipeUnusedChannelCandidate(){if(!remoteDevicePending)
        bench::wipeKeyBuffer(&remoteDeviceCandidate,sizeof(remoteDeviceCandidate));}
    } wipeUnusedCandidate;
    remoteDeviceCandidate.core=candidate;
    remoteDeviceCandidate.modules=moduleJournal.config();
    remoteDeviceCandidate.completion=completionJournal.config();
    remoteDeviceCandidate.notifications=notificationJournal.config();
    remoteDeviceCandidate.mqtt=mqttJournal.config();
    if(!remoteDeviceCandidate.mqtt.setChannelUplink(candidate,consentIndex,enabled)||
       !remoteDeviceCandidate.valid())return -EINVAL;
    if(!memcmp(remoteDeviceCandidate.mqtt.bytes,mqttJournal.config().bytes,
        mt::network::MqttGatewaySettings::Size))return saveRemoteConfig(candidate,now);
    // Check before creating an intent; an older recovered slot may carry stale
    // consent. Unchanged metadata and previously bound intents keep recovery.
    if(!mqttAdmin.remotePolicyWritable())return -EACCES;
    struct Preparing {
      Preparing(){remoteDevicePreparing=true;}
      ~Preparing(){remoteDevicePreparing=false;}
    } preparing;
    return saveRemoteConfig(remoteDeviceCandidate.core,now);
#else
    (void)candidate;(void)consentIndex;(void)enabled;(void)now;return -ENOTSUP;
#endif
  }
  int saveRemoteLoRa(const mt::Config &candidate,bool configOk,uint32_t now)override {
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!preferenceLockOwned()||!ready()||!candidate.valid()||remoteDevicePreparing||
       remoteDevicePending||settingsEdit.open()||persistentWorkPending())return -EACCES;
    if(candidate.role()!=service.config.role()||candidate.rebroadcastMode()!=service.config.rebroadcastMode()||
       candidate.licensedOperation()!=service.config.licensedOperation())return -EINVAL;
    // MQTT metadata exists on proxy/config-only profiles too. An unavailable
    // journal is not proof that its existing regional root or consent is empty.
    if(!mqttAdmin.ready()||!preferenceDomainReady||!roleJournalsReady(nullptr))return -EACCES;
    struct WipeUnusedLoRaCandidate {
      ~WipeUnusedLoRaCandidate(){if(!remoteDevicePending)
        bench::wipeKeyBuffer(&remoteDeviceCandidate,sizeof(remoteDeviceCandidate));}
    } wipeUnusedCandidate;
    remoteDeviceCandidate.core=candidate;
    remoteDeviceCandidate.modules=moduleJournal.config();
    remoteDeviceCandidate.completion=completionJournal.config();
    remoteDeviceCandidate.notifications=notificationJournal.config();
    remoteDeviceCandidate.mqtt=mqttJournal.config();
    auto &mqtt=remoteDeviceCandidate.mqtt;
    char nextRoot[mt::network::MqttGatewaySettings::RootMax+1]{};
    mt::detail::VolatileWipe wipeRoot{nextRoot,sizeof(nextRoot)};
    const char *region=candidate.regionName();
    const auto root=mt::network::MqttRegionRootPolicy::apply(
      reinterpret_cast<const char *>(mqtt.bytes+mt::network::MqttGatewaySettings::Address),
      mqtt.bytes[mt::network::MqttGatewaySettings::AddressLength],
      reinterpret_cast<const char *>(mqtt.bytes+mt::network::MqttGatewaySettings::Root),
      mqtt.bytes[mt::network::MqttGatewaySettings::RootLength],region,strlen(region),nextRoot,sizeof(nextRoot));
    if(root==mt::network::MqttRegionRootPolicy::Result::Invalid)return -EINVAL;
    if(root==mt::network::MqttRegionRootPolicy::Result::Changed) {
      auto &scratch=remoteDeviceWork.beginProjection();
      mt::detail::VolatileWipe wipeProjection{&remoteDeviceWork,sizeof(remoteDeviceWork)};
      auto &wire=scratch.wire.payload_variant.mqtt;
      if(!mt::network::MqttGatewayConfigMapping::toConfig(mqtt,wire))return -EINVAL;
      memset(wire.root,0,sizeof(wire.root));memcpy(wire.root,nextRoot,strlen(nextRoot));
      if(!mqtt.assign(wire))return -EINVAL;
    }
    if(!mqtt.setConfigOkToMqtt(configOk)||!remoteDeviceCandidate.valid())return -EINVAL;
    if(!memcmp(mqtt.bytes,mqttJournal.config().bytes,mt::network::MqttGatewaySettings::Size))
      return saveRemoteConfig(candidate,now);
    if(!mqttAdmin.remotePolicyWritable())return -EACCES;
    struct Preparing {
      Preparing(){remoteDevicePreparing=true;}
      ~Preparing(){remoteDevicePreparing=false;}
    } preparing;
    return saveRemoteConfig(remoteDeviceCandidate.core,now);
#else
    (void)candidate;(void)configOk;(void)now;return -ENOTSUP;
#endif
  }
  int saveSecurityControls(const mt::Config &candidate,uint32_t now)override {
    if(!ready()||!candidate.valid()||!journal.ready())return -EACCES;
    if(settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending()||
       pendingBluetooth.busy()||persistentWorkPending())return -EBUSY;
    mt::Config expected=service.config;
    if(!expected.setSecurityControls(candidate.isManaged(),candidate.serialEnabled())||
       !expected.setSecurityOptions(candidate.debugLogApiEnabled(),candidate.adminChannelEnabled())||
       !expected.setPacketSignaturePolicy(candidate.packetSignaturePolicy())||
       !expected.same(candidate))return -EINVAL;
    if(candidate.adminChannelEnabled()) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
      if(!legacyAdminChannelConfigured(candidate))return -EACCES;
#else
      return -ENOTSUP;
#endif
    }
#if defined(CONFIG_LOG_BACKEND_UART) && CONFIG_LOG_BACKEND_UART
    // The raw printk hook cannot gate an independent UART log backend.
    if(!candidate.serialEnabled())return -ENOTSUP;
#endif
    if(candidate.isManaged()) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
      // A managed node needs an already usable authenticated recovery owner.
      // A merely configured public key or unpinned NodeDB record is not enough.
      if(candidate.licensedOperation()||!adminKeyJournal.ready()||
         !adminKeyJournal.settings().valid()||!adminKeyJournal.settings().count())return -EACCES;
      bool currentAdmin=false;
      for(size_t i=0;i<adminKeyJournal.settings().count();++i) {
        const auto *key=adminKeyJournal.settings().key(i);
        const uint32_t peer=bench::MtSignature::nodeId(key);
        if(service.privateAdminPeerCurrent(peer,key)){currentAdmin=true;break;}
      }
      if(!currentAdmin)return -EACCES;
#else
      return -ENOTSUP;
#endif
    }
    const int result=settingsEdit.save(candidate,now);
    // An uncertain persistence result closes admission when the journal loses
    // authority, even if the old in-memory policy still says enabled.
    syncConsoleSecurityPolicy();syncApiLogPolicy();return result;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
public:
  bool adminKeys(meshtastic_Config_SecurityConfig &out)const override {
    out={};
    return storageReady&&!rebootSchedule.pending()&&adminKeyJournal.project(out);
  }
  bool adminPasskey(uint8_t *out,size_t size)override {
    return size==8&&privateEntropy.fill(out,size);
  }
  uint32_t adminSessionNow()const override {return k_uptime_get_32();}
  bool stageRemoteHardwarePins(uint32_t peer,const uint8_t peerKey[32],
      const meshtastic_ModuleConfig_RemoteHardwareConfig &config,uint32_t)override {
    if(!remoteHardwarePinJournalReady||!remoteHardwarePinJournal.ready()||
       !ready()||service.faulted()||!peerKey||peer<=1||peer>=0xfffffffeu||
       peer==service.route.identity()||!service.privateAdminPeerCurrent(peer,peerKey)||
       !preferenceDomainReady||preferenceDomain.node!=service.route.identity())return false;
    uint8_t localKey[32]{};
    const bool sameDomain=service.privatePublicIdentity(localKey)&&
      !memcmp(localKey,preferenceDomain.publicKey,sizeof(localKey));
    bench::wipeKeyBuffer(localKey,sizeof(localKey));
    if(!sameDomain||config.available_pins_count>mt::admin::RemoteHardwarePinCache::PeerCapacity)
      return false;

    mt::admin::RemoteHardwarePinCache::Entry pins[mt::admin::RemoteHardwarePinCache::PeerCapacity]{};
    const size_t count=config.enabled?config.available_pins_count:0;
    for(size_t i=0;i<count;++i) {
      pins[i].node_num=peer;pins[i].has_pin=true;
      pins[i].pin=config.available_pins[i];
    }
    // Companion invokes this only after the live private query matcher and
    // canonical response decoder succeed. This call copies typed metadata to
    // RAM; no key or raw Admin bytes are retained and no storage I/O occurs.
    const bool staged=remoteHardwarePinCache.updatePeer(peer,true,true,pins,count);
    if(staged)remoteHardwarePinDirty=true;
    return staged;
  }
  int saveAdminKeys(const meshtastic_Config_SecurityConfig &value,uint32_t now)override {
    if(!ready()||!adminKeyJournal.ready())return -EIO;
    if(settingsEdit.open()||rebootSchedule.pending())return -EBUSY;
    // This operation changes only the public allowlist in its own journal.
    // Core security controls must match the separately committed Config.
    if(value.private_key.size||value.is_managed!=service.config.isManaged()||
       value.serial_enabled!=service.config.serialEnabled()||
       (service.config.isManaged()&&!value.admin_key_count)||
       value.debug_log_api_enabled!=service.config.debugLogApiEnabled()||
       value.admin_channel_enabled!=service.config.adminChannelEnabled()||
       mt::detail::enumNumber(value.packet_signature_policy)!=service.config.packetSignaturePolicy()||
       value.admin_key_count>3)return -EINVAL;
    uint8_t publicKey[32]{};
    if(value.public_key.size&&
       (value.public_key.size!=32||!service.privatePublicIdentity(publicKey)||
        memcmp(value.public_key.bytes,publicKey,32)))return -EINVAL;
    uint8_t packed[96]{};
    for(unsigned i=0;i<value.admin_key_count;++i){
      if(value.admin_key[i].size!=32)return -EINVAL;
      memcpy(packed+i*32,value.admin_key[i].bytes,32);
    }
    mt::admin::AdminKeySettings candidate;
    if(!candidate.assign(value.admin_key_count,packed,size_t(value.admin_key_count)*32))return -EINVAL;
    // A key removed then re-added must not resurrect a previously issued token.
    // Invalidate every transport before an attempted permission persistence.
    clearRemoteAdminSessions();
    return mt::admin::AdminKeyCommit::save(service,radio,adminKeyJournal,settings,candidate,now);
  }
#endif
  static bool extendedModuleTag(unsigned tag) {
    if(tag==meshtastic_ModuleConfig_traffic_management_tag||tag==meshtastic_ModuleConfig_mesh_beacon_tag||
       tag==meshtastic_ModuleConfig_canned_message_tag||tag==meshtastic_ModuleConfig_serial_tag)return true;
#if defined(CONFIG_MT_TAK_TRACKER)
    return tag==meshtastic_ModuleConfig_tak_tag;
#else
    return false;
#endif
  }
public:
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  int stageStockIdentity(const uint8_t *scalar,const uint8_t *pub,bool usbFrame,uint32_t epoch)override {
    if(!usbFrame||keyVerificationOwner()!=0)return -EACCES;
    return stockImport.stage(scalar,pub,usbFrame,epoch);
  }
#endif
  int freshProvision(const uint8_t *data,size_t size,uint32_t epoch,char *out,size_t capacity)override {
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    using Import=mt::pki::StockImportConsent<FreshUsbContext>;
    if(data&&size>=sizeof(Import::Prefix)-1&&!memcmp(data,Import::Prefix,sizeof(Import::Prefix)-1)){
      const auto boot=privateStartup.state();
      bool canArm=(boot==mt::pki::PkiStartup::State::Empty||boot==mt::pki::PkiStartup::State::Held)&&
        freshProvisioning.state()!=mt::pki::PkiFreshProvisioning::State::Staged&&!settingsEdit.open()&&
        !firmwareUpdate.pending()&&!rebootSchedule.pending()&&!service.config.isManaged();
#if MT_HAS_DEVICE_RESET
      canArm=canArm&&resetHead.state==deviceReset::Floor::State::Unused;
#endif
      const int rc=stockImport.command(data,size,epoch,out,capacity,canArm,mt::ble::stockImportLink(mt::ble::session()),
                                       boot==mt::pki::PkiStartup::State::Held);
      if(rc>0&&importedProvisioning.state()==mt::pki::PkiFreshProvisioning::State::Committed)
        scheduleOrdinaryReboot(2,uint64_t(k_uptime_get()));
      return rc;
    }
    if(stockImport.active())return -EBUSY;
#endif
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    constexpr char expand[]="zt_pki_contacts_expand_v1 ";
    if(data&&size>=sizeof(expand)-1&&!memcmp(data,expand,sizeof(expand)-1)){
      constexpr size_t off=sizeof(expand)-1;
      uint32_t node=0;uint8_t key[32]{};
      if(!out||capacity<90)return -EINVAL;
      if(!identityRecoverySession.bind(epoch)||!identityRecoverySession.current(epoch))return -EACCES;
      mt::pki::IdentityBinding local;auto &store=privateStorage.storage();
      if(size==off+6&&!memcmp(data+off,"status",6)){
        if(!ready()||!store.preparedBinding(local))return -EACCES;
        mt::pki::PkiPeerSet small;const bool expanded=expandedContacts.ready();
        if(!expanded&&!store.preparedPeers(small))return -EIO;
        const auto count=expanded?expandedContacts.pins().count():unsigned(small.count());
        if(!identityRecoverySession.current(epoch))return -EACCES;
        const int written=snprintf(out,capacity,
          "pki_contacts_expand_v1 rc=0 capacity=%u count=%u expanded=%u gen=%u through=%u",
          expanded?198u:16u,count,unsigned(expanded),unsigned(store.contactGeneration()),unsigned(store.nonceReservedThrough()));
        return written>0&&size_t(written)<capacity?written:-ENOSPC;
      }
      if(size!=off+8+1+64||data[off+8]!=' '||!mt::pki::detail::decodeNode(data,off,node)||
         !mt::pki::detail::decodePub(data,off+9,key))return -EINVAL;
      if(!ready()||!peerContext.ready()||!mt::update::signedOta::available()||
         !store.preparedBinding(local)||local.node!=node||memcmp(local.publicKey,key,32))return -EACCES;
      if(!peerContext.pause())return -EBUSY;
      int rc=identityRecoverySession.current(epoch)?store.enableExpanded(local):-EACCES;
      if(store.state()!=mt::pki::PkiStorage::State::Active||!peerContext.resume()){peerContext.hold();return rc?rc:-EIO;}
      if(!identityRecoverySession.current(epoch))return -EACCES;
      if(!rc)scheduleOrdinaryReboot(2,uint64_t(k_uptime_get()));
      return snprintf(out,capacity,"pki_contacts_expand_v1 rc=%d capacity=%u count=%u reboot=%u",rc,
        expandedContacts.ready()?198u:16u,expandedContacts.ready()?expandedContacts.pins().count():unsigned(privateStartup.loadedPeers()),unsigned(!rc));
    }
#endif
    return freshIngress.execute(data,size,epoch,out,capacity);
#else
    (void)data;(void)size;(void)epoch;(void)out;(void)capacity;return -ENOTSUP;
#endif
  }
  void freshProvisionDisconnect()override {
    keyVerification.disconnect(0,k_uptime_get_32());
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    freshIngress.disconnect();identityRecoverySession.clear();
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    stockImport.cancel();
#endif
#endif
  }
  int restoreExistingIdentity(const uint8_t *scalar,size_t length,uint32_t epoch)override {
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    if(!scalar||length!=mt::pki::UsbIdentityRestoreParser::CommandLength)return -EINVAL;
    const int result=identityRestore.execute(scalar,length,epoch);
    identityRecoverySession.clear();return result;
#else
    (void)scalar;(void)length;(void)epoch;return -ENOTSUP;
#endif
  }
  virtual uint8_t keyVerificationOwner()const{return 0;}
  int keyVerificationAdmin(const meshtastic_KeyVerificationAdmin &request,uint32_t now)override {
    if(!ready()||settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
    return keyVerification.admin(keyVerificationOwner(),request,now);
  }
  int uiConfig(meshtastic_DeviceUIConfig &out)const override {
    out=mt::ui::UiConfigSettings::defaults();
    if(!ready()||!uiConfigJournal.ready()||!mt::ui::UiConfigSettings::valid(uiConfigJournal.config()))return -EACCES;
    out=uiConfigJournal.config();return 0;
  }
  int hardwarePinInventory(meshtastic_NodeRemoteHardwarePinsResponse &out)const override {
    out={};if(!ready())return -EACCES;
    meshtastic_NodeRemoteHardwarePin remote[mt::admin::HardwarePinInventory::RemoteCapacity]{};
    size_t remoteCount=0;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(!remoteHardwarePinJournalReady||!remoteHardwarePinJournal.ready()||service.faulted())return -EIO;
    if(!remoteHardwarePinCache.copyTo(remote,sizeof(remote)/sizeof(remote[0]),remoteCount))return -EIO;
#endif
    meshtastic_RemoteHardwarePin local[mt::admin::HardwarePinInventory::LocalCapacity]{};
    size_t count=0;
#if defined(CONFIG_MT_GPIO_MODULES)
    if(!gpioModuleEdit.trusted())return -EIO;
    const auto caps=mt::modules::zephyrGpioModuleBackend().capabilities();
    const auto &configuredRemote=gpioModuleEdit.config().remote();
    if(configuredRemote.available_pins_count>mt::admin::HardwarePinInventory::LocalCapacity)return -EIO;
    for(pb_size_t i=0;i<configuredRemote.available_pins_count;++i) {
      const auto &pin=configuredRemote.available_pins[i];
      if(pin.gpio_pin<64&&(caps.exported&(UINT64_C(1)<<pin.gpio_pin)))local[count++]=pin;
    }
#endif
    // Only configured exports from the reviewed board backend are local pins.
    // A board without exports has a valid empty inventory, not a raw SoC map.
    return mt::admin::HardwarePinInventory::build(service.route.identity(),remote,remoteCount,local,count,out)?0:-EIO;
  }
  int preferences(pb_size_t operation,uint32_t now,mt::UsbSessionEpoch::RestoreReceipt *receipt)override {
    if(!ready()||(receipt&&(!usbAccepted_||receipt->source()!=usbAcceptedGeneration_)))return -EACCES;
    return preferenceCommand(operation,now,receipt);
  }
  bool sendRestoreReceipt(mt::UsbSessionEpoch::RestoreReceipt &receipt,uint32_t request,
                          const uint8_t *data,size_t size)override {
    const unsigned key=irq_lock();const auto line=observeUsbLineLocked(usb);
    const bool admitted=receipt.take(request,line)&&usbAccepted_&&usbAcceptedGeneration_==receipt.source()&&
      preferenceRecoveryHeld&&!mt_security::serialConsoleAllowed();
    const bool queued=admitted&&size&&ring_buf_space_get(&usbTx)>=size&&ring_buf_put(&usbTx,data,size)==size;
    if(queued)uart_irq_tx_enable(usb);
    irq_unlock(key);return queued;
  }
  int resumePreferences(uint32_t now)override {return resumePreferenceRestore(now);}
  int saveUiConfig(const meshtastic_DeviceUIConfig &value,uint32_t now)override {
    if(!ready()||!uiConfigJournal.ready())return -EACCES;
    if(settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
    return mt::ui::UiConfigCommit::save(service,radio,uiConfigJournal,settings,value,now);
  }
  int ringtone(char *out,size_t capacity)const override {
    if(!out||capacity<mt::notification::RingtoneSettings::Capacity)return -EINVAL;
    if(!ready()||!ringtoneJournal.ready()||!ringtoneJournal.config().valid())return -EIO;
    memcpy(out,ringtoneJournal.config().ringtone,mt::notification::RingtoneSettings::Capacity);return 0;
  }
  int saveRingtone(const char *text,size_t length,uint32_t now)override {
    if(!ready()||!ringtoneJournal.ready())return -EACCES;
    auto candidate=ringtoneJournal.config();const auto assigned=candidate.assign(text,length);
    if(assigned==mt::notification::RingtoneSettings::AssignResult::Invalid)return -EINVAL;
    if(assigned!=mt::notification::RingtoneSettings::AssignResult::Changed)return 0;
    if(settingsEdit.open()||rebootSchedule.pending())return -EBUSY;
    return mt::notification::RingtoneCommit::save(service,radio,ringtoneJournal,settings,candidate,now);
  }
  int cannedMessageMessages(char *out,size_t capacity)const override {
    if(!out||capacity<mt::modules::CannedMessageSettings::Capacity)return -EINVAL;
    if(!ready()||!cannedJournal.ready()||!cannedJournal.config().valid())return -EIO;
    memcpy(out,cannedJournal.config().messages,mt::modules::CannedMessageSettings::Capacity);return 0;
  }
  int saveCannedMessageMessages(const char *value,size_t length,uint32_t now)override {
    if(!ready()||!cannedJournal.ready())return -EIO;
    auto candidate=cannedJournal.config();
    const auto assigned=candidate.assign(value,length);
    if(assigned==mt::modules::CannedMessageSettings::AssignResult::Invalid)return -EINVAL;
    if(assigned!=mt::modules::CannedMessageSettings::AssignResult::Changed)return 0;
    // This independently persisted stock Admin list is outside the core
    // begin/commit transaction. A pending batch/reboot retains its ownership.
    if(settingsEdit.open()||rebootSchedule.pending())return -EBUSY;
    return mt::modules::CannedMessageCommit::save(service,radio,cannedJournal,settings,candidate,now);
  }
  void boardMetadata(meshtastic_DeviceMetadata &out)const override {
    out.canShutdown=completionRuntime.supportsRequestedPowerOff();
#if defined(CONFIG_MT_GPIO_MODULES)
    out.hasRemoteHardware=gpioModuleAvailable()&&gpioModuleEdit.ready()&&gpioModuleEdit.config().remote().enabled;
#endif
  }
  void boardMyInfo(meshtastic_MyNodeInfo &out,bool identify)const override {
#if defined(CONFIG_SOC_ESP32S3)
    out.reboot_count=myBootCount;
#endif
    if(identify&&mt::hardware::siliconId(out.device_id.bytes))out.device_id.size=sizeof(out.device_id.bytes);
  }
  uint32_t supportedModules()const override {
    const uint32_t existing=moduleEdit.ready()?meshtastic_ExcludedModules_STATUSMESSAGE_CONFIG|meshtastic_ExcludedModules_NEIGHBORINFO_CONFIG|
      meshtastic_ExcludedModules_RANGETEST_CONFIG|meshtastic_ExcludedModules_TELEMETRY_CONFIG:0;
    uint32_t mqtt=mqttAdmin.ready()?meshtastic_ExcludedModules_MQTT_CONFIG:0;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(storeForwardEdit.ready())mqtt|=meshtastic_ExcludedModules_STOREFORWARD_CONFIG;
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
    if(gpioModuleEdit.ready()&&gpioModuleAvailable())mqtt|=
      meshtastic_ExcludedModules_REMOTEHARDWARE_CONFIG|meshtastic_ExcludedModules_DETECTIONSENSOR_CONFIG;
#endif
    const uint32_t inputs=(cannedInputAvailable()?meshtastic_ExcludedModules_CANNEDMSG_CONFIG:0)|
      (serialInputAvailable()?meshtastic_ExcludedModules_SERIAL_CONFIG:0);
#if defined(CONFIG_MT_TAK_TRACKER)
    if(extendedEdit.ready())mqtt|=meshtastic_ExcludedModules_TAK_CONFIG;
#endif
    return existing|mqtt|inputs|(completionEdit.ready()?completionRuntime.availableModules():0)|(extendedEdit.ready()?(meshtastic_ExcludedModules_TRAFFICMANAGEMENT_CONFIG|meshtastic_ExcludedModules_MESHBEACON_CONFIG):0)|((notificationEdit.ready()&&mt::notification::available())?meshtastic_ExcludedModules_EXTNOTIF_CONFIG:0);
  }
  bool moduleConfig(meshtastic_ModuleConfig&out,unsigned tag)const override {
    if(completionEdit.ready()&&mt::modules::CompletionRuntime::handles(tag))
      return mt::modules::completionConfig(completionEdit.config(),moduleEdit.config(),notificationEdit.config(),extendedEdit.config(),out,tag);
    if(completionEdit.ready()&&(tag==meshtastic_ModuleConfig_serial_tag||tag==meshtastic_ModuleConfig_canned_message_tag||tag==meshtastic_ModuleConfig_range_test_tag)&&completionEdit.config().get(out,tag))return true;
    if(mqttAdmin.moduleConfig(out,tag,true))return true;
#if defined(CONFIG_MT_GPIO_MODULES)
    if(tag==meshtastic_ModuleConfig_remote_hardware_tag||tag==meshtastic_ModuleConfig_detection_sensor_tag)
      return gpioModuleEdit.ready()&&gpioModuleAvailable()&&gpioModuleEdit.config().get(out,tag);
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(tag==meshtastic_ModuleConfig_store_forward_tag){
      out={};out.which_payload_variant=tag;
      return storeForwardEdit.ready()&&storeForwardEdit.config().get(out.payload_variant.store_forward);
    }
#endif
    if(tag==meshtastic_ModuleConfig_external_notification_tag)
      return notificationEdit.ready()&&mt::NotificationConfig::get(notificationEdit.config(),out);
    if(extendedModuleTag(tag))
      return extendedEdit.ready()&&extendedEdit.config().get(out,tag);
    return moduleEdit.ready()&&moduleEdit.config().get(out,tag);
  }
  int saveModuleConfig(const meshtastic_ModuleConfig&config,uint32_t now)override {
    return saveModuleConfigOwned(config,now,false);
  }
  int saveRemoteModuleConfig(const meshtastic_ModuleConfig&config,uint32_t now)override {
    return saveModuleConfigOwned(config,now,true);
  }
  int saveModuleConfigOwned(const meshtastic_ModuleConfig&config,uint32_t now,bool remoteMutation) {
    if(!ready()||rebootSchedule.pending())return -EACCES;
    if(mt::modules::CompletionRuntime::handles(config.which_payload_variant)){
      if(!completionEdit.ready())return -EACCES;
      if(!completionRuntime.supports(config))return -ENOTSUP;
      auto candidate=completionEdit.config();if(!candidate.set(config))return -EINVAL;
      // I2S has one owner: audio capture/playback and ringtone cannot hold it together.
      meshtastic_ModuleConfig a{},n{};
      if(candidate.get(a,meshtastic_ModuleConfig_audio_tag)&&candidate.get(n,meshtastic_ModuleConfig_external_notification_tag)&&
         a.payload_variant.audio.codec2_enabled&&n.payload_variant.external_notification.enabled&&n.payload_variant.external_notification.use_i2s_as_buzzer)return -EBUSY;
      auto legacy=mt::modules::legacyProjection(config);
      if(config.which_payload_variant==meshtastic_ModuleConfig_telemetry_tag){
        auto check=moduleEdit.config();if(!moduleEdit.ready()||!check.set(legacy))return -EINVAL;
      }else if(config.which_payload_variant==meshtastic_ModuleConfig_external_notification_tag){
        if(!mt::notification::available()){
          const auto &v=config.payload_variant.external_notification;
          if(v.alert_message||v.alert_bell||v.output)return -ENOTSUP;
          legacy.payload_variant.external_notification.enabled=false;
        }
        auto check=notificationEdit.config();
        if(!notificationEdit.ready()||!mt::NotificationConfig::set(check,legacy,mt::notification::available(),mt::notification::pin()))return -EINVAL;
      }
      const bool openedHere=!settingsEdit.open();if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
      int result=0;
      if(config.which_payload_variant==meshtastic_ModuleConfig_telemetry_tag)result=moduleEdit.save(legacy,now,true);
      else if(config.which_payload_variant==meshtastic_ModuleConfig_external_notification_tag)
        result=notificationEdit.save(legacy,now,true,mt::notification::available(),mt::notification::pin());
      if(!result)result=completionEdit.stage(candidate);
      if(!result){settingsEdit.activity(now);if(openedHere)result=settingsCommit.commit(now);}
      return result;
    }

#if defined(CONFIG_MT_GPIO_MODULES)
    if(config.which_payload_variant==meshtastic_ModuleConfig_remote_hardware_tag||
       config.which_payload_variant==meshtastic_ModuleConfig_detection_sensor_tag) {
      auto candidate=gpioModuleEdit.config();
      if(!candidate.set(config))return -EINVAL;
      if(!gpioModuleRuntime.validateConfig(candidate))return -ENOTSUP;
      const int result=gpioModuleEdit.save(config,now,settingsEdit.open());
      if(!result){settingsEdit.activity(now);refreshGpioModules();}return result;
    }
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(config.which_payload_variant==meshtastic_ModuleConfig_store_forward_tag){
      const int result=storeForwardEdit.save(config,now,settingsEdit.open());
      if(!result)settingsEdit.activity(now);return result;
    }
#endif
    if(config.which_payload_variant==meshtastic_ModuleConfig_mesh_beacon_tag){
      auto candidate=extendedEdit.config();if(!candidate.set(config))return -EINVAL;
      const auto &beacon=config.payload_variant.mesh_beacon;
      if((beacon.flags&meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_BROADCAST_ENABLED)&&
         !mt::modules::BeaconController::targetsSupported(service.config,beacon,beaconTargets))return -ENOTSUP;
    }
    if(config.which_payload_variant==meshtastic_ModuleConfig_range_test_tag){
      if(!completionEdit.ready())return -EACCES;
      const auto &value=config.payload_variant.range_test;
      if((value.save||value.clear_on_reboot)&&!mt::modules::RangeTestLog::available())return -ENOTSUP;
      auto candidate=completionEdit.config();if(!candidate.set(config))return -EINVAL;
      const auto projection=mt::modules::legacyProjection(config);auto check=moduleEdit.config();if(!check.set(projection))return -EINVAL;
      const bool openedHere=!settingsEdit.open();if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
      int result=moduleEdit.save(projection,now,true);if(!result)result=completionEdit.stage(candidate);
      if(!result){settingsEdit.activity(now);if(openedHere)result=settingsCommit.commit(now);}return result;
    }
    if(config.which_payload_variant==meshtastic_ModuleConfig_canned_message_tag){
      if(!completionEdit.ready())return -EACCES;const auto &value=config.payload_variant.canned_message;
      if(value.enabled&&!cannedInputAvailable())return -ENOTSUP;
      if(!cannedGpio.configSupported(value))return -ENOTSUP;
      auto candidate=completionEdit.config();if(!candidate.set(config))return -EINVAL;
      auto projection=config;auto &old=projection.payload_variant.canned_message;
      old.rotary1_enabled=false;old.updown1_enabled=false;old.inputbroker_pin_a=old.inputbroker_pin_b=old.inputbroker_pin_press=0;
      old.inputbroker_event_cw=old.inputbroker_event_ccw=old.inputbroker_event_press=meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_NONE;
      memset(old.allow_input_source,0,sizeof(old.allow_input_source));
      const bool openedHere=!settingsEdit.open();if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
      int result=extendedEdit.save(projection,now,true);if(!result)result=completionEdit.stage(candidate);
      if(!result){settingsEdit.activity(now);if(openedHere)result=settingsCommit.commit(now);}return result;
    }
    if(config.which_payload_variant==meshtastic_ModuleConfig_serial_tag) {
#if defined(CONFIG_MT_SERIAL_MODULE)
      const auto &value=config.payload_variant.serial;mt_serial::Config runtime{};uint32_t baud=0;
      if(!serialRuntimeConfig(value,runtime,baud)||!completionEdit.ready())return -EINVAL;
      if(value.enabled&&!serialModule.configSupported(runtime,baud,value.rxd,value.txd))return -ENOTSUP;
      auto candidate=completionEdit.config();if(!candidate.set(config))return -EINVAL;
      const bool openedHere=!settingsEdit.open();if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
      int result=completionEdit.stage(candidate);if(!result){settingsEdit.activity(now);if(openedHere)result=settingsCommit.commit(now);}
      return result;
#else
      return -ENOTSUP;
#endif
    }
    if(config.which_payload_variant==meshtastic_ModuleConfig_mqtt_tag){
      const auto &input=config.payload_variant.mqtt;
      if(!memchr(input.address,0,sizeof(input.address)))return -EINVAL;
#if defined(CONFIG_MT_MQTT_GATEWAY)
#if defined(CONFIG_MT_M119_TLS_TEST)
      if(!input.tls_enabled||input.proxy_to_client_enabled)return -EACCES;
#endif
      char host[64]{};uint16_t port=0;
      if(input.enabled&&!mt::mqtt::BootSnapshot::broker(input.address,input.tls_enabled,host,sizeof(host),port))return -EINVAL;
      const bool runtimeSupported=true;
      const bool trustReady=!input.proxy_to_client_enabled&&mqttTrustAvailable;
#else
      char host[64]{};uint16_t port=0;
      if(input.enabled&&!mt::mqtt::BootSnapshot::broker(input.address,input.tls_enabled,host,sizeof(host),port))return -EINVAL;
      const bool runtimeSupported=input.proxy_to_client_enabled;
      const bool trustReady=false; // Phone proxy TLS is not device-verified TLS.
#endif
      for(size_t i=0;i<sizeof(input.root)&&input.root[i];++i)
        if(uint8_t(input.root[i])<0x20||input.root[i]=='+'||input.root[i]=='#')return -EINVAL;
#if defined(CONFIG_MT_MQTT_GATEWAY)
      const uint32_t generation=mqttJournal.generation();
#endif
      const int saved=remoteMutation?
        mqttAdmin.saveRemote(input,true,runtimeSupported,trustReady,now):
        mqttAdmin.save(input,true,runtimeSupported,trustReady,now);
#if defined(CONFIG_MT_MQTT_GATEWAY)
      mqttRestart.saved(saved,generation,mqttJournal.generation(),mqttEdit.dirty());
#endif
      refreshMqttProxyBridge();
      return applicationAfterCommit(saved,uint64_t(k_uptime_get()));
    }
    const int result=config.which_payload_variant==meshtastic_ModuleConfig_external_notification_tag?
      notificationEdit.save(config,now,settingsEdit.open(),mt::notification::available(),mt::notification::pin()):
      extendedModuleTag(config.which_payload_variant)?
      extendedEdit.save(config,now,settingsEdit.open()):moduleEdit.save(config,now,settingsEdit.open());
    if(config.which_payload_variant==meshtastic_ModuleConfig_telemetry_tag&&
       (!moduleEdit.ready()||!moduleEdit.config().environmentEnabled()))service.measuredEnvironment({});
    if(config.which_payload_variant==meshtastic_ModuleConfig_telemetry_tag&&
       (!moduleEdit.ready()||!moduleEdit.config().powerEnabled()))service.measuredPower({});
    if(!result){settingsEdit.activity(now);refreshCannedInput();refreshSerialInput();}
    return result;
  }
  bool powerConfig(meshtastic_Config_PowerConfig &out)const override {
    out={};if(!completionEdit.ready())return false;(void)completionEdit.config().power(out);return true;
  }
  int savePowerConfig(const meshtastic_Config_PowerConfig &value,uint32_t now)override {
    if(!ready()||!completionEdit.ready()||rebootSchedule.pending())return -EACCES;
    if(!completionRuntime.supportsPower(value))return -ENOTSUP;
    auto candidate=completionEdit.config();if(!candidate.setPower(value))return -EINVAL;
    const bool openedHere=!settingsEdit.open();if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
    const int result=completionEdit.stage(candidate);if(result)return result;
    settingsEdit.activity(now);return openedHere?settingsCommit.commit(now):0;
  }
  bool extraTelemetry(uint8_t variant,uint32_t request,uint8_t *out,size_t capacity,size_t &size)override {
    const auto now=uint64_t(k_uptime_get());
    return completionRuntime.response(variant,request,now,wallClock.at(now),out,capacity,size);
  }
  int localNeighbors(meshtastic_NeighborInfo&out)override {
    return modules.localNeighbors(service,moduleEdit.config(),ready()&&moduleEdit.ready(),ztUptimeMsProvider(nullptr),out);
  }
  int localEnvironment(meshtastic_EnvironmentMetrics &out)override {
    out={};if(!moduleEdit.ready()||!moduleEdit.config().environmentEnabled())return -ENOTSUP;
    out=zt::telemetry::toMeshtasticEnvironmentMetrics(service.environment());
    return zt::telemetry::hasEnvironmentMetrics(out)?0:-ENODATA;
  }
  int localPower(zt::telemetry::PowerReadings &out)override {
    out={};if(!moduleEdit.ready()||!moduleEdit.config().powerEnabled())return -ENOTSUP;
    out=service.power();
    return zt::telemetry::hasPowerMetrics(zt::telemetry::buildPowerMetrics(out.voltage,out.current))?0:-ENODATA;
  }
  void observeRadioPeer(uint32_t id,unsigned hops,bool known,float snr,uint32_t)override {
    const int slot=service.nodes.find(id);const auto *peer=slot>=0?service.nodes.get(unsigned(slot)):nullptr;
    if(peer&&!peer->placeholder())trafficWindow.observeRole(id,peer->role());
    modules.observe(id,service.route.identity(),hops,ztUptimeMsProvider(nullptr),known,snr);
  }
  void observeRadioNeighborInfo(uint32_t id,unsigned hops,bool known,float snr,uint32_t,
                                const meshtastic_NeighborInfo&info)override {
    const int slot=service.nodes.find(id);const auto *peer=slot>=0?service.nodes.get(unsigned(slot)):nullptr;
    if(peer&&!peer->placeholder())trafficWindow.observeRole(id,peer->role());
    modules.observeNeighborInfo(service,id,service.route.identity(),hops,
      ztUptimeMsProvider(nullptr),known,snr,info);
  }
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
  int provision(const uint8_t *p,size_t n,uint32_t epoch,char *out,size_t capacity)override {
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    constexpr char prepareStatus[]="zt_pki_prep_status_v1";
    if(p&&n==sizeof(prepareStatus)-1&&!memcmp(p,prepareStatus,n)){
      if(!out||capacity<128||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      mt::pki::TailPreparationResult result;int error=0;bool observed=false;
      if(!privateStorage.storage().preparationReport(result,error,observed)||!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_prep_status_v1 rc=%d observed=%u attempted=%u verified=%u blank=%u complete=%u",
        error,unsigned(observed),unsigned(result.attempted),unsigned(result.verified),unsigned(result.blank),unsigned(result.complete));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
    constexpr char prepareCommand[]="zt_pki_tail_prepare_v1";
    if(p&&n>=sizeof(prepareCommand)-1&&!memcmp(p,prepareCommand,sizeof(prepareCommand)-1)){
      mt::pki::UsbTailPrepareCommand command;
      if(!out||capacity<128||!mt::pki::UsbTailPrepareParser::parse(p,n,command))return -EINVAL;
      if(!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      UsbTailPreparationProgress progress(epoch);mt::pki::TailPreparationResult result;
      const int rc=privateStorage.prepareFormerCode(peerAdministration,progress,command,epoch,result);
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_prepare_v1 rc=%d attempted=%u verified=%u blank=%u complete=%u",
        rc,unsigned(result.attempted),unsigned(result.verified),unsigned(result.blank),unsigned(result.complete));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
    constexpr char spaceCommand[]="zt_pki_space_v1";
    if(p&&n==sizeof(spaceCommand)-1&&!memcmp(p,spaceCommand,n)) {
      if(!out||capacity<96||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      bool blank=false;const int rc=privateStorage.historyBlank(blank);
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_space_v1 rc=%d bytes=172032 blank=%u erase=0",
        rc,unsigned(!rc&&blank));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
    constexpr char tailCommand[]="zt_pki_tail_v1";
    if(p&&n==sizeof(tailCommand)-1&&!memcmp(p,tailCommand,n)) {
      if(!out||capacity<224||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      struct Scratch {
        mt::pki::TailProbeResult probe;char digest[65]{};
        ~Scratch(){mt::pki::eraseIdentityBytes(this,sizeof(*this));}
      } scratch;
      auto &probe=scratch.probe;auto *digest=scratch.digest;uint32_t generation=0,crc=0;
      const int rc=privateStorage.probeLegacyTail(probe,generation,crc);
      if(rc||!provisionContext.current(epoch))return rc?rc:-EACCES;
      constexpr char hex[]="0123456789abcdef";
      for(size_t i=0;i<32;++i){digest[2*i]=hex[probe.digest[i]>>4];digest[2*i+1]=hex[probe.digest[i]&15];}
      const int size=snprintf(out,capacity,
        "pki_tail_v1 rc=0 bytes=172032 blank=%u possible=%u gen=%u crc=%u nonff=%u digest=%s",
        unsigned(probe.blank),unsigned(probe.possibleJournal),unsigned(generation),unsigned(crc),unsigned(probe.nonFF),digest);
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    constexpr char storeCommand[]="zt_pki_store_enable_v1";
    if(p&&n>=sizeof(storeCommand)-1&&!memcmp(p,storeCommand,sizeof(storeCommand)-1)) {
      mt::pki::UsbStorageCommand command;
      if(!out||capacity<96||!mt::pki::UsbStorageParser::parse(p,n,command))return -EINVAL;
      if(!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      const int rc=peerAdministration.enableStorage(command,epoch);
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_store_v1 rc=%d extended=%u free=%u erase=0",rc,
        unsigned(privateStorage.storage().extendedStorage()),unsigned(privateStorage.storage().appendRemaining()));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    constexpr char contactsCommand[]="zt_pki_contacts_enable_v1";
    if(p&&n>=sizeof(contactsCommand)-1&&!memcmp(p,contactsCommand,sizeof(contactsCommand)-1)) {
      mt::pki::UsbStorageCommand command;
      if(!out||capacity<112||!mt::pki::UsbContactsParser::parse(p,n,command))return -EINVAL;
      if(!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      const int rc=peerAdministration.enableContacts(command,epoch);
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_contacts_v1 rc=%d compact=%u gen=%u through=%u history_erase=0",rc,
        unsigned(privateStorage.storage().compactContacts()),unsigned(privateStorage.storage().contactGeneration()),
        unsigned(privateStorage.storage().nonceReservedThrough()));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if !MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED
    // The normal compact image uses one status envelope for both durable
    // contact state and the preserved legacy-history/cold-boot counters.
    constexpr char stateStatus[]="zt_pki_state_v1";
    if(p&&n==sizeof(stateStatus)-1&&!memcmp(p,stateStatus,n)) {
      if(!out||capacity<192||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      const auto boot=privateStorage.bootDiagnostic();
      const int size=snprintf(out,capacity,
        "pki_state_v1 compact=%u gen=%u state=%u bounded=%u through=%u legacy_free=%u boot_names=%u boot_bytes=%u history_erase=0",
        unsigned(privateStorage.storage().compactContacts()),unsigned(privateStorage.storage().contactGeneration()),
        unsigned(privateStorage.storage().state()),unsigned(privateStorage.storage().boundedHistory()),
        unsigned(privateStorage.storage().nonceReservedThrough()),unsigned(privateStorage.storage().appendRemaining()),
        unsigned(boot.names),unsigned(boot.valueBytes));
      if(!provisionContext.current(epoch))return -EACCES;
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#else
    constexpr char contactsStatus[]="zt_pki_contacts_status_v1";
    if(p&&n==sizeof(contactsStatus)-1&&!memcmp(p,contactsStatus,n)) {
      if(!out||capacity<112||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_contacts_status_v1 compact=%u gen=%u through=%u history_erase=0",
        unsigned(privateStorage.storage().compactContacts()),unsigned(privateStorage.storage().contactGeneration()),
        unsigned(privateStorage.storage().nonceReservedThrough()));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    constexpr char lifecycleCommand[]="zt_pki_lifecycle_enable_v1";
    if(p&&n>=sizeof(lifecycleCommand)-1&&!memcmp(p,lifecycleCommand,sizeof(lifecycleCommand)-1)) {
      mt::pki::UsbLifecycleCommand parsed;
      if(!out||capacity<112||!mt::pki::UsbLifecycleParser::parse(p,n,parsed))return -EINVAL;
      if(!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      mt::pki::UsbStorageCommand command;
      command.localNode=parsed.localNode;memcpy(command.localPublic,parsed.localPublic,32);
      const int rc=peerAdministration.enableStorage(command,epoch,true);
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_lifecycle_v1 rc=%d bounded=%u through=%u legacy_free=%u history_erase=0",rc,
        unsigned(privateStorage.storage().boundedHistory()),unsigned(privateStorage.storage().nonceReservedThrough()),
        unsigned(privateStorage.storage().appendRemaining()));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#if defined(CONFIG_BOARD_HELTEC_T096) && CONFIG_BOARD_HELTEC_T096
    constexpr char capacityStatus[]="zt_storage_capacity_status_v1";
    if(p&&n==sizeof(capacityStatus)-1&&!memcmp(p,capacityStatus,n)) {
      if(!out||capacity<233||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      // Existing initialized NVS only; fixed LFS raw reads. No mount/init/write/erase.
      const auto nvs=mt::m153::capacity_probe::existingNvsFreeSpace();
      if(!provisionContext.current(epoch))return -EACCES;
      const auto lfs=mt::m153::capacity_probe::scanLfs(mt::m153::capacity_probe::readLfs,nullptr);
      if(!provisionContext.current(epoch))return -EACCES;
      int used=snprintf(out,capacity,
        "capacity_v1 nrc=%d nf=%d scanned=%u sectors=%u rc=%d ff=",
        int(nvs.error),int(nvs.free_bytes),unsigned(lfs.bytes_scanned),
        unsigned(lfs.sectors_complete),int(lfs.error));
      if(used<0||size_t(used)>=capacity)return -ENOSPC;
      size_t at=size_t(used);
      for(unsigned i=0;i<unsigned(lfs.sectors_complete);++i) {
        const int wrote=snprintf(out+at,capacity-at,"%s%u",i?",":"",unsigned(lfs.ff_bytes[i]));
        if(wrote<0||size_t(wrote)>=capacity-at)return -ENOSPC;
        at+=size_t(wrote);
      }
      return int(at);
    }
#endif
    constexpr char spareStatus[]="zt_storage_spare_status_v1";
    if(p&&n==sizeof(spareStatus)-1&&!memcmp(p,spareStatus,n)) {
      if(!out||capacity<160||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      // Service-owner local USB epoch only. This fixed-range adapter has no
      // mount, initialization, hash, raw-byte response, write or erase path.
      const auto result=mt::m151::spare_status::scan();
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,
        "storage_spare_status_v1 bytes=%u scanned=%u ff=%u nonff=%u complete=%u rc=%d read_only=1",
        unsigned(result.partition_bytes),unsigned(result.bytes_scanned),unsigned(result.ff_bytes),
        unsigned(result.non_ff_bytes),unsigned(result.complete),int(result.error));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED
    constexpr char lifecycleStatus[]="zt_pki_lifecycle_status_v1";
    if(p&&n==sizeof(lifecycleStatus)-1&&!memcmp(p,lifecycleStatus,n)) {
      if(!out||capacity<160||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      const auto boot=privateStorage.bootDiagnostic();
      const int size=snprintf(out,capacity,
        "pki_lifecycle_status_v1 state=%u bounded=%u through=%u legacy_free=%u cache_peers=4 cache_ids=64 boot_names=%u boot_bytes=%u",
        unsigned(privateStorage.storage().state()),unsigned(privateStorage.storage().boundedHistory()),
        unsigned(privateStorage.storage().nonceReservedThrough()),unsigned(privateStorage.storage().appendRemaining()),
        unsigned(boot.names),unsigned(boot.valueBytes));
      if(!provisionContext.current(epoch))return -EACCES;
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED
    constexpr char statusCommand[]="zt_pki_limits_v1";
    if(p&&n==sizeof(statusCommand)-1&&!memcmp(p,statusCommand,n)) {
      if(!out||capacity<224||!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      mt::pki::PkiLedger::Status status;
      const bool ok=privateStorage.storage().ledgerStatus(status);
      if(!provisionContext.current(epoch))return -EACCES;
      if(ok&&status.format==3) {
        const unsigned free=privateStorage.storage().appendRemaining();
        const int size=snprintf(out,capacity,
          "pki_limits_v2 rc=0 format=3 gen=%u tx=%u tx_ceiling=%u rx_total=%u rx_total_ceiling=%u active=%u stored=%u max_active=4 boot_peers=%u event_free=%u event_max=2624 dormant=%u",
          unsigned(status.generation),unsigned(status.sent),unsigned(status.sent)+free,
          unsigned(status.received),unsigned(status.received)+free,unsigned(status.active),unsigned(status.stored),
          unsigned(privateStartup.loadedPeers()),free,unsigned(service.privateDormant()));
        return size>0&&size<int(capacity)?size:-ENOSPC;
      }
      const int size=snprintf(out,capacity,
        "pki_limits_v1 rc=%d format=%u gen=%u tx=%u txmax=144 rx_total=%u rx_per_peer_max=72 active=%u stored=%u maxpeers=4 boot_peers=%u",
        ok?0:-EACCES,unsigned(status.format),unsigned(status.generation),unsigned(status.sent),unsigned(status.received),
        unsigned(status.active),unsigned(status.stored),unsigned(privateStartup.loadedPeers()));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    constexpr char prefix[]="zt_pki_peer_add_v1";
    if(p&&n>=sizeof(prefix)-1&&!memcmp(p,prefix,sizeof(prefix)-1)) {
      mt::pki::UsbPeerCommand command;
      if(!out||capacity<96||!mt::pki::UsbPeerParser::parse(p,n,command))return -EINVAL;
      if(!provisionContext.bind(epoch)||!provisionContext.current(epoch))return -EACCES;
      const int result=peerAdministration.add(command,epoch);
      if(!provisionContext.current(epoch))return -EACCES;
      const int size=snprintf(out,capacity,"pki_peer_v1 rc=%d node=%08x peer=%08x reboot=%u",
        result,unsigned(command.localNode),unsigned(command.peerNode),unsigned(result==0));
      return size>0&&size<int(capacity)?size:-ENOSPC;
    }
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    return provisionIngress.execute(p,n,epoch,out,capacity);
#else
    return -ENOTSUP;
#endif
  }
  void provisionDisconnect()override{
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    provisionIngress.disconnect();
#else
    provisionContext.clear();
#endif
  }
#endif
#if MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED
  size_t extendedStorageDiagnostic(char *out,size_t capacity)const override {
    if(!out||!capacity)return 0;
    // serviceLock is held. Only a RAM snapshot is read; no backend access,
    // journal repair, radio change, key name, or stored bytes are emitted.
    const auto d=settings.extendedDiagnostic();
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    const auto b=privateStorage.bootDiagnostic();
#else
    struct {int result=0,nameLength=0,valueLength=0;unsigned stage=0,lastNameId=0,slot=0,names=0,kind=0,valueBytes=0;} b;
#endif
    const int n=snprintf(out,capacity,
      "extended_store v=1 state=%u gen=%u active=%d ready=%u dirty=%u r=%u w=%u fail=%u op=%u slot=%u rc=%d len=%u cap=%u first=%u fs=%u fr=%d boot=%u br=%d last=%u bs=%u names=%u kind=%u nl=%d vl=%d init=%d bytes=%u",
      unsigned(extendedJournal.state()),unsigned(extendedJournal.generation()),extendedJournal.activeSlot(),
      unsigned(extendedEdit.ready()),unsigned(extendedEdit.dirty()),unsigned(d.reads),unsigned(d.writes),
      unsigned(d.failures),unsigned(d.lastOp),unsigned(d.lastSlot),int(d.lastRc),
      unsigned(d.lastLength),unsigned(d.lastCapacity),unsigned(d.firstFailedOp),
      unsigned(d.firstFailedSlot),int(d.firstFailedRc),unsigned(b.stage),int(b.result),unsigned(b.lastNameId),
      unsigned(b.slot),unsigned(b.names),unsigned(b.kind),int(b.nameLength),int(b.valueLength),settingsStartupResult,unsigned(b.valueBytes));
    return n>0&&size_t(n)<capacity?size_t(n):0;
  }
  size_t channelDiagnostic(char *out,size_t capacity)const override {
    if(!out||!capacity)return 0;
    // The Service owner already holds serviceLock. Read RAM only: no new
    // clock sample, queue drain, quality decision or modem operation.
    const auto &meter=service.channelMeter();const auto &t=meter.diagnosticTrace();
    struct sx126x_airtime_observer_info info{};
    const int error=sx126x_airtime_observer_info(radioDevice,&info);
    const int n=snprintf(out,capacity,
      "channel_diag v=1 k=%u u=%u q=%u loss=%u lim=%u sup=%u tx=%u ov=%u oth=%u ms=%u irq=%u len=%u why=%u known=%u bound=%u on=%u pend=%u avail=%u stop=%u store=%u err=%d en=%u cfg=%u sf=%u cr=%u bw=%u pre=%u ldro=%u crc=%u ts=%u td=%u tt=%u ti=%u",
      unsigned(t.rxKnown),unsigned(t.rxUnknown),unsigned(t.uncertain),unsigned(t.loss),unsigned(t.limit),
      unsigned(t.support),unsigned(t.tx),unsigned(t.overlap),unsigned(t.other),unsigned(t.lastMs),
      unsigned(t.irq),unsigned(t.len),unsigned(t.reason),unsigned(t.known),unsigned(meter.diagnosticBound()),
      unsigned(meter.diagnosticSupported()),unsigned(meter.diagnosticPending()),unsigned(meter.diagnosticLastAvailable()),
      unsigned(radio.diagnosticStopCalls()),unsigned(radio.diagnosticStorageCalls()),error,
      unsigned(info.enabled),unsigned(info.config_state),unsigned(info.sf),unsigned(info.cr),
      unsigned(info.bandwidth_hz),unsigned(info.preamble_len),unsigned(info.ldro),unsigned(info.crc),
      unsigned(info.tx_started),unsigned(info.tx_done),unsigned(info.tx_timeouts),unsigned(info.last_tx_irq));
    if(n<=0||size_t(n)>=capacity){out[0]=0;return 0;}return size_t(n);
  }
#endif
#if defined(CONFIG_MT_M119_TLS_TEST)
  size_t mqttTlsTestDiagnostic(char *out,size_t capacity)const override {
    if(!out||!capacity)return 0;
    const auto wifi=mt::network::wifiStatus();
    const auto runtime=mt::mqtt::status();
    const int n=snprintf(out,capacity,"m119_tls v=1 ca=%u trust=%u wifi=%u clock=%u state=%u err=%d",
      unsigned(mqttTestCaReady),unsigned(mqttTrustAvailable),unsigned(wifi.mqttReady),
      unsigned(mt::mqtt::certificateTimeReady()),static_cast<unsigned>(runtime.state),runtime.lastError);
    if(n<=0||size_t(n)>=capacity){out[0]=0;return 0;}return size_t(n);
  }
#endif
  bool send(const uint8_t *p,size_t n) override {
    return sendUsb(p,n,false);
  }
#if defined(CONFIG_MT_RUNTIME_RESOURCES)
  size_t runtimeResources(char *out,size_t n) override {return ::runtimeResources.request(out,n);}
#endif
  bool bluetooth() const override {return mt::ble::ready();}
  bool wifi()const override{return mt::network::wifiAvailable();}
  bool mqttAvailable()const override{return mqttAdmin.ready();}
  bool mqttRegionRootRetryPending()const override{return mqttAdmin.regionRootRetryPending();}
  bool mqttRuntimeSupported()const override {
    if(IS_ENABLED(CONFIG_MT_T096_ROUTER_ONLY))return false;
    // This profile always has the cross-profile Companion proxy core; direct
    // MQTT remains compile-time limited to the WiFi gateway.
    return true;
  }
  bool mqttTrustReady()const override {
#if defined(CONFIG_MT_MQTT_GATEWAY)
    return mqttTrustAvailable&&!mqttProxyConfigured();
#else
    return false;
#endif
  }
  bool configOkToMqtt()const override{return mqttAdmin.configOkToMqtt(true);}
  bool mqttProxyPolicy(mt::Companion::Host::MqttProxyPolicy &out)const override {
    out={};
#if defined(CONFIG_MT_M119_TLS_TEST)
    return false; // Test mode never delegates MQTT ingress to a phone.
#else
    if(!mqttSettingsUsable())return false;
    const auto &config=committedMqttSettings();
    out.mqttEnabled=config.enabled();out.proxyToClientEnabled=config.proxyEnabled();
    out.brokerConfigured=config.enabled();
    out.tlsEnabled=(config.bytes[mt::network::MqttGatewaySettings::Flags]&mt::network::MqttGatewaySettings::Tls)!=0;
    out.encryptionEnabled=config.encryptionEnabled();
    out.root=reinterpret_cast<const char *>(config.bytes+mt::network::MqttGatewaySettings::Root);
    return true;
#endif
  }
  bool takeMqttProxyPublish(meshtastic_MqttClientProxyMessage &out)override {
    return mqttProxyConfigured()&&service.takeMqttProxyPublish(out);
  }
  int saveConfigOkToMqtt(bool enabled,uint32_t now)override{
    if(!ready()||rebootSchedule.pending())return -EACCES;
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttAdmin.runtimePolicy(true,mqttTrustAvailable&&!mqttEdit.config().proxyEnabled());
#else
    mqttAdmin.runtimePolicy(mqttEdit.config().proxyEnabled(),false);
#endif
#if defined(CONFIG_MT_MQTT_GATEWAY)
    const uint32_t generation=mqttJournal.generation();
#endif
    const int result=mqttAdmin.saveConfigOkToMqtt(enabled,true,now);
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttRestart.saved(result,generation,mqttJournal.generation(),mqttEdit.dirty());
#endif
    refreshMqttProxyBridge();
    return applicationAfterCommit(result,uint64_t(k_uptime_get()));
  }
  int saveMqttRegionRootTopic(const char *region,size_t regionLength,
                              bool configOkToMqtt,uint32_t now)override{
    return saveMqttRegionRootTopicOwned(region,regionLength,configOkToMqtt,now,false);
  }
  int saveRemoteMqttRegionRootTopic(const char *region,size_t regionLength,
                                    bool configOkToMqtt,uint32_t now)override{
    return saveMqttRegionRootTopicOwned(region,regionLength,configOkToMqtt,now,true);
  }
  int saveMqttRegionRootTopicOwned(const char *region,size_t regionLength,
                                   bool configOkToMqtt,uint32_t now,bool remoteMutation){
    if(!ready()||rebootSchedule.pending()){
      if(!settingsEdit.open())mqttAdmin.markRegionRootRetryPending();
      return -EACCES;
    }
    // A missing/unavailable MQTT journal has no module value to adjust; keep
    // the already-admitted LoRa region save independent of that feature.
    if(!mqttAdmin.ready()&&!mqttAdmin.canRetryIoError()){
      if(configOkToMqtt){if(!settingsEdit.open())mqttAdmin.markRegionRootRetryPending();return -EACCES;}
      return 0;
    }
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttAdmin.runtimePolicy(true,mqttTrustAvailable&&!mqttEdit.config().proxyEnabled());
#else
    mqttAdmin.runtimePolicy(mqttEdit.config().proxyEnabled(),false);
#endif
#if defined(CONFIG_MT_MQTT_GATEWAY)
    const uint32_t generation=mqttJournal.generation();
#endif
    const int result=remoteMutation?
      mqttAdmin.applyRemoteRegionRootTopic(region,regionLength,configOkToMqtt,now,!settingsEdit.open()):
      mqttAdmin.applyRegionRootTopic(region,regionLength,configOkToMqtt,now,!settingsEdit.open());
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttRestart.saved(result,generation,mqttJournal.generation(),mqttEdit.dirty());
#endif
    refreshMqttProxyBridge();
    return applicationAfterCommit(result,uint64_t(k_uptime_get()));
  }
  bool channelUplinkEnabled(const mt::Config &config,unsigned index)const override{
    return mqttAdmin.channelUplinkEnabled(config,index,true);
  }
  int saveChannelUplink(const mt::Config &config,unsigned index,bool enabled,uint32_t now)override{
    return saveChannelUplinkOwned(config,index,enabled,now,false);
  }
  int saveRemoteChannelUplink(const mt::Config &config,unsigned index,bool enabled,uint32_t now)override{
    return saveChannelUplinkOwned(config,index,enabled,now,true);
  }
  int saveChannelUplinkOwned(const mt::Config &config,unsigned index,bool enabled,uint32_t now,bool remoteMutation){
    if(!ready()||rebootSchedule.pending())return -EACCES;
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttAdmin.runtimePolicy(true,mqttTrustAvailable&&!mqttEdit.config().proxyEnabled());
#else
    mqttAdmin.runtimePolicy(mqttEdit.config().proxyEnabled(),false);
#endif
#if defined(CONFIG_MT_MQTT_GATEWAY)
    const uint32_t generation=mqttJournal.generation();
#endif
    const int result=remoteMutation?
      mqttAdmin.saveRemoteChannelUplink(config,index,enabled,true,now):
      mqttAdmin.saveChannelUplink(config,index,enabled,true,now);
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttRestart.saved(result,generation,mqttJournal.generation(),mqttEdit.dirty());
#endif
    refreshMqttProxyBridge();
    return applicationAfterCommit(result,uint64_t(k_uptime_get()));
  }
  bool wifiConnection(meshtastic_WifiConnectionStatus &out)const override{
    out={};if(!wifi())return false;
    const auto observed=mt::network::wifiStatus();out.has_status=true;
    out.status.is_connected=observed.connected;out.status.ip_address=observed.ip;
    // Active association belongs to the immutable boot snapshot. Staged
    // replacement credentials are visible through NetworkConfig only.
    memcpy(out.ssid,observed.ssid,sizeof(out.ssid));
    return true;
  }
  bool networkConfig(meshtastic_Config_NetworkConfig &out)const override{return wifiAdmin.get(out);}
  int saveNetworkConfig(const meshtastic_Config_NetworkConfig &value,uint32_t now)override{
    if(!ready())return -EACCES;
    return wifiAdmin.save(value,wifi(),now,uint64_t(k_uptime_get()));
  }
  bool bluetoothConfig(meshtastic_Config_BluetoothConfig &out) const override {
    if(IS_ENABLED(CONFIG_MT_T096_ROUTER_ONLY))return mt::Companion::Host::bluetoothConfig(out);
    if(!bluetoothJournal.ready())return false;
    const auto &value=bluetoothJournal.config();out={};out.enabled=value.enabled;
    out.mode=static_cast<meshtastic_Config_BluetoothConfig_PairingMode>(value.mode);
    out.fixed_pin=value.fixedPin;return true;
  }
  // Caller holds serviceLock, never the BLE session lock. Durable app policy
  // and the transport handoff are separate so a BLE result cannot be purged.
  int persistBluetoothConfig(const meshtastic_Config_BluetoothConfig &value,uint32_t now) {
    if(IS_ENABLED(CONFIG_MT_T096_ROUTER_ONLY))return -ENOTSUP;
    if(!storageReady||!journal.ready()||!bluetoothJournal.ready())return -EIO;
    if(settingsEdit.open()||rebootSchedule.pending())return -EBUSY;
    mt::BluetoothSettings candidate;candidate.enabled=value.enabled;
    if(unsigned(value.mode)>2)return -EINVAL;
    candidate.mode=uint8_t(value.mode);candidate.fixedPin=value.fixed_pin;
    bool displayReady=false;
#if defined(CONFIG_MT_DISPLAY)
    displayReady=mt::ui::ready();
#endif
    if(!mt::BluetoothSettings::valid(candidate,displayReady))return -EINVAL;
    if(bluetoothConfigReady&&candidate.same(bluetoothJournal.config()))return 0;
    // An explicit local setter may repair a single-good-slot recovery only by
    // verifying a fresh record; boot never silently advertises an older policy.
    const int result=mt::BluetoothSettingsCommit::save(service,radio,bluetoothJournal,
      settings,candidate,now,bluetoothJournal.state()==mt::BluetoothJournal::State::Recovered);
    if(result==-EBUSY&&bluetoothJournal.ready()&&!service.faulted())return result;
    bluetoothConfigReady=result==0&&bluetoothJournal.ready();
    // This is past semantic validation. Only the verified clean busy path
    // above is retryable; backend/native errno values are not malformed input.
    return result?-EIO:0;
  }
  int saveBluetoothConfig(const meshtastic_Config_BluetoothConfig &value,uint32_t now) override {
    if(pendingBluetooth.busy()||bluetoothWorkReady||mt::ble::adminBusy()||mt::ble::connected())return -EBUSY;
    const int result=persistBluetoothConfig(value,now);
    if(result==-EBUSY&&bluetoothJournal.ready())return result;
    // This is a bounded RAM handoff. Stack disconnect/reconfigure runs from
    // ble::poll after serviceLock is released, never inside a storage commit.
    (void)mt::ble::requestConfiguration(bluetoothJournal.config(),bluetoothConfigReady);
    return result;
  }
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  size_t bluetoothDiagnostic(char *out,size_t capacity) const override {return mt::ble::diagnostics(out,capacity);}
#endif
  Security bluetoothSecurity() const override {
    const auto s=mt::ble::security();return {s.connected,s.pending,s.request,s.level,s.error};
  }
  void rangeObservation(uint32_t from,uint32_t id,const uint8_t *payload,size_t size,int16_t rssi,float snr,bool known,uint8_t hopLimit,uint32_t receivedAt)override {
#if defined(CONFIG_MT_RANGE_TEST_LOG)
    meshtastic_ModuleConfig config{};
    if(!ready()||!completionEdit.trusted()||!moduleConfig(config,meshtastic_ModuleConfig_range_test_tag)||
       !known||size>233)return;
    const auto &range=config.payload_variant.range_test;
    mt::stock::RangeTestModuleRadio::handleReceived(range.enabled,range.save,from==service.route.identity(),[&] {
      static struct Seen{uint32_t from=0,id=0,at=0;} seen[8];static unsigned next=0;const uint32_t now=k_uptime_get_32();
      for(const auto &entry:seen)if(entry.from==from&&entry.id==id&&uint32_t(now-entry.at)<600000)return;
      seen[next++%8]={from,id,now};mt::modules::RangeTestLog::Record record{};record.from=from;record.rxRssi=rssi;record.rxSnr=snr;record.hopLimit=hopLimit;
      record.timeValid=receivedAt!=0;const uint32_t day=receivedAt%86400;record.utcHour=day/3600;record.utcMinute=(day%3600)/60;record.utcSecond=day%60;
      record.payloadSize=uint16_t(size);if(size)memcpy(record.payload,payload,size);
      const int found=service.nodes.find(from);const auto *entry=found>=0?service.nodes.get(unsigned(found)):nullptr;
      if(entry&&!entry->placeholder()){
        strncpy(record.senderName,reinterpret_cast<const char*>(entry->bytes+mt::NodeCache::Entry::Name),sizeof(record.senderName)-1);
        meshtastic_Position position{};entry->position.exportTo(position);record.senderPositionValid=position.has_latitude_i&&position.has_longitude_i;
        record.senderLatitudeI=position.latitude_i;record.senderLongitudeI=position.longitude_i;record.senderAltitudeM=position.altitude;
      }
      meshtastic_Position local{};record.receiverPositionValid=service.localPosition(local);record.receiverLatitudeI=local.latitude_i;record.receiverLongitudeI=local.longitude_i;record.receiverAltitudeM=local.altitude;
      (void)rangeLog.enqueue(record);
    });
#else
    (void)from;(void)id;(void)payload;(void)size;(void)rssi;(void)snr;(void)known;(void)hopLimit;(void)receivedAt;
#endif
  }
  void preview(uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *p,size_t n,int16_t rssi,int8_t snr,uint32_t now,bool local)override {
    notifications.configure(notificationEdit.config(),!completionRuntime.noticeMuted()&&ready()&&notificationEdit.ready()&&mt::notification::available());
    if(!local){
      const bool muted=to==UINT32_MAX?service.channelMuted():service.nodes.muted(from);
      const bool rangeEnabled=moduleEdit.ready()&&moduleEdit.config().bytes[15];
      (void)notifications.receive(from,service.route.identity(),id,port,p,n,muted,rangeEnabled,now);
    }
#if defined(CONFIG_MT_DISPLAY)
    mt::stock::text::Settings textSettings;
    textSettings.rangeEnabled=moduleEdit.ready() && moduleEdit.config().bytes[15];
    if(!local && mt::stock::text::wantPacket(port,textSettings.rangeEnabled)){
      textSettings.role=service.config.role();
      const auto charge=service.hardware().fresh(k_uptime_get()).cellPercent;
      if(charge)textSettings.batteryPercent=*charge;
      if(notificationEdit.ready()){
        textSettings.externalNotifications=notificationEdit.config().enabled();
        textSettings.bellOutput=notificationEdit.config().alertBell();
      }
      completionEdit.textNotificationPolicy(textSettings.externalNotifications,textSettings.bellOutput);
    }
    uiController.preview(service,from,to,id,port,p,n,rssi,snr,now,local,textSettings);
#else
    (void)rssi;(void)snr;
#endif
  }
#if defined(CONFIG_MT_POWER_STRESS)
  int powerStress(uint32_t id,const uint8_t *payload,size_t size)override {
    if(!ready() || !completionEdit.trusted() || settingsEdit.open() ||
       !completionRuntime.powerMonitorEnabled() || rebootSchedule.pending())return -EACCES;
    meshtastic_PowerStressMessage request{};
    if(!payload || !size)return -EINVAL;
    auto stream=pb_istream_from_buffer(payload,size);
    if(!pb_decode(&stream,meshtastic_PowerStressMessage_fields,&request) || stream.bytes_left)return -EINVAL;
    return powerStressModule.receive(id,request);
  }
#endif
  void startWaypointNotification(uint32_t now){
    notifications.configure(notificationEdit.config(),!completionRuntime.noticeMuted()&&ready()&&notificationEdit.ready()&&mt::notification::available());
    (void)notifications.startNotification(now);
    completionRuntime.queueLocalNotification();
  }
  void positionForGeofence(uint32_t from,uint32_t to,unsigned channel,bool privatePacket,const uint8_t *bytes,size_t n)override{
    if(!ready()||channel>=8)return;
    const auto clock=uint64_t(k_uptime_get());
    waypoints.domain(service.route.identity(),service.waypointDomainEpoch());
    if(!waypoints.subscribed())return;
    if(privatePacket)waypointHistory.syncContacts(waypoints,waypointHistoryContacts());
    const int slot=service.nodes.find(from);
    const auto *peer=slot>=0?service.nodes.get(unsigned(slot)):nullptr;
    const bool favorite=peer&&peer->favorite();
    mt::ChannelRecord record;
    const bool muted=to==UINT32_MAX ? (service.config.channelRecord(channel,record)&&record.muted()) : service.nodes.muted(from);
    waypoints.evaluatePosition(from,{privatePacket?from:0,uint8_t(channel)},bytes,n,wallClock.at(clock),favorite,
      [&](const mt::stock::waypoint::Entry &entry,mt::stock::waypoint::Crossing crossing){
        if(muted)return;
        notifyPowerActivity();
        startWaypointNotification(uint32_t(clock));
#if defined(CONFIG_MT_DISPLAY)
        mt::ui::showGeofence(from,entry.name,crossing==mt::stock::waypoint::Crossing::Enter,uint32_t(clock));
#else
        (void)entry;(void)crossing;
#endif
      });
  }
  void waypointPacket(uint32_t from,uint32_t to,uint32_t id,unsigned channel,bool privatePacket,bool local,
                      const uint8_t *payload,size_t size)override {
    if(!ready()||channel>=8)return;
    const auto clock=uint64_t(k_uptime_get());
    waypoints.domain(service.route.identity(),service.waypointDomainEpoch());
    const mt::stock::waypoint::Scope scope{privatePacket?(local?to:from):0,uint8_t(channel)};
    if(privatePacket)waypointHistory.syncContacts(waypoints,waypointHistoryContacts());
    const auto result=waypoints.handleReceived(from,id,scope,local,payload,size,wallClock.at(clock));
    if(result==mt::stock::waypoint::Result::Ignored)return;
    notifyPowerActivity();
    mt::ChannelRecord record;
    const bool muted=to==UINT32_MAX ? (service.config.channelRecord(channel,record)&&record.muted()) : service.nodes.muted(from);
    if(!local&&!muted&&result==mt::stock::waypoint::Result::Stored){
#if defined(CONFIG_MT_DISPLAY)
      mt::ui::showWaypoint(waypoints.newest()->name,uint32_t(clock));
      startWaypointNotification(uint32_t(clock)); // WaypointModule's HAS_SCREEN receive alert.
#endif
    }
  }
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
  void displayReport(uint32_t from,uint32_t id,bool shared,const meshtastic_Telemetry &report) override {
    if(ready())observeDisplayReport(from,id,shared,report);
  }
#endif
  void peripheralPacket(uint32_t from,uint32_t to,uint32_t id,unsigned port,unsigned channel,
                        const uint8_t *payload,size_t size,bool local)override {
    if(!local&&ready())completionRuntime.receive(from,to,service.route.identity(),id,port,channel,payload,size,
      to==UINT32_MAX?service.channelMuted():service.nodes.muted(from),moduleEdit.ready()&&moduleEdit.config().bytes[15],k_uptime_get());
#if defined(CONFIG_MT_SERIAL_MODULE)
    (void)local;
    if(!ready()||!serialModule.ready()||!id||
       (to!=UINT32_MAX&&to!=service.route.identity()))return;
    meshtastic_ModuleConfig_SerialConfig config{};
    if(!extendedEdit.trusted()||!completionEdit.trusted()||!serialConfig(config)||!config.enabled)return;
    const auto serialMode=mt::detail::enumNumber(config.mode);
    const bool positionMode = mt::stock::serial::positionMode(serialMode);
    if (positionMode) {
      if (port != meshtastic_PortNum_POSITION_APP || from == service.route.identity()) return;
    } else {
      if(serialMode!=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE&&serialMode!=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_DEFAULT&&serialMode!=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG)return;
      const bool text=config.mode==meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG;
      if(text?port!=meshtastic_PortNum_TEXT_MESSAGE_APP:
         (port!=meshtastic_PortNum_SERIAL_APP||namedSerialChannel()!=int(channel)))return;
    }
    const uint32_t now=k_uptime_get_32();
    // Companion fanout can observe the same admitted packet for USB/BLE/TCP.
    // The physical UART consumes that packet once, independently of clients.
    for(const auto &seen:serialSeen)
      if(seen.from==from&&seen.id==id&&seen.channel==channel&&uint32_t(now-seen.at)<600000)return;
    serialSeen[nextSerialSeen++%8]={from,id,now,channel};
    const int slot=service.nodes.find(from);
    const auto *node=slot>=0?service.nodes.get(unsigned(slot)):nullptr;
    if (positionMode) {
      const char *name = node && !node->placeholder() ?
        reinterpret_cast<const char *>(node->bytes + mt::NodeCache::Entry::Name) : "";
      (void)mt::stock::serial::handleReceivedPosition(payload, size, name,
          [](const mt_serial::Position &fix, const char *sender) { return serialModule.meshPosition(fix, sender); });
      return;
    }
    const char *name=node&&!node->placeholder()?
      reinterpret_cast<const char *>(node->bytes+mt::NodeCache::Entry::Short):"???";
    serialModule.mesh_receive(from==service.route.identity(),id,name,payload,size);
#else
    (void)from;(void)to;(void)id;(void)port;(void)channel;(void)payload;(void)size;(void)local;
#endif
  }
#if defined(CONFIG_MT_DISPLAY)
  int uiInput(uint32_t code,uint32_t now)override {
    refreshCannedInput();
    return uiController.input(code,now);
  }
#endif
#if defined(CONFIG_MT_PEER_DIRECTORY)
#if defined(CONFIG_MT_PAGED_SATELLITES)
  void satelliteReceived(uint32_t id,unsigned kind,unsigned channel,mt::companion::SatelliteReplay::Observation obs,
                         const uint8_t *body,size_t size)override{
    k_mutex_lock(&directoryLock,K_FOREVER);
    const int slot=service.nodes.find(id);const auto *e=slot<0?nullptr:service.nodes.get(unsigned(slot));
    if(!e||(!e->ignored()&&!e->placeholder())){
      int rc=-EAGAIN;
      if(ready()&&!settingsEdit.open()&&directorySettingsGen==journal.generation()&&directoryBound&&
         (!e||service.nodes.replayEpoch(unsigned(slot))))rc=reportPages.offer(peerDirectory.ticket(id),id,kind,channel,obs,body,size,
        e?e->bytes+mt::NodeCache::Entry::Key:nullptr,e?int(e->bytes[mt::NodeCache::Entry::KeyLength]):-1);
      if(rc==-EAGAIN&&e)reportPages.dirty(unsigned(slot),kind);
    }
    k_mutex_unlock(&directoryLock);
  }
  int satellitePacket(unsigned owner,unsigned kind,unsigned ordinal,mt::companion::SatelliteSample &out)override{
    if(!ready()||!directoryBound||settingsEdit.open()||directorySettingsGen!=journal.generation())return 0;
    if(k_mutex_lock(&directoryLock,K_NO_WAIT))return -EAGAIN;
    int rc=reportPages.request(peerDirectory,owner,kind,ordinal,out);
    if(rc==1){
      const int slot=service.nodes.find(out.owner.id);const auto *e=slot<0?nullptr:service.nodes.get(unsigned(slot));
      mt::PrimaryChannel::Selection selected{};
      if(!service.config.channelSelection(out.channel,selected))rc=2;
      mt::PrimaryChannel::clear(selected);
      if(e&&(e->ignored()||e->placeholder()||e->bytes[mt::NodeCache::Entry::KeyLength]!=out.owner.keySize||
          memcmp(e->bytes+mt::NodeCache::Entry::Key,out.owner.key,32)||
          (kind==0&&e->position.present())||(kind==1&&e->deviceMetrics.present())||
          (kind==2&&e->replayEnvironment)||(kind==3&&e->status.present())))rc=2;
    }
    k_mutex_unlock(&directoryLock);return rc;
  }
#endif
  unsigned directoryLimit()const override{return DirectoryLimit;}
  unsigned directoryCount(const mt::NodeCache &hot)const override{
    if(!ready()||!directoryBound||settingsEdit.open()||directorySettingsGen!=journal.generation())return hot.count();
    k_mutex_lock(&directoryLock,K_FOREVER);const unsigned n=peerDirectory.visibleCount(hot);k_mutex_unlock(&directoryLock);return n;
  }
  int directoryPeer(unsigned owner,unsigned ordinal,const mt::NodeCache &hot,mt::NodeCache::Entry &e) override{
    if(!ready()||!directoryBound||settingsEdit.open()||directorySettingsGen!=journal.generation())return 0;
    if(k_mutex_lock(&directoryLock,K_NO_WAIT))return -EAGAIN;
    const int result=peerDirectory.next(owner,ordinal,hot,e);k_mutex_unlock(&directoryLock);return result;
  }
  void directoryObserved(const mt::NodeCache::Entry &e,uint32_t receivedAt=0)override{
    k_mutex_lock(&directoryLock,K_FOREVER);
#if defined(CONFIG_MT_PAGED_SATELLITES)
    if(receivedAt)reportPages.supersedeHeader(e.id());
    peerDirectory.observe(e,service.satellites.publicHeader(e.id()));
#else
    (void)receivedAt;peerDirectory.observe(e);
#endif
    k_mutex_unlock(&directoryLock);
  }
  uint32_t directoryRevision()const override{
    k_mutex_lock(&directoryLock,K_FOREVER);const auto revision=peerDirectory.revision();k_mutex_unlock(&directoryLock);return revision;
  }
  static int invalidateDirectoryRow(uint32_t id){
    k_mutex_lock(&directoryLock,K_FOREVER);peerDirectory.invalidate(id);k_mutex_unlock(&directoryLock);
    return 0;
  }
  static int prepareDirectoryEdit(uint32_t id,mt::NodeCache::Edit op){
    return op==mt::NodeCache::Edit::Remove||op==mt::NodeCache::Edit::Ignore?forgetDirectory(id):invalidateDirectoryRow(id);
  }
  int promoteDirectoryFavorite(unsigned owner,uint32_t id,uint32_t now)override{
    if(!directoryBound)return -ENOENT;
    if(!ready()||!nodeJournal.ready()||!service.nodes.ready())return -EACCES;
    if(settingsEdit.open()||rebootSchedule.pending()||!peerContext.ready()||directorySettingsGen!=journal.generation())return -EBUSY;
    mt::NodeCache::Entry candidate{};
    k_mutex_lock(&directoryLock,K_FOREVER);
    const int prepared=peerDirectory.favoriteCandidate(owner,id,service.nodes,candidate,[](uint32_t peer,uint8_t *key){
      return privateStorage.storage().lookup(peer,key);
    });
    k_mutex_unlock(&directoryLock);if(prepared)return prepared;
    return mt::NodeAdmin::promoteFavorite(peerContext,[&](){
      const int rc=nodeJournal.ownerContact(settings,service.nodes,candidate,false,false,now,
        service.privateStoragePaused(),true,0,invalidateDirectoryRow);
      if(rc)return rc;
      const int slot=service.nodes.find(id);if(slot>=0)directoryObserved(*service.nodes.get(unsigned(slot)));
      (void)syncHotNodeFlags();return 0;
    });
  }
  void directoryDisconnect(unsigned owner)override{
    k_mutex_lock(&directoryLock,K_FOREVER);peerDirectory.disconnect(owner);
#if defined(CONFIG_MT_PAGED_SATELLITES)
    reportPages.disconnect(owner);
#endif
    k_mutex_unlock(&directoryLock);
  }
#endif
  bool ready() const override {return initialized&&storageReady&&journal.ready()&&!remoteConfigPending()&&!beaconTargets.radioHeld()&&!atomic_get(&shutdownAdmissionClosed);}
  bool acceptTransportFrame(uint32_t generation) override {
    // This is the USB request's single acceptance/linearization point. The
    // same IRQ exclusion used by the CDC ISR orders an observed DTR transition
    // either before this request (reject) or after its accepted ownership.
    const unsigned key=irq_lock();const auto line=observeUsbLineLocked(usb);
    const bool accepted=(!line.measured||line.connected)&&line.generation==generation;
    usbAccepted_=accepted;usbAcceptedGeneration_=generation;
    irq_unlock(key);return accepted;
  }
  bool beginUnmeasuredHandshakeBoundary() override {
    // XIAO USB Serial/JTAG exposes no DTR or equivalent connection event. A
    // canonical framed want_config_id is its explicit client epoch boundary.
    // Clear only queued old output here; Companion owns parser/replay reset and
    // already-extracted bytes after the handshake remain in the new epoch.
    if(!IS_ENABLED(CONFIG_BOARD_XIAO_ESP32S3))return false;
    const unsigned key=irq_lock();uint32_t dtr=0;const uint32_t generation=usbSession.generation();
    if(uart_line_ctrl_get(usb,UART_LINE_CTRL_DTR,&dtr)==0){irq_unlock(key);return false;}
    ring_buf_reset(&usbTx);uart_irq_tx_disable(usb);irq_unlock(key);
    (void)generation;return true;
  }
  void completeUnmeasuredHandshakeBoundary(uint32_t generation)override {
    if(!IS_ENABLED(CONFIG_BOARD_XIAO_ESP32S3))return;
    const unsigned key=irq_lock();uint32_t dtr=0;
    const bool current=uart_line_ctrl_get(usb,UART_LINE_CTRL_DTR,&dtr)!=0&&usbSession.generation()==generation;
    irq_unlock(key);if(!current)return;
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    identityRecoverySession.authorizeUnmeasured(generation);
    freshUsbContext.authorizeUnmeasured(generation);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    importUsbContext.authorizeUnmeasured(generation);
#endif
#endif
  }
  int setWallTime(uint32_t value) override {
    return wallClock.set(value,uint64_t(k_uptime_get()));
  }
  bool uptimeSeconds(uint32_t &seconds) const override {
    const int64_t milliseconds=k_uptime_get();
    if(milliseconds<0||uint64_t(milliseconds)/1000>UINT32_MAX){seconds=0;return false;}
    seconds=uint32_t(uint64_t(milliseconds)/1000);return true;
  }
  bool gnssPresent() const override{return mt::gnss::present();}
  size_t gnssDiagnostic(char *out,size_t capacity) const override {
    if(!out||!capacity)return 0;
    const auto state=mt::gnss::diagnostics();
    const int n=snprintf(out,capacity,
      "gnss_diag v=1 fitted=%u init=%u ready=%u fault=%u active=%u sentences=%u callbacks=%u promoted=%u sats=%u",
      unsigned(mt::gnss::present()),unsigned(state.initialized),unsigned(state.ready),unsigned(state.fault),
      unsigned(state.acquiring),unsigned(state.sentences),unsigned(state.callbacks),unsigned(state.promotions),unsigned(state.satellites));
    if(n<=0||size_t(n)>=capacity){out[0]=0;return 0;}return size_t(n);
  }
  uint32_t wallTime() const override {
    return wallClock.at(uint64_t(k_uptime_get()));
  }
  Serial serialStatus() const override {
    uint32_t dtr=0,baud=0;
    const bool haveDtr=uart_line_ctrl_get(usb,UART_LINE_CTRL_DTR,&dtr)==0;
    const bool haveBaud=uart_line_ctrl_get(usb,UART_LINE_CTRL_BAUD_RATE,&baud)==0;
    return {mt_security::serialConsoleAllowed()&&dtr!=0,baud,haveDtr&&haveBaud};
  }
  int addOwnerContact(const mt::OwnerContactCandidate &contact,uint32_t now) override {
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    if(!ready()||!nodeJournal.ready()||!service.nodes.ready())return -EACCES;
    if(!peerContext.ready())return -EBUSY;
    mt::NodeCache::Entry resolved{};
    const bool favorite=service.config.role()!=meshtastic_Config_DeviceConfig_Role_CLIENT_BASE;
    const uint32_t heard=wallClock.at(uint64_t(k_uptime_get()));
    int result=nodeJournal.previewOwnerContact(service.nodes,contact.node,contact.shouldIgnore,
      contact.manualVerificationRequested,resolved,favorite,heard);
    if(result)return result;
    mt::pki::PkiPeerPin pin{};pin.node=resolved.id();
    memcpy(pin.publicKey,resolved.bytes+mt::NodeCache::Entry::Key,32);
    mt::detail::VolatileWipe wipePin{&pin,sizeof(pin)};
    uint8_t converted[32]{};mt::detail::VolatileWipe wipeConverted{converted,sizeof(converted)};
    const bool install=!resolved.ignored()&&resolved.bytes[mt::NodeCache::Entry::KeyLength]==32&&
      bench::MtSignature::admissiblePublicKey(pin.publicKey,converted);
    auto &store=privateStorage.storage();
    mt::pki::IdentityBinding binding;mt::pki::KeyRef ref;
    auto *identity=store.runtimeIdentity();
    if(!identity||!identity->binding(binding,ref)||binding.node!=service.route.identity())return -EACCES;
    // A malformed replacement cannot leave an old private key paired with a
    // different visible unignored contact key.
    uint8_t oldKey[32]{};mt::detail::VolatileWipe wipeOld{oldKey,sizeof(oldKey)};
    const bool hadKey=store.lookup(pin.node,oldKey);
    if(!resolved.ignored()&&resolved.bytes[mt::NodeCache::Entry::KeyLength]==32&&!install)return -EINVAL;
    const bool changedKey=install&&hadKey&&memcmp(oldKey,pin.publicKey,32);
    if(!peerContext.pause())return -EBUSY;
#if defined(CONFIG_MT_PEER_DIRECTORY)
    auto withdraw=[](uint32_t id){return forgetDirectory(id);};
    result=install?store.putOwnerPeer(binding,pin,withdraw):withdraw(pin.node);
#else
    if(install)result=store.putOwnerPeer(binding,pin);
#endif
    if(result){
      const bool ordinary=result==-EINVAL||result==-ENOSPC||result==-EEXIST||result==-EACCES||
        result==-ENOTSUP||result==-EBUSY||result==-EAGAIN||result==-EOVERFLOW;
      if(!ordinary||store.state()!=mt::pki::PkiStorage::State::Active)peerContext.hold();
      else if(!peerContext.resume())peerContext.hold();
      return result;
    }
    result=nodeJournal.ownerContact(settings,service.nodes,contact.node,contact.shouldIgnore,
      contact.manualVerificationRequested,now,service.privateStoragePaused(),favorite,heard);
    // Storage and metadata are separate verified records. Never roll back an
    // accepted key or acknowledge a partial/uncertain transaction as success.
    if(result){
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
      if(!install&&nodeJournal.verifiedCapacityRefusal(result,service.nodes)){
        if(peerContext.resume())return result;
        peerContext.hold();return -EIO;
      }
#endif
      peerContext.hold();return install?mt::Companion::Host::ContactPartiallyCommitted:result;
    }
#if defined(CONFIG_MT_PEER_DIRECTORY)
    const int directorySlot=service.nodes.find(contact.node.id());
    if(directorySlot>=0)directoryObserved(*service.nodes.get(unsigned(directorySlot)));
#endif
    if(install&&(!hadKey||changedKey)&&!privateStartup.adoptPersistedPeer(pin)){
      peerContext.hold();return mt::Companion::Host::ContactPartiallyCommitted;
    }
    if(changedKey)service.signature.forgetPublicKey(pin.node);
    if(!peerContext.resume()){peerContext.hold();return mt::Companion::Host::EditCommittedDegraded;}
    return 0;
#else
    (void)contact;(void)now;return -ENOTSUP;
#endif
  }
#if defined(CONFIG_MT_PHONE_FILES)
  int appFileInfo(uint8_t owner,meshtastic_FileInfo &info)override{
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    return publicManifest.request(owner,info);
#else
    uint32_t size=0;const int result=phoneFiles.manifest(owner,size);
    if(result>0){strcpy(info.file_name,mt::stock::files::PublicName);info.size_bytes=size;}
    return result;
#endif
  }
  void appFileInfoSent(uint8_t owner)override{
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    publicManifest.advance(owner);
#else
    phoneFiles.manifestSent(owner);
#endif
  }
  bool appFileSubmit(uint8_t owner,const meshtastic_XModem &packet,uint32_t now)override{
    return !rebootSchedule.pending()&&!firmwareUpdate.pending()&&phoneFiles.submit(owner,packet,now);
  }
  bool appFileResponse(uint8_t owner,meshtastic_XModem &packet)override{return phoneFiles.response(owner,packet);}
  void appFileSent(uint8_t owner)override{phoneFiles.sent(owner);}
  void appFileDisconnect(uint8_t owner)override{phoneFiles.disconnect(owner);
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    publicManifest.reset(owner);
#endif
  }
#endif
  int removeAppFile(const char *name)override {
#if defined(CONFIG_MT_RANGE_TEST_LOG)
    if(!ready()||settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
    return rangeLog.removeFile(name);
#else
    (void)name;return -ENOTSUP;
#endif
  }
#if MT_HAS_DEVICE_RESET
  int requestFactoryDeviceReset()override {
    if(service.config.isManaged())return -EACCES;
    if(!ready()||persistentWorkPending()||administrationWorkPending()||firmwareUpdate.pending()||
       rebootSchedule.pending()||preferenceRecoveryHeld||!service.storageStopSafe()||radio.busy())return -EBUSY;
    mt::pki::IdentityBinding identity{};
    if(!preferenceDomainReady||!privateStorage.storage().preparedBinding(identity)||
       deviceReset::freshBinding(settings,identity,deviceReset::RootUse::RetireCurrent))return -ENOTSUP;
    const bool receiving=service.receiving(),relaying=service.relaying(),armed=service.endpointArmed();
    const int paused=service.pauseForStorage();if(paused)return paused;
    if(!service.storageQuiescent()||!radio.ready()||radio.busy()||radio.health()!=0)return -EIO;
    bool attempted=false;
    const int result=deviceReset::begin(resetFloor,settings,identity,privateEntropy,attempted);
    if(result&&!attempted){
      if(receiving&&(!service.start(relaying,k_uptime_get_32())||!service.armEndpoint(armed)))return -EIO;
      return result;
    }
    scheduleOrdinaryReboot(7,uint64_t(k_uptime_get()));
    factoryConfigResetScheduled=true;storageReady=false;return result;
  }
#endif
  int requestFactoryConfigReset()override {
    if(!ready()||persistentWorkPending()||administrationWorkPending()||firmwareUpdate.pending()||
       rebootSchedule.pending()||preferenceRecoveryHeld||!service.storageStopSafe()||radio.busy())return -EBUSY;
    const int paused=service.pauseForStorage();if(paused)return paused;
    if(!service.storageQuiescent()||!radio.ready()||radio.busy()||radio.health()!=0)return -EIO;
    // Never activate RF after an uncertain intent write. Boot verifies the
    // original owner binding before making any change to storage.
    const int marked=mt::stock::admin::factory::mark(settings,service.route.identity(),preferenceDomain.publicKey,
                                                     FactoryPhase::RetireContacts);
    scheduleOrdinaryReboot(7,uint64_t(k_uptime_get()));
    factoryConfigResetScheduled=true;storageReady=false;
    return marked;
  }
  int resetNodeDb(bool keepFavorites,uint32_t now)override {
    if(!ready()||settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending()||
       administrationWorkPending()||persistentWorkPending()||preferenceRecoveryHeld)return -EBUSY;
    if(!hotNodeCountAvailable)return -EIO;
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    if(preferenceDomainReady&&!hotNodeJournal.ready())return -EIO;
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(preferenceDomainReady&&(!remoteHardwarePinJournalReady||!remoteHardwarePinJournal.ready()))return -EIO;
#endif
    const int result=mt::NodeAdmin::reset(service,radio,nodeJournal,settings,keepFavorites,now,
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
      [](uint32_t id,void *context){
        // A full reset can retire 199 rows. Each callback follows a verified
        // Remove transaction; a stalled flash or retirement cannot feed here.
        forgetCompanionPeer(id,context);
        if(preparationWatchdog&&preparationWatchdogChannel>=0)
          (void)wdt_feed(preparationWatchdog,preparationWatchdogChannel);
      },
#else
      forgetCompanionPeer,
#endif
      [&](){
        const auto keep=[&](uint32_t id){return id==service.route.identity()||service.nodes.find(id)>=0;};
        for(size_t i=hotNodeWindow.hotCount();i>0;--i){
          const auto id=hotNodeWindow.hotAt(i-1)->node;
          if(!keep(id)&&hotNodeWindow.remove(id))markHotNodeDirty(now);
        }
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
        if(hotNodeDirty&&preferenceDomainReady){
          const int saved=hotNodeJournal.save(settings,hotNodeWindow,service.route.identity(),hotNodePublicKey);
          if(saved)return saved;
          hotNodeDirty=false;hotNodeLastWrite=now;
        }
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
        for(size_t i=0;i<remoteHardwarePinCache.size();){
          const auto id=remoteHardwarePinCache.data()[i].node_num;
          if(keep(id)){++i;continue;}
          if(!remoteHardwarePinCache.removePeer(id))return -EIO;
          remoteHardwarePinDirty=true;
        }
        if(remoteHardwarePinDirty){
          mt::admin::RemoteHardwarePinJournal::Snapshot snapshot{};
          if(!remoteHardwarePinCache.copyTo(snapshot.entries,mt::admin::RemoteHardwarePinCache::Capacity,snapshot.count))
            return -EIO;
          const int saved=remoteHardwarePinJournal.save(settings,snapshot);
          if(saved)return saved;
          remoteHardwarePinDirty=false;
        }
#endif
#if defined(CONFIG_MT_PEER_DIRECTORY)
        return forgetDirectory(0,true);
#else
        return 0;
#endif
      }
#if defined(CONFIG_MT_PEER_DIRECTORY)
      ,prepareDirectoryEdit
#endif
    );
    if(!result)scheduleOrdinaryReboot(7,uint64_t(k_uptime_get()));
    return result;
  }
  int editNode(uint32_t id,mt::NodeCache::Edit op,uint32_t now) override {
    if(!ready())return -EACCES;
    const int result=mt::NodeAdmin::edit(service,radio,nodeJournal,settings,id,op,now,
      settingsEdit.open()||rebootSchedule.pending(),forgetCompanionPeer,nullptr
#if defined(CONFIG_MT_PEER_DIRECTORY)
      ,prepareDirectoryEdit
#endif
    );
    if(result==0){
      (void)syncHotNodeFlags();
#if defined(CONFIG_MT_PEER_DIRECTORY)
      const int slot=service.nodes.find(id);if(slot>=0)directoryObserved(*service.nodes.get(unsigned(slot)));
#endif
    }
    return result;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  int editRemoteFavorite(uint32_t id,bool favorite,uint32_t now) override {
    if(!ready())return -EACCES;
    const int result=mt::admin::RemoteFavoriteCommit::edit(service,radio,nodeJournal,settings,
      id,favorite,now,settingsEdit.open()||rebootSchedule.pending()
#if defined(CONFIG_MT_PEER_DIRECTORY)
      ,prepareDirectoryEdit
#endif
    );
    if(result==0){
      (void)syncHotNodeFlags();
#if defined(CONFIG_MT_PEER_DIRECTORY)
      const int slot=service.nodes.find(id);if(slot>=0)directoryObserved(*service.nodes.get(unsigned(slot)));
#endif
    }
    return result;
  }
#endif
  int beginEdit(uint32_t now) override {
    // A previously scheduled reboot must not interrupt a newly accepted batch.
    if(!ready()||rebootSchedule.pending())return -EACCES;
    return settingsEdit.begin(now);
  }
  int commitEdit(uint32_t now) override {
    return ready()?applicationAfterCommit(settingsCommit.commit(now),uint64_t(k_uptime_get())):-EACCES;
  }
  bool editing() const override {return settingsEdit.open();}
  bool scheduleReboot(int32_t seconds) override {
    if(!ready()||firmwareUpdate.pending())return false;
    scheduleOrdinaryReboot(seconds,uint64_t(k_uptime_get()));return true;
  }
  void nodeInfoPing(uint32_t now)override {
    if(IS_ENABLED(CONFIG_MT_T096_ROUTER_ONLY))return;
    if(ready()&&!settingsEdit.open()&&!persistentWorkPending()&&!administrationWorkPending()&&
       !rebootSchedule.pending()&&!firmwareUpdate.pending())
      (void)service.phoneNodeInfoPing(now,sys_rand32_get());
  }
  int scheduleFirmwareUpdate(const meshtastic_AdminMessage_OTAEvent &request,uint32_t)override {
    if(!ready()||settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
    mt::update::Mode mode=mt::update::Mode::None;
#if defined(CONFIG_SOC_SERIES_NRF52)
    if(request.reboot_ota_mode==meshtastic_OTAMode_OTA_BLE)mode=mt::update::Mode::NordicBleDfu;
#elif defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
    if(request.reboot_ota_mode==meshtastic_OTAMode_OTA_BLE)mode=mt::update::Mode::EspBleLoader;
    else if(request.reboot_ota_mode==meshtastic_OTAMode_OTA_WIFI)mode=mt::update::Mode::EspWifiLoader;
#endif
    if(mode==mt::update::Mode::None)return -ENOTSUP;
    const int result=firmwareUpdate.schedule(mode,mode==mt::update::Mode::EspBleLoader||mode==mt::update::Mode::EspWifiLoader?
      request.ota_hash.bytes:nullptr,mode==mt::update::Mode::EspBleLoader||mode==mt::update::Mode::EspWifiLoader?request.ota_hash.size:0);
    if(result)return result;
    rebootSchedule.request(1,uint64_t(k_uptime_get()),ScheduledOwnerAction::FirmwareUpdate);
    updateFailureReported=false;return 0;
  }
  int enterDfu(uint32_t)override {
    if(!ready()||settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
    mt::update::Mode mode=mt::update::Mode::None;
#if defined(CONFIG_SOC_SERIES_NRF52)
    mode=mt::update::Mode::NordicBleDfu;
#endif
    if(mode==mt::update::Mode::None)return -ENOTSUP;
    const int result=firmwareUpdate.schedule(mode);
    if(result)return result;
    rebootSchedule.request(1,uint64_t(k_uptime_get()),ScheduledOwnerAction::Dfu);
    updateFailureReported=false;return 0;
  }
  bool scheduleShutdown(int32_t seconds)override {
    if(!ready()||firmwareUpdate.pending())return false;
    if(seconds>=0&&(!completionRuntime.supportsRequestedPowerOff()||rebootSchedule.pending()))return false;
    rebootSchedule.request(seconds,uint64_t(k_uptime_get()),ScheduledOwnerAction::Shutdown);return true;
  }
  int save(const mt::Config &c) override {
    if(!ready()||!c.valid())return -EACCES;
    return settingsEdit.save(c,k_uptime_get_32());
  }
  bool heartbeatDisabled()const override {
    return !notificationEdit.ready()||notificationEdit.config().heartbeatDisabled();
  }
  int saveDevice(const mt::Config &c,bool disabled,uint32_t now)override {
    if(!ready()||rebootSchedule.pending())return -EACCES;
    if(c.role()==mt::modules::RouterRoleDefaults::RoleRouter&&c.role()!=service.config.role()) {
      if(!moduleEdit.ready()||!completionEdit.ready())return -EACCES;
      meshtastic_ModuleConfig fullTelemetry{},pax{};meshtastic_Config_PowerConfig fullPower{};
      if(!moduleConfig(fullTelemetry,meshtastic_ModuleConfig_telemetry_tag)||!moduleConfig(pax,meshtastic_ModuleConfig_paxcounter_tag)||!powerConfig(fullPower))return -EACCES;
      mt::modules::RouterRoleDefaults::Projection projected;
      if(mt::modules::RouterRoleDefaults::project(service.config.role(),c,moduleEdit.config(),projected,&fullTelemetry.payload_variant.telemetry,&fullPower)!=
         mt::modules::RouterRoleDefaults::Result::Applied)return -EINVAL;
      fullTelemetry.payload_variant.telemetry=projected.telemetry;pax.payload_variant.paxcounter.paxcounter_update_interval=projected.paxcounterInterval;
      auto supplement=completionEdit.config();
      if(!supplement.set(fullTelemetry)||!supplement.set(pax)||!supplement.setPower(projected.power))return -EINVAL;
      if(!completionRuntime.supports(fullTelemetry)||!completionRuntime.supportsPower(projected.power))return -ENOTSUP;
      const bool openedHere=!settingsEdit.open();if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
      int result=completionEdit.stage(supplement);if(result)return result;
      result=mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,settingsCommit,
        projected.core,disabled,mt::notification::available(),now,&moduleEdit,nullptr,&projected.modules);
      if(!result&&openedHere)result=settingsCommit.commit(now);
      return result;
    }
    if(mt::modules::OperatingRole::handles(c.role())&&c.role()!=service.config.role()){
      if(!moduleEdit.ready())return -EACCES;
      auto core=c;auto modules=moduleEdit.config();
      const bool hidden=c.role()==mt::DeviceRolePolicy::Role::CLIENT_HIDDEN;
      meshtastic_ModuleConfig telemetry{};
      if(hidden&&(!completionEdit.ready()||!moduleConfig(telemetry,meshtastic_ModuleConfig_telemetry_tag)))return -EACCES;
      if(!mt::modules::OperatingRole::project(core,modules,&telemetry))return -EINVAL;
      const bool openedHere=hidden&&!settingsEdit.open();
      if(hidden){
        auto supplement=completionEdit.config();
        if(!supplement.set(telemetry))return -EINVAL;
        if(openedHere){const int result=settingsEdit.begin(now);if(result)return result;}
        const int result=completionEdit.stage(supplement);if(result)return result;
      }
      int result=mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,settingsCommit,
        core,disabled,mt::notification::available(),now,&moduleEdit,nullptr,&modules);
      if(!result&&openedHere)result=settingsCommit.commit(now);
      return result;
    }
    if(c.role()==mt::DeviceRolePolicy::Role::ROUTER_LATE&&c.role()!=service.config.role()) {
      if(!moduleEdit.ready())return -EACCES;
      mt::modules::RouterLateRoleDefaults::Projection projected;
      if(mt::modules::RouterLateRoleDefaults::projectRequest(c,moduleEdit.config(),projected)!=mt::modules::RouterLateRoleDefaults::Result::Applied)return -EINVAL;
      meshtastic_ModuleConfig telemetry{};
      if(!projected.modules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag))return -EINVAL;
      return mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,settingsCommit,
        projected.core,disabled,mt::notification::available(),now,&moduleEdit,&telemetry);
    }
#if defined(CONFIG_MT_TAK_TRACKER)
    if(c.role()==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER&&c.role()!=service.config.role()) {
      if(!moduleEdit.ready())return -EACCES;
      mt::modules::TakRoleDefaults::Projection projected;
      if(mt::modules::TakRoleDefaults::projectRequest(c,moduleEdit.config(),projected)!=mt::modules::TakRoleDefaults::Result::Applied)return -EINVAL;
      meshtastic_ModuleConfig telemetry{};
      if(!projected.modules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag))return -EINVAL;
      return mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,settingsCommit,
        projected.core,disabled,mt::notification::available(),now,&moduleEdit,&telemetry);
    }
#endif
    return mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,
      settingsCommit,c,disabled,mt::notification::available(),now);
  }
  int saveDeviceDeprecatedRole(const mt::Config &c,bool disabled,uint32_t now) override {
    if(!ready()||rebootSchedule.pending())return -EACCES;
    bool stagedServer=false;
    bool openedHere=false;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(storeForwardEdit.ready()){
      meshtastic_ModuleConfig store{};store.which_payload_variant=meshtastic_ModuleConfig_store_forward_tag;
      if(!storeForwardEdit.config().get(store.payload_variant.store_forward))return -EACCES;
      if(store.payload_variant.store_forward.enabled&&!store.payload_variant.store_forward.is_server){
        if(!settingsEdit.open()){
          const int begin=settingsEdit.begin(now);
          if(begin)return begin;
          openedHere=true;
        }
        store.payload_variant.store_forward.is_server=true;
        // This setter's module-side effect shares the existing ordered,
        // independently verified SettingsCommit with the core role change.
        const int result=storeForwardEdit.save(store,now,true);
        if(result)return result;
        stagedServer=true;
      }
    }
#endif
    int result;
    if(stagedServer){
      // ready() intentionally closes the Admin surface once a transaction is
      // open, so after creating the private batch call the same bounded
      // DeviceSettingsCommit path directly rather than re-entering that gate.
      result=mt::DeviceSettingsCommit::save(service,settingsEdit,notificationEdit,settingsCommit,
        c,disabled,mt::notification::available(),now);
    }else result=saveDevice(c,disabled,now);
    if(result)return result;
    // DeviceSettingsCommit's unchanged-core fast path does not call the
    // coordinator. If only the Store & Forward alias side effect changed,
    // commit that staged independent journal through the same barrier.
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    if(stagedServer&&openedHere)
      result=applicationAfterCommit(settingsCommit.commit(now),uint64_t(k_uptime_get()));
#endif
    if(!result&&stagedServer&&openedHere){
      scheduleOrdinaryReboot(1,uint64_t(k_uptime_get()));
    }
    return result;
  }
};
class BleHost final:public ClientHost {
public:
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bool stockImportFrameAuthenticated=false;
  bool stockIdentityBleLink()const override{return stockImportFrameAuthenticated&&mt::ble::stockImportLink(sessionToken);}
  int stageStockIdentity(const uint8_t *scalar,const uint8_t *pub,bool isUsb,uint32_t)override{
    if(isUsb||!stockIdentityBleLink())return -EACCES;
    return stockImport.stage(scalar,pub,false,sessionToken);
  }
#endif
  uint8_t keyVerificationOwner()const override{return 1;}
  int restoreExistingIdentity(const uint8_t *,size_t,uint32_t)override{return -ENOTSUP;}
  // Admission only under validated dispatch: claim before releasing its lock,
  // so a disconnect can suppress the result but cannot cancel accepted work.
  int saveBluetoothConfig(const meshtastic_Config_BluetoothConfig &,uint32_t) override {return -EBUSY;}
  int saveBluetoothConfigRequest(const meshtastic_Config_BluetoothConfig &value,uint32_t,
                                uint32_t id,bool wantAck,bool wantResponse) override {
    if(pendingBluetooth.busy()||bluetoothWorkReady||mt::ble::adminBusy())return -EBUSY;
    if(!storageReady||!journal.ready()||!bluetoothJournal.ready())return -EIO;
    if(settingsEdit.open()||rebootSchedule.pending())return -EBUSY;
    mt::ble::PendingBluetoothAdmin::Request request;request.session=sessionToken;
    request.packetId=id;request.wantAck=wantAck;request.wantResponse=wantResponse;
    request.desired.enabled=value.enabled;
    if(unsigned(value.mode)>2)return -EINVAL;
    request.desired.mode=uint8_t(value.mode);request.desired.fixedPin=value.fixed_pin;
    bool displayReady=false;
#if defined(CONFIG_MT_DISPLAY)
    displayReady=mt::ui::ready();
#endif
    if(!pendingBluetooth.stage(request,displayReady))return -EINVAL;
    if(!mt::ble::beginAdmin(sessionToken)){pendingBluetooth.disconnected(sessionToken);return -EBUSY;}
    if(!pendingBluetooth.claim(sessionToken,bluetoothWork)){
      bluetoothConfigReady=false;(void)mt::ble::requestConfiguration(bluetoothJournal.config(),false);return -EIO;
    }
    bluetoothWorkReady=true;return BluetoothDeferred;
  }
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
  int provision(const uint8_t *,size_t,uint32_t,char *,size_t)override{return -ENOTSUP;}
  void provisionDisconnect()override{}
#endif
  // BLE packet lifecycle is bound by bindBleSession(), not CDC DTR.
  bool acceptTransportFrame(uint32_t) override {return true;}
  bool beginUnmeasuredHandshakeBoundary() override {return false;}
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  // Diagnostics are exposed only on the local USB companion transport.
  size_t bluetoothDiagnostic(char *,size_t) const override {return 0;}
#endif
  size_t gnssDiagnostic(char *,size_t) const override {return 0;}
  uint32_t sessionToken=0;
  bool completingBluetooth=false;
  bool sendIfQueueEmpty(const uint8_t *p,size_t n)override{return !completingBluetooth && mt::ble::send(p,n,sessionToken,true);}
  bool send(const uint8_t *p,size_t n)override{return completingBluetooth?
    mt::ble::stageAdminResult(p,n,sessionToken):mt::ble::send(p,n,sessionToken);}
};
static BleHost bleHost;
static mt::Companion bleCompanion MT_WIFI_PSRAM_BSS(ble_companion)(service,bleHost,mt::board::Model,companionWorkspace,1);
static ClientHost clientHost;
static uint8_t publishModuleData(const uint8_t *data,size_t size,uint32_t id,unsigned channel=0,bool idleOnly=false,uint8_t recipients=mt::ModuleController::AllPhones);
#if defined(CONFIG_MT_DISPLAY)||defined(CONFIG_MT_SERIAL_MODULE)
static void publishLocalInput(unsigned port,const uint8_t *payload,size_t size,uint32_t id,unsigned channel);
#endif
#if defined(CONFIG_MT_DISPLAY)
#if defined(CONFIG_MT_BASEUI_GAMES)
static bool gameScoreStoreReady=false;
static void loadGameScores(){
  auto &games=mt::ui::localGames();
  const int n=settings.read("mtgames/scores1",reinterpret_cast<uint8_t *>(games.scores),sizeof(games.scores));
  bool valid=n==int(sizeof(games.scores));
  if(valid)for(const auto &table:games.scores){
    uint32_t last=UINT32_MAX;
    for(const auto &entry:table.entries){
      if(entry.initials[3]||entry.value>last)valid=false;
      last=entry.value;
    }
  }
  if(!valid)for(auto &table:games.scores)table={};
  gameScoreStoreReady=valid||n==-ENOENT;
}
static void flushGameScores(uint32_t now){
  auto &games=mt::ui::localGames();
  static bool delaying=false;static uint32_t lastAttempt=0;
  if(!gameScoreStoreReady||!games.dirty||!clientHost.ready()||settingsEdit.open()||
     rebootSchedule.pending()||(delaying&&uint32_t(now-lastAttempt)<1000))return;
  const int rc=mt::ui::UiConfigCommit::writePaused(service,radio,now,[&]{
    return settings.write("mtgames/scores1",reinterpret_cast<const uint8_t *>(games.scores),sizeof(games.scores));
  });
  if(!rc)games.dirty=false;
  lastAttempt=now;delaying=rc!=0;
}
#endif
#endif // MT_DISPLAY
static int runLocalSystemCommand(mt::stock::ui::SystemCommandsModule::Action action,uint32_t now) {
  using Action=mt::stock::ui::SystemCommandsModule::Action;
  if(!clientHost.ready()||settingsEdit.open()||rebootSchedule.pending()||firmwareUpdate.pending())return -EBUSY;
  switch(action){
  case Action::Bluetooth:{
    meshtastic_Config_BluetoothConfig cfg{};
    if(!clientHost.bluetoothConfig(cfg))return -ENOTSUP;
    cfg.enabled=!cfg.enabled;
    return clientHost.saveBluetoothConfig(cfg,now);
  }
  case Action::Gps:case Action::Privacy:{
    if(!mt::gnss::present())return -ENOTSUP;
    auto candidate=service.config;auto cfg=candidate.gnssSettings();
    cfg.enabled=!cfg.enabled;
    if(!candidate.setGnssSettings(cfg))return -ENOTSUP;
    const int rc=clientHost.save(candidate);
    if(!rc&&action==Action::Privacy)mt::stock::ui::buzzer::privacy(!cfg.enabled);
    return rc;
  }
  case Action::Mute:{
    if(!notificationEdit.ready()||!notificationEdit.config().enabled())return -ENOTSUP;
    notifications.stop();completionRuntime.muteNotices(!completionRuntime.noticeMuted());return 0;
  }
  case Action::Ping:{
    uint8_t channel=0;
    const bool available=mt::stock::findPositionChannel(mt::PositionChannels(service.config),channel);
    mt::stock::ui::buzzer::input(0xaf,now);
    return service.localMeshPing(available,channel,now,sys_rand32_get());
  }
  case Action::Reboot:return clientHost.scheduleReboot(7)?0:-EBUSY;
  case Action::Shutdown:return clientHost.scheduleShutdown(1)?0:-ENOTSUP;
#if MT_HAS_DEVICE_RESET
  case Action::FactoryReset:return clientHost.requestFactoryDeviceReset();
#endif
  default:return -ENOTSUP;
  }
}
#if !defined(CONFIG_MT_DISPLAY) && DT_HAS_ALIAS(sw0)
static const gpio_dt_spec localKey=GPIO_DT_SPEC_GET(DT_ALIAS(sw0),gpios);
static mt::stock::ui::HeadlessButton headlessButton;
static bool localKeyReady=false;
static void pollHeadlessButton(uint64_t now){
  if(!localKeyReady)return;
  const int down=gpio_pin_get_dt(&localKey);if(down<0)return;
  const auto action=headlessButton.update(down!=0,now,notifyPowerActivity);
  if(action==mt::stock::ui::SystemCommandsModule::Action::None)return;
  if(notifications.armed()){notifications.stop();return;}
  if(completionRuntime.requestNoticeDismissal())return;
  if(action==mt::stock::ui::SystemCommandsModule::Action::UserPress){
    mt::stock::ui::buzzer::input(28,uint32_t(now));return;
  }
  (void)runLocalSystemCommand(action,uint32_t(now));
}
#endif
#if defined(CONFIG_MT_DISPLAY)
static void pollCannedInput(uint32_t now) {
#if defined(CONFIG_MT_BASEUI_GAMES)
  auto local=service.config.node();
  memcpy(mt::ui::localGames().ownerShortName,local.bytes+bench::MtNodeRecord::Short,4);
  mt::ui::localGames().epoch=wallClock.at(uint64_t(k_uptime_get()));
  flushGameScores(now);
#if defined(CONFIG_MT_GAMES_ANNOUNCE_HIGH_SCORE)
  auto &game=mt::ui::localGames();
  if(game.announcement[0]){
    const auto *text=reinterpret_cast<const uint8_t *>(game.announcement);
    const size_t n=strlen(game.announcement);uint32_t id=0;
    if(clientHost.ready()&&!settingsEdit.open()&&!rebootSchedule.pending()&&
       !service.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,text,n,0,false,now,sys_rand32_get(),id))
      publishLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,text,n,id,0);
    game.announcement[0]=0;
  }
#endif
#endif
  refreshCannedInput();
  mt::Endpoint::Result completed;
  while(service.takeLocalInputResult(completed)) {
    if(completed.id!=cannedPacketId)continue;
    cannedPacketId=0;
    uiController.reportCannedCompletion(completed.kind==mt::Endpoint::Kind::Relay,
      completed.kind!=mt::Endpoint::Kind::Failed&&completed.code==0);
  }
  while(service.takeLocalUiResult(completed)) {
    if(completed.id!=cannedPacketId)continue;
    cannedPacketId=0;
    const bool authenticated=completed.kind==mt::Endpoint::Kind::Authenticated||
      completed.kind==mt::Endpoint::Kind::PrivateResponse;
    uiController.reportCannedCompletion(false,authenticated&&completed.code==0);
  }
  mt::ui::PendingCannedSend request;
  if(!uiController.takePendingCannedSend(request))return;
  if(cannedPacketId){uiController.reportCannedResult(-EBUSY);return;}
  int result=-EACCES;
  if(clientHost.ready()&&cannedInputAvailable()&&extendedEdit.trusted()&&
     extendedEdit.config().cannedEnabled()&&!settingsEdit.open()&&!rebootSchedule.pending()) {
    mt::ui::CannedMenu menu;
    menu.bind(cannedJournal.config().messages,cannedJournal.config().length());
    const char *text=nullptr;
    size_t size=0;
    bool selected=request.index<menu.count()&&menu.entry(request.index,text,size);
#if defined(CONFIG_MT_ONSCREEN_KEYBOARD)
    if(request.index==UINT8_MAX){text=request.draft;size=strnlen(text,sizeof(request.draft));selected=true;}
#endif
    if(selected&&size&&size<=200) {
      uint8_t payload[201];
      size = mt::stock::canned::copyTextPayload(payload, sizeof(payload), text, size,
                                               extendedEdit.config().cannedBell());
      uint32_t id=0;
      const bool broadcast=request.destination==UINT32_MAX;
      result=mt::stock::canned::sendText(request.destination,request.channel,payload,size,
        [&](const uint8_t *body,size_t length,unsigned channel,bool wantAck){
          return service.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,body,length,channel,wantAck,now,sys_rand32_get(),id);
        },[&](uint32_t to,const uint8_t *body,size_t length,unsigned channel,bool wantAck){
          return service.submitLocalPrivateInput(meshtastic_PortNum_TEXT_MESSAGE_APP,to,body,length,channel,wantAck,now,sys_rand32_get(),id);
        });
      if(!result) {
        cannedPacketId=id;
        if(broadcast) {
          publishLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,payload,size,id,request.channel);
          clientHost.peripheralPacket(service.route.identity(),UINT32_MAX,id,
            meshtastic_PortNum_TEXT_MESSAGE_APP,request.channel,payload,size,true);
        }
        clientHost.preview(service.route.identity(),request.destination,id,
          meshtastic_PortNum_TEXT_MESSAGE_APP,payload,size,0,0,now,true);
      }
      bench::wipeKeyBuffer(payload,sizeof(payload));
    } else result=-EINVAL;
  }
  uiController.reportCannedResult(result);
}
#endif
#if defined(CONFIG_MT_SERIAL_MODULE)
static bool sendSerialInput(void *,mt_serial::Port port,const uint8_t *payload,size_t size,
                            int channel,bool wantAck) {
  if(!clientHost.ready()||!extendedEdit.trusted()||settingsEdit.open()||
     rebootSchedule.pending()||!serialModule.ready())return false;
  if(channel==mt_serial::kChannelNamedSerial) {
    channel=namedSerialChannel();
    // Stock Channels::getByName falls back to primary when no name matches.
    if(channel<0)channel=0;
  }
  const unsigned portNumber=port==mt_serial::Port::TextMessageApp?
    meshtastic_PortNum_TEXT_MESSAGE_APP:meshtastic_PortNum_SERIAL_APP;
  const uint32_t now=k_uptime_get_32();
  uint32_t id=0;
  const int result=service.submitLocalInput(portNumber,payload,size,unsigned(channel),wantAck,
    now,sys_rand32_get(),id);
  if(result)return false;
  publishLocalInput(portNumber,payload,size,id,unsigned(channel));
  clientHost.peripheralPacket(service.route.identity(),UINT32_MAX,id,portNumber,
    unsigned(channel),payload,size,true);
  clientHost.preview(service.route.identity(),UINT32_MAX,id,portNumber,payload,size,0,0,now,true);
  return true;
}
#endif
static bool trafficNodeInfoFrame(void*,const uint8_t *frame,size_t length,const bench::RouteMetadata &m,unsigned channel,uint32_t now,uint32_t random){
  if(!frame||length<17||!clientHost.ready()||!extendedEdit.trusted()||settingsEdit.open()||rebootSchedule.pending())return false;
  // Only the serialized bytes are needed for this cache. Constant initialization
  // avoids a lazy C++ guard on the minimal target runtime.
  static uint8_t previous[mt::modules::TrafficManagementSettings::Size]{};
  static bool configured=false;
  const auto current=extendedEdit.config().traffic();
  if(!configured||memcmp(previous,current.bytes,sizeof(previous))){
    (void)trafficNodeInfo.configure(current);
    memcpy(previous,current.bytes,sizeof(previous));configured=true;
  }
  const uint32_t from=bench::le32(frame+4),to=bench::le32(frame),id=bench::le32(frame+8);
  const unsigned hop=frame[12]&7,start=(frame[12]>>5)&7;
  if(hop>start||!from||from==service.route.identity()||service.nodes.ignored(from))return false;
  meshtastic_Data canonical{};canonical.portnum=meshtastic_PortNum_NODEINFO_APP;canonical.want_response=m.wantResponse;
  canonical.request_id=m.request_id;canonical.reply_id=m.reply_id;canonical.emoji=m.emoji;canonical.has_bitfield=m.hasBitfield;canonical.bitfield=m.bitfield;
  canonical.payload.size=m.payloadLength;memcpy(canonical.payload.bytes,m.payload,m.payloadLength);
  canonical.xeddsa_signature.size=m.signatureLength;if(m.signatureLength)memcpy(canonical.xeddsa_signature.bytes,m.signature,m.signatureLength);
  uint8_t encoded[239]{};auto output=pb_ostream_from_buffer(encoded,sizeof(encoded));
  if(!pb_encode(&output,meshtastic_Data_fields,&canonical)||!service.codec.validatePublicNodeInfoData(frame,length,encoded,output.bytes_written))return false;
  (void)trafficNodeInfo.observePublicUser(from,m.payload,m.payloadLength,uint64_t(k_uptime_get()),uint8_t(channel),frame[13],true);
  if(!m.wantResponse||m.request_id||m.reply_id||m.emoji||m.signatureLength||to==UINT32_MAX||to==service.route.identity()||to<=1)return false;
  mt::modules::TrafficNodeInfoRequest request{};request.requester=from;request.target=to;request.requestId=id;
  request.hopsAway=start-hop;request.nowMs=uint64_t(k_uptime_get());request.channelHash=frame[13];request.channelIndex=uint8_t(channel);
  request.publicRequest=true;request.requestShapeValid=m.nodeInfoValid&&m.hasBitfield&&!(m.bitfield&~3u);
  request.replayAdmitted=true; // responder's separate bounded request-ID history owns replay admission here
  request.requesterIgnored=service.nodes.ignored(from);request.requesterMuted=service.nodes.muted(from);
  request.targetIgnored=service.nodes.ignored(to);request.targetMuted=service.nodes.muted(to);request.channelMuted=service.channelMuted();
  request.routerRole=service.config.role()==2;
  struct Context{uint32_t now,random;} context{now,random};
  mt::modules::TrafficNodeInfoOwner owner{};owner.context=&context;
  owner.proxyRouteAvailable=[](void*){return service.publicProxyAvailable();};
  owner.allocatePublicPacketId=[](void *context,uint32_t &id){id=service.publicProxyPacketId(static_cast<Context*>(context)->random);return id!=0;};
  owner.submitPublicProxy=[](void *context,const mt::modules::TrafficNodeInfoReply &reply){return service.submitPublicNodeInfoProxy(reply,static_cast<Context*>(context)->now)==0;};
  return trafficNodeInfo.respond(request,service.route.identity(),owner)==mt::modules::TrafficNodeInfoResponder::Result::Replied;
}
static mt::Companion companion MT_WIFI_PSRAM_BSS(usb_companion)(service,clientHost,mt::board::Model,companionWorkspace);
#if defined(CONFIG_MT_SERIAL_MODULE)
class SerialHost final:public ClientHost {
  struct Frame{uint16_t size=0;uint8_t bytes[mt_serial::kMaxPhoneApiPayload]{};};
  Frame frames_[2]{};unsigned head_=0,count_=0;
public:
  uint8_t keyVerificationOwner()const override{return 3;}
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
  int scheduleFirmwareUpdate(const meshtastic_AdminMessage_OTAEvent &,uint32_t)override{return -EACCES;}
#endif
  int restoreExistingIdentity(const uint8_t *,size_t,uint32_t)override{return -ENOTSUP;}
  bool sendIfQueueEmpty(const uint8_t *data,size_t size)override{return !count_ && send(data,size);}
  bool send(const uint8_t *data,size_t size)override{
    if(!serialModule.ready()||!data||!size||size>mt_serial::kMaxPhoneApiPayload||count_==2)return false;
    auto &out=frames_[(head_+count_)%2];out.size=uint16_t(size);memcpy(out.bytes,data,size);++count_;return true;
  }
  size_t take(uint8_t *out,size_t capacity){
    if(!count_||capacity<frames_[head_].size)return 0;const auto size=frames_[head_].size;
    memcpy(out,frames_[head_].bytes,size);head_=(head_+1)%2;--count_;return size;
  }
  void clear(){head_=count_=0;}
};
static SerialHost serialHost;
static mt::Companion serialCompanion MT_WIFI_PSRAM_BSS(serial_companion)(service,serialHost,mt::board::Model,companionWorkspace,3);
static void pollSerialProtoUnlocked(){
  static bool active=false;
  k_mutex_lock(&serviceLock,K_FOREVER);meshtastic_ModuleConfig_SerialConfig config{};
  const bool ready=serialModule.ready()&&completionEdit.trusted()&&serialConfig(config)&&
    mt::detail::enumNumber(config.mode)==meshtastic_ModuleConfig_SerialConfig_Serial_Mode_PROTO;
  if(ready!=active){keyVerification.disconnect(3,k_uptime_get_32());serialCompanion.resetStream();serialHost.clear();active=ready;}
  k_mutex_unlock(&serviceLock);if(!ready)return;
  // UART TX/RX polling occurs without Service ownership. The parser callbacks
  // take the same owner lock and shared decode-workspace proof as USB/BLE/TCP.
  (void)serialModule.pollProto([](void*,const uint8_t *data,size_t size){
    k_mutex_lock(&serviceLock,K_FOREVER);serialCompanion.packet(data,size,k_uptime_get_32(),companionWorkspaceProof());k_mutex_unlock(&serviceLock);
  },[](void*,uint8_t *out,size_t capacity)->size_t{
    k_mutex_lock(&serviceLock,K_FOREVER);const size_t size=serialHost.take(out,capacity);k_mutex_unlock(&serviceLock);return size;
  },nullptr);
}
static mt_serial::Position serialPosition(const meshtastic_Position &p,uint32_t utc){
  mt_serial::Position out{};out.valid=p.has_latitude_i&&p.has_longitude_i;out.latitude_i=p.latitude_i;out.longitude_i=p.longitude_i;
  out.altitude_m=p.altitude;out.geoidal_separation_m=p.altitude_geoidal_separation;out.hdop=p.HDOP;
  out.fix_quality=p.fix_quality;out.satellites=p.sats_in_view;
  if(!utc)utc=p.timestamp;out.timestamp_millis_adjust=p.timestamp_millis_adjust;
  out.utc_valid=utc!=0;const uint32_t day=utc%86400;out.utc_hour=day/3600;out.utc_minute=(day%3600)/60;out.utc_second=day%60;
  return out;
}
static bool serialWeather(void*,const mt_serial::WeatherReport &weather){
  if(!clientHost.ready()||!completionEdit.trusted()||settingsEdit.open()||rebootSchedule.pending())return false;
  meshtastic_Telemetry value{};mt::stock::serial::weatherTelemetry(weather,value);
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TELEMETRY_APP;data.has_bitfield=true;
  auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));if(!pb_encode(&payload,meshtastic_Telemetry_fields,&value))return false;
  data.payload.size=payload.bytes_written;uint8_t encoded[239]{};auto output=pb_ostream_from_buffer(encoded,sizeof(encoded));
  if(!pb_encode(&output,meshtastic_Data_fields,&data))return false;uint32_t id=0;
  const int result=service.submitModuleAnnouncement(encoded,output.bytes_written,service.config.hopLimit(),k_uptime_get_32(),sys_rand32_get(),id,false,true);
  if(result||!id)return false;publishModuleData(encoded,output.bytes_written,id);return true;
}
static void pollSerialPosition(uint32_t now){
  meshtastic_ModuleConfig_SerialConfig config{};if(!serialModule.ready()||!serialConfig(config))return;
  const auto mode=mt::detail::enumNumber(config.mode);
  if(mode==meshtastic_ModuleConfig_SerialConfig_Serial_Mode_WS85){(void)serialModule.pollWs85(now,serialWeather,nullptr);return;}
  if(mode!=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_NMEA&&mode!=meshtastic_ModuleConfig_SerialConfig_Serial_Mode_CALTOPO)return;
  meshtastic_Position position{};(void)service.localPosition(position);const auto local=serialPosition(position,wallClock.at(uint64_t(k_uptime_get())));
  if(mode==meshtastic_ModuleConfig_SerialConfig_Serial_Mode_NMEA){(void)serialModule.pollNmea(now,local);return;}
  mt_serial::Waypoint waypoints[17]{};size_t count=0;
  if(local.valid){waypoints[count].position=local;strncpy(waypoints[count].source_name,"local",sizeof(waypoints[count].source_name)-1);++count;}
  for(unsigned index=0;index<mt::NodeCache::Capacity&&count<17;++index){
    const auto *entry=service.nodes.get(index);if(!entry||entry->placeholder()||!entry->position.present()||entry->id()==service.route.identity())continue;
    meshtastic_Position position{};entry->position.exportTo(position);waypoints[count].position=serialPosition(position,position.time);
    strncpy(waypoints[count].source_name,reinterpret_cast<const char*>(entry->bytes+mt::NodeCache::Entry::Short),sizeof(waypoints[count].source_name)-1);++count;
  }
  (void)serialModule.pollCalTopo(now,waypoints,count);
}
#endif

#if defined(CONFIG_MT_RF_ADMIN_GET)
static bool privateAdminSnapshot(void *context,uint32_t local,uint32_t from,
                                const uint8_t key[32],uint32_t request,
                                const uint8_t *data,size_t length,
                                uint8_t *out,size_t capacity,size_t &size) {
  auto *owner=static_cast<mt::Companion *>(context);
  if(!owner){size=0;return false;}
  return owner->privateAdminReplyData(local,from,key,request,data,length,out,capacity,size,companionWorkspaceProof());
}
static bool privateAdminMutation(void *context,uint32_t local,uint32_t from,
                                 const uint8_t key[32],uint32_t request,
                                 const uint8_t *data,size_t length,uint32_t now,
                                 uint32_t &routingError) {
  if(!preferenceLockOwned()){routingError=meshtastic_Routing_Error_BAD_REQUEST;return false;}
  RemoteMutationScope mutationScope(RemoteAdminLane::Private,from,request);
  auto *owner=static_cast<mt::Companion *>(context);
  return owner&&owner->privateAdminMutation(local,from,key,request,data,length,now,routingError,companionWorkspaceProof());
}
static bool legacyAdminRequest(void *context,const mt::admin::LegacyAdminRequest &request,
    uint32_t now,uint8_t *out,size_t capacity,size_t &size) {
  size=0;
  if(!preferenceLockOwned())return false;
  RemoteMutationScope mutationScope(RemoteAdminLane::Shared,request.from(),request.packetId());
  auto *owner=static_cast<mt::Companion *>(context);
  return owner&&owner->legacyAdminRequest(request,now,out,capacity,size,companionWorkspaceProof());
}
static void remoteAdminResponseQueued(void *,bool privateMessage,uint32_t peer,
    uint32_t request,uint32_t wireId) {
  const auto lane=privateMessage?RemoteAdminLane::Private:RemoteAdminLane::Shared;
  if(preferenceLockOwned()&&pendingRemoteConfig.pending()&&pendingRemoteOwner.lane==lane&&
     pendingRemoteOwner.peer==peer&&pendingRemoteOwner.request==request)
    (void)pendingRemoteConfig.actualResponseQueued(wireId,k_uptime_get_32());
}
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
class TcpHost final:public ClientHost {
public:
  uint8_t keyVerificationOwner()const override{return 2;}
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
  int scheduleFirmwareUpdate(const meshtastic_AdminMessage_OTAEvent &,uint32_t)override{return -EACCES;}
#endif
  int restoreExistingIdentity(const uint8_t *,size_t,uint32_t)override{return -ENOTSUP;}
  uint32_t sessionToken=0;
  bool sendIfQueueEmpty(const uint8_t *bytes,size_t size)override{return mt::tcp::send(bytes,size,sessionToken,true);}
  bool send(const uint8_t *bytes,size_t size)override{return mt::tcp::send(bytes,size,sessionToken);}
  bool acceptTransportFrame(uint32_t)override{return true;}
  bool beginUnmeasuredHandshakeBoundary()override{return false;}
  int provision(const uint8_t*,size_t,uint32_t,char*,size_t)override{return -ENOTSUP;}
  void provisionDisconnect()override{}
  size_t gnssDiagnostic(char*,size_t)const override{return 0;}
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
  size_t bluetoothDiagnostic(char*,size_t)const override{return 0;}
#endif
};
static TcpHost tcpHost;
static mt::Companion tcpCompanion MT_WIFI_PSRAM_BSS(tcp_companion)(service,tcpHost,mt::board::Model,companionWorkspace,2);
static void bindTcpSession(uint32_t token){
  if(tcpHost.sessionToken!=token){keyVerification.disconnect(2,k_uptime_get_32());tcpCompanion.resetStream();tcpHost.sessionToken=token;}
}
static void handleTcpPacket(const mt::tcp::Packet &packet,void*){
  bindTcpSession(packet.session);tcpCompanion.packet(packet.bytes,packet.length,k_uptime_get_32(),companionWorkspaceProof());
}
#endif
#undef MT_WIFI_PSRAM_BSS
static uint8_t publishModuleData(const uint8_t *data,size_t size,uint32_t id,unsigned channel,bool idleOnly,uint8_t recipients){
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
  bench::RouteMetadata metadata{};
  if(bench::MtCodec::decodeData(data,size,metadata)&&metadata.portnum==meshtastic_PortNum_TELEMETRY_APP&&
     !metadata.wantResponse&&!metadata.request_id){
    meshtastic_Telemetry report{};
    if(mt::RemoteTelemetryQuery::decodeCanonical(metadata.payload,metadata.payloadLength,0,report))
      observeDisplayReport(service.route.identity(),id,true,report);
  }
#endif
  uint8_t accepted=0;
  auto deliver=[&](uint8_t bit,mt::Companion &client){
    if((recipients & bit)&&client.modulePacket(data,size,id,channel,companionWorkspaceProof(),idleOnly))
      accepted|=bit;
  };
  deliver(1,companion);
  deliver(2,bleCompanion);
#if defined(CONFIG_MT_SERIAL_MODULE)
  deliver(4,serialCompanion);
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
  deliver(8,tcpCompanion);
#endif
  return accepted;
}
#if defined(CONFIG_MT_RF_ADMIN_GET)
static void clearRemoteAdminSessions() {
  companion.clearPrivateAdminSessions();bleCompanion.clearPrivateAdminSessions();
#if defined(CONFIG_MT_SERIAL_MODULE)
  serialCompanion.clearPrivateAdminSessions();
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
  tcpCompanion.clearPrivateAdminSessions();
#endif
}
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
static void gpioLocalPacket(void *,const uint8_t *data,size_t size,uint32_t id,unsigned channel) {
  publishModuleData(data,size,id,channel);
}
#endif
#if defined(CONFIG_MT_DISPLAY)||defined(CONFIG_MT_SERIAL_MODULE)
static void publishLocalInput(unsigned port,const uint8_t *payload,size_t size,uint32_t id,unsigned channel) {
  uint8_t data[239]{};
  size_t encoded=0;
  if(!mt::Service::encodeLocalInputData(port,payload,size,data,sizeof(data),encoded))return;
  publishModuleData(data,encoded,id,channel);
}
#endif
#if !defined(CONFIG_MT_M119_TLS_TEST)
static void mqttLocalRx(void *,const uint8_t *frame,size_t size,uint32_t now) {
  const uint32_t receivedAt=wallClock.at(uint64_t(k_uptime_get()));
  // Shared-channel receive applies each phone owner’s filters and duplicate
  // check. PKI requires an immutable authenticated event for that owner.
  companion.receive(frame,size,0,0,now,receivedAt,false,0,false,companionWorkspaceProof());
  bleCompanion.receive(frame,size,0,0,now,receivedAt,false,0,false,companionWorkspaceProof());
#if defined(CONFIG_MT_SERIAL_MODULE)
  serialCompanion.receive(frame,size,0,0,now,receivedAt,false,0,false,companionWorkspaceProof());
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
  tcpCompanion.receive(frame,size,0,0,now,receivedAt,false,0,false,companionWorkspaceProof());
#endif
}
#endif
struct UsbBatch {uint32_t length=0,generation=0;};
static UsbBatch takeUsbBytes(uint8_t *bytes,size_t capacity) {
  // DTR sampling, transition handling and extraction exclude the producer.
  // The ISR independently observes the same epoch before admitting each RX
  // chunk, so low-state bytes are drained but never enter usbRx.
  unsigned key=irq_lock();const auto first=observeUsbLineLocked(usb);
  pollUsbTxLocked(usb);
  bool reset=atomic_set(&usbEpochChanged,0)!=0;
  if(atomic_set(&usbDrops,0)){wipeUsbRxLocked();reset=true;}
  uint32_t length=0;
  if(first.measured&&!first.connected)wipeUsbRxLocked();
  else while(length<capacity){
    uint8_t *claimed=nullptr;
    const uint32_t count=ring_buf_get_claim(&usbRx,&claimed,uint32_t(capacity-length));
    if(!count)break;
    memcpy(bytes+length,claimed,count);bench::wipeKeyBuffer(claimed,count);
    if(ring_buf_get_finish(&usbRx,count)!=0){bench::wipeKeyBuffer(bytes+length,count);break;}
    length+=count;
  }
  irq_unlock(key);
  if(reset)companion.resetStream();
  if(!length)return {0,first.generation};
  // A second producer-excluding sample rejects an extracted old-generation
  // frame if DTR fell while the main thread was leaving the first section.
  key=irq_lock();const auto current=observeUsbLineLocked(usb);
  const bool valid=(!current.measured||current.connected)&&current.generation==first.generation;
  reset=atomic_set(&usbEpochChanged,0)!=0;irq_unlock(key);
  if(reset)companion.resetStream();
  if(!valid)bench::wipeKeyBuffer(bytes,length);
  return {valid?length:0,first.generation};
}
static int commitVerifiedPeer(void *,uint32_t peer,const uint8_t key[32]) {
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  if(!key||peer<=1||peer>=0xfffffffeu||peer==service.route.identity()||
     !clientHost.ready()||settingsEdit.open()||rebootSchedule.pending()||service.nodes.ignored(peer))return -EACCES;
  // Confirmation is not key replacement authority. Recheck the durable pin
  // under the same owner immediately before entering the existing two-store
  // contact operation; RF discovery cannot race this serialized callback.
  uint8_t pinned[32]{};mt::detail::VolatileWipe wipePinned{pinned,sizeof(pinned)};
  if(privateStorage.storage().lookup(peer,pinned)&&memcmp(pinned,key,32))return -EEXIST;
  mt::OwnerContactCandidate candidate{};mt::detail::VolatileWipe wipeCandidate{&candidate,sizeof(candidate)};
  const int slot=service.nodes.find(peer);
  const auto *existing=slot<0?nullptr:service.nodes.get(unsigned(slot));
  if(existing&&!existing->placeholder())candidate.node=*existing;
  else candidate.node.identity(peer,0,0);
  candidate.node.bytes[mt::NodeCache::Entry::LocalFlags]&=uint8_t(~mt::NodeCache::Entry::Placeholder);
  candidate.node.bytes[mt::NodeCache::Entry::KeyLength]=32;
  memcpy(candidate.node.bytes+mt::NodeCache::Entry::Key,key,32);
  candidate.peer.node=peer;memcpy(candidate.peer.publicKey,key,32);
  candidate.hasPublicKey=true;candidate.pkiKeyAdmissible=true;candidate.hasPeerPin=true;
  candidate.manualVerificationRequested=true;candidate.shouldIgnore=false;
  // This verifies the admitted key again, commits the durable pin then the
  // manually-verified NodeJournal metadata, adopts it into the existing live
  // runtime and preserves partial-commit/RX-restore error distinctions.
  return clientHost.addOwnerContact(candidate,k_uptime_get_32());
#else
  (void)peer;(void)key;return -ENOTSUP;
#endif
}
static void pollKeyVerification(uint32_t now) {
  // One device-wide protocol owner consumes each admitted RF event once.
  // Companion receive fanout below cannot create additional sessions.
  mt::pki::KeyVerificationReceive received{};
  if(service.takeKeyVerificationReceive(received)){
    (void)keyVerification.receive(received,now);bench::wipeKeyBuffer(&received,sizeof(received));
  }
  keyVerification.poll(now);
  meshtastic_ClientNotification notification{};
  while(keyVerification.takeNotification(notification)||service.takeRefusedIdentityNotice(notification)){
    (void)companion.notifyKeyVerification(notification,companionWorkspaceProof());
    (void)bleCompanion.notifyKeyVerification(notification,companionWorkspaceProof());
#if defined(CONFIG_MT_SERIAL_MODULE)
    (void)serialCompanion.notifyKeyVerification(notification,companionWorkspaceProof());
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
    (void)tcpCompanion.notifyKeyVerification(notification,companionWorkspaceProof());
#endif
    bench::wipeKeyBuffer(&notification,sizeof(notification));
  }
}
static void forgetCompanionPeer(uint32_t id,void *) {
  waypoints.forgetPeer(id);
#if defined(CONFIG_MT_DISPLAY)
  mt::ui::clearWaypoint();
#endif
  // NodeAdmin calls only after verified removal, with serviceLock held/RF off.
  if(hotNodeCountAvailable&&hotNodeWindow.remove(id))markHotNodeDirty(k_uptime_get_32());
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(remoteHardwarePinJournalReady&&remoteHardwarePinJournal.ready()) {
    bool found=false;
    for(size_t i=0;i<remoteHardwarePinCache.size();++i)
      if(remoteHardwarePinCache.data()[i].node_num==id)found=true;
    if(found&&remoteHardwarePinCache.removePeer(id))remoteHardwarePinDirty=true;
  }
#endif
  trafficWindow.forget(id);
  trafficNodeInfo.forget(id);
  companion.forgetPeer(id);bleCompanion.forgetPeer(id);
#if defined(CONFIG_MT_SERIAL_MODULE)
  serialCompanion.forgetPeer(id);
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
  tcpCompanion.forgetPeer(id);
#endif
}
static void bindBleSession(uint32_t token) {
  // Called only with serviceLock held. Tokens, rather than a delayed disconnect
  // flag, prevent an old lifecycle event from cancelling a new connection.
  if(bleHost.sessionToken!=token){keyVerification.disconnect(1,k_uptime_get_32());pendingBluetooth.disconnected(bleHost.sessionToken);bleCompanion.resetStream();bleHost.sessionToken=token;}
}
static void handleBlePacket(const mt::ble::Packet &p,void *) {
  bindBleSession(p.session);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bleHost.stockImportFrameAuthenticated=p.stockImportAuthenticated;
#endif
  bleCompanion.packet(p.bytes,p.length,k_uptime_get_32(),companionWorkspaceProof());
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bleHost.stockImportFrameAuthenticated=false;
#endif
}
static void finishBluetoothAdmin() {
  // Main owns serviceLock here, after dispatch released the session lock.
  if(!bluetoothWorkReady)return;
  meshtastic_Config_BluetoothConfig value{};value.enabled=bluetoothWork.desired.enabled;
  value.mode=static_cast<meshtastic_Config_BluetoothConfig_PairingMode>(bluetoothWork.desired.mode);
  value.fixed_pin=bluetoothWork.desired.fixedPin;
  const int result=bleHost.persistBluetoothConfig(value,k_uptime_get_32());
  bindBleSession(mt::ble::session());
  mt::ble::PendingBluetoothAdmin::Completion completion;
  const bool finished=pendingBluetooth.finish(bleHost.sessionToken,result,completion);
  if(finished&&completion.replyAllowed){
    bleHost.completingBluetooth=true;
    bleCompanion.completeBluetoothConfig(completion.packetId,result,companionWorkspaceProof());
    bleHost.completingBluetooth=false;
  }
  if(!finished||!mt::ble::finishAdminConfiguration(bluetoothJournal.config(),bluetoothConfigReady,
                                                 completion.session,completion.replyAllowed)){
    bluetoothConfigReady=false;(void)mt::ble::requestConfiguration(bluetoothJournal.config(),false);
  }
  // Clear the claimed copy as well as the helper's retained sensitive fields.
  bluetoothWork={};bluetoothWorkReady=false;value={};
}
static void flushPeersBeforeReset() {
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
  flushBroadcastsBeforeReset();
#endif
  const uint32_t now=k_uptime_get_32();
  if(storageReady&&journal.ready()&&service.storageQuiescent()&&radio.ready()&&radio.health()==0)
    (void)nodeJournal.flush(settings,service.nodes,now,true,true);
}
#if defined(CONFIG_MT_RF_ADMIN_GET)
static bool restoreRemotePinRadio(bool wasReceiving,bool wasRelaying,bool wasArmed,uint32_t now) {
  if(!wasReceiving)return !wasArmed;
  if(service.faulted()||!service.config.provisioned()||!service.start(wasRelaying,now))return false;
  if(wasArmed&&(!service.normalMode()||!service.txAllowed()||!service.config.txEnabled()))return false;
  return service.armEndpoint(wasArmed)&&!service.faulted();
}
// Only the main owner calls this with serviceLock held, after transport work
// was drained. A stop race returns busy and preserves dirty metadata for retry.
static int flushRemoteHardwarePins(uint32_t now,bool clientIngressSaturated) {
  if(!remoteHardwarePinDirty)return 0;
  if(!initialized||!storageReady||!remoteHardwarePinJournalReady||
     !remoteHardwarePinJournal.ready()||settingsEdit.open()||rebootSchedule.pending()||
     firmwareUpdate.pending()||rebootSchedule.action()!=ScheduledOwnerAction::None||
     atomic_get(&shutdownAdmissionClosed)||preferenceRecoveryHeld||persistentWorkPending()||
     pendingBluetooth.busy()||bluetoothWorkReady||mt::ble::adminBusy()||clientIngressSaturated||
     service.faulted()||!service.storageStopSafe()||radio.busy()||!radio.ready()||
     k_msgq_num_used_get(&rxQueue)!=0||ring_buf_size_get(&usbRx)!=0||
     mt::ble::pendingInput()!=0||service.privateAdminMutationQueued()||service.legacyAdminWorkQueued()||
     service.privateAdminQueriesPending()||nodeJournal.due(now)||hotNodeDirty)
    return -EBUSY;
  const bool wasReceiving=service.receiving(),wasRelaying=service.relaying(),wasArmed=service.endpointArmed();
  const int preHealth=radio.health();
  if(preHealth!=(wasReceiving?1:0))return -EBUSY;
  const int paused=service.pauseForStorage();
  if(paused)return paused==-EBUSY&&!service.faulted()?-EBUSY:-EIO;

  int saved=-EIO;
  const bool safe=service.storageQuiescent()&&!service.faulted()&&radio.ready()&&
    !radio.busy()&&radio.health()==0&&k_msgq_num_used_get(&rxQueue)==0;
  if(safe) {
    mt::admin::RemoteHardwarePinJournal::Snapshot candidate{};
    if(remoteHardwarePinCache.copyTo(candidate.entries,
        sizeof(candidate.entries)/sizeof(candidate.entries[0]),candidate.count))
      saved=remoteHardwarePinJournal.save(settings,candidate);
  }
  if(!remoteHardwarePinJournal.ready())remoteHardwarePinJournalReady=false;
  if(saved==0)remoteHardwarePinDirty=false;

  const bool restored=restoreRemotePinRadio(wasReceiving,wasRelaying,wasArmed,now);
  if(!restored) {
    // Keep the inventory surface unavailable after failed RF restoration;
    // never turn this local metadata failure into identity/key recovery.
    service.stop();remoteHardwarePinJournalReady=false;
    return -EIO;
  }
  return saved;
}
#endif
static bool poweroffWasReceiving=false,poweroffWasRelaying=false,poweroffWasArmed=false,poweroffOwnerPrepared=false,poweroffPauseAttempted=false;
static bool poweroffClosedAdmission=false;
static bool powerPrepareTimed(void *){
  k_mutex_lock(&serviceLock,K_FOREVER);
  const bool receiving=service.receiving();
  const bool safe=initialized&&!persistentWorkPending()&&!administrationWorkPending()&&!rebootSchedule.pending()&&
      !firmwareUpdate.pending()&&rebootSchedule.action()==ScheduledOwnerAction::None&&
    !service.faulted()&&service.storageStopSafe()&&
    radio.ready()&&!radio.busy()&&radio.health()==(receiving?1:0);
  k_mutex_unlock(&serviceLock);return safe;
}
static bool powerQuiescentTimed(void *){
  k_mutex_lock(&serviceLock,K_FOREVER);
  const bool receiving=service.receiving();
  const bool safe=service.storageStopSafe()&&!service.faulted()&&
      !persistentWorkPending()&&!administrationWorkPending()&&!rebootSchedule.pending()&&!firmwareUpdate.pending()&&
    radio.ready()&&!radio.busy()&&radio.health()==(receiving?1:0)&&k_msgq_num_used_get(&rxQueue)==0;
  k_mutex_unlock(&serviceLock);return safe;
}
static void powerResumeTimed(void *){}
static bool powerPrepareOff(void *){
  k_mutex_lock(&serviceLock,K_FOREVER);
  // Drain already admitted Admin before closing Host admission; the trusted
  // owner callback needs that admission to return its numeric outcome.
  if(administrationWorkPending()){k_mutex_unlock(&serviceLock);return false;}
  // Fence ISR/BT admission before inspecting accepted input. A battery-driven
  // attempt owns this temporary fence; an explicit shutdown already owns it.
  if(!atomic_get(&shutdownAdmissionClosed)){
    atomic_set(&shutdownAdmissionClosed,1);mt::ble::setIngressOpen(false);poweroffClosedAdmission=true;
  }
  const bool explicitRequest=rebootSchedule.action()==ScheduledOwnerAction::Shutdown;
  const bool batteryRequest=completionRuntime.powerState()==mt::power::PowerPolicy::State::BatteryShutdownPending;
  if(!initialized||firmwareUpdate.pending()||persistentWorkPending()||(!explicitRequest&&!batteryRequest)||service.faulted()||
     !service.storageStopSafe()||radio.busy()||(!explicitRequest&&(mt::ble::connected()||clientHost.serialStatus().connected))||
     k_msgq_num_used_get(&rxQueue)!=0||ring_buf_size_get(&usbRx)!=0||mt::ble::pendingInput()!=0) {k_mutex_unlock(&serviceLock);return false;}
  poweroffWasReceiving=service.receiving();poweroffWasRelaying=service.relaying();poweroffWasArmed=service.endpointArmed();
  const int paused=service.pauseForStorage();
  poweroffPauseAttempted=!paused;
  bool safe=!paused&&service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0;
  if(safe&&storageReady&&journal.ready()){
    const int flushed=nodeJournal.flush(settings,service.nodes,k_uptime_get_32(),true,true);
    safe=flushed>=0&&flushed!=-EAGAIN;
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
    if(safe)flushBroadcastsBeforeReset();
#endif
  }
  poweroffOwnerPrepared=safe;
  k_mutex_unlock(&serviceLock);return safe;
}
static bool powerQuiescentOff(void *){
  k_mutex_lock(&serviceLock,K_FOREVER);
  const bool safe=poweroffOwnerPrepared&&service.storageQuiescent()&&!service.faulted()&&
    radio.ready()&&!radio.busy()&&
    (rebootSchedule.action()==ScheduledOwnerAction::Shutdown||(!mt::ble::connected()&&!clientHost.serialStatus().connected))&&
    radio.health()==0&&k_msgq_num_used_get(&rxQueue)==0&&ring_buf_size_get(&usbRx)==0&&mt::ble::pendingInput()==0;
  k_mutex_unlock(&serviceLock);return safe;
}
static void powerResumeOff(void *){
  k_mutex_lock(&serviceLock,K_FOREVER);
  if(poweroffPauseAttempted&&poweroffWasReceiving&&!service.faulted()&&service.config.provisioned()&&
     service.start(poweroffWasRelaying,k_uptime_get_32())){
    if(poweroffWasArmed){const bool enabled=service.normalMode()&&service.txAllowed()&&service.config.txEnabled();
      if(!service.armEndpoint(enabled))service.stop();}
  }
  poweroffOwnerPrepared=false;poweroffPauseAttempted=false;poweroffWasReceiving=false;poweroffWasRelaying=false;poweroffWasArmed=false;
  if(poweroffClosedAdmission){poweroffClosedAdmission=false;mt::ble::setIngressOpen(true);atomic_set(&shutdownAdmissionClosed,0);}
  k_mutex_unlock(&serviceLock);
}
static const mt::power::PowerOwnerHooks powerOwnerHooks{
  nullptr,&powerPrepareTimed,&powerQuiescentTimed,&powerResumeTimed,
  &powerPrepareOff,&powerQuiescentOff,&powerResumeOff};
// Keep the 640-byte candidate and apply scratch out of the frame that calls
// Service.poll(), whose private signing path uses the target's bounded stack.
#if defined(CONFIG_MT_RF_ADMIN_GET)
[[gnu::noinline]] static void applyReadyRemoteConfig(uint32_t now) {
  auto fingerprint=configFingerprint(journal.config());
  mt::detail::VolatileWipe wipeFingerprint{&fingerprint,sizeof(fingerprint)};
  mt::Config candidate;
  mt::detail::VolatileWipe wipeCandidate{&candidate,sizeof(candidate)};
  mt::admin::PendingRemoteConfig::Completion completion{};
  mt::detail::VolatileWipe wipeCompletion{&completion,sizeof(completion)};
  const bool ready=pendingRemoteConfig.takeReadyApply(now,journal.generation(),
    fingerprint,candidate,completion);
  if(!ready) {
    if(pendingRemoteConfig.state()==mt::admin::PendingRemoteConfig::State::NeedsApplyOrRecovery)
      remoteConfigHeld=true;
    return;
  }
  if(service.active())return;
  // Native success proves the old-profile response left this radio. A reply
  // deadline also requires aligning the already durable value, without an ACK
  // claim. Neither case permits overwriting it with the old RAM settings.
  const int before=radio.ready()?radio.health():-ENODEV;
  if(before!=(service.receiving()?1:0)){remoteConfigHeld=true;return;}
  if(remoteDevicePending&&!remoteDeviceJournalsMatch()){remoteConfigHeld=true;return;}
  const bool clearLocalPosition=service.config.fixedPosition()&&!candidate.fixedPosition();
  service.stop();
  bool applied=!service.faulted()&&radio.ready()&&!radio.busy()&&radio.health()==0;
  if(applied&&remoteDevicePending)
    applied=moduleEdit.load()&&completionEdit.load()&&notificationEdit.load()&&mqttEdit.load()&&remoteDeviceJournalsMatch();
  if(applied)applied=service.apply(candidate);
  if(applied)applied=mt::NormalTxStartup::activate(service,
    IS_ENABLED(CONFIG_MT_NORMAL_ENDPOINT),IS_ENABLED(CONFIG_MT_NORMAL_TX_DEFAULT),now);
  if(applied&&service.config.same(candidate)&&
     pendingRemoteConfig.confirmResolved(journal.generation(),fingerprint)) {
    service.configurationTransition(false);pendingRemoteOwner={};remoteConfigHeld=false;
    if(remoteDevicePending){
#if defined(CONFIG_MT_MQTT_GATEWAY)
      mqttRestart.saved(0,remoteDeviceMqttGeneration,mqttJournal.generation(),mqttEdit.dirty());
#endif
      refreshMqttProxyBridge();remoteDevicePending=false;
      bench::wipeKeyBuffer(&remoteDeviceCandidate,sizeof(remoteDeviceCandidate));
    }
    syncConsoleSecurityPolicy();syncApiLogPolicy();
    companion.remoteConfigurationApplied(clearLocalPosition,companionWorkspaceProof());
    bleCompanion.remoteConfigurationApplied(clearLocalPosition,companionWorkspaceProof());
#if defined(CONFIG_MT_SERIAL_MODULE)
    serialCompanion.remoteConfigurationApplied(clearLocalPosition,companionWorkspaceProof());
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
    tcpCompanion.remoteConfigurationApplied(clearLocalPosition,companionWorkspaceProof());
#endif
  } else remoteConfigHeld=true;
}
#endif
// Runs after borrowed Service/Endpoint callback stacks return. While a core
// candidate is durable but not live, other producers/settings writers stay out.
static bool pollRemoteConfigTransition(uint32_t now,uint32_t random) {
#if defined(CONFIG_MT_RF_ADMIN_GET)
  if(!remoteConfigPending())return false;
  if(!remoteConfigHeld&&pendingRemoteConfig.pending()) {
    service.poll(now,random);
    (void)pendingRemoteConfig.poll(now);
    if(service.faulted()||!journal.ready())remoteConfigHeld=true;
    else applyReadyRemoteConfig(now);
  }
  if(remoteConfigHeld&&!administrationWorkPending()) {
    // Retain durable intent/fault state. Only a deliberate existing USB loader
    // touch may reset into that journal: no automatic reset or identity repair.
    (void)mt::usbBootloader::poll(usb,uint64_t(k_uptime_get()),[]{service.stop();},flushPeersBeforeReset);
  }
  return true;
#else
  (void)now;(void)random;return false;
#endif
}
struct ApplicationStartup {const device *watchdog;int channel;};
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
static ApplicationStartup updateWatchdog{};
static void signedUpdateProgress() {
  for(;;) {
    if(updateWatchdog.watchdog&&updateWatchdog.channel>=0)(void)wdt_feed(updateWatchdog.watchdog,updateWatchdog.channel);
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    k_mutex_lock(&serviceLock,K_FOREVER);
    const bool busy=bleStorageWindow.pollLocked(uint64_t(k_uptime_get()));
    k_mutex_unlock(&serviceLock);
    if(busy){bleStorageWindow.grantUnlocked();k_sleep(K_MSEC(5));continue;}
#endif
    return;
  }
}
#endif
static ApplicationStartup setupApplicationWatchdog(){
  const device *watchdog=DEVICE_DT_GET(DT_ALIAS(watchdog0));int channel=-1;
  if(device_is_ready(watchdog)) {
    wdt_timeout_cfg c{};c.window.max=8000;c.flags=WDT_FLAG_RESET_SOC;
    channel=wdt_install_timeout(watchdog,&c);
    if(channel>=0&&wdt_setup(watchdog,0)!=0)channel=-1;
  }
  return {watchdog,channel};
}
// Boot-only settings/cache scratch must leave the stack before any runtime
// signing or private-message operation. Preserve the initialization order.
// Real 64-bit monotonic uptime for airtime accounting; ~0 marks an invalid
// clock (the field is then unavailable, never a fabricated zero).
static uint64_t ztUptimeMsProvider(void *) {
  const int64_t ms=k_uptime_get();
  return ms<0?~uint64_t(0):static_cast<uint64_t>(ms);
}
// The main loop already owns serviceLock. Snapshotting only reads/prunes RAM;
// private RX intentionally invokes this while the radio is paused.
static bool neighborRelayEnabled(void *) {
  return initialized && storageReady && journal.ready() && moduleEdit.ready() &&
         !settingsEdit.open() && !rebootSchedule.pending() && moduleJournal.config().bytes[9];
}
static bool neighborSnapshot(void *,meshtastic_NeighborInfo &out) {
  const bool eligible=initialized&&storageReady&&journal.ready()&&moduleEdit.ready()&&
    !settingsEdit.open()&&!rebootSchedule.pending();
  return modules.localNeighbors(service,moduleEdit.config(),eligible,ztUptimeMsProvider(nullptr),out)==0;
}
[[gnu::noinline]] static ApplicationStartup initializeApplication() {
  k_mutex_lock(&serviceLock,K_FOREVER);
#if !defined(CONFIG_MT_M119_TLS_TEST)
  radio.init();
  (void)sx126x_airtime_observer_start(radioDevice);
#endif
  (void)completionRuntime.bindPowerOwner(powerOwnerHooks);
  keyVerification.bindCommit(commitVerifiedPeer,nullptr);
  uint8_t hardware[16]{};
#if defined(CONFIG_MT_PAXCOUNTER)
  (void)service.bindPaxSnapshot([](void *, meshtastic_Paxcount &out) {
    return initialized && storageReady && completionEdit.trusted() && !settingsEdit.open() &&
           !rebootSchedule.pending() && completionRuntime.paxSnapshot(uint64_t(k_uptime_get()), out);
  }, nullptr);
#endif
  (void)service.bindNeighborSnapshot(neighborSnapshot,nullptr,neighborRelayEnabled);
#if defined(CONFIG_MT_RF_ADMIN_GET)
  (void)service.bindPrivateAdminSnapshot(privateAdminSnapshot,&companion);
  (void)service.bindPrivateAdminMutation(privateAdminMutation,&companion);
  (void)service.setLegacyAdminHandler(legacyAdminRequest,&companion);
  remoteResponseObserverReady=service.bindAdminResponseQueued(remoteAdminResponseQueued,nullptr);
#endif
  (void)service.channelMeter().bind(mt::UptimeClock{ztUptimeMsProvider,nullptr},drainChannelObservations,nullptr);
  // Known empty airtime observation start, before any possible Service TX.
  // The 64-bit clock is the Zephyr monotonic uptime, never the legacy
  // uint32 Service now.
  (void)service.airtimeMeter().bind(mt::UptimeClock{ztUptimeMsProvider,nullptr});
  service.airtimeMeter().ensureAnchored();
  const ssize_t n=hwinfo_get_device_id(hardware,sizeof(hardware));
  uint32_t node=n>=6?mt::identity(hardware,size_t(n)):0;
#if MT_CHECK_PRIOR_DEVICE_RESET
  // An enrolled board must run a profile that enforces its external floor.
  // This check never writes any part of the original filesystem.
  if(mt::stock::admin::device_reset::priorEnrollment()){
    storageReady=false;mt::ble::setIngressOpen(false);
    const auto watchdog=setupApplicationWatchdog();k_mutex_unlock(&serviceLock);
    for(;;){if(watchdog.channel>=0)(void)wdt_feed(watchdog.watchdog,watchdog.channel);k_sleep(K_SECONDS(1));}
  }
#endif
  settingsStartupResult=settings_subsys_init();
  storageReady=settingsStartupResult==0;
#if MT_HAS_DEVICE_RESET
  // Read the independent floor before activating, repairing or exposing NVS
  // identity state. A blank floor performs no settings reads or writes.
  int resetResult=storageReady?deviceReset::inspect(resetFloor,settings,resetHead):-EIO;
  if(!resetResult&&resetHead.state!=deviceReset::Floor::State::Unused){
    const int prepared=deviceReset::ready(settings,resetHead);
    if(prepared&&resetHead.state==deviceReset::Floor::State::Selected&&
       (prepared==-ENOENT||prepared==-ESTALE)){
      resetRecoveryBoot=true;resetBootRetry.begin();pollDeviceResetBoot();
    }else if(prepared)resetResult=prepared;
  }
  if(resetResult||resetRecoveryBoot){
    storageReady=false;resetRecoveryBoot=true;mt::ble::setIngressOpen(false);
    const auto watchdog=setupApplicationWatchdog();k_mutex_unlock(&serviceLock);return watchdog;
  }
#endif
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
  storageReady=storageReady&&service.requirePrivateRuntime();
#endif
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  // Existing mounted storage only. Empty keeps the hardware-derived Public
  // identity; every partial/invalid private state holds RF until recovery.
  // Scan before public settings reads, whose NVS backend can repair names.
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  privateStorage.storage().attachExpanded(expandedContacts);
  privateStartup.checkpoint(expandedFlash);expandedFlash.checkpoint(feedExpandedWatchdog);
#endif
  if(storageReady)storageReady=privateStorage.bindService(service)&&privateStartup.prepare(node);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  if(storageReady&&privateStorage.storage().importedIdentity())service.restrictImportedChannelOrigins();
#endif
  if(storageReady) {
    mt::pki::IdentityBinding binding{};
    if(privateStorage.storage().preparedBinding(binding)) {
      preferenceDomain.node=binding.node;
      memcpy(preferenceDomain.publicKey,binding.publicKey,sizeof(preferenceDomain.publicKey));
      preferenceDomainReady=true;
    }
  }
#endif
#if MT_HAS_DEVICE_RESET
  if(resetHead.state!=deviceReset::Floor::State::Unused){
    mt::pki::IdentityBinding binding{};
    const bool bound=storageReady&&privateStorage.storage().preparedBinding(binding)&&
      !deviceReset::freshBinding(settings,binding)&&!memcmp(binding.publicKey,resetHead.newKey,32);
    if(bound&&resetHead.state!=deviceReset::Floor::State::Active){
      if(!deviceReset::Floor::activate(resetFloor,resetHead,binding.publicKey))sys_reboot(SYS_REBOOT_COLD);
    }
    if(!bound||resetHead.state!=deviceReset::Floor::State::Active||
       settings.eraseVerified(deviceReset::RequestKey)){
      storageReady=false;resetRecoveryBoot=true;mt::ble::setIngressOpen(false);
      const auto watchdog=setupApplicationWatchdog();k_mutex_unlock(&serviceLock);return watchdog;
    }
  }
#endif
  FactoryPhase resetPhase=FactoryPhase::Idle;
  if(storageReady&&mt::stock::admin::factory::inspect(settings,node,preferenceDomain.publicKey,resetPhase))storageReady=false;
  if(storageReady&&resetPhase==FactoryPhase::ErasePreferences){
    factoryEraseNode=node;factoryEraseRetry.begin();pollFactoryErase(uint64_t(k_uptime_get()));
    storageReady=false;mt::ble::setIngressOpen(false);
    // Normal failed-startup handling freezes the private service, which would
    // invalidate the quiescent storage proof on a later mount retry.
    const auto resetWatchdog=setupApplicationWatchdog();
    k_mutex_unlock(&serviceLock);
    return resetWatchdog;
  }
  if(storageReady) {
    auto intent=mt::admin::PreferenceArchive::IntentState::Absent;
    const int inspected=preferenceArchive.inspectRestoreIntent(settings,intent);
    preferenceRecoveryHeld=inspected||intent==mt::admin::PreferenceArchive::IntentState::Pending||
      intent==mt::admin::PreferenceArchive::IntentState::Corrupt;
    // A multi-journal restore cannot boot RF with a mixed settings snapshot.
    // Keep its exact public identity binding in RAM for explicit USB resume.
    if(preferenceRecoveryHeld)storageReady=false;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  // Complete any verified role projection before exposing a mixed snapshot.
  // Failure preserves the exact intent and keeps storage/RF admission closed.
  if(storageReady&&recoverRemoteDeviceAtBoot()) {storageReady=false;remoteConfigHeld=true;}
#endif
  if(storageReady)storageReady=journal.load(settings)==0;
  if(storageReady) {
    (void)moduleJournal.load(settings);
    (void)completionJournal.load(settings);
#ifdef CONFIG_MT_T096_ROUTER_ONLY
    storageReady=mt::modules::seedRouterDefaults(journal,moduleJournal,completionJournal);
#endif
  }
  if(storageReady)(void)uiConfigJournal.load(settings);
  if(storageReady)storageReady=mt::NormalTxStartup::migrate(
    journal,settings,IS_ENABLED(CONFIG_MT_NORMAL_TX_DEFAULT))==0;
  if(storageReady)storageReady=service.apply(journal.config());
  // A separate unavailable module journal disables modules; it cannot reset
  // core settings, identity, contact keys or the existing NodeDB journal.
  if(storageReady&&moduleJournal.ready())(void)moduleEdit.load();
  if(storageReady&&completionJournal.ready())(void)completionEdit.load();
  settingsCommit.completion(completionEdit);
  service.nodeInfoIntervalProvider([](mt::Service &owner,uint32_t &interval){
    return modules.nodeInfoInterval(owner,ztUptimeMsProvider(nullptr),interval);
  });
  (void)service.bindTrafficNodeInfoSink(trafficNodeInfoFrame,nullptr);
  service.extraTelemetry(nullptr,[](void*,uint8_t variant,uint32_t request,uint8_t *out,size_t capacity,size_t &size){
    const auto now=uint64_t(k_uptime_get());
    return completionRuntime.response(variant,request,now,wallClock.at(now),out,capacity,size);
  });
#if defined(CONFIG_MT_GPIO_MODULES)
  if(storageReady&&gpioModuleJournal.load(settings)==0)(void)gpioModuleEdit.load();
  settingsCommit.gpioModules(gpioModuleEdit);
  (void)service.bindGpioFrameSink(gpioModuleFrame,nullptr);
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  storeForwardPsramReady=esp_psram_is_initialized()&&esp_psram_get_size()>=8*1024*1024;
  if(storageReady&&storeForwardJournal.load(settings)==0)(void)storeForwardEdit.load();
  settingsCommit.storeForward(storeForwardEdit);
  (void)service.bindStoreForwardSink(storeForwardFrame,nullptr);
  (void)service.bindStoreForwardArchiveProvider(storeForwardArchive,nullptr);
  (void)service.bindStoreForwardLocalTextSink(storeForwardLocalText,nullptr);
#endif
  // A failed canned-list load disables only that independent Admin surface.
#if !defined(CONFIG_MT_DISPLAY) && DT_HAS_ALIAS(sw0)
  localKeyReady=gpio_is_ready_dt(&localKey)&&gpio_pin_configure_dt(&localKey,GPIO_INPUT)==0;
#endif
  if(storageReady)(void)cannedJournal.load(settings);
  if(storageReady)(void)ringtoneJournal.load(settings);
#if defined(CONFIG_MT_RF_ADMIN_GET)
  // Absent means deny-all without a write; corruption disables this surface
  // and cannot recover an older grant or change the operating radio identity.
  if(storageReady)(void)adminKeyJournal.load(settings);
  remoteHardwarePinJournalReady=false;remoteHardwarePinDirty=false;
  remoteHardwarePinCache.clear();
  if(storageReady&&preferenceDomainReady) {
    mt::admin::RemoteHardwarePinJournal::Domain pinDomain{};
    pinDomain.node=preferenceDomain.node;
    memcpy(pinDomain.publicKey,preferenceDomain.publicKey,sizeof(pinDomain.publicKey));
    const int loaded=remoteHardwarePinJournal.load(settings,pinDomain);
    if(loaded==0&&remoteHardwarePinJournal.ready()) {
      const auto &snapshot=remoteHardwarePinJournal.snapshot();
      bool localNodeEntry=false;
      for(size_t i=0;i<snapshot.count;++i)
        if(snapshot.entries[i].node_num==pinDomain.node)localNodeEntry=true;
      if(!localNodeEntry&&remoteHardwarePinCache.restoreCommittedSnapshot(snapshot.entries,snapshot.count))
        remoteHardwarePinJournalReady=true;
    }
  }
#endif
  if(storageReady&&extendedJournal.load(settings)==0)(void)extendedEdit.load();
  if(storageReady&&notificationJournal.load(settings)==0)(void)notificationEdit.load();
  if(storageReady&&wifiJournal.load(settings)==0&&wifiEdit.load())(void)wifiAdmin.loaded();
#if defined(CONFIG_MT_MQTT_GATEWAY)
#if defined(CONFIG_MT_M119_TLS_TEST)
  mqttTestCaReady=mt::mqtt::installM119TestCa();
  mqttTrustAvailable=mqttTestCaReady&&mt::mqtt::tlsTrustReady(mt::mqtt::BootSnapshot::SecurityTag);
#elif defined(CONFIG_MQTT_LIB_TLS)
  mqttTrustAvailable=mt::mqtt::installMqttRootCa()&&mt::mqtt::tlsTrustReady(mt::mqtt::BootSnapshot::SecurityTag);
#else
  mqttTrustAvailable=false;
#endif
  mqttAdmin.runtimePolicy(true,mqttTrustAvailable);
#else
  mqttAdmin.runtimePolicy(false,false);
#endif
  settingsCommit.mqtt(mqttAdmin);
  if(storageReady&&mqttJournal.load(settings)==0){
    (void)mqttAdmin.loaded();
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mqttRestart.boot(mqttJournal.generation());
#endif
  }
#if !defined(CONFIG_MT_MQTT_GATEWAY)
  mqttAdmin.runtimePolicy(mqttEdit.config().proxyEnabled(),false);
#endif
  refreshMqttProxyBridge();
  (void)service.bindMqttFrameSink(mqttFrame,nullptr);
  (void)service.bindMqttPrivateTxSink(mqttPrivateTx,nullptr);
  (void)service.bindMqttPkiTransitSink(mqttPkiTransit,nullptr);
  (void)service.bindHotNodeObservationSink(observeHotNode,nullptr);
#if !defined(CONFIG_MT_M119_TLS_TEST)
  (void)service.bindMqttLocalRxSink(mqttLocalRx,nullptr);
#endif
  if(storageReady&&bluetoothJournal.load(settings)==0)
    bluetoothConfigReady=bluetoothJournal.ready()&&bluetoothJournal.state()!=mt::BluetoothJournal::State::Recovered;
  service.route.configureContinuous(uint8_t(node),service.config.node().bytes[4]);service.route.configureIdentity(node);service.discovery.identity(node);
  // Cache failures freeze peer updates; local configuration/text remain usable.
#if defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  nodeJournal.files(extendedNodes);
#endif
  if(storageReady)(void)nodeJournal.load(settings,service.nodes,node);
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS) && defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  // Contact edits can arrive before satellite history has an identity and epoch.
  // Prepare the large NodeDB after validating its existing journal selectors.
  if(storageReady && nodeJournal.ready()){
    const int mountResult=satelliteVolume.mount(true,false);
    if(mountResult)printk("Expanded NodeDB volume unavailable (%d)\n",mountResult);
  }
#endif
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
  if(storageReady)bindBroadcastHistory(node);
#endif
  if(storageReady)loadHotNodeMetadata(node);
#if defined(CONFIG_MT_SATELLITE_HISTORY)
#if MT_HAS_DEVICE_RESET && defined(CONFIG_BOARD_HELTEC_T096)
  // Before the first explicitly requested device reset the full legacy LFS
  // extent must stay erased. An Active floor proves retirement is enrolled.
  if(storageReady)loadSatelliteHistory(resetHead.state==deviceReset::Floor::State::Active);
#else
  if(storageReady)loadSatelliteHistory(true);
#endif
#endif
  if(storageReady){
    waypoints.domain(node,service.waypointDomainEpoch());
    (void)waypointHistory.load(settings,waypoints,waypointHistoryDomain(),waypointHistoryContacts(),wallClock.at(k_uptime_get()));
  }
  const auto bootWatchdog=setupApplicationWatchdog();
  const device *watchdog=bootWatchdog.watchdog;const int channel=bootWatchdog.channel;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  expandedWatchdog=watchdog;expandedChannel=channel;
#endif
  if(channel<0||!node)storageReady=false;
#if (defined(CONFIG_MT_PKI_STOCK_TRIAL) && MT_PKI_LEGACY_TAIL_TOOLS_ENABLED) || defined(CONFIG_MT_XIAO_LARGE_NODEDB)
  preparationWatchdog=watchdog;preparationWatchdogChannel=channel;
#endif
  // The ESP CSPRNG authority requires the enabled Bluetooth controller. Prepare
  // that stack under its existing boot/settings lease BEFORE checking entropy
  // and opening PKI, while advertising and payload admission remain closed.
  mt::ble::setIngressOpen(false);
#if defined(CONFIG_MT_DISPLAY)
#if defined(CONFIG_MT_BASEUI_GAMES)
  if(storageReady)loadGameScores();
#endif
  mt::ui::setSystemCommandOwner(runLocalSystemCommand,MT_HAS_DEVICE_RESET && preferenceDomainReady);
  mt::ui::setButtonConsumer(dismissLocalNotificationInput);
  mt::ui::init(service.config);
  if(bluetoothJournal.config().mode==mt::BluetoothSettings::RandomPin&&!mt::ui::ready())bluetoothConfigReady=false;
#endif
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  const bool bleBoot=mt::ble::store::beginBoot(storageReady&&service.storageQuiescent()&&radio.ready()&&!radio.busy()&&radio.health()==0);
  if(bleBoot){
    const int result=mt::ble::prepareBoot(node,bluetoothJournal.config(),bluetoothConfigReady);
    mt::ble::store::endBoot(result);
  }
#else
  (void)mt::ble::prepareBoot(node,bluetoothJournal.config(),bluetoothConfigReady);
#endif
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  if(storageReady) {
    const bool prepared=privateStartup.state()==mt::pki::PkiStartup::State::Prepared;
    const bool sourceReady=prepared&&mt_psa_threading_ready()&&privateEntropy.ready();
    const bool backendReady=sourceReady&&privateBackend.begin();
    storageReady=privateStartup.activate(service,backendReady);
  }
  if(!storageReady)(void)privateStartup.hold(service);
#endif
  if(storageReady&&resetPhase==FactoryPhase::RetireContacts){
    // Normal startup has bound the verified identity and contact authority;
    // no radio or client ingress is open yet. Revocations survive interruption.
    const int advanced=mt::stock::admin::factory::retireAndAdvance(settings,node,preferenceDomain.publicKey,[&](){
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
      if(preferenceDomainReady){
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
        if(expandedContacts.ready()){
          while(expandedContacts.pins().count()){
            feedExpandedWatchdog();const auto node=expandedContacts.pins().at(0)->node;
            if(!service.revokePrivatePeer(node)||expandedContacts.pins().find(node))return -EIO;
          }
          return 0;
        }
#endif
        mt::pki::PkiPeerSet peers;
        if(!privateStorage.storage().preparedPeers(peers))return -EIO;
        for(size_t i=0;i<peers.count();++i){
          if(channel>=0)(void)wdt_feed(watchdog,channel);
          if(!service.revokePrivatePeer(peers.at(i)->node))return -EIO;
        }
        if(!privateStorage.storage().preparedPeers(peers)||peers.count())return -EIO;
      }
#endif
      return 0;
    });
    if(!advanced)sys_reboot(SYS_REBOOT_COLD);
    storageReady=false;
  }
  mt::ble::setActivityWake(notifyPowerActivity);
  if(startApplicationUsbRx())storageReady=false;
#if defined(CONFIG_SOC_ESP32S3)
  // Private boot and reset recovery have finished. Generic NVS reads may
  // repair name records, so even reading this counter before admission can
  // change the evidence used by those owners.
  if(storageReady)myBootCount=mt::companion::incrementBootCounter(settings);
#endif
#if !defined(CONFIG_MT_M119_TLS_TEST)
  bool normalBoot=mt::NormalTxStartup::bootPolicyEnabled(IS_ENABLED(CONFIG_MT_NORMAL_ENDPOINT),IS_ENABLED(CONFIG_MT_PKI_TARGET_RUNTIME));
#if defined(CONFIG_MT_PKI_STOCK_TRIAL) || defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  // Both companion profiles restore the configured radio. Only XIAO also
  // supports public-channel operation with a verified empty private store.
  normalBoot=IS_ENABLED(CONFIG_MT_NORMAL_ENDPOINT)&&
    privateStartup.permitsCompanionRadioBoot(IS_ENABLED(CONFIG_MT_STORE_FORWARD_PSRAM));
#endif
  if(storageReady)storageReady=mt::NormalTxStartup::activate(
    service,normalBoot,
    IS_ENABLED(CONFIG_MT_NORMAL_TX_DEFAULT),k_uptime_get_32());
#endif
  if(!storageReady&&service.receiving())service.stop();
  // Do not advertise a boot whose application/storage authority failed. Desired
  // Bluetooth settings remain unchanged; only this boot's policy is withheld.
  if(!storageReady)bluetoothConfigReady=false;
  (void)mt::ble::requestConfiguration(bluetoothJournal.config(),bluetoothConfigReady&&storageReady);
  mt::ble::setIngressOpen(storageReady);
  initialized=true;
  syncConsoleSecurityPolicy();
  syncApiLogPolicy();
  k_mutex_unlock(&serviceLock);
  return {watchdog,channel};
}
#include "KernelHeapStats.h"
static void observeLocalStats(const mt::NodeCache &hot,mt::LocalStatsObservation &stats){
  stats.noiseFloor=radioNoiseFloor.measured();
  stats.badRx=uint32_t(atomic_get(&radioRxBad));
  stats.packetsRx=uint32_t(atomic_get(&radioRxGood))+stats.badRx;
  const uint32_t utc=wallClock.at(uint64_t(k_uptime_get()));
  stats.onlineNodes=mt::hotOnline(hot,utc);
#if defined(CONFIG_MT_PEER_DIRECTORY)
  if(directoryBound){
    stats.onlineNodes=0;
    if(storageReady&&!settingsEdit.open()&&directorySettingsGen==journal.generation()&&
       !k_mutex_lock(&directoryLock,K_NO_WAIT)){
      stats.totalNodes=uint16_t(1+peerDirectory.visibleCount(hot,utc,&stats.onlineNodes));
      k_mutex_unlock(&directoryLock);
    }
  }
#endif
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && CONFIG_HEAP_MEM_POOL_SIZE>0
  sys_memory_stats memory{};
  if(!mt::kernelHeapStats(memory)){
    stats.heapFree=memory.free_bytes;
    stats.heapTotal=memory.free_bytes+memory.allocated_bytes;
  }
#endif
}
int main() {
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
  mt::update::signedOta::initializeBootVersion();
#endif
  service.bindLocalStats(observeLocalStats);
  mt_security::initializeConsoleSecurity();
  mt::notification::init();
  mt::hardware::init();
  mt::hardware::poll(uint64_t(k_uptime_get()));
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
  mt::ble::store::initialize();
#endif
  auto startup=initializeApplication();
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
  if(startup.channel>=0){
    freshSettings.setScanProgress([](void *context){
      const auto &owner=*static_cast<const ApplicationStartup *>(context);
      (void)wdt_feed(owner.watchdog,owner.channel);
    },&startup);
  }
#endif
#if MT_HAS_DEVICE_RESET
  while(resetRecoveryBoot){
    if(startup.channel>=0)(void)wdt_feed(startup.watchdog,startup.channel);
    k_sleep(K_MSEC(250));
    if(!resetBootRetry.waiting())continue;
    k_mutex_lock(&serviceLock,K_FOREVER);pollDeviceResetBoot();k_mutex_unlock(&serviceLock);
  }
#endif
  // The application stays off during late mount recovery. Kernel/filesystem
  // workers remain runnable and the watchdog uses the ordinary boot channel.
  while(factoryEraseNode){
    if(startup.channel>=0)(void)wdt_feed(startup.watchdog,startup.channel);
    k_sleep(K_MSEC(250));
    if(!factoryEraseRetry.waiting())continue;
    k_mutex_lock(&serviceLock,K_FOREVER);
    pollFactoryErase(uint64_t(k_uptime_get()));
    k_mutex_unlock(&serviceLock);
  }
  // ZephCore retains these pointers across reconnects. wifiStart copies the
  // verified boot policy once and starts outside the Service mutex. A recovered
  // single-good-slot policy is readable but never silently associates.
  (void)mt::network::wifiStart(wifiEdit.config(),storageReady&&wifiEdit.ready()&&
    wifiJournal.state()!=mt::network::WifiStationJournal::State::Recovered);
  mt::tcp::init(storageReady&&wifiEdit.ready()&&wifiEdit.config().enabled()&&
    wifiJournal.state()!=mt::network::WifiStationJournal::State::Recovered);
#if defined(CONFIG_MT_MQTT_GATEWAY)
  if(storageReady&&mqttSettingsUsable()&&!mqttProxyConfigured()&&
     wifiEdit.ready()&&wifiEdit.config().enabled()&&wifiJournal.state()!=mt::network::WifiStationJournal::State::Recovered&&
#if defined(CONFIG_MT_M119_TLS_TEST)
     mqttTestCaReady&&(committedMqttSettings().bytes[mt::network::MqttGatewaySettings::Flags]&mt::network::MqttGatewaySettings::Tls)&&
     !committedMqttSettings().proxyEnabled()&&
#endif
     mqttBoot.capture(committedMqttSettings(),service.route.identity(),true,mqttTrustAvailable,service.config)) {
#if defined(CONFIG_MT_M119_TLS_TEST)
    // A connection-only test has no channel/map demand. Override only this
    // boot snapshot copy; do not persist MQTT feature/consent settings.
    auto testSettings=mqttBoot.settings();
    if(testSettings.enabled&&!testSettings.proxyToClient){
      testSettings.wantsLink=true;
      testSettings.subscriptionTopicCount=0; // Connection-only: do not subscribe to device downlink topics.
      mqttStarted=mt::mqtt::init(testSettings,mt::network::wifiStatus().mqttReady);
    }
#else
    if(mqttBoot.settings().enabled&&mqttBoot.settings().wantsLink&&!mqttBoot.settings().proxyToClient)
      mqttStarted=mt::mqtt::init(mqttBoot.settings(),mt::network::wifiStatus().mqttReady);
#endif
  }
  refreshMqttProxyBridge();
#endif
  const device *const watchdog=startup.watchdog;const int channel=startup.channel;
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
  updateWatchdog=startup;
  if(initialized&&storageReady&&journal.ready()&&radio.ready()&&!radio.busy()&&
     !service.faulted()&&radio.health()==0)
    mt::update::signedOta::confirmHealthyBoot();
#endif
  mt::PositionController positions;
  uint32_t lastPhonePositionCheck=0;
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
  if(service.broadcastHistory)positions.restoreLastSend(broadcastHistory.last(mt::history::Kind::Position));
#endif
  for(;;) {
    // ADC settling and acquisition run without the service/radio mutex.
    const uint64_t hardwareNow=uint64_t(k_uptime_get());
    mt::hardware::poll(hardwareNow);
    mt::network::wifiPoll();
#if defined(CONFIG_MT_DISPLAY)
    // Controller serializes model snapshots with transport UI input using
    // the same recursive Service mutex. Cache acquisition remains owner-only.
    mt::ui::SelectedPeerForTelemetry selectedPeer{};
    const bool selectedPeerReady=uiController.selectedPeerForTelemetry(selectedPeer);
    mt::ui::PeerTelemetryView peerView{};
#endif
#if defined(CONFIG_MT_MQTT_GATEWAY)
    mt::mqtt::setWifiReady(mt::network::wifiStatus().mqttReady);
#endif
    k_mutex_lock(&serviceLock,K_FOREVER);
    uint32_t syncedUtc=0;
    if(mt::network::wifiTakeTime(syncedUtc))(void)wallClock.set(syncedUtc,uint64_t(k_uptime_get()));
    const auto gpsSettings=service.config.gnssSettings();const bool fixed=service.config.fixedPosition();
    const bool environmentEnabled=moduleEdit.ready()&&moduleEdit.config().environmentEnabled();
    const bool powerEnabled=moduleEdit.ready()&&moduleEdit.config().powerEnabled();
#if defined(CONFIG_MT_MAX30102_OPTICAL)
    const bool opticalEnabled=completionRuntime.telemetry().configured()&&completionRuntime.telemetry().config().health_measurement_enabled;
#endif
    k_mutex_unlock(&serviceLock);
#if defined(CONFIG_MT_MAX30102_OPTICAL)
    mt::stock::telemetry::max30102::poll(opticalEnabled,hardwareNow);
#endif
    mt::gnss::poll(gpsSettings,fixed,hardwareNow);
    mt::environment::poll(environmentEnabled,powerEnabled,hardwareNow);
    const auto environmentReadings=mt::environment::snapshot(hardwareNow);
    const auto powerReadings=mt::environment::powerSnapshot(hardwareNow);
    const auto gpsPosition=mt::gnss::snapshot(hardwareNow);
    const uint32_t gpsUtc=mt::gnss::utcSnapshot(hardwareNow);
    k_mutex_lock(&serviceLock,K_FOREVER);
    syncConsoleSecurityPolicy();
    syncApiLogPolicy();observeApiOwnerState();
    const bool beaconTick=beacons.takeScheduledTick(extendedEdit,hardwareNow);
    if(pollRemoteConfigTransition(k_uptime_get_32(),sys_rand32_get())) {
      k_mutex_unlock(&serviceLock);
      if(channel>=0)(void)wdt_feed(watchdog,channel);
      k_sleep(K_MSEC(5));continue;
    }
    if(beaconTargets.radioHeld()){
      // Exclusive native owner until target completion and verified home RX.
      // USB/BLE/TCP callbacks retain their bounded ingress queues meanwhile.
      beaconTargets.poll(k_uptime_get_32(),beaconSettingsEpoch(),service.route.identity(),
        initialized&&storageReady&&extendedEdit.trusted()&&!settingsEdit.open()&&!rebootSchedule.pending());
      k_mutex_unlock(&serviceLock);
      if(channel>=0)(void)wdt_feed(watchdog,channel);
      k_sleep(K_MSEC(5));continue;
    }
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    // Before every MT writer/Service mutation. No stack call while a granted
    // native store owns the RF-off interval. Wake only after releasing Service.
    if(bleStorageWindow.pollLocked(uint64_t(k_uptime_get()))) {
      k_mutex_unlock(&serviceLock);bleStorageWindow.grantUnlocked();
      if(channel>=0)(void)wdt_feed(watchdog,channel);
      k_sleep(K_MSEC(5));continue;
    }
#endif
    service.measuredHardware(mt::hardware::snapshot(hardwareNow));
    service.measuredPosition(gpsPosition);
    service.measuredEnvironment(moduleEdit.ready()&&moduleEdit.config().environmentEnabled()?environmentReadings:zt::telemetry::EnvironmentReadings{});
    service.measuredPower(moduleEdit.ready()&&moduleEdit.config().powerEnabled()?powerReadings:zt::telemetry::PowerReadings{});
    // WallClock's existing correction guard preserves a recent phone anchor.
    // Calendar-based accessory maintenance also uses this UTC estimate. Radio deadlines use uptime.
    if(gpsUtc)(void)wallClock.set(gpsUtc,hardwareNow);
#if defined(CONFIG_MT_BME680_IAQ)
    mt::stock::telemetry::bme680::clock(wallClock.at(hardwareNow));
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
    mt::stock::telemetry::accessory::clock(wallClock.at(hardwareNow));
#endif
    // Existing privacy consent and fresh measured/fixed source precede the
    // stock cadence policy; no location is invented to make a send eligible.
    if(!firmwareUpdate.pending()&&!administrationWorkPending()&&
       mt::usbBootloader::poll(usb,uint64_t(k_uptime_get()),[]{service.stop();},flushPeersBeforeReset)) {
      // A latched local touch takes priority over queued config/LoRa work.
      k_mutex_unlock(&serviceLock);
      if(channel>=0)(void)wdt_feed(watchdog,channel);
      k_sleep(K_MSEC(5));
      continue;
    }
    drainChannelObservations(service.channelMeter(),nullptr);
#if !defined(CONFIG_MT_M119_TLS_TEST)
    // Drain already-completed RF events before local admin. The storage-stop
    // barrier below also closes callbacks that race after this bounded drain.
    Rx event;
    for(unsigned count=0;count<8&&k_msgq_get(&rxQueue,&event,K_NO_WAIT)==0;++count)
    {
      const uint64_t receiveUptime=uint64_t(k_uptime_get());
      const uint32_t receiveNow=uint32_t(receiveUptime),receiveUtc=wallClock.at(receiveUptime);
      if(event.length)mt::notification::radioReceived(receiveNow);
      service.measuredUptime(receiveUptime/1000);
      if(extendedEdit.trusted()&&!settingsEdit.open()){
        trafficWindow.configure(extendedEdit.config().traffic());service.trafficManagement(&trafficWindow,receiveUptime);
      }else service.trafficManagement(nullptr,receiveUptime);
      bool accepted=false;
      service.rx(event.length?event.data:nullptr,event.length,event.snr,receiveNow,sys_rand32_get(),true,&accepted,false,
                 receiveUtc,uint32_t(receiveUptime/1000u),event.rssi,event.snrKnown?mt::packetSnrDb(event.snrRaw):float(event.snr));
      pollKeyVerification(receiveNow);
      if(accepted){beaconTargets.receiveAccepted();
        recordApiEvent(mt::api::ApiLogBuffer::Event::RadioRxAcceptedBytes,event.length);}
      if(service.trafficSuppressed())continue;
      companion.receive(event.length?event.data:nullptr,event.length,event.rssi,event.snr,receiveNow,receiveUtc,event.snrKnown,event.snrRaw,true,companionWorkspaceProof());
      bleCompanion.receive(event.length?event.data:nullptr,event.length,event.rssi,event.snr,receiveNow,receiveUtc,event.snrKnown,event.snrRaw,true,companionWorkspaceProof());
#if defined(CONFIG_MT_SERIAL_MODULE)
      serialCompanion.receive(event.length?event.data:nullptr,event.length,event.rssi,event.snr,receiveNow,receiveUtc,event.snrKnown,event.snrRaw,true,companionWorkspaceProof());
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
      tcpCompanion.receive(event.length?event.data:nullptr,event.length,event.rssi,event.snr,receiveNow,receiveUtc,event.snrKnown,event.snrRaw,true,companionWorkspaceProof());
#endif
#if defined(CONFIG_MT_REPLY_BOT) || defined(CONFIG_MT_REPLY_EXAMPLE)
      service.flushStockReply(receiveNow,sys_rand32_get());
#endif
    }
#endif
#if defined(CONFIG_MT_MQTT_GATEWAY) && !defined(CONFIG_MT_M119_TLS_TEST)
    // Socket callbacks only copy bounded publishes into the receive mailbox.
    // Decode and route at most its fixed two messages here on the Service owner.
    for(unsigned count=0;count<2;++count) {
      mt::mqtt::DownlinkMessage incoming;
      if(!mt::mqtt::takeDownlink(incoming))break;
      mt::mqtt::MqttServiceEnvelopeDecoder::Decoded envelope{};
      const bool decoded=mt::mqtt::MqttServiceEnvelopeDecoder::decode(
        incoming.payload,incoming.payloadLength,&envelope);
      if(decoded&&mqttDownlinkTopicMatches(incoming,envelope)&&!mqttRestart.pending()&&
         !mqttEdit.dirty()&&!settingsEdit.open()&&!rebootSchedule.pending()) {
        const auto &settings=mqttEdit.config();const auto runtime=mt::mqtt::status();
        mt::mqtt::MqttDownlinkAdmission::Context context{};
        context.mqttEnabled=settings.enabled();
        context.brokerConfigured=settings.enabled();
        context.transportReady=mqttStarted&&runtime.state==mt::mqtt::State::Online;
        context.tlsEnabled=(settings.bytes[mt::network::MqttGatewaySettings::Flags]&mt::network::MqttGatewaySettings::Tls)!=0;
        context.tlsTrustReady=mqttTrustAvailable;
        context.encryptionEnabled=settings.encryptionEnabled();
        const uint64_t receiveUptime=k_uptime_get();
        (void)service.processMqttDownlink(envelope,context,mqttBoot.gateway(),
          uint32_t(receiveUptime),sys_rand32_get(),wallClock.at(receiveUptime),uint32_t(receiveUptime/1000u));
      }
      bench::wipeKeyBuffer(&incoming,sizeof(incoming));
      bench::wipeKeyBuffer(&envelope,sizeof(envelope));
    }
#endif
#if !defined(CONFIG_MT_M119_TLS_TEST)
    // Observe native RX even while the endpoint is TX-disabled, so the first
    // later save is not the first observation of an old raw-preamble flag.
    mt::observeIdleRx(service,radio);
#endif
    bool clientIngressSaturated=false;
    uint8_t serialBytes[128];const auto serialBatch=takeUsbBytes(serialBytes,sizeof(serialBytes));
    for(uint32_t i=0;i<serialBatch.length;++i)
      companion.byte(serialBytes[i],k_uptime_get_32(),serialBatch.generation,companionWorkspaceProof());
    bench::wipeKeyBuffer(serialBytes,sizeof(serialBytes));
#if defined(CONFIG_MT_PKI_STOCK_TRIAL) && MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
    provisionIngress.tick();
#endif
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    freshIngress.tick();
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    stockImport.tick();
#endif
#endif
    bindBleSession(mt::ble::session());
    mt::ble::Packet bp;
    unsigned bleDrained=0;
    for(;bleDrained<4&&mt::ble::receive(bp);++bleDrained){
      notifyPowerActivity();
      (void)mt::ble::dispatch(bp,handleBlePacket,nullptr);
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
      bench::wipeKeyBuffer(&bp,sizeof(bp));
#endif
      finishBluetoothAdmin();
    }
    clientIngressSaturated=bleDrained==4;
#if defined(CONFIG_MT_TCP_COMPANION)
    bindTcpSession(mt::tcp::session());
    mt::tcp::Packet tcpPacket;
    unsigned tcpDrained=0;
    for(;tcpDrained<4&&mt::tcp::receive(tcpPacket);++tcpDrained)
      (void)mt::tcp::dispatch(tcpPacket,handleTcpPacket,nullptr);
    clientIngressSaturated=clientIngressSaturated||tcpDrained==4;
#endif
    // Client handlers (including synchronous settings writes) have returned.
    // A due update is a one-shot transaction: defer while accepted work remains,
    // stop RF race-safely, flush verified peer state, and only then prepare the
    // platform handoff. Ordinary reboot never consumes/cancels a staged update.
    const auto action=rebootSchedule.takeDueAction(uint64_t(k_uptime_get()));
    if(action!=ScheduledOwnerAction::None){
      if(administrationWorkPending()) {
        // Keep normal owner admission while the bounded request/response
        // drains. A transport fence here would make its callback fail ready().
        rebootSchedule.request(1,uint64_t(k_uptime_get()),action);
      } else
      if(action==ScheduledOwnerAction::FirmwareUpdate||action==ScheduledOwnerAction::Dfu){
        atomic_set(&shutdownAdmissionClosed,1);mt::ble::setIngressOpen(false);
        const bool ingressEmpty=k_msgq_num_used_get(&rxQueue)==0&&ring_buf_size_get(&usbRx)==0&&mt::ble::pendingInput()==0;
        if(!ingressEmpty||clientIngressSaturated||persistentWorkPending()||radio.busy()||!service.storageStopSafe()){
          rebootSchedule.request(1,uint64_t(k_uptime_get()),action);
        }else{
          const bool wasReceiving=service.receiving(),wasRelaying=service.relaying(),wasArmed=service.endpointArmed();
          const int paused=service.pauseForStorage();
          int result=paused;
          if(!paused&&service.storageQuiescent()&&!service.faulted()&&radio.ready()&&!radio.busy()&&radio.health()==0){
            result=nodeJournal.flush(settings,service.nodes,k_uptime_get_32(),true,true);
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
            if(!result)flushBroadcastsBeforeReset();
#endif
            if(result>=0){
              meshtastic_Config_NetworkConfig network{};
              const auto mode=firmwareUpdate.mode();
              const bool espLoader=mode==mt::update::Mode::EspBleLoader||mode==mt::update::Mode::EspWifiLoader;
              if(espLoader&&!wifiAdmin.get(network))result=-EIO;
              else result=firmwareUpdate.execute(true,network);
              bench::wipeKeyBuffer(&network,sizeof(network));
#if defined(CONFIG_MT_DISPLAY) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
              mt::ui::showFirmwareUpdate(firmwareUpdate);
#endif
            }
          }else if(paused==-EBUSY&&!service.faulted()){
            rebootSchedule.request(1,uint64_t(k_uptime_get()),action);result=0;
          }else if(!result)result=-EIO;
          if(result&&firmwareUpdate.state()==mt::update::FirmwareUpdateController::State::Scheduled)
            (void)firmwareUpdate.freeze(result);
          if(!rebootSchedule.pending()){
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
            if(!result&&(firmwareUpdate.mode()==mt::update::Mode::EspWifiLoader||
                         firmwareUpdate.mode()==mt::update::Mode::EspBleLoader)){
              const bool ble=firmwareUpdate.mode()==mt::update::Mode::EspBleLoader;
              k_mutex_unlock(&serviceLock);
              mt::update::signedOta::serve(ble,signedUpdateProgress,myBootCount);
              sys_reboot(SYS_REBOOT_COLD);
              k_mutex_lock(&serviceLock,K_FOREVER);result=-EIO;
            }
#endif
            if(!result){firmwareUpdate.reboot();result=firmwareUpdate.error();}
            if(result){
              if(!updateFailureReported){printk("Firmware update handoff failed: %d\n",result);updateFailureReported=true;}
              rebootSchedule.clear();
              if(wasReceiving&&!service.faulted()&&service.config.provisioned()&&service.start(wasRelaying,k_uptime_get_32())){
                if(wasArmed){const bool enabled=service.normalMode()&&service.txAllowed()&&service.config.txEnabled();
                  if(!service.armEndpoint(enabled))service.stop();}
              }
              // A frozen handoff may have changed persistent boot selection.
              // Keep producer/reset admission closed until an out-of-band
              // recovery; a verified rollback does not replay this attempt.
              if(!firmwareUpdate.pending()){mt::ble::setIngressOpen(true);atomic_set(&shutdownAdmissionClosed,0);}
            }
          }
        }
      }else if(action==ScheduledOwnerAction::Shutdown){
        atomic_set(&shutdownAdmissionClosed,1);mt::ble::setIngressOpen(false);
        if(clientIngressSaturated||persistentWorkPending()||k_msgq_num_used_get(&rxQueue)!=0||ring_buf_size_get(&usbRx)!=0||mt::ble::pendingInput()!=0)
          rebootSchedule.request(1,uint64_t(k_uptime_get()),action);
        else explicitShutdownDue=true;
      }else if(action==ScheduledOwnerAction::Reboot){
        if(!firmwareUpdate.pending()){service.stop();flushPeersBeforeReset();sys_reboot(SYS_REBOOT_COLD);}
      }
    }
    pollKeyVerification(k_uptime_get_32());
    if(storageReady&&journal.ready()) {
      const bool nativeReady=radio.ready();const int nativeHealth=nativeReady?radio.health():-ENODEV;
      const bool coherent=nativeReady&&
        ((service.receiving()&&nativeHealth==1)||(!service.receiving()&&nativeHealth==0));
      const uint32_t now=k_uptime_get_32();
      if(coherent&&settingsEdit.due(now)&&(moduleEdit.dirty()||notificationEdit.dirty()||wifiEdit.dirty()||extendedEdit.dirty()||completionEdit.dirty()||wifiAdmin.changedFromBoot()||mqttEdit.dirty()
#if defined(CONFIG_MT_MQTT_GATEWAY)
          ||mqttRestart.pending()
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
          ||storeForwardEdit.dirty()
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
          ||gpioModuleEdit.dirty()
#endif
          ))
        (void)applicationAfterCommit(settingsCommit.commit(now),uint64_t(k_uptime_get()));
      else (void)settingsEdit.poll(now,coherent);
    }
    const uint64_t moduleNow=ztUptimeMsProvider(nullptr);
    modules.pollReports(service,moduleEdit.config(),storageReady&&moduleEdit.ready()&&!settingsEdit.open()&&!rebootSchedule.pending(),
      moduleNow,sys_rand32_get(),wallClock.at(moduleNow),[](const uint8_t*data,size_t size,uint32_t id,mt::ModuleController::PhoneDelivery delivery,uint8_t recipients){
        return publishModuleData(data,size,id,0,delivery==mt::ModuleController::PhoneDelivery::Idle,recipients);
      });
    completionRuntime.ownerPoll(service,modules,moduleNow,wallClock.at(moduleNow),sys_rand32_get(),[](const uint8_t*data,size_t size,uint32_t id,bool idleOnly=false){
      publishModuleData(data,size,id,0,idleOnly);
    });
    if(beaconTick)beacons.pollScheduledTargets(service,extendedEdit,storageReady&&!settingsEdit.open()&&!rebootSchedule.pending(),
      hardwareNow,wallClock.at(hardwareNow),sys_rand32_get(),[](const uint8_t*data,size_t size,uint32_t id,unsigned channel){
        publishModuleData(data,size,id,channel);
      },beaconTargets,beaconSettingsEpoch());
#if defined(CONFIG_MT_TAK_TRACKER)
    refreshTakConfiguration();
#endif
    positions.poll(service,modules,storageReady&&!settingsEdit.open()&&!rebootSchedule.pending(),hardwareNow,sys_rand32_get());
    completionRuntime.roleSleep(service.config.role(),modules.takeRoleSleep(),hardwareNow);
    refreshSerialInput();
#if defined(CONFIG_MT_SERIAL_MODULE)
    (void)serialModule.poll(k_uptime_get_32(),sendSerialInput,nullptr);
    pollSerialPosition(k_uptime_get_32());
#endif
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    storeForwardRuntime.poll(service,storeForwardEdit.config(),
      storageReady&&storeForwardEdit.trusted(),settingsEdit.open()||rebootSchedule.pending(),
      storeForwardPsramReady,k_uptime_get_32(),sys_rand32_get());
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
    refreshGpioModules();
    gpioModuleService.poll(k_uptime_get_32(),sys_rand32_get());
#endif
#if defined(CONFIG_MT_PKI_TARGET_RUNTIME)
    // Owner-loop only: never persist or publish first-contact pins from RX.
    // This point holds serviceLock, has drained RX, and precedes new RF work.
    pollVerifiedPeerCandidate();
#endif
    service.poll(k_uptime_get_32(),sys_rand32_get());
    if(remoteConfigPending()) {
      // The callback above may have committed a remote candidate in this
      // pass. Enter the transition-only path before any subsequent owner work.
      k_mutex_unlock(&serviceLock);
      if(channel>=0)(void)wdt_feed(watchdog,channel);
      k_sleep(K_MSEC(5));continue;
    }
    (void)syncHotNodeFlags();
    const uint64_t hotNow=k_uptime_get();
    backfillHotNodeClock(wallClock.at(hotNow),uint32_t(hotNow/1000u),uint32_t(hotNow));
    persistHotNodeMetadata(k_uptime_get_32());
    if(waypoints.domain(storageReady?service.route.identity():0,service.waypointDomainEpoch())){
#if defined(CONFIG_MT_DISPLAY)
      mt::ui::clearWaypoint();
#endif
    }
    waypoints.purgeExpired(wallClock.at(hotNow));
    saveWaypoints(hotNow);
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
    if(!settingsEdit.open()&&!persistentWorkPending()&&!rebootSchedule.pending()&&!firmwareUpdate.pending()&&
       !clientIngressSaturated&&!administrationWorkPending()&&!beaconTargets.radioHeld()&&
       !k_msgq_num_used_get(&rxQueue)&&!ring_buf_size_get(&usbRx)&&!mt::ble::pendingInput())
      (void)checkpointBroadcasts(false);
#endif
#if defined(CONFIG_MT_GENERIC_THREAD)
    static mt::stock::optional::GenericThreadModule genericThread;
    genericThread.poll(uint32_t(hotNow),[](const char *line){
      (void)companion.localModuleLog("GenericThread",line,companionWorkspaceProof());
    });
#endif
#if defined(CONFIG_MT_DROPZONE)
    static mt::stock::optional::DropzoneBoard dropzoneBoard;
    if(service.takeDropzoneRequest(uint32_t(hotNow)))dropzoneBoard.request(service.route.identity(),service.waypointDomainEpoch());
    mt::stock::optional::Conditions observed{};
    if(dropzoneBoard.take(service.route.identity(),service.waypointDomainEpoch(),observed)){
      char message[200];mt::stock::optional::DropzoneModule::format(observed,wallClock.at(hotNow),message);
      uint32_t packetId=0;
      (void)service.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,reinterpret_cast<const uint8_t*>(message),
          strlen(message),0,false,uint32_t(hotNow),sys_rand32_get(),packetId);
    }
#endif
    pollMqttMapReport(k_uptime_get_32());
    companion.poll(companionWorkspaceProof());bleCompanion.poll(companionWorkspaceProof());
    if(storageReady&&!settingsEdit.open()&&!rebootSchedule.pending()&&
       uint32_t(hardwareNow-lastPhonePositionCheck)>=5000){
      lastPhonePositionCheck=uint32_t(hardwareNow);
      companion.sendLocalPosition(lastPhonePositionCheck,companionWorkspaceProof());
      bleCompanion.sendLocalPosition(lastPhonePositionCheck,companionWorkspaceProof());
    }
#if defined(CONFIG_MT_SERIAL_MODULE)
    if(serialModule.ready())serialCompanion.poll(companionWorkspaceProof());
#endif
#if defined(CONFIG_MT_TCP_COMPANION)
    tcpCompanion.poll(companionWorkspaceProof());
#endif
    const uint32_t cacheNow=k_uptime_get_32();
#if defined(CONFIG_MT_SATELLITE_HISTORY)
    pollSatelliteHistory(uint64_t(k_uptime_get()));
#if defined(CONFIG_MT_PEER_DIRECTORY)
    captureDirectory();
#endif
#endif
    nodeJournal.observe(service.nodes,cacheNow);
    if(storageReady&&journal.ready()&&nodeJournal.due(cacheNow)) {
      const bool safe=service.storageQuiescent()&&radio.ready()&&radio.health()==0;
      (void)nodeJournal.flush(settings,service.nodes,cacheNow,safe);
    }
#if defined(CONFIG_MT_DISPLAY)
    // An unavailable DTR read is conservatively shown as disconnected, which
    // matches ClientHost::serialStatus() and never changes transport admission.
    mt::ui::TelemetrySnapshot sensorView{};const uint64_t sensorViewNow=uint64_t(k_uptime_get());
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
    validateDisplayReports(sensorViewNow);
#endif
    meshtastic_ModuleConfig sensorConfig{};
    if(completionEdit.trusted()&&clientHost.moduleConfig(sensorConfig,meshtastic_ModuleConfig_telemetry_tag)){
      const auto &cfg=sensorConfig.payload_variant.telemetry;
      sensorView.showEnvironment=cfg.environment_screen_enabled;sensorView.showPower=cfg.power_screen_enabled;
      sensorView.showAirQuality=cfg.air_quality_screen_enabled;sensorView.showHealth=cfg.health_screen_enabled;sensorView.fahrenheit=cfg.environment_display_fahrenheit;
      const auto env=zt::telemetry::toMeshtasticEnvironmentMetrics(service.environment());
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
      if(cfg.environment_screen_enabled)mt::ui::telemetryReports().localEnvironment(env);
#endif
      if(env.has_temperature&&env.temperature>=-3276.8f&&env.temperature<=3276.7f){sensorView.hasTemperature=true;sensorView.temperatureTenthsC=int16_t(env.temperature*10);}
      if(env.has_relative_humidity&&env.relative_humidity>=0&&env.relative_humidity<=100){sensorView.hasHumidity=true;sensorView.humidityTenths=uint16_t(env.relative_humidity*10);}
      if(env.has_barometric_pressure&&env.barometric_pressure>=0&&env.barometric_pressure<=42949670.f){sensorView.hasPressure=true;sensorView.pressureHpa=uint32_t(env.barometric_pressure+0.5f);}
      const auto &power=service.power();
      for(unsigned index=0;index<3;++index){
        if(power.voltage[index]&&*power.voltage[index]>=0&&*power.voltage[index]<=65.535f){sensorView.hasPowerVoltage[index]=true;sensorView.powerMilliVolts[index]=uint16_t(*power.voltage[index]*1000);}
        if(power.current[index]&&*power.current[index]>=-32768.f&&*power.current[index]<=32767.f){sensorView.hasPowerCurrent[index]=true;sensorView.powerMilliAmps[index]=int16_t(*power.current[index]);}
      }
      mt::telemetry::CompletionTelemetry::Snapshot metrics{};
      if(completionRuntime.telemetry().snapshot(sensorViewNow,metrics)==mt::telemetry::CompletionTelemetry::Result::Ok){
        if(metrics.hasAir){const auto &m=metrics.air;
          if(m.has_pm25_standard&&m.pm25_standard<=65535){sensorView.hasPm25=true;sensorView.pm25UgM3=uint16_t(m.pm25_standard);}
          if(m.has_pm100_standard&&m.pm100_standard<=65535){sensorView.hasPm10=true;sensorView.pm10UgM3=uint16_t(m.pm100_standard);}
          if(m.has_co2&&m.co2<=65535){sensorView.hasCo2=true;sensorView.co2Ppm=uint16_t(m.co2);}
        }
        if(metrics.hasHealth){const auto &m=metrics.health;sensorView.hasHeartRate=m.has_heart_bpm;sensorView.heartBpm=m.heart_bpm;
          sensorView.hasSpO2=m.has_spO2;sensorView.spO2Percent=m.spO2;
          if(m.has_temperature&&m.temperature>=-3276.8f&&m.temperature<=3276.7f){sensorView.hasBodyTemperature=true;sensorView.bodyTemperatureC=m.temperature;}
        }
      }
      // Env/power presence was refreshed from the producer's 90s-limited
      // snapshots earlier in this same owner iteration; missing data stays absent.
      sensorView.sampledAtMs=sensorViewNow;
    }
    if(selectedPeerReady&&storageReady) {
      const int peerSlot=service.nodes.find(selectedPeer.nodeId);
      const auto *peer=peerSlot>=0?service.nodes.get(unsigned(peerSlot)):nullptr;
      if(peer&&!peer->placeholder()&&!peer->ignored())
        (void)mt::ui::projectPeerTelemetry(completionRuntime.telemetry(),selectedPeer,sensorViewNow,peerView);
    }
    mt::ui::telemetry(sensorView,sensorViewNow);
    const auto pairing=mt::ble::pairing();
    mt::ui::pairing(pairing.active,pairing.passkey,k_uptime_get_32());
    const auto gnssState=mt::gnss::diagnostics();
    const mt::ui::GnssView gnssView{zt::gnss::classifyDisplayState(mt::gnss::present(),gpsSettings.enabled&&!fixed,
      gnssState.ready,gnssState.fault,gpsPosition.has_latitude_i&&gpsPosition.has_longitude_i),gnssState.satellites};
    refreshCannedInput();
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
    mt::ui::setClockUtc(wallClock.at(uint64_t(k_uptime_get())));
#endif
    uiController.poll(service,radio.ready(),mt::ble::ready(),mt::ble::connected(),clientHost.serialStatus().connected,k_uptime_get_32(),clientHost.ready(),gnssView);
    meshtastic_ModuleConfig cannedConfig{};
    if(completionEdit.trusted()&&cannedInputAvailable()&&!settingsEdit.open()&&
       mt::modules::completionConfig(completionEdit.config(),moduleEdit.config(),notificationEdit.config(),extendedEdit.config(),cannedConfig,meshtastic_ModuleConfig_canned_message_tag)){
      uint8_t wire[128]{};auto encoded=pb_ostream_from_buffer(wire,sizeof(wire));
      if(pb_encode(&encoded,meshtastic_ModuleConfig_fields,&cannedConfig)&&
         (encoded.bytes_written!=cannedGpioWireSize||memcmp(wire,cannedGpioWire,encoded.bytes_written))){
        const auto result=cannedGpio.configure(cannedConfig.payload_variant.canned_message);
        if(result==mt::input::CannedGpioInput::Result::Ok||result==mt::input::CannedGpioInput::Result::Disabled){
          memcpy(cannedGpioWire,wire,encoded.bytes_written);cannedGpioWireSize=encoded.bytes_written;
        }
      }
      (void)cannedGpio.poll(k_uptime_get_32(),[](void *context,const mt::input::CannedGpioInput::Event &event){
        const auto &cfg=*static_cast<const meshtastic_ModuleConfig_CannedMessageConfig*>(context);
        if(!cfg.allow_input_source[0]||!strcmp(cfg.allow_input_source,"_any")||!strcmp(cfg.allow_input_source,event.source)){
          notifyPowerActivity();
          if(!dismissLocalNotificationInput()){
            mt::stock::ui::buzzer::input(event.code,event.uptimeMs);
            (void)uiController.input(event.code,event.uptimeMs);
          }
        }
      },&cannedConfig.payload_variant.canned_message);
    }else {cannedGpio.disable();cannedGpioWireSize=0;}
    pollCannedInput(k_uptime_get_32());
    // Random PIN authentication depends on a working display throughout the
    // session, not just its compile-time capability or successful boot probe.
    if(bluetoothConfigReady&&bluetoothJournal.config().enabled&&
       bluetoothJournal.config().mode==mt::BluetoothSettings::RandomPin&&!mt::ui::ready()){
      bluetoothConfigReady=false;
      (void)mt::ble::requestConfiguration(bluetoothJournal.config(),false);
    }
#endif
#if !defined(CONFIG_MT_DISPLAY) && DT_HAS_ALIAS(sw0)
    pollHeadlessButton(uint64_t(k_uptime_get()));
#endif
    mt::stock::ui::buzzer::poll(k_uptime_get_32());
    const bool radioIdle=initialized&&storageReady&&!persistentWorkPending()&&
      !atomic_get(&shutdownAdmissionClosed)&&k_msgq_num_used_get(&rxQueue)==0&&!rebootSchedule.pending()&&
      !firmwareUpdate.pending()&&service.storageStopSafe()&&!service.faulted()&&
      radio.ready()&&!radio.busy()&&radio.health()==1;
#ifdef CONFIG_MT_NORMAL_ENDPOINT
    agcSchedule.poll(k_uptime_get_32(),uint32_t(atomic_get(&agcLastGood)),radioIdle&&service.receiving());
#endif
    const bool dfuOwnerIdle=mt::board::HasUpdateTarget&&radioIdle&&!clientIngressSaturated&&ring_buf_size_get(&usbRx)==0;
#if !defined(CONFIG_MT_M119_TLS_TEST)
    radioNoiseFloor.poll(k_uptime_get_32(),[&] {return radioIdle&&service.receiving();},[] {
      const int16_t value=sx126x_get_rssi_inst(radioDevice);
      return sx126x_is_receiving(radioDevice)?int16_t(-128):value;
    });
#endif
    mt::ble::setDfuWindow(dfuOwnerIdle);
    if(dfuOwnerIdle&&mt::ble::takeDfuRequest()){
      const int result=firmwareUpdate.schedule(mt::update::Mode::NordicBleDfu);
      if(!result){rebootSchedule.request(1,uint64_t(k_uptime_get()),ScheduledOwnerAction::Dfu);updateFailureReported=false;}
    }
    notifications.configure(notificationEdit.config(),!completionRuntime.noticeMuted()&&storageReady&&notificationEdit.ready()&&mt::notification::available());
    notifications.poll(k_uptime_get_32());
    const auto notificationOutput=notifications.output();
    const bool heartbeatEnabled=storageReady&&notificationEdit.ready()&&!notificationEdit.config().heartbeatDisabled();
    beaconTargets.poll(k_uptime_get_32(),beaconSettingsEpoch(),service.route.identity(),
      initialized&&storageReady&&extendedEdit.trusted()&&!settingsEdit.open()&&!rebootSchedule.pending());
    if(ringtoneJournal.ready())completionRuntime.ringtone(ringtoneJournal.config().ringtone,ringtoneJournal.config().length());
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    completionRuntime.paxPermitted(bluetoothConfigReady&&bluetoothJournal.ready()&&wifiJournal.ready()&&mt::ble::paxRadioIdle(),
      bluetoothJournal.config().enabled,wifiJournal.config().enabled()||mt::network::wifiBootActive(),service.config.role()==2);
#endif
    completionRuntime.stage(completionEdit.config(),moduleEdit.config(),notificationEdit.config(),extendedEdit.config(),
      initialized&&storageReady&&completionEdit.trusted()&&!settingsEdit.open()&&!rebootSchedule.pending(),service.config.regionCode());
#if defined(CONFIG_MT_RANGE_TEST_LOG)
    meshtastic_ModuleConfig rangeConfig{};mt::modules::RangeTestLog::Config rangeLogConfig{};
    if(completionEdit.trusted()&&clientHost.moduleConfig(rangeConfig,meshtastic_ModuleConfig_range_test_tag)){
      const auto &r=rangeConfig.payload_variant.range_test;rangeLogConfig={r.enabled,r.save,r.clear_on_reboot};
    }
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
    (void)flushRemoteHardwarePins(k_uptime_get_32(),clientIngressSaturated);
#endif
    const auto hardwarePower=mt::hardware::snapshot(hardwareNow);
    mt::power::PowerPolicy::Activity moduleActivity{persistentWorkPending()||clientIngressSaturated||!service.storageStopSafe()||k_msgq_num_used_get(&rxQueue)!=0,
      radio.busy(),mt::ble::connected(),clientHost.serialStatus().connected};
    moduleActivity.externalPowerKnown=bool(hardwarePower.externalPower);
    moduleActivity.externalPower=hardwarePower.externalPower?*hardwarePower.externalPower:false;
#if defined(CONFIG_MT_POWER_STRESS)
    using mt::stock::powerstress::Action;
    static bool stressBluetoothOverride=false,stressTrace=false;
    static uint8_t previousPowerFlags=UINT8_MAX;
    const bool stressEnabled=storageReady && completionEdit.trusted() && !settingsEdit.open() &&
      !rebootSchedule.pending() && completionRuntime.powerMonitorEnabled();
    if(!stressEnabled){
      powerStressModule.reset();
      if(stressBluetoothOverride && bluetoothJournal.ready())
        (void)mt::ble::requestConfiguration(bluetoothJournal.config(),bluetoothConfigReady);
      stressBluetoothOverride=false;stressTrace=false;previousPowerFlags=UINT8_MAX;
    }
    const auto stress=powerStressModule.poll(k_uptime_get_32());
    if(stress.action==Action::BluetoothOff || stress.action==Action::BluetoothOn){
      auto temporary=bluetoothJournal.config();
      temporary.enabled=temporary.enabled && stress.action==Action::BluetoothOn;
      if(mt::ble::requestConfiguration(temporary,bluetoothConfigReady))
        stressBluetoothOverride=stress.action==Action::BluetoothOff;
    }
    if(stress.action==Action::Info)stressTrace=true;
    if(stressTrace){
      const uint8_t flags=(radio.ready()?1:0)|(radio.busy()?2:0)|(mt::ble::ready()?4:0)|
        (moduleActivity.bluetooth?8:0)|(moduleActivity.usb?16:0)|(moduleActivity.externalPower?32:0);
      if(flags!=previousPowerFlags || stress.action==Action::Info){
        char text[100];
        snprintf(text,sizeof(text),"uptime=%u ms radio=%u tx=%u BLE=%u connected=%u usb=%u external=%u",
               k_uptime_get_32(),unsigned(flags&1),unsigned((flags>>1)&1),unsigned((flags>>2)&1),
               unsigned((flags>>3)&1),unsigned((flags>>4)&1),unsigned((flags>>5)&1));
        if(companion.localModuleLog("PowerStress",text,companionWorkspaceProof()))previousPowerFlags=flags;
      }
    }
    if(stress.started || stress.action==Action::Mark){
      char text[24];snprintf(text,sizeof(text),"S:PS:%u",unsigned(stress.opcode));
      (void)companion.localModuleLog("PowerStress",text,companionWorkspaceProof());
    }
#endif
    k_mutex_unlock(&serviceLock);
#if defined(CONFIG_MT_DISPLAY)
    uiController.peerTelemetry(peerView);
#endif
#if defined(CONFIG_MT_RANGE_TEST_LOG)
    static bool rangeStarted=false,rangeTried=false;static uint32_t rangeRetryAt=0;
    const uint32_t rangeNow=k_uptime_get_32();
#if defined(CONFIG_MT_PHONE_FILES)
#if defined(CONFIG_MT_PHONE_PUBLIC_TREE)
    publicManifest.poll();
#endif
    phoneFiles.poll(rangeNow);
#endif
    if(!rangeStarted&&(!rangeTried||int32_t(rangeNow-rangeRetryAt)>=0)){
      const auto result=rangeLog.start(rangeLogConfig);rangeTried=true;rangeRetryAt=rangeNow+1000;
      // Unavailable means no unlink/write was attempted: an explicitly mounted
      // directory may appear after app startup. A failed clear is terminal for
      // this boot and is never retried, because its outcome can be uncertain.
      rangeStarted=result!=mt::modules::RangeTestLog::StartResult::Unavailable;
    }
    (void)rangeLog.poll(rangeLogConfig);
#endif
#if defined(CONFIG_MT_DROPZONE)
    dropzoneBoard.hardwarePoll(k_uptime_get_32());
#endif
#if defined(CONFIG_MT_PEER_DIRECTORY)
    // All file-backed peer prefetch runs after the service mutex is released.
    if(directoryBound&&!k_mutex_lock(&directoryLock,K_NO_WAIT)){
#if defined(CONFIG_MT_WARM_PEERS)
      static uint64_t retryWarmAt=0;
      const uint64_t tick=uint64_t(k_uptime_get());
      const bool warmAvailable=tick>=retryWarmAt;
      bindWarm();
      DirectoryOwner::Row evicted{};
      // When warm storage is faulty, the phone directory keeps accepting
      // fresh identities. Preserve its last durable handoff until recovery.
      (void)peerDirectory.poll(directoryVolume,directoryNodeGen,warmAvailable?&evicted:nullptr,
                               !warmAvailable||!peerDirectory.pendingDemotion().id());
#if defined(CONFIG_MT_PAGED_SATELLITES)
      static uint64_t retryReportsAt=0;
      if(reportPages.bound()&&tick>=retryReportsAt){
        const int err=reportPages.poll(peerDirectory,directoryVolume,reportVolume);
        if(err&&err!=-EAGAIN){retryReportsAt=tick+60000;reportPages.pauseReads();}
      }
#endif
      if(warmAvailable&&peerDirectory.ready()){
        const auto &pending=peerDirectory.pendingDemotion();
        if(pending.id()&&warmPeers.demotionSpace())warmPeers.demote(pending.user,mt::NodeCache::Entry::get32(pending.utc));
        const int result=warmPeers.poll(warmVolume);
        if(result){retryWarmAt=tick+60000;warmPeers.suspend();}
        if(warmPeers.absorbed()&&!peerDirectory.finishDemotion(directoryVolume))warmPeers.acknowledged();
      }
#else
      (void)peerDirectory.poll(directoryVolume,directoryNodeGen);
#endif
      k_mutex_unlock(&directoryLock);
    }
#endif
    completionRuntime.hardwarePoll(uint64_t(k_uptime_get()),moduleActivity);
    mt::hardware::setAdcMultiplierOverride(completionRuntime.adcMultiplierOverride());
    (void)mt::hardware::setInaBatteryAddress(completionRuntime.inaBatteryAddress());
#if defined(CONFIG_MT_SERIAL_MODULE)
    pollSerialProtoUnlocked();
#endif
    mt::notification::render(notificationOutput,heartbeatEnabled,k_uptime_get_32());
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
    // A pre-grant stack caller waits without Settings locks. Keep servicing
    // accepted RF work, but avoid a stack call that might wait for that caller.
    if(!mt::ble::store::waiting())mt::ble::poll();
#else
    mt::ble::poll();
#endif
#if defined(CONFIG_MT_RUNTIME_RESOURCES)
    runtimeResources.poll();
#endif
    if(explicitShutdownDue){
      auto currentActivity=moduleActivity;
      k_mutex_lock(&serviceLock,K_FOREVER);
      currentActivity.application=persistentWorkPending()||clientIngressSaturated||!service.storageStopSafe()||k_msgq_num_used_get(&rxQueue)!=0||mt::ble::pendingInput()!=0;
      currentActivity.radio=radio.busy();currentActivity.bluetooth=mt::ble::connected();
      currentActivity.usb=clientHost.serialStatus().connected;
      k_mutex_unlock(&serviceLock);
      const auto result=completionRuntime.executeRequestedPowerOff(currentActivity,true);
      explicitShutdownDue=false;
      if(result==mt::power::PowerPolicy::DeepSleepResult::NotQuiescent){
        rebootSchedule.request(1,uint64_t(k_uptime_get()),ScheduledOwnerAction::Shutdown);
      }else{
        rebootSchedule.clear();mt::ble::setIngressOpen(true);atomic_set(&shutdownAdmissionClosed,0);
        if(result!=mt::power::PowerPolicy::DeepSleepResult::BackendFailed)
          printk("Poweroff request unavailable or failed: %u\n",unsigned(result));
        else printk("Poweroff action returned unexpectedly\n");
      }
    }else if(completionRuntime.powerState()==mt::power::PowerPolicy::State::DeepSleepPending||
             completionRuntime.powerState()==mt::power::PowerPolicy::State::BatteryShutdownPending){
      auto currentActivity=moduleActivity;
      k_mutex_lock(&serviceLock,K_FOREVER);
      currentActivity.application=persistentWorkPending()||clientIngressSaturated||!service.storageStopSafe()||k_msgq_num_used_get(&rxQueue)!=0;
      currentActivity.radio=radio.busy();currentActivity.bluetooth=mt::ble::connected();
      currentActivity.usb=clientHost.serialStatus().connected;
      k_mutex_unlock(&serviceLock);
      (void)completionRuntime.executePowerAction(currentActivity);
    }
    if(channel>=0)(void)wdt_feed(watchdog,channel);
#if defined(CONFIG_MT_POWER_STRESS)
    if(stress.action==Action::Busy && stress.remainingMs){
      k_busy_wait(1000u*(stress.remainingMs<5?stress.remainingMs:5));
      k_yield();continue;
    }
    if(stress.action==Action::Sleep && stress.remainingMs>=1000){
      const auto backend=mt::power::ZephyrPowerBackend::instance().timedBackend();
      const uint32_t seconds=stress.remainingMs/1000;
      if(backend.wakePathReady(backend.context,seconds) && backend.prepare(backend.context)){
        if(backend.quiescent(backend.context))(void)backend.enter(backend.context,seconds);
        backend.resume(backend.context);
      }
    }
#endif
    k_sleep(K_MSEC(5));
  }
}
