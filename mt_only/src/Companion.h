// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if defined(CONFIG_MT_PAGED_SATELLITES)
#include "companion/SatelliteWire.h"
#endif
#include "companion/NoticeText.h"
#include "PeerSnr.h"
#include "Service.h"
#include "BoardIdentity.h"
#include "UsbSessionEpoch.h"
#include "Version.h"
#include "stock/modules/StatusMessageModule.h"
#include "NeighborInfoPayload.h"
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
#include "modules/StoreForwardLocalReplay.h"
#endif
#if defined(CONFIG_MT_GPIO_MODULES)
#include "modules/GpioModuleChannels.h"
#endif
#include "RegionCatalogue.h"
#if defined(CONFIG_MT_RF_ADMIN_GET)
#include "admin/AdminAuthorization.h"
#include "admin/RemoteAdminProtocol.h"
#include "admin/RemoteAdminRedaction.h"
#include "admin/AdminSessionPasskeys.h"
#include "admin/RemoteAdminMutationProtocol.h"
#include "admin/LegacyAdminSession.h"
#endif
#include "admin/CompanionWorkspace.h"
#include "ExtendedDiagnostics.h"
#include "meshtastic/admin.pb.h"
#include "OwnerContact.h"
#include "ui/UiConfigSettings.h"
#include "security/ApiLogBuffer.h"
#include "admin/HamModeProjection.h"
#include "pki/verification/KeyVerification.h"
#include <pb_decode.h>
#include <pb_encode.h>
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
#include "pki/UsbProvisionCommand.h"
#endif

namespace mt {
namespace detail {
  struct VolatileWipe {
    void *value;size_t size;
    ~VolatileWipe(){volatile uint8_t *p=static_cast<volatile uint8_t*>(value);while(size--)*p++=0;}
  };
  template<typename Enum> static uint32_t enumNumber(const Enum &value) {
    static_assert(sizeof(Enum)==1||sizeof(Enum)==2||sizeof(Enum)==4);
    // A width-matched integer keeps this valid with target -fshort-enums and
    // either byte order. Taking the address never loads an invalid enum value.
    if constexpr(sizeof(Enum)==1){uint8_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
    else if constexpr(sizeof(Enum)==2){uint16_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
    else {uint32_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
  }
}
// USB and encrypted, nonbonding BLE companion administration. BLE JustWorks
// does not authenticate owner identity. The opt-in RF Admin slice is narrowly
// gated to authenticated allowlisted queries and passkey-bound typed mutations. Local network
// credentials use the host's independent journal and are never echoed over RF.
class Companion {
public:
  struct Host {
    static constexpr int EditCommittedDegraded=1;
    // A Router role transition was accepted, but one or more upstream
    // defaults have no representation in this build's persisted settings.
    static constexpr int DeviceDefaultsPartial=2;
    static constexpr int BluetoothDeferred=1;
    struct Security {bool connected=false,pending=false;int request=0;uint8_t level=0,error=0;};
    virtual bool send(const uint8_t *,size_t)=0;
    virtual bool sendIfQueueEmpty(const uint8_t *,size_t){return false;}
    // Ordinary owner-0 console traffic can be closed by persistent policy or
    // an unknown Config. Narrow recovery replies retain a separate root-owned
    // transport epoch and must not reopen the ordinary byte stream.
#if defined(CONFIG_MT_PHONE_FILES)
    virtual int appFileInfo(uint8_t,meshtastic_FileInfo &){return 0;}
    virtual void appFileInfoSent(uint8_t){}
    virtual bool appFileSubmit(uint8_t,const meshtastic_XModem &,uint32_t){return false;}
    virtual bool appFileResponse(uint8_t,meshtastic_XModem &){return false;}
    virtual void appFileSent(uint8_t){}
    virtual void appFileDisconnect(uint8_t){}
#endif
    virtual bool consoleAllowed()const{return true;}
    virtual bool sendRecovery(const uint8_t *data,size_t size){return send(data,size);}
    virtual int save(const Config &)=0;
    // RF Admin profile changes must first persist a candidate while the
    // current radio profile remains live for its correlated response. The
    // owner applies it only after that response has completed transmission.
    virtual int saveRemoteConfig(const Config &,uint32_t){return -ENOTSUP;}
    // RF LoRa/profile changes stage both the core Config and derived MQTT
    // region-root/consent metadata before the old-profile response.
    virtual int saveRemoteLoRa(const Config &,bool,uint32_t){return -ENOTSUP;}
    // RF DeviceConfig may change role/rebroadcast policy as well as the
    // heartbeat setting. The owner must durably prepare the complete
    // multi-journal candidate before replying, then apply it after the old
    // profile's correlated response is transmitted.
    virtual int saveRemoteDevice(const Config &,bool,uint32_t){return -ENOTSUP;}
    // Security controls share the typed Config journal. A legacy host may
    // change serial/signature policy while unmanaged, but must explicitly
    // implement the authority/recovery checks needed to enable managed mode.
    virtual int saveSecurityControls(const Config &config,uint32_t){
      return config.isManaged()?-ENOTSUP:save(config);
    }
    virtual bool safeApiLogEnabled()const{return false;}
    virtual void safeApiLogSession(bool,bool){}
    virtual bool nextSafeApiLog(bool,meshtastic_LogRecord &out,uint32_t &dropped){out={};dropped=0;return false;}
    virtual bool heartbeatDisabled()const{return true;}
    virtual int saveDevice(const Config &config,bool disabled,uint32_t){
      return disabled?save(config):-ENOTSUP;
    }
    // The stock setter accepts deprecated enum aliases but stores their
    // canonical role and may apply a role-specific module side effect.
    virtual int saveDeviceDeprecatedRole(const Config &config,bool disabled,uint32_t now){
      return saveDevice(config,disabled,now);
    }
    virtual bool wifi()const{return false;}
    // MQTT settings are separately journaled. These callbacks expose only
    // explicit consent and typed config; production supplies them only in the
    // WiFi MQTT-capable build.
    virtual bool mqttAvailable()const{return false;}
    virtual bool mqttRegionRootRetryPending()const{return false;}
    virtual bool mqttRuntimeSupported()const{return false;}
    virtual bool mqttTrustReady()const{return false;}
    virtual bool configOkToMqtt()const{return false;}
    struct MqttProxyPolicy {
      bool mqttEnabled=false,proxyToClientEnabled=false,brokerConfigured=false;
      bool tlsEnabled=false,encryptionEnabled=false;
      const char *root=nullptr;
    };
    // No broker credentials or TLS claims are exposed. In proxy mode the
    // phone owns the broker session; Companion only sees persisted policy.
    virtual bool mqttProxyPolicy(MqttProxyPolicy &out)const{out={};return false;}
    virtual bool takeMqttProxyPublish(meshtastic_MqttClientProxyMessage &out){out={};return false;}
    virtual int saveConfigOkToMqtt(bool,uint32_t){return -ENOTSUP;}
    virtual int saveMqttRegionRootTopic(const char *,size_t,bool configOkToMqtt,uint32_t){
      return configOkToMqtt?-ENOTSUP:0;
    }
    virtual int saveRemoteMqttRegionRootTopic(const char *region,size_t length,bool configOk,uint32_t now){
      return saveMqttRegionRootTopic(region,length,configOk,now);
    }
    virtual bool channelUplinkEnabled(const Config&,unsigned)const{return false;}
    virtual int saveChannelUplink(const Config&,unsigned,bool,uint32_t){return -ENOTSUP;}
    virtual int saveRemoteChannelUplink(const Config &config,unsigned index,bool enabled,uint32_t now){
      return saveChannelUplink(config,index,enabled,now);
    }
    // RF channel identity and explicit MQTT consent are prepared together so
    // consent cannot remain attached to the replaced channel identity.
    virtual int saveRemoteChannel(const Config&,unsigned,bool,uint32_t){return -ENOTSUP;}
    virtual bool wifiConnection(meshtastic_WifiConnectionStatus &out)const{out={};return false;}
    virtual bool powerConfig(meshtastic_Config_PowerConfig &out)const{out={};return true;}
    virtual int savePowerConfig(const meshtastic_Config_PowerConfig &,uint32_t){return -ENOTSUP;}
    virtual bool networkConfig(meshtastic_Config_NetworkConfig &out)const{out={};return true;}
    virtual int saveNetworkConfig(const meshtastic_Config_NetworkConfig &,uint32_t){return -ENOTSUP;}
#if defined(CONFIG_MT_RF_ADMIN_GET)
    // An independent PUBLIC Admin-key allowlist, never inferred from peers.
    virtual bool adminKeys(meshtastic_Config_SecurityConfig &out)const{out={};return true;}
    virtual int saveAdminKeys(const meshtastic_Config_SecurityConfig &,uint32_t){return -ENOTSUP;}
    // Fresh hardware entropy only. No session-passkey persistence or logging.
    virtual bool adminPasskey(uint8_t *,size_t){return false;}
    // Monotonic millisecond clock used only for RAM-only private Admin sessions.
    virtual uint32_t adminSessionNow()const{return 0;}
#endif
    // Compiled module behavior is separate from disabled compatibility stubs.
    virtual uint32_t supportedModules()const{return 0;}
    virtual void nodeInfoPing(uint32_t){}
    virtual bool gnssPresent()const{return false;}
    virtual void boardMetadata(meshtastic_DeviceMetadata &)const{}
    virtual void boardMyInfo(meshtastic_MyNodeInfo &,bool)const{}
    virtual bool moduleConfig(meshtastic_ModuleConfig &,unsigned)const{return false;}
    virtual int saveModuleConfig(const meshtastic_ModuleConfig &,uint32_t){return -ENOTSUP;}
    virtual int saveRemoteModuleConfig(const meshtastic_ModuleConfig &value,uint32_t now){
      return saveModuleConfig(value,now);
    }
#if defined(CONFIG_MT_RF_ADMIN_GET)
    // Stages canonical correlated remote GPIO inventory in bounded RAM only.
    // Peer key/config references are borrowed for this call; copy only the
    // bounded typed descriptors. The owner loop journals later, outside RF RX.
    virtual bool stageRemoteHardwarePins(uint32_t,const uint8_t[32],
        const meshtastic_ModuleConfig_RemoteHardwareConfig &,uint32_t){return false;}
#endif
    // The stock canned-list Admin API is separate from ModuleConfig tag 7.
    // Getter returns a fixed, NUL-terminated UTF-8 list of at most 200 bytes.
    virtual int ringtone(char*,size_t)const{return -ENOTSUP;}
    virtual int saveRingtone(const char*,size_t,uint32_t){return -ENOTSUP;}
    virtual int cannedMessageMessages(char *out,size_t capacity)const{
      if(out&&capacity)out[0]='\0';
      return -ENOTSUP;
    }
    virtual int saveCannedMessageMessages(const char *,size_t,uint32_t){return -ENOTSUP;}
    // Stock local Admin device-UI settings. Providers should load the new
    // mtui two-slot journal and may supply a previously loaded legacy value;
    // saving never mutates or removes that legacy record.
    virtual int uiConfig(meshtastic_DeviceUIConfig &out)const {
      out=ui::UiConfigSettings::defaults();return -ENOTSUP;
    }
    virtual int saveUiConfig(const meshtastic_DeviceUIConfig &,uint32_t){return -ENOTSUP;}
    virtual int hardwarePinInventory(meshtastic_NodeRemoteHardwarePinsResponse &out)const {
      out={};return -ENOTSUP;
    }
    virtual int preferences(pb_size_t,uint32_t,UsbSessionEpoch::RestoreReceipt *){return -ENOTSUP;}
    virtual bool sendRestoreReceipt(UsbSessionEpoch::RestoreReceipt &,uint32_t,const uint8_t *,size_t){return false;}
    virtual int removeAppFile(const char *){return -ENOTSUP;}
    virtual int requestFactoryDeviceReset(){return -ENOTSUP;}
    virtual int requestFactoryConfigReset(){return -ENOTSUP;}
    virtual int resetNodeDb(bool,uint32_t){return -ENOTSUP;}
    virtual int resumePreferences(uint32_t){return -ENOTSUP;}
    // Root owns one device-wide KeyVerification runtime and forwards canonical
    // local Admin requests here. Per-transport Companion instances must not
    // each create their own RF verification session.
    virtual int keyVerificationAdmin(const meshtastic_KeyVerificationAdmin &,uint32_t){return -ENOTSUP;}
    virtual void rangeObservation(uint32_t,uint32_t,const uint8_t*,size_t,int16_t,float,bool,uint8_t,uint32_t){}
    virtual void positionForGeofence(uint32_t,uint32_t,unsigned,bool,const uint8_t *,size_t) {}
    virtual void waypointPacket(uint32_t,uint32_t,uint32_t,unsigned,bool,bool,const uint8_t *,size_t) {}
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
    virtual void displayReport(uint32_t,uint32_t,bool,const meshtastic_Telemetry &) {}
#endif
    virtual void peripheralPacket(uint32_t,uint32_t,uint32_t,unsigned,unsigned,
                                  const uint8_t *,size_t,bool){}

    // Local observation read only: zero includes a valid empty neighbor list.
    virtual int localNeighbors(meshtastic_NeighborInfo &){return -ENOTSUP;}
#if defined(CONFIG_MT_PEER_DIRECTORY)
#if defined(CONFIG_MT_PAGED_SATELLITES)
    virtual void satelliteReceived(uint32_t,unsigned,unsigned,companion::SatelliteReplay::Observation,const uint8_t*,size_t){}
    virtual int satellitePacket(unsigned,unsigned,unsigned,companion::SatelliteSample &){return 0;}
#endif
    virtual unsigned directoryLimit()const{return 0;}
    virtual unsigned directoryCount(const NodeCache &hot)const{return hot.count();}
    virtual int directoryPeer(unsigned,unsigned,const NodeCache &,NodeCache::Entry &){return 0;}
    virtual void directoryObserved(const NodeCache::Entry &,uint32_t=0){}
    virtual void directoryDisconnect(unsigned){}
    virtual uint32_t directoryRevision()const{return 0;}
    virtual int promoteDirectoryFavorite(unsigned,uint32_t,uint32_t){return -ENOENT;}
#endif
    virtual bool ready() const=0;
    // The framed USB path supplies its captured CDC generation here. The
    // production host serializes this acceptance check with the RX ISR; after
    // it returns true the request owns that position in the session ordering.
    // Packet transports and memory-only hosts have no CDC generation to bind.
    virtual bool acceptTransportFrame(uint32_t) {return true;}
    // USB Serial/JTAG has no DTR lifecycle. Its production host returns true
    // only after atomically clearing pre-handshake output for a canonical fresh
    // handshake; Companion then clears parser and replay ownership.
    virtual bool beginUnmeasuredHandshakeBoundary() {return false;}
    virtual void completeUnmeasuredHandshakeBoundary(uint32_t){}
    // One RAM clock shared by the local transports. Never change uptime timers.
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS))
    virtual int sensorAdmin(const meshtastic_SensorConfig &,uint32_t,uint32_t){return -ENOTSUP;}
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
    virtual int scaleAdmin(uint32_t,uint32_t,uint32_t){return -ENOTSUP;}
#endif
    virtual bool sensorAdminResult(uint32_t,uint32_t &,int &){return false;}
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
    virtual int oneWireAdmin(unsigned){return -ENOTSUP;}
#endif
#if defined(CONFIG_MT_SHTXX_ADMIN)
    virtual int shtAccuracy(const meshtastic_SensorConfig &){return -ENOTSUP;}
#endif
    virtual int setWallTime(uint32_t) {return -ENOTSUP;}
    virtual uint32_t wallTime()const {return 0;}
#if defined(CONFIG_MT_POWER_STRESS)
    virtual int powerStress(uint32_t,const uint8_t *,size_t){return -ENOTSUP;}
#endif
    // Fresh monotonic seconds; unavailable is distinct from a valid zero.
    virtual bool uptimeSeconds(uint32_t &seconds)const {seconds=0;return false;}
    // Optional RAM-only scalar diagnostics; zero means no record.
    virtual size_t channelDiagnostic(char *,size_t)const {return 0;}
    virtual size_t extendedStorageDiagnostic(char *,size_t)const {return 0;}
#if defined(CONFIG_MT_M119_TLS_TEST)
    virtual size_t mqttTlsTestDiagnostic(char *,size_t)const {return 0;}
#endif
    virtual int beginEdit(uint32_t) {return -ENOTSUP;}
    virtual int commitEdit(uint32_t) {return -ENOTSUP;}
    virtual bool editing() const {return false;}
    struct Serial {bool connected=false;uint32_t baud=0;bool available=false;};
    virtual Serial serialStatus() const {return {};}
    virtual int editNode(uint32_t,NodeCache::Edit,uint32_t) {return -ENOTSUP;}
    // Typed local owner request only. Production must commit the bounded
    // contact through its serialized durable owner path; legacy hosts refuse.
    // An explicit owner key may be durably saved before a separate display
    // metadata write fails. This is an error with a committed side effect.
    static constexpr int ContactPartiallyCommitted=2;
    virtual int addOwnerContact(const OwnerContactCandidate &,uint32_t) {return -ENOTSUP;}
#if defined(CONFIG_MT_RF_ADMIN_GET)
    virtual int editRemoteFavorite(uint32_t,bool,uint32_t) {return -ENOTSUP;}
#endif
    // Volatile local display input only. Headless compositions refuse.
    virtual int uiInput(uint32_t,uint32_t) {return -ENOTSUP;}
    // Schedule only; the main loop owns RF quiescence and the actual reset.
    virtual bool scheduleReboot(int32_t) {return false;}
    // Local canonical Admin control requests. The Host schedules work only;
    // the main loop owns native bootloader/update/shutdown execution.
    virtual int scheduleFirmwareUpdate(const meshtastic_AdminMessage_OTAEvent &,uint32_t){return -ENOTSUP;}
    virtual int enterDfu(uint32_t){return -ENOTSUP;}
    virtual bool scheduleShutdown(int32_t){return false;}
    // Reserved local USB identity restore. The callback returns only a status;
    // Companion never accepts or displays text derived from the private input.
    virtual int restoreExistingIdentity(const uint8_t *,size_t,uint32_t){return -ENOTSUP;}
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    virtual bool stockIdentityBleLink()const{return false;}
    virtual int stageStockIdentity(const uint8_t *,const uint8_t *,bool,uint32_t){return -ENOTSUP;}
#endif
    virtual int freshProvision(const uint8_t *,size_t,uint32_t,char *,size_t){return -ENOTSUP;}
    virtual void freshProvisionDisconnect(){}
    // Explicit experimental USB-local provisioning. Defaults perform no work;
    // BLE hosts must refuse and raw/private identity bytes are never returned.
    virtual int provision(const uint8_t *,size_t,uint32_t,char *,size_t) {return -ENOTSUP;}
    virtual void provisionDisconnect() {}
#if defined(CONFIG_MT_RUNTIME_RESOURCES)
    virtual size_t runtimeResources(char *,size_t) {return 0;}
#endif
    virtual bool bluetooth() const {return false;}
    // Capability, configured policy and negotiated link security are separate.
    // Legacy/headless hosts retain their existing read-only NO_PIN response.
    virtual bool bluetoothConfig(meshtastic_Config_BluetoothConfig &out) const {
      out={};out.enabled=bluetooth();
      out.mode=meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;return true;
    }
    virtual int saveBluetoothConfig(const meshtastic_Config_BluetoothConfig &,uint32_t) {return -ENOTSUP;}
    // BluetoothDeferred transfers completion ownership without confirming a
    // durable save. Legacy hosts keep their synchronous setter unchanged.
    virtual int saveBluetoothConfigRequest(const meshtastic_Config_BluetoothConfig &value,uint32_t now,
                                          uint32_t,bool,bool) {return saveBluetoothConfig(value,now);}
    virtual Security bluetoothSecurity() const {return {};}
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
    virtual size_t bluetoothDiagnostic(char *,size_t) const {return 0;}
#endif
    virtual size_t gnssDiagnostic(char *,size_t) const {return 0;}
    virtual int localEnvironment(meshtastic_EnvironmentMetrics &out){out={};return -ENOTSUP;}
    virtual bool extraTelemetry(uint8_t,uint32_t,uint8_t*,size_t,size_t&){return false;}
    virtual int localPower(zt::telemetry::PowerReadings &out){out={};return -ENOTSUP;}
    virtual void observeRadioPeer(uint32_t,unsigned,bool,float,uint32_t){}
    virtual void observeRadioNeighborInfo(uint32_t from,unsigned hops,bool snrKnown,float snr,uint32_t now,
                                          const meshtastic_NeighborInfo&){
      observeRadioPeer(from,hops,snrKnown,snr,now);
    }
    virtual void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool) {}
  };
private:
  Service &service_;Host &host_;companion::CompanionWorkspace &workspace_;uint8_t model_,owner_;
  uint8_t frame_[512]{};size_t length_=0,used_=0;unsigned state_=0;uint32_t frameEpoch_=0;
  uint32_t lastByte_=0,sequence_=0;bool connected_=false,recoveryOnly_=false,recoveryReply_=false,acceptedUsbFrame_=false;
  static constexpr uint32_t OnlyConfig=69420,OnlyNodes=69421;
  enum class HandshakePhase:uint8_t {Idle,MyInfo,UiData,Metadata,Region,Channels,Configs,Modules,LocalNode,Nodes,Diagnostics,QueueStatus,
#if defined(CONFIG_MT_PHONE_FILES)
    Files,
#endif
    Complete};
  HandshakePhase handshakePhase_=HandshakePhase::Idle;
  unsigned handshakeIndex_=0;
#if defined(CONFIG_MT_PEER_DIRECTORY)
  unsigned directoryCursor_=0,directoryEmitted_=0;
  uint32_t favoriteRequest_=0,favoriteNode_=0,favoriteRevision_=0,favoriteAt_=0;
#endif
  uint32_t handshakeNonce_=0,handshakeNow_=0;
  bool configComplete_=false,positionSentToPhone_=false;
  uint32_t lastPositionToPhoneMs_=0;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  uint32_t localHistorySession_=0;
  modules::StoreForwardLocalReplay localHistory_;
  struct LocalHistoryReader {const modules::StoreForwardArchive *archive=nullptr;} localHistoryReader_;
#endif
  using Satellite=companion::SatelliteReplay;
  Satellite::Kind replayKind_=Satellite::Done;
#if defined(CONFIG_MT_PAGED_SATELLITES)
  unsigned archiveCursor_=0;
#endif
  uint32_t statusReplayPeers_[NodeCache::Capacity]{};unsigned statusReplayNext_=0,statusReplayCount_=0;
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS))
  uint32_t sensorSession_=0;
#endif
  bool apiLogSessionActive_=false;
  meshtastic_ToRadio &input_;meshtastic_FromRadio &output_;
  meshtastic_AdminMessage &admin_,&reply_;
  struct RecoveryReplyScope {
    bool &flag;bool previous;
    explicit RecoveryReplyScope(bool &value):flag(value),previous(value){flag=true;}
    ~RecoveryReplyScope(){flag=previous;}
  };
#if defined(CONFIG_MT_RF_ADMIN_GET)
  admin::LegacyAdminSession legacyAdminSession_{};
  uint32_t legacyAdminDomainEpoch_=0,legacyAdminChannelHash_=0;
  uint8_t legacyAdminChannelIndex_=0xff;
  bool legacyAdminScopeKnown_=false;
  admin::AdminSessionPasskeys adminPasskeys_{};
#endif
  // Sticky, numeric-only diagnostics for the last failed local LoRa save.
  // Kept across transport reconnects; never contains packets or settings.
  StoragePauseDiagnostic lastLoRaPause_{};int lastLoRaSaveResult_=0;
  uint32_t loRaSaveFailures_=0;bool lastLoRaPauseFresh_=false;

  // MQTT phone-proxy ingress is decoded while the shared Companion workspace
  // is leased, but Service may synchronously fan the accepted private frame
  // back into USB and BLE. Keep only this bounded, pointer-free admission
  // result across the lease boundary; Service processing itself runs after
  // the shared protobuf scratch has been released.
  struct DeferredProxy {
    mqtt::MqttServiceEnvelopeDecoder::Decoded envelope{};
    mqtt::MqttDownlinkAdmission::Context context{};
    char gateway[10]{};
    uint32_t now=0,random=0;
    bool active=false;
  };

  struct Seen {uint32_t from=0,id=0,at=0;};Seen seen_[64]{};unsigned nextSeen_=0;
  // Successful non-idempotent mute toggles only. The bounded transport-local
  // stream epoch prevents a lost response from flipping the same peer twice.
  static constexpr uint8_t MuteToggleReplay=1;
  struct AdminReplay {uint32_t request=0,target=0,from=0;uint8_t operation=0;bool degraded=false;};AdminReplay muteReplay_[8]{};unsigned nextMuteReplay_=0;
  void fresh(pb_size_t tag) {output_={};output_.id=++sequence_;output_.which_payload_variant=tag;}
  size_t encodeOutput(uint8_t *bytes,size_t capacity){
    auto s=pb_ostream_from_buffer(bytes+4,capacity-4);
    if(!pb_encode(&s,meshtastic_FromRadio_fields,&output_)){++encodeErrors;return 0;}
    bytes[0]=0x94;bytes[1]=0xc3;bytes[2]=uint8_t(s.bytes_written>>8);bytes[3]=uint8_t(s.bytes_written);
    return s.bytes_written+4;
  }
  bool emit(bool countDrop=true,bool idleOnly=false) {
    const bool recoveryOutput=owner_==0&&(recoveryOnly_||!host_.consoleAllowed());
    if(recoveryOutput&&!recoveryReply_){if(countDrop)++outputDrops;return false;}
    uint8_t bytes[516];detail::VolatileWipe wipeBytes{bytes,sizeof(bytes)};
    const auto size=encodeOutput(bytes,sizeof(bytes));if(!size)return false;
    const bool sent=recoveryOutput?host_.sendRecovery(bytes,size):
      (idleOnly?host_.sendIfQueueEmpty(bytes,size):host_.send(bytes,size));
    if(!sent){if(countDrop)++outputDrops;return false;}return true;
  }
  void pollSafeApiLog() {
    const bool transportAllowed=owner_<=1;
    const bool ready=transportAllowed&&connected_&&configComplete_&&host_.safeApiLogEnabled()&&
      (owner_!=0||host_.consoleAllowed());
    if(apiLogSessionActive_&&!ready) {
      host_.safeApiLogSession(owner_==1,false);apiLogSessionActive_=false;
    } else if(!apiLogSessionActive_&&ready) {
      host_.safeApiLogSession(owner_==1,true);apiLogSessionActive_=true;
    }
    if(!apiLogSessionActive_)return;
    meshtastic_LogRecord record{};uint32_t dropped=0;
    if(!host_.nextSafeApiLog(owner_==1,record,dropped))return;
    (void)dropped; // Per-consumer overwrite counts are deliberately not log events.
#if defined(CONFIG_MT_LOCAL_CLOCK)
    record.time=service_.config.localClock(record.time);
#endif
    fresh(meshtastic_FromRadio_log_record_tag);output_.log_record=record;
    (void)emit(); // A failed transport send drops this record and charges outputDrops.
  }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  static bool historySnapshot(void *context,modules::StoreForwardLocalReplay::ArchiveView &out){
    const auto *archive=static_cast<LocalHistoryReader*>(context)->archive;
    if(!archive||!archive->ready())return false;
    out.epoch=archive->epoch();out.count=archive->size();return true;
  }
  static const modules::StoreForwardArchive::Record *historyRecord(void *context,size_t index){
    const auto *archive=static_cast<LocalHistoryReader*>(context)->archive;
    return archive?archive->recordAt(index):nullptr;
  }
  static bool emitHistory(void *context,const meshtastic_MeshPacket &packet){
    auto *self=static_cast<Companion*>(context);
    if(!self||!self->connected_||!self->configComplete_)return false;
    self->fresh(meshtastic_FromRadio_packet_tag);self->output_.packet=packet;
    return self->emit(false);
  }
  void replayLocalHistory(){
    const auto *archive=service_.storeForwardArchive();
    if(!connected_||!configComplete_||handshakeNonce_==OnlyConfig||!host_.ready()||!archive){localHistoryReader_.archive=nullptr;localHistory_.clear();return;}
    localHistoryReader_.archive=archive;
    modules::StoreForwardLocalReplay::Reader reader{&localHistoryReader_,&historySnapshot,&historyRecord};
    meshtastic_MeshPacket packet{};
    (void)localHistory_.poll(true,connected_,configComplete_,
      replayKind_!=Satellite::Done,localHistorySession_,service_.route.identity(),reader,
      &emitHistory,this,packet);
  }
#endif
  static bool decodeNeighborInfoForObservation(const uint8_t *payload,size_t length,
                                               meshtastic_NeighborInfo&out){
    if((!payload&&length)||length>233)return false;
    const uint8_t empty=0;if(!payload)payload=&empty;
    meshtastic_NeighborInfo candidate{};
    auto stream=pb_istream_from_buffer(payload,length);
    if(!pb_decode(&stream,meshtastic_NeighborInfo_fields,&candidate)||stream.bytes_left||
       candidate.neighbors_count>sizeof(candidate.neighbors)/sizeof(candidate.neighbors[0]))return false;
    out=candidate;return true;
  }
  void observeRadioPacket(uint32_t from,unsigned hops,bool snrKnown,float snr,uint32_t now,
                          meshtastic_PortNum port,const uint8_t *payload,size_t length,
                          bool radioReceive,bool viaMqtt,uint8_t relay,uint32_t utc,bool hopsKnown){
    if(!radioReceive)return;
    if(port==meshtastic_PortNum_NEIGHBORINFO_APP){
      if(viaMqtt)return; // Stock excludes MQTT-origin NeighborInfo.
      meshtastic_NeighborInfo info{};
      if(decodeNeighborInfoForObservation(payload,length,info)) {
        // This is the local topology observation. Delivery retains the sender's wire payload.
        info.last_sent_by_id=stock::NeighborInfoModule::lastTransmitter(service_.nodes,service_.route.identity(),
          from,hopsKnown?int(hops):-1,relay,utc,info);
        host_.observeRadioNeighborInfo(from,hops,snrKnown,snr,now,info);
      } else host_.observeRadioPeer(from,hops,snrKnown,snr,now); // Stock null-decode direct fallback.
      return;
    }
    if(radioReceive)host_.observeRadioPeer(from,hops,snrKnown,snr,now);
  }
  bool xiaoWithoutExternalLna() const {
#if defined(CONFIG_BOARD_XIAO_ESP32S3) || !defined(__ZEPHYR__)
    // The supported XIAO uses the Wio-SX1262 kit, with no external FEM LNA control.
    return board::capabilityModel(model_)==meshtastic_HardwareModel_SEEED_XIAO_S3;
#else
    return false;
#endif
  }
  void user(meshtastic_User &u) const {
    u={};snprintf(u.id,sizeof(u.id),"!%08x",service_.route.identity());
    auto r=service_.config.node();memcpy(u.long_name,r.bytes+bench::MtNodeRecord::Name,24);
    memcpy(u.short_name,r.bytes+bench::MtNodeRecord::Short,4);
    u.hw_model=static_cast<meshtastic_HardwareModel>(model_);
    u.role=static_cast<meshtastic_Config_DeviceConfig_Role>(service_.config.role());
    u.is_licensed=service_.config.licensedOperation();
    uint8_t publicKey[32];
    if(service_.privatePublicIdentity(publicKey)){u.public_key.size=32;memcpy(u.public_key.bytes,publicKey,32);}
    u.has_is_unmessagable=true;u.is_unmessagable=service_.config.ownerUnmessagable();
  }
  void connectionStatus(meshtastic_DeviceConnectionStatus &status) const {
    const auto serial=host_.serialStatus();status.has_serial=serial.available;
    if(serial.available){status.serial.is_connected=serial.connected;status.serial.baud=serial.baud;}
    status.has_bluetooth=host_.bluetooth();
    if(status.has_bluetooth)status.bluetooth.is_connected=host_.bluetoothSecurity().connected;
    status.has_wifi=host_.wifiConnection(status.wifi);
    // Unmeasured RSSI and unavailable Ethernet remain absent/default.
  }
  void metadata(meshtastic_DeviceMetadata &m) const {
    m={};
    const auto &version = firmwareVersion();
    static_assert(sizeof(version) <= sizeof(m.firmware_version));
    memcpy(m.firmware_version, version, sizeof(version));
    uint8_t publicKey[32];m.hasPKC=service_.privatePublicIdentity(publicKey);
    m.has_xeddsa=m.hasPKC;
    m.position_flags=service_.config.gnssSettings().positionFlags;
    host_.boardMetadata(m);
    m.device_state_version=1;m.hasBluetooth=host_.bluetooth();m.hasWifi=host_.wifi();
    m.hw_model=static_cast<meshtastic_HardwareModel>(model_);
    m.role=static_cast<meshtastic_Config_DeviceConfig_Role>(service_.config.role());
    // Stock metadata reports absent modules separately from disabled getter
    // records. SERIAL_CONFIG is the serial-to-mesh module, not USB companion.
    m.excluded_modules=meshtastic_ExcludedModules_MQTT_CONFIG|
      meshtastic_ExcludedModules_SERIAL_CONFIG|meshtastic_ExcludedModules_EXTNOTIF_CONFIG|
      meshtastic_ExcludedModules_STOREFORWARD_CONFIG|meshtastic_ExcludedModules_RANGETEST_CONFIG|
      meshtastic_ExcludedModules_TELEMETRY_CONFIG|meshtastic_ExcludedModules_CANNEDMSG_CONFIG|
      meshtastic_ExcludedModules_AUDIO_CONFIG|meshtastic_ExcludedModules_REMOTEHARDWARE_CONFIG|
      meshtastic_ExcludedModules_NEIGHBORINFO_CONFIG|meshtastic_ExcludedModules_AMBIENTLIGHTING_CONFIG|
      meshtastic_ExcludedModules_DETECTIONSENSOR_CONFIG|meshtastic_ExcludedModules_PAXCOUNTER_CONFIG|
      meshtastic_ExcludedModules_NETWORK_CONFIG|meshtastic_ExcludedModules_STATUSMESSAGE_CONFIG|
      meshtastic_ExcludedModules_TRAFFICMANAGEMENT_CONFIG|meshtastic_ExcludedModules_TAK_CONFIG|
      meshtastic_ExcludedModules_MESHBEACON_CONFIG;
    m.excluded_modules&=~host_.supportedModules();
    if(m.hasWifi)m.excluded_modules&=~meshtastic_ExcludedModules_NETWORK_CONFIG;
    if(!m.hasBluetooth)m.excluded_modules|=meshtastic_ExcludedModules_BLUETOOTH_CONFIG;
  }
  void channel(meshtastic_Channel &c,unsigned index) const {
    // Stock firmware emits empty settings even for disabled slots. This also
    // supports clients that gate channel updates on settings presence.
    c={};c.index=index;c.has_settings=true;c.settings.has_module_settings=true;
    if(index>0) {
      ChannelRecord stored;detail::VolatileWipe storedWipe{&stored,sizeof(stored)};
      if(!service_.config.channelRecord(index,stored))return;
      c.role=static_cast<meshtastic_Channel_Role>(stored.role());
      c.settings.psk.size=pb_size_t(stored.keySize());if(stored.keySize())memcpy(c.settings.psk.bytes,stored.key(),stored.keySize());
      memcpy(c.settings.name,stored.name(),strlen(stored.name())+1);
      c.settings.module_settings.is_muted=stored.muted();c.settings.module_settings.position_precision=service_.config.positionSharing()?stored.precision():0;
      c.settings.downlink_enabled=stored.downlink();c.settings.use_aead=stored.aead();
      c.settings.uplink_enabled=host_.channelUplinkEnabled(service_.config,index);return;
    }
    auto record=service_.config.node();detail::VolatileWipe recordWipe{&record,sizeof(record)};
    if(index==0&&service_.config.channelConfigured()) {
      c.role=meshtastic_Channel_Role_PRIMARY;c.has_settings=true;
      if(PrimaryChannel::defaultKey(record)){c.settings.psk.size=1;c.settings.psk.bytes[0]=1;}
      else {
        c.settings.psk.size=record.bytes[3];
        memcpy(c.settings.psk.bytes,record.bytes+bench::MtNodeRecord::Key,record.bytes[3]);
      }
      memcpy(c.settings.name,service_.config.primaryName(),strlen(service_.config.primaryName())+1);
      c.settings.module_settings.is_muted=service_.config.channelMuted();
      c.settings.module_settings.position_precision=service_.config.positionSharing()?service_.config.primaryPositionPrecision():0;
    }
    c.settings.use_aead=service_.config.primaryAead();
    c.settings.downlink_enabled=service_.config.primaryDownlinkEnabled();
    c.settings.uplink_enabled=host_.channelUplinkEnabled(service_.config,index);
  }
  static void redactChannelPsk(meshtastic_Channel &channel) {
    auto &psk=channel.settings.psk;
    if(psk.size<=1)return; // Empty or stock default-key marker carries no secret.
    volatile pb_byte_t *bytes=psk.bytes;
    for(size_t i=0;i<sizeof(psk.bytes);++i)bytes[i]=0;
    static constexpr char marker[]="sekrit";
    memcpy(psk.bytes,marker,sizeof(marker)-1);psk.size=sizeof(marker)-1;
  }
  bool config(meshtastic_Config &c,unsigned tag) const {
    c={};c.which_payload_variant=tag;
    switch(tag) {
    case meshtastic_Config_device_tag:
      c.payload_variant.device.role=static_cast<meshtastic_Config_DeviceConfig_Role>(service_.config.role());
      c.payload_variant.device.rebroadcast_mode=static_cast<meshtastic_Config_DeviceConfig_RebroadcastMode>(service_.config.rebroadcastMode());
      c.payload_variant.device.serial_enabled=service_.config.serialEnabled();
      c.payload_variant.device.is_managed=service_.config.isManaged();
      c.payload_variant.device.led_heartbeat_disabled=host_.heartbeatDisabled();
#if defined(CONFIG_MT_LOCAL_CLOCK)
      service_.config.timezone(c.payload_variant.device.tzdef);
#endif
      c.payload_variant.device.node_info_broadcast_secs=service_.config.node().interval();break;
    case meshtastic_Config_position_tag: {
      const auto g=service_.config.gnssSettings();auto &p=c.payload_variant.position;
      p.fixed_position=service_.config.fixedPosition();p.gps_update_interval=g.updateSecs;p.gps_attempt_time=g.attemptSecs;
      p.position_broadcast_secs=g.broadcastSecs;p.position_flags=g.positionFlags;
      p.position_broadcast_smart_enabled=g.smart;p.broadcast_smart_minimum_distance=g.smartMinDistance;
      p.broadcast_smart_minimum_interval_secs=g.smartMinInterval;
      p.gps_mode=host_.gnssPresent()?(g.enabled?meshtastic_Config_PositionConfig_GpsMode_ENABLED:meshtastic_Config_PositionConfig_GpsMode_DISABLED):meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;break;
    }
    case meshtastic_Config_network_tag:return host_.networkConfig(c.payload_variant.network);
    case meshtastic_Config_power_tag:{
      auto &power=c.payload_variant.power;
      if(!host_.powerConfig(power))return false;
      if(!power.ls_secs){
        const auto role=service_.config.role();
        power.ls_secs=(role==meshtastic_Config_DeviceConfig_Role_ROUTER ||
                       role==meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)?86400u:300u;
      }
      break;
    }
    case meshtastic_Config_sessionkey_tag:break;
#if defined(CONFIG_MT_RF_ADMIN_GET)
    // Stock's deprecated DEVICEUI_CONFIG GET returns this empty nested
    // variant; the separate DeviceUIConfig API owns actual UI settings.
    case meshtastic_Config_device_ui_tag:break;
#endif
    case meshtastic_Config_display_tag:
      c.payload_variant.display.screen_on_secs=service_.config.displaySeconds();
      c.payload_variant.display.flip_screen=service_.config.displayFlip();
      c.payload_variant.display.auto_screen_carousel_secs=service_.config.displayCarouselSeconds();
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
      c.payload_variant.display.use_12h_clock=service_.config.twelveHourClock();
#endif
      c.payload_variant.display.use_long_node_name=service_.config.displayLongNames();break;
    case meshtastic_Config_bluetooth_tag:
      if(!host_.bluetoothConfig(c.payload_variant.bluetooth)){
        c.payload_variant.bluetooth={};c.payload_variant.bluetooth.mode=meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
      }
      break;
    case meshtastic_Config_security_tag: {
#if defined(CONFIG_MT_RF_ADMIN_GET)
      if(!host_.adminKeys(c.payload_variant.security))return false;
#endif
      c.payload_variant.security.is_managed=service_.config.isManaged();
      c.payload_variant.security.serial_enabled=service_.config.serialEnabled();
      c.payload_variant.security.debug_log_api_enabled=service_.config.debugLogApiEnabled();
      c.payload_variant.security.admin_channel_enabled=service_.config.adminChannelEnabled();
      c.payload_variant.security.packet_signature_policy=static_cast<meshtastic_Config_SecurityConfig_PacketSignaturePolicy>(service_.config.packetSignaturePolicy());
      uint8_t publicKey[32];
      if(service_.privatePublicIdentity(publicKey)){auto &key=c.payload_variant.security.public_key;key.size=32;memcpy(key.bytes,publicKey,32);}break;
    }
    case meshtastic_Config_lora_tag: {
      c.payload_variant.lora.region=static_cast<meshtastic_Config_LoRaConfig_RegionCode>(service_.config.regionCode());
      c.payload_variant.lora.use_preset=service_.config.usePreset();c.payload_variant.lora.modem_preset=static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(service_.config.modemPreset());
      c.payload_variant.lora.hop_limit=service_.config.hopLimit();c.payload_variant.lora.channel_num=service_.config.channelNumber();c.payload_variant.lora.sx126x_rx_boosted_gain=true;
      if(xiaoWithoutExternalLna())c.payload_variant.lora.fem_lna_mode=meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT;
      c.payload_variant.lora.override_duty_cycle=service_.config.overrideDutyCycle();
      const auto radio=service_.config.radioRequest();
      c.payload_variant.lora.bandwidth=radio.bandwidth;c.payload_variant.lora.spread_factor=radio.spreadingFactor;c.payload_variant.lora.coding_rate=radio.codingRate;
      c.payload_variant.lora.override_frequency=radio.overrideMHz;c.payload_variant.lora.frequency_offset=radio.offsetMHz;
      c.payload_variant.lora.tx_power=radio.powerSpecified?radio.power:(board::capabilityModel(model_)==meshtastic_HardwareModel_HELTEC_MESH_NODE_T096?-9:2);
      // Normal profiles expose durable user intent. Editing, faults and other
      // transient quiescence must not round-trip into an unintended disable.
      // The legacy one-packet profile retains its consumable runtime-arm view.
      c.payload_variant.lora.tx_enabled=service_.normalMode()?service_.config.txEnabled():service_.endpointArmed();
      c.payload_variant.lora.config_ok_to_mqtt=host_.configOkToMqtt();
      c.payload_variant.lora.ignore_incoming_count=service_.config.ignoredSenderCount();
      for(unsigned i=0;i<c.payload_variant.lora.ignore_incoming_count;++i)
        c.payload_variant.lora.ignore_incoming[i]=service_.config.ignoredSender(i);
      c.payload_variant.lora.ignore_mqtt=service_.config.ignoreMqtt();break;
    }
    default:c={};return false;
    }return true;
  }
  void queue(uint32_t id=0) {
    fresh(meshtastic_FromRadio_queueStatus_tag);output_.queueStatus.free=service_.endpointFree();
    output_.queueStatus.maxlen=service_.normalMode()?4:1;output_.queueStatus.mesh_packet_id=id;emit();
  }
  void responsePacket(uint32_t request,meshtastic_PortNum port) {
    fresh(meshtastic_FromRadio_packet_tag);auto &p=output_.packet;
    p.from=service_.route.identity();p.to=p.from;p.id=++sequence_;
    p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=port;p.decoded.request_id=request;timestamp(p);
  }
  static void timestamp(meshtastic_MeshPacket &p,uint32_t captured) {
    p.rx_time=captured;p.has_rx_time=captured!=0;
  }
  void timestamp(meshtastic_MeshPacket &p)const {
    timestamp(p,host_.wallTime());
  }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  void storeForwardTimestamp(uint32_t &time,bool &hasRxTime)const {
    time=host_.wallTime();hasRxTime=time!=0;
    if(!hasRxTime){uint32_t uptime=0;if(host_.uptimeSeconds(uptime))time=uptime;}
  }
#endif
  bool buildAck(uint32_t request,meshtastic_Routing_Error code,uint32_t peer=0,bool authenticated=false) {
    responsePacket(request,meshtastic_PortNum_ROUTING_APP);
    if(peer)output_.packet.from=peer;
    if(authenticated)output_.packet.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;
    meshtastic_Routing r{};r.which_variant=meshtastic_Routing_error_reason_tag;r.error_reason=code;
    auto &data=output_.packet.decoded;auto stream=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(!pb_encode(&stream,meshtastic_Routing_fields,&r))return false;
    data.payload.size=stream.bytes_written;return true;
  }
  void ack(uint32_t request,meshtastic_Routing_Error code,uint32_t peer=0,bool authenticated=false){
    if(buildAck(request,code,peer,authenticated))emit();
  }
  void adminReply(uint32_t request) {
    responsePacket(request,meshtastic_PortNum_ADMIN_APP);auto &data=output_.packet.decoded;
    auto stream=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(pb_encode(&stream,meshtastic_AdminMessage_fields,&reply_)){data.payload.size=stream.bytes_written;emit();}
    else ack(request,meshtastic_Routing_Error_TOO_LARGE);
  }
  // Re-encode only the selected control, deliberately excluding the separate
  // session-passkey field. Nanopb otherwise discards unknown fields and
  // collapses duplicates. The comparison admits canonical stock encodings and
  // rejects information that policy did not inspect.
  static bool canonicalAdminOperation(const meshtastic_AdminMessage &decoded,
                                      pb_size_t expected,const uint8_t *wire,size_t wireSize) {
    if(decoded.which_payload_variant!=expected||!wire||!wireSize)return false;
    switch(expected) {
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS)) || defined(CONFIG_MT_ONE_WIRE_TELEMETRY) || defined(CONFIG_MT_SHTXX_ADMIN)
    case meshtastic_AdminMessage_sensor_config_tag:
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
    case meshtastic_AdminMessage_set_scale_tag:
#endif
#endif
    case meshtastic_AdminMessage_get_ringtone_request_tag:
    case meshtastic_AdminMessage_set_ringtone_message_tag:
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
    case meshtastic_AdminMessage_set_canned_message_module_messages_tag:
    case meshtastic_AdminMessage_set_owner_tag:
    case meshtastic_AdminMessage_set_ham_mode_tag:
    case meshtastic_AdminMessage_set_channel_tag:
    case meshtastic_AdminMessage_set_config_tag:
    case meshtastic_AdminMessage_set_fixed_position_tag:
    case meshtastic_AdminMessage_remove_fixed_position_tag:
    case meshtastic_AdminMessage_send_input_event_tag:
    case meshtastic_AdminMessage_remove_by_nodenum_tag:
    case meshtastic_AdminMessage_set_favorite_node_tag:
    case meshtastic_AdminMessage_remove_favorite_node_tag:
    case meshtastic_AdminMessage_set_ignored_node_tag:
    case meshtastic_AdminMessage_remove_ignored_node_tag:
    case meshtastic_AdminMessage_toggle_muted_node_tag:
    case meshtastic_AdminMessage_set_time_only_tag:
    case meshtastic_AdminMessage_add_contact_tag:
    case meshtastic_AdminMessage_get_ui_config_request_tag:
    case meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag:
    case meshtastic_AdminMessage_store_ui_config_tag:
    case meshtastic_AdminMessage_key_verification_tag:
    case meshtastic_AdminMessage_delete_file_request_tag:
    case meshtastic_AdminMessage_factory_reset_device_tag:
    case meshtastic_AdminMessage_factory_reset_config_tag:
    case meshtastic_AdminMessage_nodedb_reset_tag:
    case meshtastic_AdminMessage_backup_preferences_tag:
    case meshtastic_AdminMessage_restore_preferences_tag:
    case meshtastic_AdminMessage_remove_backup_preferences_tag:
    case meshtastic_AdminMessage_ota_request_tag:
    case meshtastic_AdminMessage_enter_dfu_mode_request_tag:
    case meshtastic_AdminMessage_shutdown_seconds_tag:
    case meshtastic_AdminMessage_reboot_seconds_tag:
      break;
    default:return false;
    }
    // The protobuf contains exactly one active operation and a separate session
    // passkey. Re-encoding the selected union preserves the byte-level check
    // for unknown fields, duplicates and noncanonical values.
    meshtastic_AdminMessage canonical=decoded;
    detail::VolatileWipe wipeCanonical{&canonical,sizeof(canonical)};
    canonical.session_passkey.size=0;
    // A maximum-length canned list needs 204 bytes on the wire once its
    // selected Admin field tag and length are included.
    uint8_t encoded[233]{};detail::VolatileWipe wipeEncoded{encoded,sizeof(encoded)};
    auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
    if(!pb_encode(&stream,meshtastic_AdminMessage_fields,&canonical))return false;
    if(stream.bytes_written==wireSize&&!memcmp(encoded,wire,wireSize))return true;
    if(expected!=meshtastic_AdminMessage_set_owner_tag&&expected!=meshtastic_AdminMessage_add_contact_tag)return false;
    return canonicalUserWithoutZeroMac(expected,encoded,stream.bytes_written,wire,wireSize);
  }
  // Nanopb's fixed MAC array emits six zero bytes even when a stock proto3
  // client omitted the field. Compare only that equivalent encoding; unknown
  // fields, altered lengths, duplicate fields and explicit defaults still fail.
  static bool canonicalUserWithoutZeroMac(pb_size_t operation,const uint8_t *encoded,size_t size,
                                          const uint8_t *wire,size_t wireSize) {
    size_t at=0;uint64_t tag=0,length=0;
    if(!readWireVarint(encoded,size,at,tag)||tag!=((uint64_t(operation)<<3)|2)||
       !readWireVarint(encoded,size,at,length)||length!=size-at)return false;
    const size_t contact=at;size_t userTag=0,userStart=at,userEnd=size;
    if(operation==meshtastic_AdminMessage_add_contact_tag) {
      userStart=0;
      while(at<size) {
        const size_t field=at;
        if(!readWireVarint(encoded,size,at,tag))return false;
        if((tag&7)==0){if(!readWireVarint(encoded,size,at,length))return false;continue;}
        if((tag&7)!=2||!readWireVarint(encoded,size,at,length)||length>size-at)return false;
        if(tag==18){userTag=field;userStart=at;userEnd=at+size_t(length);break;}
        at+=size_t(length);
      }
      if(!userStart)return false;
    }
    at=userStart;size_t macStart=0,macEnd=0;
    while(at<userEnd) {
      const size_t field=at;
      if(!readWireVarint(encoded,userEnd,at,tag))return false;
      if((tag&7)==0){if(!readWireVarint(encoded,userEnd,at,length))return false;continue;}
      if((tag&7)!=2||!readWireVarint(encoded,userEnd,at,length)||length>userEnd-at)return false;
      if(tag==34) {
        if(length!=6)return false;
        for(size_t i=0;i<6;++i)if(encoded[at+i])return false;
        macStart=field;macEnd=at+6;break;
      }
      at+=size_t(length);
    }
    if(!macStart)return false;
    if(operation==meshtastic_AdminMessage_set_owner_tag) {
      const size_t prefix=macStart-userStart,suffix=userEnd-macEnd;
      uint8_t header[12];auto out=pb_ostream_from_buffer(header,sizeof(header));
      return pb_encode_tag(&out,PB_WT_STRING,operation)&&pb_encode_varint(&out,prefix+suffix)&&
        wireSize==out.bytes_written+prefix+suffix&&!memcmp(wire,header,out.bytes_written)&&
        !memcmp(wire+out.bytes_written,encoded+userStart,prefix)&&
        !memcmp(wire+out.bytes_written+prefix,encoded+macEnd,suffix);
    }
    uint8_t user[meshtastic_User_size]{};detail::VolatileWipe wipeUser{user,sizeof(user)};
    auto u=pb_ostream_from_buffer(user,sizeof(user));
    if(!pb_write(&u,encoded+userStart,macStart-userStart)||
       !pb_write(&u,encoded+macEnd,userEnd-macEnd))return false;
    uint8_t contactBytes[192]{};detail::VolatileWipe wipeContact{contactBytes,sizeof(contactBytes)};
    auto c=pb_ostream_from_buffer(contactBytes,sizeof(contactBytes));
    if(!pb_write(&c,encoded+contact,userTag-contact)||!pb_encode_tag(&c,PB_WT_STRING,2)||
       !pb_encode_string(&c,user,u.bytes_written)||!pb_write(&c,encoded+userEnd,size-userEnd))return false;
    uint8_t stock[208]{};detail::VolatileWipe wipeStock{stock,sizeof(stock)};
    auto out=pb_ostream_from_buffer(stock,sizeof(stock));
    return pb_encode_tag(&out,PB_WT_STRING,meshtastic_AdminMessage_add_contact_tag)&&
      pb_encode_string(&out,contactBytes,c.bytes_written)&&out.bytes_written==wireSize&&!memcmp(stock,wire,wireSize);
  }
  static bool validUtf8(const char *text,size_t size) {
    if((!text&&size)||size>200)return false;
    size_t i=0;
    while(i<size) {
      const uint8_t first=uint8_t(text[i]);
      if(first<0x80){if(!first)return false;++i;continue;}
      unsigned count=0;uint32_t code=0,minimum=0;
      if(first>=0xc2&&first<=0xdf){count=2;code=first&0x1f;minimum=0x80;}
      else if(first>=0xe0&&first<=0xef){count=3;code=first&0x0f;minimum=0x800;}
      else if(first>=0xf0&&first<=0xf4){count=4;code=first&0x07;minimum=0x10000;}
      else return false;
      if(count>size-i)return false;
      for(unsigned j=1;j<count;++j){const uint8_t next=uint8_t(text[i+j]);if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&0x3f);}
      if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
      i+=count;
    }
    return true;
  }
  static bool validCannedMessageList(const char *text,size_t capacity,size_t &length) {
    if(!text||capacity<201)return false;
    length=strnlen(text,201);
    return length<=200&&length<capacity&&validUtf8(text,length);
  }
  static meshtastic_Routing_Error cannedStoreError(int result) {
    return !result?meshtastic_Routing_Error_NONE:
      result==-EINVAL?meshtastic_Routing_Error_BAD_REQUEST:
      result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
      meshtastic_Routing_Error_NO_INTERFACE;
  }
  // Nanopb may store generated enums in a short-enum build. Read the raw
  // LoRa region varint from the authenticated/canonical request path before
  // narrowing, so e.g. wire value 257 cannot alias US (1). Unknown fields are
  // skipped with strict bounds; canonicalAdminOperation still decides whether
  // the complete request is an accepted stock encoding before any write.
  static bool readWireVarint(const uint8_t *wire,size_t size,size_t &at,uint64_t &value) {
    value=0;
    for(unsigned i=0;i<10;++i) {
      if(at>=size)return false;
      const uint8_t byte=wire[at++];
      if(i==9&&(byte&0xfe))return false;
      value|=uint64_t(byte&0x7f)<<(7*i);
      if(!(byte&0x80))return true;
    }
    return false;
  }
  static bool findRawField(const uint8_t *wire,size_t size,uint32_t wanted,
                           uint8_t wantedWire,const uint8_t *&bytes,size_t &bytesSize,
                           uint64_t &varint,bool &found) {
    if((!wire&&size)||wanted==0)return false;
    bytes=nullptr;bytesSize=0;varint=0;found=false;
    size_t at=0;
    while(at<size) {
      uint64_t key=0;if(!readWireVarint(wire,size,at,key)||key>UINT32_MAX)return false;
      const uint32_t field=uint32_t(key>>3);const uint8_t wt=uint8_t(key&7);
      if(!field||wt==3||wt==4||wt>5)return false;
      const bool selected=field==wanted;
      if(selected&&(found||wt!=wantedWire))return false;
      if(wt==0) {
        uint64_t value=0;if(!readWireVarint(wire,size,at,value))return false;
        if(selected){varint=value;found=true;}
      } else if(wt==1||wt==5) {
        const size_t count=wt==1?8:4;if(size-at<count)return false;at+=count;
      } else {
        uint64_t count=0;if(!readWireVarint(wire,size,at,count)||count>size-at)return false;
        if(selected){bytes=wire+at;bytesSize=size_t(count);found=true;}
        at+=size_t(count);
      }
    }
    return true;
  }
  static bool rawConfigRegion(const uint8_t *wire,size_t wireSize,uint32_t &region) {
    const uint8_t *adminBytes=nullptr,*configBytes=nullptr;size_t adminSize=0,configSize=0;
    uint64_t ignored=0;bool hasAdmin=false,hasConfig=false,hasRegion=false;
    if(!findRawField(wire,wireSize,meshtastic_AdminMessage_set_config_tag,2,
                     adminBytes,adminSize,ignored,hasAdmin)||!hasAdmin||
       !findRawField(adminBytes,adminSize,meshtastic_Config_lora_tag,2,
                     configBytes,configSize,ignored,hasConfig)||!hasConfig)return false;
    const uint8_t *unused=nullptr;size_t unusedSize=0;uint64_t value=0;
    if(!findRawField(configBytes,configSize,meshtastic_Config_LoRaConfig_region_tag,0,
                     unused,unusedSize,value,hasRegion))return false;
    if(!hasRegion){region=meshtastic_Config_LoRaConfig_RegionCode_UNSET;return true;}
    if(value>UINT32_MAX)return false;
    region=uint32_t(value);return true;
  }
  // A disabled excluded module is represented by exactly one selected
  // ModuleConfig submessage whose body is empty. Compare the original wire
  // bytes with a freshly encoded canonical value: nanopb otherwise discards
  // unknown fields and collapses duplicate oneofs before policy can inspect
  // them. This also rejects explicit zero fields, noncanonical varints,
  // session-passkey fields and alternate/duplicate module variants. Current
  // protobuf clients serialize an unchanged proto3-default module this way.
  static bool canonicalDisabledModuleNoop(const meshtastic_AdminMessage &decoded,
                                           const uint8_t *wire,size_t wireSize) {
    if(decoded.which_payload_variant!=meshtastic_AdminMessage_set_module_config_tag||!wire||!wireSize)return false;
    const auto tag=decoded.set_module_config.which_payload_variant;
    if(tag<meshtastic_ModuleConfig_mqtt_tag||tag>meshtastic_ModuleConfig_mesh_beacon_tag)return false;
    meshtastic_AdminMessage canonical{};
    canonical.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;
    canonical.set_module_config.which_payload_variant=tag;
    uint8_t encoded[16]{};auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
    return pb_encode(&stream,meshtastic_AdminMessage_fields,&canonical)&&
      stream.bytes_written==wireSize&&!memcmp(encoded,wire,wireSize);
  }
  static uint32_t moduleBit(unsigned tag){
    switch(tag){
    case meshtastic_ModuleConfig_audio_tag:return meshtastic_ExcludedModules_AUDIO_CONFIG;
    case meshtastic_ModuleConfig_ambient_lighting_tag:return meshtastic_ExcludedModules_AMBIENTLIGHTING_CONFIG;
    case meshtastic_ModuleConfig_paxcounter_tag:return meshtastic_ExcludedModules_PAXCOUNTER_CONFIG;
    case meshtastic_ModuleConfig_serial_tag:return meshtastic_ExcludedModules_SERIAL_CONFIG;
#if defined(CONFIG_MT_GPIO_MODULES)
    case meshtastic_ModuleConfig_remote_hardware_tag:return meshtastic_ExcludedModules_REMOTEHARDWARE_CONFIG;
    case meshtastic_ModuleConfig_detection_sensor_tag:return meshtastic_ExcludedModules_DETECTIONSENSOR_CONFIG;
#endif
    case meshtastic_ModuleConfig_canned_message_tag:return meshtastic_ExcludedModules_CANNEDMSG_CONFIG;
    case meshtastic_ModuleConfig_mqtt_tag:return meshtastic_ExcludedModules_MQTT_CONFIG;
    case meshtastic_ModuleConfig_external_notification_tag:return meshtastic_ExcludedModules_EXTNOTIF_CONFIG;
    case meshtastic_ModuleConfig_telemetry_tag:return meshtastic_ExcludedModules_TELEMETRY_CONFIG;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    case meshtastic_ModuleConfig_store_forward_tag:return meshtastic_ExcludedModules_STOREFORWARD_CONFIG;
#endif
    case meshtastic_ModuleConfig_range_test_tag:return meshtastic_ExcludedModules_RANGETEST_CONFIG;
    case meshtastic_ModuleConfig_neighbor_info_tag:return meshtastic_ExcludedModules_NEIGHBORINFO_CONFIG;
    case meshtastic_ModuleConfig_statusmessage_tag:return meshtastic_ExcludedModules_STATUSMESSAGE_CONFIG;
    case meshtastic_ModuleConfig_traffic_management_tag:return meshtastic_ExcludedModules_TRAFFICMANAGEMENT_CONFIG;
    case meshtastic_ModuleConfig_mesh_beacon_tag:return meshtastic_ExcludedModules_MESHBEACON_CONFIG;
    case meshtastic_ModuleConfig_tak_tag:return meshtastic_ExcludedModules_TAK_CONFIG;
    default:return 0;
    }
  }
  bool moduleConfig(meshtastic_ModuleConfig &out,unsigned tag)const{
    out={};out.which_payload_variant=tag;
    return !(host_.supportedModules()&moduleBit(tag))||host_.moduleConfig(out,tag);
  }
  bool canonicalModuleConfig(const uint8_t *wire,size_t size)const{
    meshtastic_AdminMessage canonical{};detail::VolatileWipe wipeCanonical{&canonical,sizeof(canonical)};
    canonical.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;
    canonical.set_module_config=admin_.set_module_config;
    uint8_t bytes[512]{};detail::VolatileWipe wipeBytes{bytes,sizeof(bytes)};
    auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    return pb_encode(&stream,meshtastic_AdminMessage_fields,&canonical)&&stream.bytes_written==size&&!memcmp(bytes,wire,size);
  }
  void position(meshtastic_Position &p) const {(void)service_.localPosition(p);}
  void localNode(bool clearPosition=false) {
    fresh(meshtastic_FromRadio_node_info_tag);output_.node_info.num=service_.route.identity();
    output_.node_info.has_user=true;user(output_.node_info.user);
    output_.node_info.has_position=service_.localPosition(output_.node_info.position)||clearPosition;
    service_.hardware().apply(output_.node_info.device_metrics);
    output_.node_info.has_device_metrics=output_.node_info.device_metrics.has_voltage||
      output_.node_info.device_metrics.has_battery_level;
    emit();
  }
  static void cachedNode(meshtastic_NodeInfo &node,const NodeCache::Entry &entry,bool thin=false){
    using E=NodeCache::Entry;
    node={};node.num=entry.id();node.has_user=!entry.placeholder();auto &u=node.user;
    node.is_favorite=entry.favorite();node.is_ignored=entry.ignored();node.is_muted=entry.muted();
    node.is_key_manually_verified=entry.manuallyVerified();
    node.has_xeddsa_signed=entry.verifiedSigner();
    snprintf(u.id,sizeof(u.id),"!%08x",entry.id());
    memcpy(u.long_name,entry.bytes+E::Name,sizeof(u.long_name));memcpy(u.short_name,entry.bytes+E::Short,sizeof(u.short_name));
    u.hw_model=static_cast<meshtastic_HardwareModel>(entry.model());u.role=static_cast<meshtastic_Config_DeviceConfig_Role>(entry.role());
    u.public_key.size=entry.bytes[E::KeyLength];memcpy(u.public_key.bytes,entry.bytes+E::Key,32);
    u.has_is_unmessagable=entry.bytes[E::Flags]&1;u.is_unmessagable=entry.bytes[E::Flags]&2;
    u.is_licensed=entry.licensed();
    node.snr=entry.snr;node.has_hops_away=entry.heard;node.hops_away=entry.hops;node.heard_on_current_lora=entry.heard;
    node.last_heard=entry.lastHeard();node.via_mqtt=entry.viaMqtt;
    if(thin)return; // Stock PhoneAPI sends peer measurements separately after config_complete.
    node.has_device_metrics=entry.deviceMetrics.present();
    if(node.has_device_metrics)entry.deviceMetrics.exportTo(node.device_metrics);
    node.has_position=entry.position.present();
    if(node.has_position)entry.position.exportTo(node.position);
  }
  static bool cachedPosition(const uint8_t *payload,size_t size,PeerPosition &position){
    if(!payload||!size||size>meshtastic_Position_size)return false;
    meshtastic_Position p{};auto input=pb_istream_from_buffer(payload,size);
    if(!pb_decode(&input,meshtastic_Position_fields,&p)||input.bytes_left)return false;
    uint8_t canonical[meshtastic_Position_size]{};auto output=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&output,meshtastic_Position_fields,&p)&&output.bytes_written==size&&
      !memcmp(canonical,payload,size)&&position.assign(p);
  }
#if defined(CONFIG_MT_PHONE_FILES)
  bool appFilesAllowed()const{
    if(owner_>1||!connected_||!host_.ready()||!service_.normalMode()||service_.config.isManaged()||host_.editing())return false;
    if(owner_==0)return !recoveryOnly_&&host_.consoleAllowed();
    const auto s=host_.bluetoothSecurity();
    return s.connected&&!s.pending&&!s.error&&s.level>=2;
  }
#endif
  void handshake(uint32_t nonce,uint32_t now) {
#if defined(CONFIG_MT_PEER_DIRECTORY)
    host_.directoryDisconnect(owner_);favoriteRequest_=0;directoryCursor_=directoryEmitted_=0;
#endif
#if defined(CONFIG_MT_PHONE_FILES)
    host_.appFileDisconnect(owner_);
#endif
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS))
    ++sensorSession_;
#endif
    statusReplayNext_=statusReplayCount_=0;replayKind_=Satellite::Done;
    if(apiLogSessionActive_){host_.safeApiLogSession(owner_==1,false);apiLogSessionActive_=false;}
    configComplete_=false;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    ++localHistorySession_;if(!localHistorySession_)++localHistorySession_;
    localHistory_.clear();
#endif
    connected_=false;++handshakes;
    handshakeNonce_=nonce;handshakeNow_=now;handshakeIndex_=0;
    recoveryOnly_=owner_==0&&!host_.consoleAllowed();
    if(recoveryOnly_) {
      // A closed console gets only the public local ID and the nonce marker
      // needed to finish the transport epoch. It never receives the ordinary
      // Config/Channel/NodeDB stream, and configComplete_ remains false.
      connected_=true;handshakePhase_=HandshakePhase::Idle;
      fresh(meshtastic_FromRadio_my_info_tag);
      output_.my_info.my_node_num=service_.route.identity();
      {RecoveryReplyScope scope(recoveryReply_);if(!emit(false))return;}
      fresh(meshtastic_FromRadio_config_complete_id_tag);output_.config_complete_id=handshakeNonce_;
      {RecoveryReplyScope scope(recoveryReply_);(void)emit(false);}
      return;
    }
    handshakePhase_=nonce==OnlyNodes?HandshakePhase::LocalNode:HandshakePhase::MyInfo;
    progressHandshake();
  }
  void progressHandshake() {
    while(handshakePhase_!=HandshakePhase::Idle) {
      switch(handshakePhase_) {
      case HandshakePhase::Idle:return;
      case HandshakePhase::MyInfo:
        fresh(meshtastic_FromRadio_my_info_tag);output_.my_info.my_node_num=service_.route.identity();
#if defined(CONFIG_MT_PEER_DIRECTORY)
        output_.my_info.nodedb_count=1+host_.directoryCount(service_.nodes);
#else
        output_.my_info.nodedb_count=1+service_.nodes.count();
#endif
        output_.my_info.min_app_version=30200;
        {
          bool identify=owner_==0&&host_.consoleAllowed();
          if(owner_==1){const auto link=host_.bluetoothSecurity();identify=link.connected&&!link.pending&&!link.error&&link.level>=2;}
          host_.boardMyInfo(output_.my_info,identify && !service_.config.isManaged());
        }
        strcpy(output_.my_info.pio_env,board::firmwareTarget(model_));
        if(!emit(false))return;
        handshakePhase_=HandshakePhase::UiData;break;
      case HandshakePhase::UiData:
        fresh(meshtastic_FromRadio_deviceuiConfig_tag);
        if(!host_.uiConfig(output_.deviceuiConfig)&&!emit(false))return;
        handshakePhase_=HandshakePhase::LocalNode;break;
      case HandshakePhase::Metadata:
        fresh(meshtastic_FromRadio_metadata_tag);metadata(output_.metadata);
        if(!emit(false))return;
        handshakePhase_=HandshakePhase::Region;break;
      case HandshakePhase::Region:
        // The bounded catalogue is advisory: if it cannot be encoded, continue
        // with the stock stream. A full queue leaves this exact stage pending.
        fresh(meshtastic_FromRadio_region_presets_tag);
        if(!radio::RegionCatalogue::encode(output_.region_presets))++encodeErrors;
        else if(!emit(false))return;
        connected_=true;handshakeIndex_=0;handshakePhase_=HandshakePhase::Channels;break;
      case HandshakePhase::Channels:
        if(handshakeIndex_>=8){handshakeIndex_=1;handshakePhase_=HandshakePhase::Configs;break;}
        fresh(meshtastic_FromRadio_channel_tag);channel(output_.channel,handshakeIndex_);
        if(!emit(false))return;
        ++handshakeIndex_;break;
      case HandshakePhase::Configs:
        if(handshakeIndex_>meshtastic_Config_sessionkey_tag){handshakeIndex_=1;handshakePhase_=HandshakePhase::Modules;break;}
        fresh(meshtastic_FromRadio_config_tag);
        if(config(output_.config,handshakeIndex_)&&!emit(false))return;
        ++handshakeIndex_;break;
      case HandshakePhase::Modules:
        if(handshakeIndex_>meshtastic_ModuleConfig_mesh_beacon_tag){handshakeIndex_=0;handshakePhase_=handshakeNonce_==OnlyConfig?HandshakePhase::QueueStatus:HandshakePhase::Nodes;break;}
        fresh(meshtastic_FromRadio_moduleConfig_tag);
        if(moduleConfig(output_.moduleConfig,handshakeIndex_)&&!emit(false))return;
        ++handshakeIndex_;break;
      case HandshakePhase::LocalNode:
        fresh(meshtastic_FromRadio_node_info_tag);output_.node_info.num=service_.route.identity();
        output_.node_info.is_favorite=true;output_.node_info.has_user=true;user(output_.node_info.user);
        output_.node_info.has_position=service_.localPosition(output_.node_info.position);
        service_.hardware().apply(output_.node_info.device_metrics);
        output_.node_info.has_device_metrics=output_.node_info.device_metrics.has_voltage||
          output_.node_info.device_metrics.has_battery_level;
        if(!emit(false))return;
        handshakeIndex_=0;
        handshakePhase_=handshakeNonce_==OnlyNodes?HandshakePhase::Nodes:HandshakePhase::Metadata;
        break;
      case HandshakePhase::Nodes:
        if(handshakeIndex_>=service_.nodes.count()){
#if defined(CONFIG_MT_PEER_DIRECTORY)
          if(directoryEmitted_<host_.directoryLimit()){
            NodeCache::Entry archived{};
            const int result=host_.directoryPeer(owner_,directoryCursor_,service_.nodes,archived);
            if(result==-EAGAIN)return;
            if(result==2){++directoryCursor_;break;}
            if(result==1){
              fresh(meshtastic_FromRadio_node_info_tag);cachedNode(output_.node_info,archived,true);
              if(!emit(false))return;++directoryCursor_;++directoryEmitted_;break;
            }
          }
          host_.directoryDisconnect(owner_);favoriteRequest_=0;
#endif
          handshakeIndex_=0;handshakePhase_=handshakeNonce_==OnlyNodes?HandshakePhase::Complete:HandshakePhase::QueueStatus;break;
        }
        fresh(meshtastic_FromRadio_node_info_tag);cachedNode(output_.node_info,*service_.nodes.get(handshakeIndex_),true);
        if(!emit(false))return;
#if defined(CONFIG_MT_PEER_DIRECTORY)
        ++directoryEmitted_;
#endif
        ++handshakeIndex_;break;
      case HandshakePhase::Diagnostics: {
        bool emitted=false;
        if(handshakeIndex_==0) {
          fresh(meshtastic_FromRadio_log_record_tag);strcpy(output_.log_record.source,"ZephTastic");
          output_.log_record.level=meshtastic_LogRecord_Level_INFO;
          const auto security=host_.bluetoothSecurity();
          snprintf(output_.log_record.message,sizeof(output_.log_record.message),
            "version=%s uptime_ms=%u ready=%u rf=%s tx_compiled=%u fault=%u rx=%u delivered=%u denied_tx=%u ble_link=%u ble_pending=%u ble_request=%d ble_level=%u ble_error=%u node_cache_ready=%u node_cache_dirty=%u",
            firmwareVersion(),unsigned(handshakeNow_),unsigned(host_.ready()),service_.receiving()?"rx":"off",unsigned(service_.txAllowed()),
            unsigned(service_.faulted()),unsigned(service_.received),unsigned(receivedMessages),unsigned(deniedTx),
            unsigned(security.connected),unsigned(security.pending),security.request,unsigned(security.level),unsigned(security.error),
            unsigned(service_.nodes.ready()),unsigned(service_.nodes.dirty()));emitted=true;
        } else if(handshakeIndex_==1) {
          fresh(meshtastic_FromRadio_log_record_tag);strcpy(output_.log_record.source,"ZephTastic");
          output_.log_record.level=meshtastic_LogRecord_Level_INFO;
          snprintf(output_.log_record.message,sizeof(output_.log_record.message),
            "endpoint_diag v=1 armed=%u free=%u controls=%u completed_tx=%u fault=%u",
            unsigned(service_.endpointArmed()),unsigned(service_.endpointFree()),unsigned(service_.endpointControls()),
            unsigned(service_.localStats().packetsTx),unsigned(service_.faulted()));emitted=true;
#if MT_PKI_EXTENDED_DIAGNOSTICS_ENABLED
        } else if(handshakeIndex_==2||handshakeIndex_==3) {
          const auto d=service_.radioDiagnostic();fresh(meshtastic_FromRadio_log_record_tag);
          strcpy(output_.log_record.source,"ZephTastic");output_.log_record.level=meshtastic_LogRecord_Level_INFO;
          const auto &n=d.native;int size;
          if(handshakeIndex_==2) size=snprintf(output_.log_record.message,sizeof(output_.log_record.message),
            "radio_stage v=1 frozen=%u tx_n=%u tx_f=%u tx_s=%u tx_r=%d cfg_n=%u cfg_f=%u cfg_s=%u cfg_r=%d rx_n=%u rx_f=%u rx_s=%u rx_r=%d",
            unsigned(d.faultStage!=0),unsigned(n.send.calls),unsigned(n.send.failedCall),unsigned(n.send.stage),int(n.send.result),
            unsigned(n.config.calls),unsigned(n.config.failedCall),unsigned(n.config.stage),int(n.config.result),
            unsigned(n.restart.calls),unsigned(n.restart.failedCall),unsigned(n.restart.stage),int(n.restart.result));
          else size=snprintf(output_.log_record.message,sizeof(output_.log_record.message),
            "radio_service v=1 submit=%u rejected=%u faults=%u reject_s=%u reject_d=%u reject_v=%d reject_f=%u fault_s=%u fault_d=%u fault_v=%d fault_f=%u native=%u",
            unsigned(d.submits),unsigned(d.rejected),unsigned(d.faults),unsigned(d.rejectStage),unsigned(d.rejectDomain),int(d.rejectValue),unsigned(d.rejectFlags),
            unsigned(d.faultStage),unsigned(d.faultDomain),int(d.faultValue),unsigned(d.faultFlags),unsigned(d.nativeKnown));
          emitted=size>=0&&size_t(size)<sizeof(output_.log_record.message);
        } else if(handshakeIndex_==4&&service_.privateConfigured()) {
          const auto d=service_.privateReplyDiagnostics();fresh(meshtastic_FromRadio_log_record_tag);
          strcpy(output_.log_record.source,"ZephTastic");output_.log_record.level=meshtastic_LogRecord_Level_INFO;
          snprintf(output_.log_record.message,sizeof(output_.log_record.message),
            "private_reply_diag v=1 attempts=%u queued=%u no_slot=%u producer=%u id=%u frame=%u queue=%u",
            unsigned(d.attempts),unsigned(d.queued),unsigned(d.no_slot),unsigned(d.producer),
            unsigned(d.id),unsigned(d.frame),unsigned(d.queue));emitted=true;
        } else if(handshakeIndex_==5) {
          fresh(meshtastic_FromRadio_log_record_tag);strcpy(output_.log_record.source,"ZephTastic");
          output_.log_record.level=meshtastic_LogRecord_Level_INFO;
          snprintf(output_.log_record.message,sizeof(output_.log_record.message),
            "storage_diag count=%u fresh=%u seq=%u reason=%u mask=%u result=%d",
            unsigned(loRaSaveFailures_),unsigned(lastLoRaPauseFresh_),unsigned(lastLoRaPause_.sequence),
            unsigned(lastLoRaPause_.reason),unsigned(lastLoRaPause_.mask),lastLoRaSaveResult_);emitted=true;
        } else if(handshakeIndex_==6||handshakeIndex_==7) {
          fresh(meshtastic_FromRadio_log_record_tag);strcpy(output_.log_record.source,"ZephTastic");
          output_.log_record.level=meshtastic_LogRecord_Level_INFO;
          const size_t size=handshakeIndex_==6?
            host_.extendedStorageDiagnostic(output_.log_record.message,sizeof(output_.log_record.message)):
            host_.channelDiagnostic(output_.log_record.message,sizeof(output_.log_record.message));
          emitted=size&&size<sizeof(output_.log_record.message)&&output_.log_record.message[size]==0;
#endif
        }
#if defined(CONFIG_MT_M119_TLS_TEST)
        else if(handshakeIndex_==8) {
          fresh(meshtastic_FromRadio_log_record_tag);
          const size_t size=host_.mqttTlsTestDiagnostic(output_.log_record.message,sizeof(output_.log_record.message));
          if(size&&size<sizeof(output_.log_record.message)&&output_.log_record.message[size]==0) {
            strcpy(output_.log_record.source,"ZephTastic");output_.log_record.level=meshtastic_LogRecord_Level_INFO;emitted=true;
          }
        }
#endif
        else if(handshakeIndex_==9&&!owner_) {
          fresh(meshtastic_FromRadio_log_record_tag);
          const size_t size=host_.gnssDiagnostic(output_.log_record.message,sizeof(output_.log_record.message));
          if(size&&size<sizeof(output_.log_record.message)&&output_.log_record.message[size]==0) {
            strcpy(output_.log_record.source,"ZephTastic");output_.log_record.level=meshtastic_LogRecord_Level_INFO;emitted=true;
          }
        }
#if defined(CONFIG_MT_BLE_DIAGNOSTICS)
        else if(handshakeIndex_==10&&!owner_) {
          fresh(meshtastic_FromRadio_log_record_tag);
          if(host_.bluetoothDiagnostic(output_.log_record.message,sizeof(output_.log_record.message))) {
            strcpy(output_.log_record.source,"ZephTastic");output_.log_record.level=meshtastic_LogRecord_Level_INFO;emitted=true;
          }
        }
#endif
#if defined(CONFIG_MT_RUNTIME_RESOURCES)
        else if(handshakeIndex_==11&&!owner_) {
          fresh(meshtastic_FromRadio_log_record_tag);
          const size_t size=host_.runtimeResources(output_.log_record.message,sizeof(output_.log_record.message));
          if(size&&size<sizeof(output_.log_record.message)&&output_.log_record.message[size]==0) {
            strcpy(output_.log_record.source,"ZephTastic");output_.log_record.level=meshtastic_LogRecord_Level_INFO;emitted=true;
          }
        }
#endif
        if(emitted&&!emit(false))return;
        if(++handshakeIndex_>=12)handshakePhase_=HandshakePhase::QueueStatus;
        break;
      }
      case HandshakePhase::QueueStatus:
        fresh(meshtastic_FromRadio_queueStatus_tag);output_.queueStatus.free=service_.endpointFree();
        output_.queueStatus.maxlen=service_.normalMode()?4:1;output_.queueStatus.mesh_packet_id=0;
        if(!emit(false))return;
#if defined(CONFIG_MT_PHONE_FILES)
        handshakePhase_=HandshakePhase::Files;break;
      case HandshakePhase::Files:
        if(handshakeNonce_!=OnlyNodes && appFilesAllowed()){
          fresh(meshtastic_FromRadio_fileInfo_tag);
          const int state=host_.appFileInfo(owner_,output_.fileInfo);
          if(state==-EAGAIN || (state>0&&!emit(false)))return;
          host_.appFileInfoSent(owner_);
          if(state==2)return; // Poll for the next entry after the worker advances this cursor.
        }
#endif
        handshakePhase_=HandshakePhase::Complete;break;
      case HandshakePhase::Complete:
        fresh(meshtastic_FromRadio_config_complete_id_tag);output_.config_complete_id=handshakeNonce_;
        if(!emit(false))return;
        connected_=configComplete_=true;
        replayKind_=handshakeNonce_==OnlyConfig?Satellite::Done:Satellite::Position;
#if defined(CONFIG_MT_PAGED_SATELLITES)
        archiveCursor_=0;
#endif
        statusReplayNext_=0;statusReplayCount_=service_.nodes.count();
        for(unsigned i=0;i<statusReplayCount_;++i)statusReplayPeers_[i]=service_.nodes.get(i)->id();
        handshakePhase_=HandshakePhase::Idle;break;
      }
    }
  }
  void privateSatellite(uint32_t id,unsigned kind,bool accepted){
#if defined(CONFIG_MT_PAGED_SATELLITES)
    if(accepted)host_.satelliteReceived(id,kind,0,{},nullptr,0);
#else
    (void)id;(void)kind;(void)accepted;
#endif
  }
  bool emitSatellite(const NodeCache::Entry &entry,Satellite::Kind kind) {
#if defined(CONFIG_MT_PAGED_SATELLITES)
    fresh(meshtastic_FromRadio_packet_tag);
    if(!companion::SatelliteWire::livePacket(entry,service_.satellites,kind,service_.config.hopLimit(),output_.packet)){
      ++encodeErrors;return true;
    }
    return emit(false);
#else
    fresh(meshtastic_FromRadio_packet_tag);auto &pkt=output_.packet;
    pkt.from=entry.id();pkt.to=UINT32_MAX;pkt.rx_snr=entry.snr;pkt.via_mqtt=entry.viaMqtt;
    pkt.channel=service_.satellites.channel(entry.id(),kind);
    pkt.rx_time=entry.lastHeard();pkt.has_rx_time=pkt.rx_time>=1577836800u;
    if(entry.heard){pkt.hop_start=service_.config.hopLimit();pkt.hop_limit=entry.hops<pkt.hop_start?pkt.hop_start-entry.hops:0;}
#if defined(CONFIG_MT_SATELLITE_HISTORY)
    auto stored=service_.satellites.received(entry.id(),kind);
    if(stored.restored()){
      pkt.rx_snr=stored.quarterSnr/4.f;pkt.via_mqtt=stored.viaMqtt();
      pkt.hop_start=stored.heard()?service_.config.hopLimit():0;
      pkt.hop_limit=stored.heard()&&stored.hops()<pkt.hop_start?pkt.hop_start-stored.hops():0;
    }
#endif
    pkt.priority=meshtastic_MeshPacket_Priority_BACKGROUND;
    pkt.transport_mechanism=meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    pkt.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    auto out=pb_ostream_from_buffer(pkt.decoded.payload.bytes,sizeof(pkt.decoded.payload.bytes));
    unsigned idKind=0;bool encoded=false;
    if(kind==Satellite::Status){
      meshtastic_StatusMessage status{};memcpy(status.status,entry.status.text(),sizeof(status.status));
      idKind=pkt.decoded.portnum=meshtastic_PortNum_NODE_STATUS_APP;
      encoded=pb_encode(&out,meshtastic_StatusMessage_fields,&status);
    } else if(kind==Satellite::Position){
      meshtastic_Position pos{};entry.position.exportReplay(pos);
      idKind=pkt.decoded.portnum=meshtastic_PortNum_POSITION_APP;
      encoded=pb_encode(&out,meshtastic_Position_fields,&pos);
    } else {
      meshtastic_Telemetry t{};
      if(kind==Satellite::Device){
        t.which_variant=meshtastic_Telemetry_device_metrics_tag;entry.deviceMetrics.exportTo(t.variant.device_metrics);
      } else {
        size_t n=0;const auto *wire=service_.satellites.environment(entry.id(),n);
        if(!wire)return true;auto input=pb_istream_from_buffer(wire,n);
        if(!pb_decode(&input,meshtastic_Telemetry_fields,&t)||t.which_variant!=meshtastic_Telemetry_environment_metrics_tag)return true;
      }
      t.time=pkt.rx_time;idKind=t.which_variant;pkt.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;
      encoded=pb_encode(&out,meshtastic_Telemetry_fields,&t);
    }
    if(!encoded){++encodeErrors;return true;}
    uint32_t id=entry.id()*2654435761u+pkt.rx_time;id=id*2654435761u+idKind;pkt.id=id?id:1;
    pkt.decoded.payload.size=out.bytes_written;
    return emit(false);
#endif
  }
  void replaySatellites() {
    if(!connected_)return;
    while(replayKind_!=Satellite::Done){
      if(statusReplayNext_==statusReplayCount_){
#if defined(CONFIG_MT_PAGED_SATELLITES)
        companion::SatelliteSample report{};
        const int rc=host_.satellitePacket(owner_,replayKind_,archiveCursor_,report);
        if(rc==-EAGAIN)return;
        if(rc==2){++archiveCursor_;return;}
        if(rc==1){
          fresh(meshtastic_FromRadio_packet_tag);
          if(companion::SatelliteWire::packet(report,report.utc,service_.config.hopLimit(),output_.packet)&&!emit(false))return;
          ++archiveCursor_;return;
        }
        archiveCursor_=0;
#endif
        replayKind_=static_cast<Satellite::Kind>(replayKind_+1);statusReplayNext_=0;continue;
      }
      const int slot=service_.nodes.find(statusReplayPeers_[statusReplayNext_]);
      const auto *e=slot<0?nullptr:service_.nodes.get(unsigned(slot));
      if(e&&!e->ignored()&&!e->placeholder()&&
         ((replayKind_==Satellite::Position&&e->replayPosition)||
          (replayKind_==Satellite::Device&&e->replayDevice)||
          (replayKind_==Satellite::Environment&&e->replayEnvironment)||
          (replayKind_==Satellite::Status&&e->status.replayable()&&e->status.text()[0]))){
        if(emitSatellite(*e,replayKind_))++statusReplayNext_;
        return;
      }
      ++statusReplayNext_;
    }
  }  bool canonicalHandshakeBoundary(const uint8_t *wire,size_t wireSize,uint32_t &nonce) {
    meshtastic_ToRadio candidate{};auto input=pb_istream_from_buffer(wire,wireSize);
    if(!pb_decode(&input,meshtastic_ToRadio_fields,&candidate)||
       candidate.which_payload_variant!=meshtastic_ToRadio_want_config_id_tag)return false;
    uint8_t canonical[6]{};auto output=pb_ostream_from_buffer(canonical,sizeof(canonical));
    if(!pb_encode(&output,meshtastic_ToRadio_fields,&candidate)||output.bytes_written!=wireSize||
       memcmp(canonical,wire,wireSize))return false;
    nonce=candidate.want_config_id;return true;
  }
  bool canonicalInput()const {
    uint8_t canonical[512]{};auto output=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&output,meshtastic_ToRadio_fields,&input_)&&output.bytes_written==length_&&
      !memcmp(canonical,frame_,length_);
  }
  meshtastic_Routing_Error applyOwner(const meshtastic_User &u,const uint8_t *wire,size_t wireSize,bool local) {
    bool mac=false;for(const auto byte:u.macaddr)mac|=byte!=0;
    const auto hardware=detail::enumNumber(u.hw_model),role=detail::enumNumber(u.role);
    if((!u.long_name[0]&&!u.short_name[0]&&!u.has_is_unmessagable)||mac||
       (!u.has_is_unmessagable&&u.is_unmessagable))return meshtastic_Routing_Error_BAD_REQUEST;
    // Android clones the current User when restoring a profile. Optional
    // echoes may identify this owner; only the names and preference are saved.
    if(u.id[0]||u.public_key.size||hardware||role||u.is_licensed) {
      if(!local)return meshtastic_Routing_Error_BAD_REQUEST;
      meshtastic_User current;user(current);
      if((u.id[0]&&strcmp(u.id,current.id))||(hardware&&hardware!=detail::enumNumber(current.hw_model))||
         (role&&role!=detail::enumNumber(current.role))||(u.is_licensed&&!current.is_licensed)||
         (u.public_key.size&&(u.public_key.size!=current.public_key.size||
          memcmp(u.public_key.bytes,current.public_key.bytes,u.public_key.size))))
        return meshtastic_Routing_Error_BAD_REQUEST;
    }
    if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_owner_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    Config c=service_.config;auto n=c.node();
    if((u.long_name[0]&&!n.setName(u.long_name,false))||(u.short_name[0]&&!n.setName(u.short_name,true)))return meshtastic_Routing_Error_BAD_REQUEST;
    c.node(n);
    if(u.has_is_unmessagable)c.setOwnerUnmessagable(u.is_unmessagable);
    if(c.same(service_.config)&&!host_.editing())return meshtastic_Routing_Error_NONE;
    return host_.save(c)==0?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST;
  }
  meshtastic_Routing_Error applyHamMode(const meshtastic_HamParameters &request,
                                       const uint8_t *wire,size_t wireSize) {
    if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_ham_mode_tag,wire,wireSize)||
       !service_.normalMode()||!service_.customPrimaryAvailable())return meshtastic_Routing_Error_BAD_REQUEST;
    const auto *region=radio::RegionPolicy::lookup(service_.config.regionCode());
    if(!region)return meshtastic_Routing_Error_BAD_REQUEST;
    admin::HamModeProjection::Capabilities caps;
    caps.licensedModePath=true;caps.plaintextChannelPath=true;
    caps.minTxPower=-9;caps.maxTxPower=region->powerLimitDbm<22?region->powerLimitDbm:22;
    caps.minFrequencyMHz=region->freqStartMHz;caps.maxFrequencyMHz=region->freqEndMHz;
    Config candidate=service_.config;auto record=candidate.node();char shortName[5]{};
    memcpy(shortName,record.bytes+bench::MtNodeRecord::Short,4);
    admin::HamModeProjection::Result projected;
    if(!admin::HamModeProjection::project(request,shortName,sizeof(shortName),caps,projected)||
       !record.setName(projected.longName,false)||!record.setName(projected.shortName,true))
      return meshtastic_Routing_Error_BAD_REQUEST;
    record.write32(bench::MtNodeRecord::Interval,projected.nodeInfoBroadcastSeconds);record.seal();
    candidate.node(record);
    if(!candidate.enableLicensedOperation())return meshtastic_Routing_Error_BAD_REQUEST;
    auto radio=candidate.radioRequest();radio.overrideMHz=projected.frequencyMHz;
    radio.powerSpecified=true;radio.power=projected.txPower;
    if(!candidate.setRadio(radio))return meshtastic_Routing_Error_BAD_REQUEST;
    if(projected.disableTx)candidate.setTxEnabled(false);
    // Preserve the existing signing identity, nonce authority, contacts and
    // message history. Ham setup changes channel/settings intent only.
    return bluetoothConfigResult(host_.save(candidate));
  }
  meshtastic_Routing_Error applyChannel(const meshtastic_Channel &c,const uint8_t *wire,size_t wireSize,uint32_t now,bool remote=false) {
    const auto role=detail::enumNumber(c.role);
    if(c.index<0||c.index>=8||
      !canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_channel_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    // An unavailable consent owner is not proof that a false/revoked uplink
    // is already durable. Check before all no-op and disabled-slot returns.
    if(remote&&!host_.mqttAvailable())return meshtastic_Routing_Error_NO_INTERFACE;
    const auto &s=c.settings;
    static constexpr char RedactedPsk[]="sekrit";
    uint8_t retainedPsk[32]{};detail::VolatileWipe retainedPskWipe{retainedPsk,sizeof(retainedPsk)};
    const uint8_t *pskBytes=s.psk.bytes;size_t pskSize=s.psk.size;
    const bool redactedPsk=pskSize==sizeof(RedactedPsk)-1&&
      !memcmp(pskBytes,RedactedPsk,sizeof(RedactedPsk)-1);
    if(redactedPsk) {
      if(c.index==0) {
        PrimaryChannel::Selection current{};
        const bool available=service_.config.channelSelection(0,current)&&!current.publicDefault&&
          !current.licensedPlaintext&&(current.length==16||current.length==32);
        if(available){pskSize=current.length;memcpy(retainedPsk,current.bytes,pskSize);}
        PrimaryChannel::clear(current);
        if(!available)return meshtastic_Routing_Error_BAD_REQUEST;
      } else {
        ChannelRecord current;detail::VolatileWipe currentWipe{&current,sizeof(current)};
        if(!service_.config.channelRecord(unsigned(c.index),current)||
           (current.keySize()!=16&&current.keySize()!=32))return meshtastic_Routing_Error_BAD_REQUEST;
        pskSize=current.keySize();memcpy(retainedPsk,current.key(),pskSize);
      }
      pskBytes=retainedPsk;
    }
    const bool defaultSettings=!s.channel_num&&!s.psk.size&&!s.name[0]&&!s.id&&
      !s.uplink_enabled&&!s.downlink_enabled&&!s.use_aead&&
      (!s.has_module_settings||(!s.module_settings.position_precision&&!s.module_settings.is_muted));
    // Clients clear unused slots when saving channels. They are already
    // disabled here: do not touch storage, the active queue, RX or the TX arm.
    if(c.index!=0) {
      if((role!=meshtastic_Channel_Role_DISABLED&&role!=meshtastic_Channel_Role_SECONDARY&&role!=meshtastic_Channel_Role_PRIMARY)||
        (role!=meshtastic_Channel_Role_DISABLED&&!c.has_settings)||!PrimaryChannel::validName(s.name)||
        ((s.uplink_enabled||s.downlink_enabled)&&!host_.mqttAvailable())||s.id||s.channel_num||
        (s.has_module_settings&&s.module_settings.position_precision>32))return meshtastic_Routing_Error_BAD_REQUEST;
      ChannelRecord stored;
      const unsigned precision=s.has_module_settings?s.module_settings.position_precision:0;
      // Disabled secondary slots retain supported name/key/mute metadata.
      // Their role keeps them out of channel selection and RF admission.
      if(!stored.set(role==meshtastic_Channel_Role_PRIMARY?2:role,s.name,pskBytes,pskSize,precision,
                     s.has_module_settings&&s.module_settings.is_muted,false,
                     role!=meshtastic_Channel_Role_DISABLED&&s.downlink_enabled,
                     role!=meshtastic_Channel_Role_DISABLED&&s.use_aead))return meshtastic_Routing_Error_BAD_REQUEST;
      Config out=service_.config;if(!out.setSecondaryChannel(unsigned(c.index),stored))return meshtastic_Routing_Error_BAD_REQUEST;
      PrimaryChannel::Selection key;
      if(role!=meshtastic_Channel_Role_DISABLED&&(!out.channelSelection(unsigned(c.index),key)||
        (!key.publicDefault&&!service_.customPrimaryAvailable()))){PrimaryChannel::clear(key);return meshtastic_Routing_Error_BAD_REQUEST;}
      PrimaryChannel::clear(key);
      if(role==meshtastic_Channel_Role_PRIMARY&&!out.promoteChannel(unsigned(c.index)))return meshtastic_Routing_Error_BAD_REQUEST;
      if(remote) {
        const unsigned consentIndex=role==meshtastic_Channel_Role_PRIMARY?0u:unsigned(c.index);
        if(!out.same(service_.config))
          return remoteConfigResult(host_.saveRemoteChannel(out,consentIndex,s.uplink_enabled,now));
        const bool uplinkChanged=s.uplink_enabled!=host_.channelUplinkEnabled(service_.config,consentIndex);
        if(uplinkChanged)return !host_.mqttAvailable()?meshtastic_Routing_Error_NO_INTERFACE:
          bluetoothConfigResult(host_.saveRemoteChannelUplink(out,consentIndex,s.uplink_enabled,now));
        return meshtastic_Routing_Error_NONE;
      }
      if(!out.same(service_.config)||host_.editing()){
        const int result=host_.save(out);if(result)return bluetoothConfigResult(result);
      }
      if(!host_.mqttAvailable())return (s.uplink_enabled||s.downlink_enabled)?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NONE;
      const unsigned consentIndex=role==meshtastic_Channel_Role_PRIMARY?0u:unsigned(c.index);
      return bluetoothConfigResult(host_.saveChannelUplink(out,consentIndex,s.uplink_enabled,now));
    }
    Config out=service_.config;auto n=out.node();
    if(role!=meshtastic_Channel_Role_DISABLED&&role!=meshtastic_Channel_Role_PRIMARY)
      return meshtastic_Routing_Error_BAD_REQUEST;
    if(role==meshtastic_Channel_Role_DISABLED) {
      if(!defaultSettings)return meshtastic_Routing_Error_BAD_REQUEST;
      n.setKey(0,nullptr,0);out.setChannelMuted(false);if(!out.setPrimaryPositionPrecision(0)||!out.setPrimaryDownlinkEnabled(false))return meshtastic_Routing_Error_BAD_REQUEST;
    }
    else {
      if(!c.has_settings||role!=meshtastic_Channel_Role_PRIMARY||
        !PrimaryChannel::validName(s.name)||((s.uplink_enabled||s.downlink_enabled)&&!host_.mqttAvailable())||s.id||s.channel_num||
        (s.has_module_settings&&s.module_settings.position_precision>32))
        return meshtastic_Routing_Error_BAD_REQUEST;
      if(s.has_module_settings&&s.module_settings.position_precision!=0&&!out.positionPotential())return meshtastic_Routing_Error_BAD_REQUEST;
      PrimaryChannel::Selection selection;
      const char *name=s.name[0]?s.name:out.modemName();
      if(out.licensedOperation()?
         (s.psk.size||!PrimaryChannel::normalizeLicensedPlaintext(name,selection)):
         !PrimaryChannel::normalize(pskBytes,pskSize,selection,name))return meshtastic_Routing_Error_BAD_REQUEST;
      if(!selection.publicDefault&&!service_.customPrimaryAvailable()) {
        PrimaryChannel::clear(selection);return meshtastic_Routing_Error_BAD_REQUEST;
      }
      if(selection.licensedPlaintext)n.setLicensedPlaintext(selection.hash);
      else n.setKey(selection.hash,selection.bytes,selection.length);
      PrimaryChannel::clear(selection);
      out.setChannelMuted(s.has_module_settings&&s.module_settings.is_muted);
      if(!out.setPrimaryDownlinkEnabled(s.downlink_enabled))return meshtastic_Routing_Error_BAD_REQUEST;
      if(!out.setPrimaryPositionPrecision(s.has_module_settings?s.module_settings.position_precision:0))return meshtastic_Routing_Error_BAD_REQUEST;
    }
    out.node(n);
    if(!out.setPrimaryName(role==meshtastic_Channel_Role_DISABLED?"":s.name))return meshtastic_Routing_Error_BAD_REQUEST;
    if(!out.setPrimaryAead(role!=meshtastic_Channel_Role_DISABLED&&s.use_aead))return meshtastic_Routing_Error_BAD_REQUEST;
    if(remote) {
      if(!out.same(service_.config))
        return remoteConfigResult(host_.saveRemoteChannel(out,0,s.uplink_enabled,now));
      const bool uplinkChanged=s.uplink_enabled!=host_.channelUplinkEnabled(service_.config,0);
      if(uplinkChanged)return !host_.mqttAvailable()?meshtastic_Routing_Error_NO_INTERFACE:
        bluetoothConfigResult(host_.saveRemoteChannelUplink(out,0,s.uplink_enabled,now));
      return meshtastic_Routing_Error_NONE;
    }
    if(!out.same(service_.config)||host_.editing()){
      const int result=host_.save(out);if(result)return bluetoothConfigResult(result);
    }
    if(!host_.mqttAvailable())return (s.uplink_enabled||s.downlink_enabled)?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NONE;
    return bluetoothConfigResult(host_.saveChannelUplink(out,0,s.uplink_enabled,now));
  }
  meshtastic_Routing_Error applyDisplay(const meshtastic_Config_DisplayConfig &d,
                                        const uint8_t *wire,size_t wireSize,uint32_t now,bool remote) {
    const auto gpsFormat=detail::enumNumber(d.gps_format),units=detail::enumNumber(d.units);
    const auto oled=detail::enumNumber(d.oled),displayMode=detail::enumNumber(d.displaymode);
    const auto compassOrientation=detail::enumNumber(d.compass_orientation);
    if(gpsFormat||d.compass_north_top||units||oled||
       displayMode||d.heading_bold||d.wake_on_tap_or_motion||compassOrientation||
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
       (remote&&d.use_12h_clock!=service_.config.twelveHourClock())||
#else
       d.use_12h_clock||
#endif
       d.enable_message_bubbles||
       !canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    Config out=service_.config;
    if(!out.setDisplay(d.screen_on_secs,d.flip_screen,d.auto_screen_carousel_secs))
      return meshtastic_Routing_Error_BAD_REQUEST;
    out.setDisplayLongNames(d.use_long_node_name);
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
    out.twelveHourClock(d.use_12h_clock);
#endif
    if(out.same(service_.config))return meshtastic_Routing_Error_NONE;
    bool displayProfile=board::capabilityModel(model_)==meshtastic_HardwareModel_HELTEC_MESH_NODE_T096;
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED) && defined(CONFIG_MT_DISPLAY)
    displayProfile|=board::capabilityModel(model_)==meshtastic_HardwareModel_SEEED_XIAO_S3;
#elif defined(CONFIG_BOARD_XIAO_ESP32S3) || !defined(__ZEPHYR__)
    // Router role defaults persist the timeout on the bare XIAO as well.
    // A local owner can restore it while leaving panel-specific options alone.
    if(board::capabilityModel(model_)==meshtastic_HardwareModel_SEEED_XIAO_S3&&!remote)
      displayProfile=d.flip_screen==service_.config.displayFlip()&&
                     d.auto_screen_carousel_secs==service_.config.displayCarouselSeconds()&&
                     d.use_long_node_name==service_.config.displayLongNames();
#endif
    if(!displayProfile)return meshtastic_Routing_Error_BAD_REQUEST;
    const int result=remote?host_.saveRemoteConfig(out,now):host_.save(out);
    return remote?remoteConfigResult(result):bluetoothConfigResult(result);
  }
  meshtastic_Routing_Error applyDevice(const meshtastic_Config_DeviceConfig &d,uint32_t now,const uint8_t *wire,size_t wireSize,
                                      uint32_t packetId,bool localRequest) {
    // This is a complete DeviceConfig replacement, as in stock. Do not ACK
    // unsupported role, pin, rebroadcast or UI changes as if applied.
    // Nanopb preserves arbitrary wire enum integers. Read their representation
    // before validation: loading an out-of-range C++ enum itself is undefined.
    const auto role=detail::enumNumber(d.role),rebroadcast=detail::enumNumber(d.rebroadcast_mode),buzzer=detail::enumNumber(d.buzzer_mode);
    const bool deprecatedAlias=role==3u||role==4u;
    if((!deprecatedAlias&&!DeviceRolePolicy::supportedRole(role))||!DeviceRolePolicy::supportedRebroadcast(rebroadcast)||
       d.serial_enabled!=service_.config.serialEnabled()||d.is_managed!=service_.config.isManaged()||
       d.button_gpio||d.buzzer_gpio||d.double_tap_as_button_press||
       d.disable_triple_click||buzzer)return meshtastic_Routing_Error_BAD_REQUEST;
    if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize))return meshtastic_Routing_Error_BAD_REQUEST;
    Config out=service_.config;
#if defined(CONFIG_MT_LOCAL_CLOCK)
    if(!out.timezoneEquals(d.tzdef)&&(!localRequest||!out.setTimezone(d.tzdef)))return meshtastic_Routing_Error_BAD_REQUEST;
#else
    for(char ch:d.tzdef)if(ch)return meshtastic_Routing_Error_BAD_REQUEST;
#endif
    const uint32_t effectiveRole=deprecatedAlias?DeviceRolePolicy::Role::CLIENT:role;
    // Stock replaces an invalid ROUTER+NONE request with ALL.
    const auto normalized=(effectiveRole==2||effectiveRole==11)&&rebroadcast==4?0u:rebroadcast;
    // These deprecated aliases also require a StoreForward journal. TAK
    // has a separate owner and is never part of this RF mutation surface.
    if(!localRequest&&(deprecatedAlias||role==10u))return meshtastic_Routing_Error_NO_INTERFACE;
    if(!out.setDeviceRole(effectiveRole,normalized))return meshtastic_Routing_Error_BAD_REQUEST;
    if(!out.setNodeInfoSeconds(d.node_info_broadcast_secs))return meshtastic_Routing_Error_BAD_REQUEST;
    // Busy endpoints may safely echo supported unchanged settings. A batch's
    // same-config path refreshes activity without touching storage or RF.
    if(localRequest&&!deprecatedAlias&&out.same(service_.config)&&d.led_heartbeat_disabled==host_.heartbeatDisabled()&&!host_.editing())return meshtastic_Routing_Error_NONE;
    const int saved=!localRequest?host_.saveRemoteDevice(out,d.led_heartbeat_disabled,now):(deprecatedAlias?
      host_.saveDeviceDeprecatedRole(out,d.led_heartbeat_disabled,now):
      host_.saveDevice(out,d.led_heartbeat_disabled,now));
    // Defaults/partial status is meaningful only for the local legacy-role
    // transition. Remote preparation must be fully durable or fail closed.
    if(!localRequest)return saved?remoteConfigResult(saved):meshtastic_Routing_Error_NONE;
    if(saved==Host::DeviceDefaultsPartial) {
      if(localRequest)notification(packetId,
        MT_NOTICE("ROUTER settings accepted; power-saving and optional sensor defaults are unavailable"),false);
      return meshtastic_Routing_Error_NONE;
    }
    return !saved?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST;
  }
  meshtastic_Routing_Error applyPositionConfig(const meshtastic_Config_PositionConfig &p,
                                                const uint8_t *wire,size_t wireSize,uint32_t now,bool remote) {
    const auto gpsMode=detail::enumNumber(p.gps_mode);
    // Passive GNSS, fixed position and smart/periodic cadence. Unmeasured
    // optional fields and arbitrary hardware pin changes remain refused.
    if(p.gps_enabled||p.rx_gpio||p.tx_gpio||p.gps_en_gpio||
       gpsMode>meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT||
       (!host_.gnssPresent()&&gpsMode!=meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT)||
       !canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize))return meshtastic_Routing_Error_BAD_REQUEST;
    Config out=service_.config;
    if(p.fixed_position&&!out.fixedPosition())return meshtastic_Routing_Error_BAD_REQUEST;
    if(!p.fixed_position&&out.fixedPosition())out.clearFixedPosition();
    GnssSettings g;g.enabled=host_.gnssPresent()&&gpsMode==meshtastic_Config_PositionConfig_GpsMode_ENABLED;
    g.positionFlags=p.position_flags;g.updateSecs=p.gps_update_interval;g.attemptSecs=p.gps_attempt_time;g.broadcastSecs=p.position_broadcast_secs;
    g.smart=p.position_broadcast_smart_enabled;g.smartMinDistance=p.broadcast_smart_minimum_distance;
    g.smartMinInterval=p.broadcast_smart_minimum_interval_secs;
    if(!out.setGnssSettings(g))return meshtastic_Routing_Error_BAD_REQUEST;
    if(out.same(service_.config)&&!host_.editing())return meshtastic_Routing_Error_NONE;
    const int result=remote?host_.saveRemoteConfig(out,now):host_.save(out);
    return remote?remoteConfigResult(result):bluetoothConfigResult(result);
  }
  static bool supportedFixedPosition(const meshtastic_Position &p) {
    const auto locationSource=detail::enumNumber(p.location_source),altitudeSource=detail::enumNumber(p.altitude_source);
    if((!p.has_latitude_i&&p.latitude_i)||(!p.has_longitude_i&&p.longitude_i)||(!p.has_altitude&&p.altitude)||
      p.time||locationSource||altitudeSource||p.timestamp||p.timestamp_millis_adjust||
      p.has_altitude_hae||p.altitude_hae||p.has_altitude_geoidal_separation||p.altitude_geoidal_separation||
      p.PDOP||p.HDOP||p.VDOP||p.gps_accuracy||p.has_ground_speed||p.ground_speed||
      p.has_ground_track||p.ground_track||p.fix_quality||p.fix_type||p.sats_in_view||p.sensor_id||
      p.next_update||p.seq_number||p.precision_bits)return false;
    const int32_t latitude=p.has_latitude_i?p.latitude_i:0;
    const int32_t longitude=p.has_longitude_i?p.longitude_i:0;
    return latitude>=-900000000&&latitude<=900000000&&longitude>=-1800000000&&longitude<=1800000000;
  }
  meshtastic_Routing_Error applyFixedPosition(const meshtastic_Position &p,const uint8_t *wire,size_t wireSize,
                                                bool remote=false,uint32_t now=0) {
    if(!supportedFixedPosition(p)||
      !canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_fixed_position_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    Config out=service_.config;
    if(!out.setFixedPosition(p.has_latitude_i?p.latitude_i:0,p.has_longitude_i?p.longitude_i:0,
                            p.has_altitude,p.has_altitude?p.altitude:0))
      return meshtastic_Routing_Error_BAD_REQUEST;
    if(out.same(service_.config)&&!host_.editing())return meshtastic_Routing_Error_NONE;
    const int result=remote?host_.saveRemoteConfig(out,now):host_.save(out);
    return remote?remoteConfigResult(result):result==0?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST;
  }
  meshtastic_Routing_Error applyRemoveFixedPosition(bool remove,const uint8_t *wire,size_t wireSize,
                                                     bool remote=false,uint32_t now=0) {
    if(!remove||!canonicalAdminOperation(admin_,meshtastic_AdminMessage_remove_fixed_position_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    Config out=service_.config;out.clearFixedPosition();
    if(out.same(service_.config)&&!host_.editing())return meshtastic_Routing_Error_NONE;
    const int result=remote?host_.saveRemoteConfig(out,now):host_.save(out);
    return remote?remoteConfigResult(result):result==0?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST;
  }
  meshtastic_Routing_Error applyInputEvent(const meshtastic_AdminMessage_InputEvent &input,
                                           const uint8_t *wire,size_t wireSize,uint32_t now) {
    if(!host_.ready()||input.kb_char||input.touch_x||input.touch_y||
      !canonicalAdminOperation(admin_,meshtastic_AdminMessage_send_input_event_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    const unsigned code=input.event_code;
    if(code!=10&&code!=17&&code!=18&&code!=19&&code!=20&&code!=27&&code!=28)
      return meshtastic_Routing_Error_BAD_REQUEST;
    const int ret=host_.uiInput(code,now);
    return !ret?meshtastic_Routing_Error_NONE:
      ret==-EINVAL?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE;
  }
  bool sameLoRaRuntime(const Config &candidate,bool enable)const {
    if(!host_.ready()||!service_.normalMode()||service_.faulted())return false;
    if(host_.editing()) {
      if(!candidate.same(service_.config))return false;
      if(!candidate.provisioned())return !enable&&!service_.receiving()&&!service_.endpointArmed();
      // beginEdit() deliberately leaves an existing interface in RX with TX
      // disarmed. Preserve the durable LoRa TX intent and this quiescent batch
      // state; do not mistake the batch barrier for a requested TX change.
      return service_.receiving()&&!service_.endpointArmed()&&
        enable==service_.config.txEnabled()&&(!enable||service_.txAllowed());
    }
    if(!candidate.provisioned())return !enable&&!service_.receiving()&&!service_.endpointArmed();
    return service_.receiving()&&service_.endpointArmed()==enable&&(!enable||service_.txAllowed());
  }
  static meshtastic_Routing_Error bluetoothConfigResult(int result) {
    return !result?meshtastic_Routing_Error_NONE:
      result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
      result==-EIO?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_BAD_REQUEST;
  }
  static meshtastic_Routing_Error remoteConfigResult(int result) {
    return !result?meshtastic_Routing_Error_NONE:
      result==-EBUSY||result==-EAGAIN?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
      result==-EINVAL?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE;
  }
  meshtastic_Routing_Error applyBluetooth(const meshtastic_Config_BluetoothConfig &b,
                                           uint32_t now,const uint8_t *wire,size_t wireSize,
                                           uint32_t packetId,bool wantAck,bool wantResponse,bool &deferred) {
    const auto mode=detail::enumNumber(b.mode);
    if((mode!=meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN&&
        mode!=meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN&&
        mode!=meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN)||b.fixed_pin>999999u||
      (mode==meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN&&board::capabilityModel(model_)!=meshtastic_HardwareModel_HELTEC_MESH_NODE_T096)||
      !canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize)||host_.editing())
      return meshtastic_Routing_Error_BAD_REQUEST;
    const int result=host_.saveBluetoothConfigRequest(b,now,packetId,wantAck,wantResponse);
    deferred=result==Host::BluetoothDeferred;
    return deferred?meshtastic_Routing_Error_NONE:bluetoothConfigResult(result);
  }
  meshtastic_Routing_Error applyConfig(const meshtastic_Config &c,uint32_t now,const uint8_t *wire,size_t wireSize,
                                      uint32_t packetId,bool wantAck,bool wantResponse,bool &deferred,
                                      bool localRequest) {
    if(c.which_payload_variant==meshtastic_Config_device_tag)
      return applyDevice(c.payload_variant.device,now,wire,wireSize,packetId,localRequest);
    if(c.which_payload_variant==meshtastic_Config_display_tag)
      return applyDisplay(c.payload_variant.display,wire,wireSize,now,!localRequest);
    if(c.which_payload_variant==meshtastic_Config_position_tag)return applyPositionConfig(c.payload_variant.position,wire,wireSize,now,!localRequest);
    if(c.which_payload_variant==meshtastic_Config_bluetooth_tag)
      return applyBluetooth(c.payload_variant.bluetooth,now,wire,wireSize,packetId,wantAck,wantResponse,deferred);
    if(c.which_payload_variant==meshtastic_Config_power_tag){
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize))return meshtastic_Routing_Error_BAD_REQUEST;
      return bluetoothConfigResult(host_.savePowerConfig(c.payload_variant.power,now));
    }
    if(c.which_payload_variant==meshtastic_Config_network_tag){
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize))return meshtastic_Routing_Error_BAD_REQUEST;
      return bluetoothConfigResult(host_.saveNetworkConfig(c.payload_variant.network,now));
    }
    if(c.which_payload_variant==meshtastic_Config_security_tag){
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
      if (c.payload_variant.security.private_key.size) {
        const auto &value = c.payload_variant.security;
        const auto link = host_.bluetoothSecurity();
        const bool usb = owner_ == 0 && acceptedUsbFrame_ && !recoveryOnly_ && host_.consoleAllowed();
        const bool ble = owner_ == 1 && link.connected && link.level >= 3 && !link.pending &&
                         !link.error && host_.stockIdentityBleLink();
        const bool keysOnly = value.public_key.size == 32 && value.private_key.size == 32 &&
                              !value.admin_key_count && !value.is_managed && !value.serial_enabled &&
                              !value.debug_log_api_enabled && !value.admin_channel_enabled &&
                              !detail::enumNumber(value.packet_signature_policy);
        if (!localRequest || !connected_ || (!usb && !ble) || host_.editing() || !keysOnly ||
            !canonicalAdminOperation(admin_, meshtastic_AdminMessage_set_config_tag, wire, wireSize))
          return meshtastic_Routing_Error_BAD_REQUEST;

        const int staged = host_.stageStockIdentity(value.private_key.bytes, value.public_key.bytes, usb, frameEpoch_);
        if (staged) return meshtastic_Routing_Error_BAD_REQUEST;
        notification(packetId, MT_NOTICE("Identity staged in RAM. Confirm the public key over USB before reboot."), false);
        return meshtastic_Routing_Error_NONE;
      }
#endif

      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize)||host_.editing())
        return meshtastic_Routing_Error_BAD_REQUEST;
      const auto &value=c.payload_variant.security;
      const uint32_t policy=detail::enumNumber(value.packet_signature_policy);
      if(policy>2||value.private_key.size||value.admin_key_count>3)
        return meshtastic_Routing_Error_BAD_REQUEST;
      uint8_t publicKey[32]{};
      if(value.public_key.size&&(value.public_key.size!=32||!service_.privatePublicIdentity(publicKey)||
         memcmp(value.public_key.bytes,publicKey,32)))return meshtastic_Routing_Error_BAD_REQUEST;
      bool sameKeys=false,currentKeysValid=false;pb_size_t currentKeyCount=0;
#if defined(CONFIG_MT_RF_ADMIN_GET)
      meshtastic_Config_SecurityConfig current{};
      if(!host_.adminKeys(current))return meshtastic_Routing_Error_NO_INTERFACE;
      currentKeyCount=current.admin_key_count;
      currentKeysValid=current.admin_key_count<=3;
      sameKeys=currentKeysValid&&current.admin_key_count==value.admin_key_count;
      for(pb_size_t i=0;i<current.admin_key_count&&i<3;++i) {
        uint8_t aggregate=0;for(uint8_t b:current.admin_key[i].bytes)aggregate|=b;
        if(current.admin_key[i].size!=32||!aggregate)currentKeysValid=false;
      }
      for(pb_size_t i=0;i<value.admin_key_count;++i) {
        if(value.admin_key[i].size!=32)return meshtastic_Routing_Error_BAD_REQUEST;
        uint8_t aggregate=0;for(uint8_t b:value.admin_key[i].bytes)aggregate|=b;
        if(!aggregate)return meshtastic_Routing_Error_BAD_REQUEST;
        sameKeys=sameKeys&&current.admin_key[i].size==32&&
          !memcmp(current.admin_key[i].bytes,value.admin_key[i].bytes,32);
      }
#else
      currentKeysValid=true;
      sameKeys=value.admin_key_count==0;
#endif
      if(!currentKeysValid)return meshtastic_Routing_Error_BAD_REQUEST;
      // The remote RF selector is only for flag/policy recovery. Allowlist
      // edits remain local, single-journal operations while unmanaged.
      if(!localRequest&&!sameKeys)return meshtastic_Routing_Error_BAD_REQUEST;
      const bool coreChanged=value.is_managed!=service_.config.isManaged()||
        value.serial_enabled!=service_.config.serialEnabled()||
        policy!=service_.config.packetSignaturePolicy()||
        value.debug_log_api_enabled!=service_.config.debugLogApiEnabled()||
        value.admin_channel_enabled!=service_.config.adminChannelEnabled();
      if(coreChanged) {
        // Flags/policy and public keys use separate durable journals. Reject a
        // combined write so the ACK can never conceal a partial transaction.
        if(!sameKeys)return meshtastic_Routing_Error_BAD_REQUEST;
        if(value.is_managed&&(!currentKeyCount||!currentKeysValid))
          return meshtastic_Routing_Error_BAD_REQUEST;
        Config candidate=service_.config;
        if(!candidate.setSecurityControls(value.is_managed,value.serial_enabled)||
           !candidate.setSecurityOptions(value.debug_log_api_enabled,value.admin_channel_enabled)||
           !candidate.setPacketSignaturePolicy(policy))return meshtastic_Routing_Error_BAD_REQUEST;
        const int result=localRequest?host_.saveSecurityControls(candidate,now):host_.saveRemoteConfig(candidate,now);
        return localRequest?bluetoothConfigResult(result):remoteConfigResult(result);
      }
      if(sameKeys)return meshtastic_Routing_Error_NONE;
#if defined(CONFIG_MT_RF_ADMIN_GET)
      // Do not remove the last recovery key while managed mode remains active.
      if(service_.config.isManaged()&&!value.admin_key_count)return meshtastic_Routing_Error_BAD_REQUEST;
      // Invalidate before attempting persistence: a failed result may still
      // leave the allowlist's durable outcome uncertain.
      clearPrivateAdminSessions();
      return bluetoothConfigResult(host_.saveAdminKeys(c.payload_variant.security,now));
#else
      return sameKeys?meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST;
#endif
    }
    if(c.which_payload_variant!=meshtastic_Config_lora_tag)return meshtastic_Routing_Error_BAD_REQUEST;
    const auto &l=c.payload_variant.lora;uint32_t region=0;
    if(!rawConfigRegion(wire,wireSize,region)||region!=detail::enumNumber(l.region))
      return meshtastic_Routing_Error_BAD_REQUEST;
    const auto preset=detail::enumNumber(l.modem_preset),fem=detail::enumNumber(l.fem_lna_mode);
    // Nanopb decodes tx_power into int8_t and refuses out-of-range wire values.
    if(!Config::supportedRegionCode(region,!l.tx_enabled)||(region==0&&l.tx_enabled)||
       !l.sx126x_rx_boosted_gain||(l.tx_enabled&&!service_.txAllowed())||
       (l.override_duty_cycle&&!service_.config.licensedOperation())||
       l.serial_hal_only||(fem&&!(xiaoWithoutExternalLna()&&fem==meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT))||
       l.pa_fan_disabled||!HopPolicy::valid(l.hop_limit))
      return meshtastic_Routing_Error_BAD_REQUEST;
    if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_config_tag,wire,wireSize))
      return meshtastic_Routing_Error_BAD_REQUEST;
    Config out=service_.config;
    auto request=out.radioRequest();request.regionCode=region;request.usePreset=l.use_preset;
    request.preset=l.use_preset&&region==meshtastic_Config_LoRaConfig_RegionCode_US&&
      !RadioSettings::presetSupported(preset)?0:preset;
    request.channel=!l.channel_num&&region==service_.config.regionCode()&&l.use_preset&&service_.config.usePreset()&&preset==0&&service_.config.modemPreset()==0&&
      service_.config.channelNumber()==20&&!l.override_frequency&&!l.frequency_offset?20:l.channel_num;
    request.bandwidth=l.use_preset?l.bandwidth:(uint16_t(l.bandwidth)?uint16_t(l.bandwidth):250);
    request.spreadingFactor=ManualLoRa::spreadingFactor(l.spread_factor);request.codingRate=ManualLoRa::codingDenominator(l.coding_rate);
    request.overrideMHz=l.override_frequency;request.offsetMHz=l.frequency_offset;
    request.powerSpecified=out.powerSpecified()||l.tx_power!=(board::capabilityModel(model_)==meshtastic_HardwareModel_HELTEC_MESH_NODE_T096?-9:2);request.power=l.tx_power;
    // A full proto3 LoRa SET supplies the complete TX setting (false is
    // omitted on the wire). Validate the new region and intent as one record.
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
    out.setTxEnabled(l.tx_enabled);
#endif
    if(!out.setRadio(request)||!out.setIgnoredSenders(l.ignore_incoming,l.ignore_incoming_count)||
       !out.setHopLimit(l.hop_limit)||!out.setIgnoreMqtt(l.ignore_mqtt)||
       !out.setOverrideDutyCycle(l.override_duty_cycle))return meshtastic_Routing_Error_BAD_REQUEST;
#if !defined(CONFIG_BOARD_XIAO_ESP32S3)
    out.setTxEnabled(l.tx_enabled);
#endif
    const bool regionChanged=out.regionCode()!=service_.config.regionCode();
    // Equal persisted bytes are an inert success only when runtime state
    // already expresses the same intent. A batch still traverses save() so its
    // idle deadline is refreshed without storage or radio work.
    const bool mqttConsentChanged=l.config_ok_to_mqtt!=host_.configOkToMqtt();
    const bool unchangedRetry=!host_.editing()&&host_.mqttRegionRootRetryPending()&&
      out.same(service_.config)&&
      l.tx_enabled==service_.config.txEnabled();
    // Permit an identical, already durable LoRa request to reach the region
    // reconciliation hook after a prior MQTT journal I/O error. A new LoRa
    // change or an open edit still requires the MQTT capability to be ready.
    if(l.config_ok_to_mqtt&&!host_.mqttAvailable()&&!unchangedRetry)
      return meshtastic_Routing_Error_BAD_REQUEST;
    if(!localRequest) {
      // A fallback false getter cannot acknowledge an inaccessible durable
      // consent value. The explicit unchanged retry re-reads/verifies it.
      if(!host_.mqttAvailable()&&!unchangedRetry)return meshtastic_Routing_Error_NO_INTERFACE;
      // An unchanged profile can adjust only explicit MQTT consent or retry a
      // pending derived-root update through the existing typed MQTT owner.
      if(out.same(service_.config)&&sameLoRaRuntime(out,l.tx_enabled)) {
        if(mqttConsentChanged||host_.mqttRegionRootRetryPending()) {
          const char *regionName=out.regionName();
          return bluetoothConfigResult(host_.saveRemoteMqttRegionRootTopic(regionName,strlen(regionName),
                                                                      l.config_ok_to_mqtt,now));
        }
        return meshtastic_Routing_Error_NONE;
      }
      // The owner projects candidate-region root policy and ConfigOk consent
      // into the same durable remote-reply transaction as the core Config.
      return remoteConfigResult(host_.saveRemoteLoRa(out,l.config_ok_to_mqtt,now));
    }
    if(host_.editing()&&l.tx_enabled) {
      // A receive filter may change while an edit holds durable TX intent disarmed.
      // Save just that edit. Commit owns any eventual re-arm.
      if(!mqttConsentChanged&&!service_.endpointArmed()&&!out.same(service_.config)&&
         out.sameExceptIgnoredSenders(service_.config))
        return bluetoothConfigResult(host_.save(out));
      // A Bluetooth batch may stage an MQTT consent toggle through a LoRa
      // round-trip only. Any core/radio change or TX runtime change remains
      // refused until the batch is committed.
      if(!mqttConsentChanged||!out.same(service_.config)||!sameLoRaRuntime(out,l.tx_enabled))
        return meshtastic_Routing_Error_BAD_REQUEST;
      const int saved=host_.save(out);if(saved)return bluetoothConfigResult(saved);
      return bluetoothConfigResult(host_.saveConfigOkToMqtt(l.config_ok_to_mqtt,now));
    }
    if(out.same(service_.config)&&sameLoRaRuntime(out,l.tx_enabled)) {
      if(host_.editing()) {
        const int saved=host_.save(out);if(saved)return bluetoothConfigResult(saved);
        return mqttConsentChanged?bluetoothConfigResult(host_.saveConfigOkToMqtt(l.config_ok_to_mqtt,now)):
          meshtastic_Routing_Error_NONE;
      }
      if(!host_.mqttRegionRootRetryPending())
        return mqttConsentChanged?bluetoothConfigResult(host_.saveConfigOkToMqtt(l.config_ok_to_mqtt,now)):
          meshtastic_Routing_Error_NONE;
      const char *regionName=out.regionName();
      return bluetoothConfigResult(host_.saveMqttRegionRootTopic(
        regionName,strlen(regionName),l.config_ok_to_mqtt,now));
    }
    const uint32_t pauseBefore=service_.storagePauseDiagnostic().sequence;
    const int saved=host_.save(out);
    if(saved) {
      const auto pauseAfter=service_.storagePauseDiagnostic();
      lastLoRaPauseFresh_=pauseAfter.sequence!=pauseBefore;
      lastLoRaPause_=lastLoRaPauseFresh_?pauseAfter:StoragePauseDiagnostic{};
      lastLoRaSaveResult_=saved;++loRaSaveFailures_;
      // A valid setting may be refused by the radio/storage quiescence gate.
      // Keep that gate and all persistence behavior intact; do not label a
      // transient resource refusal as malformed LoRa configuration.
      return saved==-EBUSY||saved==-EAGAIN?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        meshtastic_Routing_Error_NO_INTERFACE;
    }
    // The requested TX state is durable. Runtime arm still requires the
    // compiled hardware gate, healthy RX, and no open settings transaction.
    if(!host_.editing()&&out.provisioned()&&!service_.receiving()&&!service_.start(false,now))return meshtastic_Routing_Error_NO_INTERFACE;
    if(!service_.armEndpoint(l.tx_enabled))return meshtastic_Routing_Error_NO_INTERFACE;
    const bool reconcileRegionRoot=regionChanged||(!host_.editing()&&host_.mqttRegionRootRetryPending());
    if(reconcileRegionRoot||mqttConsentChanged){
      const char *regionName=out.regionName();
      const int consent=reconcileRegionRoot?
        host_.saveMqttRegionRootTopic(regionName,strlen(regionName),l.config_ok_to_mqtt,now):
        host_.saveConfigOkToMqtt(l.config_ok_to_mqtt,now);
      if(consent)return bluetoothConfigResult(consent);
    }
    return meshtastic_Routing_Error_NONE;
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  void sendPrivateAdminGet(const meshtastic_MeshPacket &p,uint32_t now) {
    int result=-EINVAL;
    do {
      if(!canonicalInput()||!p.id||p.to<=1||p.to>=0xfffffffeu||p.to==service_.route.identity()||
         p.channel||p.hop_limit>HopPolicy::Maximum||p.hop_start||!p.pki_encrypted||p.xeddsa_signed||
         detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT)break;
      uint8_t data[admin::remote_protocol::MaxDataBytes]{};
      struct Wipe {uint8_t *p;~Wipe(){bench::wipeKeyBuffer(p,admin::remote_protocol::MaxDataBytes);}}wipe{data};
      auto stream=pb_ostream_from_buffer(data,sizeof(data));
      if(!pb_encode(&stream,meshtastic_Data_fields,&p.decoded))break;
      result=service_.submitPrivateAdminGet(owner_,p.to,p.id,data,stream.bytes_written,
        originHop(p),p.want_ack,p.public_key.bytes,p.public_key.size,true,now);
    } while(false);
    queue(p.id);
    if(result){
      const auto code=result==-EINVAL?meshtastic_Routing_Error_BAD_REQUEST:
        result==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EACCES||result==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_PKI_FAILED;
      reject(p.id,code,MT_NOTICE("Private Admin GET unavailable"));++deniedTx;
    }
  }

#endif
  void handleAdmin(const meshtastic_MeshPacket &p,uint32_t now) {
    // This is the stock ordinary-build managed policy: no local Admin GET or
    // SET on any companion transport while managed. The separate authenticated
    // RF mutation path does not dispatch through this local handler.
    if(service_.config.isManaged()){
      reject(p.id,meshtastic_Routing_Error_NOT_AUTHORIZED,MT_NOTICE("Local Admin disabled while managed"));return;
    }
    admin_={};reply_={};detail::VolatileWipe wipeAdmin{&admin_,sizeof(admin_)};
    detail::VolatileWipe wipeReply{&reply_,sizeof(reply_)};
    auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
    if(!pb_decode(&stream,meshtastic_AdminMessage_fields,&admin_)){reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Malformed local admin request"));return;}
    auto error=meshtastic_Routing_Error_NONE;bool nodeCommittedDegraded=false,bluetoothDeferred=false,contactPartiallyCommitted=false;
    switch(admin_.which_payload_variant) {
    case meshtastic_AdminMessage_factory_reset_device_tag:
    case meshtastic_AdminMessage_factory_reset_config_tag:
    case meshtastic_AdminMessage_delete_file_request_tag:
    case meshtastic_AdminMessage_nodedb_reset_tag: {
      const auto operation=admin_.which_payload_variant;
      if(!canonicalAdminOperation(admin_,operation,p.decoded.payload.bytes,p.decoded.payload.size)||host_.editing()){
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const int result=operation==meshtastic_AdminMessage_delete_file_request_tag?
        host_.removeAppFile(admin_.delete_file_request):operation==meshtastic_AdminMessage_nodedb_reset_tag?
          host_.resetNodeDb(admin_.nodedb_reset,now):operation==meshtastic_AdminMessage_factory_reset_device_tag?
          host_.requestFactoryDeviceReset():host_.requestFactoryConfigReset();
      // A committed reset closes storage until reboot. Return its correlated
      // ACK while this transport session is still authorized.
      if(!result&&(operation==meshtastic_AdminMessage_factory_reset_device_tag||
                   operation==meshtastic_AdminMessage_factory_reset_config_tag)) {
        ack(p.id,meshtastic_Routing_Error_NONE);return;
      }
      if(result)error=result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EINVAL||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE;
      break;
    }
    case meshtastic_AdminMessage_backup_preferences_tag:
    case meshtastic_AdminMessage_restore_preferences_tag:
    case meshtastic_AdminMessage_remove_backup_preferences_tag: {
      const auto operation=admin_.which_payload_variant;
      const uint32_t location=operation==meshtastic_AdminMessage_backup_preferences_tag?
        detail::enumNumber(admin_.backup_preferences):operation==meshtastic_AdminMessage_restore_preferences_tag?
        detail::enumNumber(admin_.restore_preferences):detail::enumNumber(admin_.remove_backup_preferences);
      if(location!=meshtastic_AdminMessage_BackupLocation_FLASH||
         !canonicalAdminOperation(admin_,operation,p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      UsbSessionEpoch::RestoreReceipt receipt(p.id,frameEpoch_);
      const bool usbRestore=owner_==0&&acceptedUsbFrame_&&connected_&&!recoveryOnly_&&p.id&&host_.consoleAllowed()&&
        operation==meshtastic_AdminMessage_restore_preferences_tag;
      const int result=host_.preferences(operation,now,usbRestore?&receipt:nullptr);
      error=!result?meshtastic_Routing_Error_NONE:
        result==-EINVAL||result==-ENOENT||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:
        result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      if(receipt.granted()){
        // Only this correlated Routing result can cross the restore's own
        // privilege fence. No notification, config, or ordinary frame uses it.
        if(buildAck(p.id,error)){
          uint8_t bytes[64];const auto size=encodeOutput(bytes,sizeof(bytes));
          if(!size||!host_.sendRestoreReceipt(receipt,p.id,bytes,size))++outputDrops;
        }
      }else if(result)reject(p.id,error,MT_NOTICE("Local preference operation failed; refresh state before retrying"));
      else ack(p.id,error);
      return;
    }
    case meshtastic_AdminMessage_get_ringtone_request_tag:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_get_ringtone_request_tag,p.decoded.payload.bytes,p.decoded.payload.size)){
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      if(p.decoded.want_response){
        reply_.which_payload_variant=meshtastic_AdminMessage_get_ringtone_response_tag;
        if(host_.ringtone(reply_.get_ringtone_response,sizeof(reply_.get_ringtone_response)))error=meshtastic_Routing_Error_NO_INTERFACE;
      }
      break;
    case meshtastic_AdminMessage_set_ringtone_message_tag:{
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_ringtone_message_tag,p.decoded.payload.bytes,p.decoded.payload.size)){
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const size_t length=strnlen(admin_.set_ringtone_message,sizeof(admin_.set_ringtone_message));
      if(length==sizeof(admin_.set_ringtone_message)){error=meshtastic_Routing_Error_BAD_REQUEST;break;}
      error=bluetoothConfigResult(host_.saveRingtone(admin_.set_ringtone_message,length,now));break;
    }
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_get_canned_message_module_messages_request_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      // Stock sends the list response only for a requested response, while
      // accepting the same canonical operation as a silent read otherwise.
      if(p.decoded.want_response) {
        char messages[201]{};size_t length=0;
        const int result=host_.cannedMessageMessages(messages,sizeof(messages));
        if(result)error=meshtastic_Routing_Error_NO_INTERFACE;
        else if(!validCannedMessageList(messages,sizeof(messages),length))error=meshtastic_Routing_Error_NO_INTERFACE;
        else {
          reply_.which_payload_variant=meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
          memcpy(reply_.get_canned_message_module_messages_response,messages,length+1);
        }
      }
      break;
    case meshtastic_AdminMessage_set_canned_message_module_messages_tag: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_canned_message_module_messages_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      size_t length=0;
      if(!validCannedMessageList(admin_.set_canned_message_module_messages,
                                 sizeof(admin_.set_canned_message_module_messages),length)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      // Stock ignores empty SETs. The store also treats unchanged values as
      // no-ops, so neither path consumes journal writes unnecessarily.
      if(length)error=cannedStoreError(host_.saveCannedMessageMessages(
          admin_.set_canned_message_module_messages,length,now));
      break;
    }
    case meshtastic_AdminMessage_get_owner_request_tag:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;user(reply_.get_owner_response);break;
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_device_metadata_response_tag;metadata(reply_.get_device_metadata_response);break;
    case meshtastic_AdminMessage_get_device_connection_status_request_tag: {
      reply_.which_payload_variant=meshtastic_AdminMessage_get_device_connection_status_response_tag;
      auto &status=reply_.get_device_connection_status_response;
      connectionStatus(status);
      break;
    }
    case meshtastic_AdminMessage_get_config_request_tag:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_config_response_tag;
      if(!config(reply_.get_config_response,unsigned(admin_.get_config_request)+1))error=meshtastic_Routing_Error_BAD_REQUEST;
      break;
    case meshtastic_AdminMessage_get_channel_request_tag:
      if(admin_.get_channel_request<1||admin_.get_channel_request>8){error=meshtastic_Routing_Error_BAD_REQUEST;break;}
      reply_.which_payload_variant=meshtastic_AdminMessage_get_channel_response_tag;channel(reply_.get_channel_response,admin_.get_channel_request-1);break;
    case meshtastic_AdminMessage_get_module_config_request_tag:
      if(unsigned(admin_.get_module_config_request)>unsigned(meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG)){error=meshtastic_Routing_Error_BAD_REQUEST;break;}
      reply_.which_payload_variant=meshtastic_AdminMessage_get_module_config_response_tag;
      if(!moduleConfig(reply_.get_module_config_response,unsigned(admin_.get_module_config_request)+1))error=meshtastic_Routing_Error_NO_INTERFACE;
      break;
    case meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      if(p.decoded.want_response) {
        reply_.which_payload_variant=meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag;
        if(host_.hardwarePinInventory(reply_.get_node_remote_hardware_pins_response))
          error=meshtastic_Routing_Error_NO_INTERFACE;
      }
      break;
    }
    case meshtastic_AdminMessage_get_ui_config_request_tag: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_get_ui_config_request_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      meshtastic_DeviceUIConfig value{};
      detail::VolatileWipe wipeValue{&value,sizeof(value)};
      if(host_.uiConfig(value)||!ui::UiConfigSettings::valid(value)) {
        error=meshtastic_Routing_Error_NO_INTERFACE;break;
      }
      reply_.which_payload_variant=meshtastic_AdminMessage_get_ui_config_response_tag;
      reply_.get_ui_config_response=value;
      break;
    }
    case meshtastic_AdminMessage_store_ui_config_tag: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_store_ui_config_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)||
         !ui::UiConfigSettings::valid(admin_.store_ui_config)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const int result=host_.saveUiConfig(admin_.store_ui_config,now);
      error=result==-EINVAL?meshtastic_Routing_Error_BAD_REQUEST:
        result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_NONE;
      break;
    }
    case meshtastic_AdminMessage_key_verification_tag: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_key_verification_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const int result=host_.keyVerificationAdmin(admin_.key_verification,now);
      error=result==-EINVAL||result==-EACCES||result==-ESTALE||result==-ERANGE?
        meshtastic_Routing_Error_BAD_REQUEST:
        result==-EBUSY||result==-EAGAIN?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_NONE;
      break;
    }
    case meshtastic_AdminMessage_set_ham_mode_tag:error=applyHamMode(admin_.set_ham_mode,p.decoded.payload.bytes,p.decoded.payload.size);break;
    case meshtastic_AdminMessage_set_owner_tag:error=applyOwner(admin_.set_owner,p.decoded.payload.bytes,p.decoded.payload.size,true);break;
    case meshtastic_AdminMessage_set_channel_tag:error=applyChannel(admin_.set_channel,p.decoded.payload.bytes,p.decoded.payload.size,now);break;
    case meshtastic_AdminMessage_set_config_tag:
      error=applyConfig(admin_.set_config,now,p.decoded.payload.bytes,p.decoded.payload.size,
                        p.id,p.want_ack,p.decoded.want_response,bluetoothDeferred,true);
      if(bluetoothDeferred)return;
      if(error==meshtastic_Routing_Error_NONE&&admin_.set_config.which_payload_variant==meshtastic_Config_position_tag)
        localNode(!service_.config.fixedPosition());
      break;
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS)) || defined(CONFIG_MT_ONE_WIRE_TELEMETRY) || defined(CONFIG_MT_SHTXX_ADMIN)
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
    case meshtastic_AdminMessage_set_scale_tag: {
      if(owner_!=0||!p.id||!canonicalAdminOperation(admin_,admin_.which_payload_variant,p.decoded.payload.bytes,p.decoded.payload.size)){
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const int result=host_.scaleAdmin(admin_.set_scale,sensorSession_,p.id);
      if(!result)return;
      error=bluetoothConfigResult(result);break;
    }
#endif
    case meshtastic_AdminMessage_sensor_config_tag: {
      if(owner_!=0||!p.id||!canonicalAdminOperation(admin_,admin_.which_payload_variant,p.decoded.payload.bytes,p.decoded.payload.size)){
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
#if defined(CONFIG_MT_SHTXX_ADMIN)
      if(admin_.sensor_config.has_shtxx_config){
        error=bluetoothConfigResult(host_.shtAccuracy(admin_.sensor_config));break;
      }
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
      if(admin_.sensor_config.has_ds248x_config){
        unsigned selected=0;int result=stock::telemetry::oneWireSelection(admin_.sensor_config,selected);
        if(!result)result=host_.oneWireAdmin(selected);
        error=bluetoothConfigResult(result);break;
      }
#endif
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS))
      const int result=host_.sensorAdmin(admin_.sensor_config,sensorSession_,p.id);
      if(!result)return;
      error=bluetoothConfigResult(result);break;
#else
      error=meshtastic_Routing_Error_BAD_REQUEST;break;
#endif
    }
#endif
    case meshtastic_AdminMessage_set_fixed_position_tag:
      error=applyFixedPosition(admin_.set_fixed_position,p.decoded.payload.bytes,p.decoded.payload.size);
      if(error==meshtastic_Routing_Error_NONE)localNode();
      break;
    case meshtastic_AdminMessage_remove_fixed_position_tag:
      error=applyRemoveFixedPosition(admin_.remove_fixed_position,p.decoded.payload.bytes,p.decoded.payload.size);
      if(error==meshtastic_Routing_Error_NONE)localNode(true);
      break;
    case meshtastic_AdminMessage_send_input_event_tag:
      error=applyInputEvent(admin_.send_input_event,p.decoded.payload.bytes,p.decoded.payload.size,now);
      break;
    case meshtastic_AdminMessage_set_module_config_tag:
      if(host_.supportedModules()&moduleBit(admin_.set_module_config.which_payload_variant)){
        if(!canonicalModuleConfig(p.decoded.payload.bytes,p.decoded.payload.size)){error=meshtastic_Routing_Error_BAD_REQUEST;break;}
        const int result=host_.saveModuleConfig(admin_.set_module_config,now);
        if(result)error=result==-EINVAL||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:
          result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      }else if(!canonicalDisabledModuleNoop(admin_,p.decoded.payload.bytes,p.decoded.payload.size))error=meshtastic_Routing_Error_BAD_REQUEST;
      break;
    case meshtastic_AdminMessage_set_time_only_tag: {
      const int ret=host_.setWallTime(admin_.set_time_only);
      if(ret)error=ret==-EINVAL?meshtastic_Routing_Error_BAD_REQUEST:
        ret==-EAGAIN?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      break;
    }
    case meshtastic_AdminMessage_set_favorite_node_tag:
    case meshtastic_AdminMessage_remove_favorite_node_tag:
    case meshtastic_AdminMessage_set_ignored_node_tag:
    case meshtastic_AdminMessage_remove_ignored_node_tag:
    case meshtastic_AdminMessage_toggle_muted_node_tag:
    case meshtastic_AdminMessage_remove_by_nodenum_tag: {
      uint32_t id=0;NodeCache::Edit op=NodeCache::Edit::Remove;
      switch(admin_.which_payload_variant){
      case meshtastic_AdminMessage_set_favorite_node_tag:id=admin_.set_favorite_node;op=NodeCache::Edit::Favorite;break;
      case meshtastic_AdminMessage_remove_favorite_node_tag:id=admin_.remove_favorite_node;op=NodeCache::Edit::Unfavorite;break;
      case meshtastic_AdminMessage_set_ignored_node_tag:id=admin_.set_ignored_node;op=NodeCache::Edit::Ignore;break;
      case meshtastic_AdminMessage_remove_ignored_node_tag:id=admin_.remove_ignored_node;op=NodeCache::Edit::Unignore;break;
      case meshtastic_AdminMessage_toggle_muted_node_tag:id=admin_.toggle_muted_node;op=NodeCache::Edit::ToggleMute;break;
      default:id=admin_.remove_by_nodenum;break;
      }
      if(!p.id||!canonicalAdminOperation(admin_,admin_.which_payload_variant,
                                        p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      if(op==NodeCache::Edit::ToggleMute) {
        const AdminReplay *replay=nullptr;
        for(const auto &seen:muteReplay_)if(seen.request==p.id&&seen.from==0&&
            seen.operation==MuteToggleReplay){replay=&seen;break;}
        if(replay) {
          const int slot=service_.nodes.find(id);
          if(replay->target!=id||slot<0)error=meshtastic_Routing_Error_BAD_REQUEST;
          else {
            fresh(meshtastic_FromRadio_node_info_tag);cachedNode(output_.node_info,*service_.nodes.get(unsigned(slot)));emit();
            if(replay->degraded)notification(p.id,MT_NOTICE("NodeDB saved; radio RX restore failed; interface faulted"),true);
          }
          break;
        }
      }
      int ret;
#if defined(CONFIG_MT_PEER_DIRECTORY)
      if(op==NodeCache::Edit::Favorite&&service_.nodes.find(id)<0){
        if(favoriteRequest_){
          if(favoriteRequest_==p.id&&favoriteNode_==id)return;
          error=meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;break;
        }
        const uint32_t revision=host_.directoryRevision();
        ret=host_.promoteDirectoryFavorite(owner_,id,now);
        if(ret==-EAGAIN){favoriteRevision_=revision;favoriteNode_=id;favoriteAt_=now;favoriteRequest_=p.id;return;}
        if(ret==-ENOENT)ret=host_.editNode(id,op,now);
      }else
#endif
      ret=host_.editNode(id,op,now);
      nodeCommittedDegraded=ret==Host::EditCommittedDegraded;
      if(ret&&!nodeCommittedDegraded)error=ret==-EINVAL||ret==-ENOENT||ret==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:
        ret==-EBUSY||ret==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      else {const int slot=service_.nodes.find(id);if(slot>=0){fresh(meshtastic_FromRadio_node_info_tag);cachedNode(output_.node_info,*service_.nodes.get(unsigned(slot)));emit();}
        if(op==NodeCache::Edit::ToggleMute)muteReplay_[nextMuteReplay_++%8]={p.id,id,0,
          MuteToggleReplay,nodeCommittedDegraded};
        if(nodeCommittedDegraded)notification(p.id,MT_NOTICE("NodeDB saved; radio RX restore failed; interface faulted"),true);}
      break;
    }
    case meshtastic_AdminMessage_add_contact_tag: {
      if(!p.id||!canonicalAdminOperation(admin_,meshtastic_AdminMessage_add_contact_tag,
                                          p.decoded.payload.bytes,p.decoded.payload.size)) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      if(!host_.ready()) {error=meshtastic_Routing_Error_NO_INTERFACE;break;}
      if(host_.editing()) {error=meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;break;}
      OwnerContactCandidate candidate{};detail::VolatileWipe wipeCandidate{&candidate,sizeof(candidate)};
      const int decoded=OwnerContact::decode(admin_.add_contact,service_.route.identity(),candidate);
      if(decoded) {error=meshtastic_Routing_Error_BAD_REQUEST;break;}
      const int ret=host_.addOwnerContact(candidate,now);
      if(ret==Host::ContactPartiallyCommitted) {
        contactPartiallyCommitted=true;
        error=meshtastic_Routing_Error_NO_INTERFACE;break;
      }
      nodeCommittedDegraded=ret==Host::EditCommittedDegraded;
      if(ret&&!nodeCommittedDegraded) {
        error=ret==-EINVAL||ret==-ENOENT||ret==-ENOTSUP||ret==-EEXIST?meshtastic_Routing_Error_BAD_REQUEST:
          ret==-EBUSY||ret==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
          meshtastic_Routing_Error_NO_INTERFACE;
      } else {
        const int slot=service_.nodes.find(candidate.node.id());
        if(slot>=0) {
          fresh(meshtastic_FromRadio_node_info_tag);
          cachedNode(output_.node_info,*service_.nodes.get(unsigned(slot)));emit();
        }
        if(nodeCommittedDegraded)
          notification(p.id,MT_NOTICE("Contact saved; radio RX restore failed; interface faulted"),true);
      }
      break;
    }
    case meshtastic_AdminMessage_begin_edit_settings_tag:
      if(host_.beginEdit(now))error=meshtastic_Routing_Error_BAD_REQUEST;
      break;
    case meshtastic_AdminMessage_commit_edit_settings_tag:
      if(host_.commitEdit(now))error=meshtastic_Routing_Error_BAD_REQUEST;
      break;
    case meshtastic_AdminMessage_reboot_seconds_tag:
      if((host_.editing()&&admin_.reboot_seconds>=0)||!host_.scheduleReboot(admin_.reboot_seconds))error=meshtastic_Routing_Error_BAD_REQUEST;
      break;
    case meshtastic_AdminMessage_ota_request_tag: {
      const auto &request=admin_.ota_request;
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_ota_request_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)||
         (request.reboot_ota_mode!=meshtastic_OTAMode_OTA_BLE&&
          request.reboot_ota_mode!=meshtastic_OTAMode_OTA_WIFI)||request.ota_hash.size!=32||host_.editing()) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const int result=host_.scheduleFirmwareUpdate(request,now);
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
      if(!result)notification(p.id,request.reboot_ota_mode==meshtastic_OTAMode_OTA_WIFI?
          "Rebooting to WiFi OTA":"Rebooting to BLE OTA",false);
#endif
      if(result)error=result==-EBUSY||result==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EINVAL||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE;
      break;
    }
    case meshtastic_AdminMessage_enter_dfu_mode_request_tag: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_enter_dfu_mode_request_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)||
         !admin_.enter_dfu_mode_request||host_.editing()) {
        error=meshtastic_Routing_Error_BAD_REQUEST;break;
      }
      const int result=host_.enterDfu(now);
      if(result)error=result==-EBUSY||result==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EINVAL||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE;
      break;
    }
    case meshtastic_AdminMessage_shutdown_seconds_tag:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_shutdown_seconds_tag,
                                  p.decoded.payload.bytes,p.decoded.payload.size)||
         (host_.editing()&&admin_.shutdown_seconds>=0)||!host_.scheduleShutdown(admin_.shutdown_seconds))
        error=meshtastic_Routing_Error_BAD_REQUEST;
      break;
    default:error=meshtastic_Routing_Error_BAD_REQUEST;break;
    }
    if(!host_.ready()&&!nodeCommittedDegraded)error=meshtastic_Routing_Error_NO_INTERFACE;
    if(error!=meshtastic_Routing_Error_NONE) {
      const char *reason=MT_NOTICE("Unsupported local admin operation; no requested change was applied");
      if(contactPartiallyCommitted)reason=MT_NOTICE("Contact key saved; contact update incomplete");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_commit_edit_settings_tag)
        reason=MT_NOTICE("Commit failed; live settings pending; durability unconfirmed");
      else if(error==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED&&
              admin_.which_payload_variant==meshtastic_AdminMessage_set_config_tag)
        reason=MT_NOTICE("Settings busy; refresh config before retrying");
      else if(error==meshtastic_Routing_Error_NO_INTERFACE&&
              admin_.which_payload_variant!=meshtastic_AdminMessage_send_input_event_tag)
        reason=MT_NOTICE("Local config/radio unavailable");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_time_only_tag)
        reason=error==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED?
          MT_NOTICE("Clock unchanged: wait 30 minutes after correction; matching UTC seconds are a no-op"):
          MT_NOTICE("Clock rejected: need nonzero UTC seconds and monotonic uptime; prior clock unchanged");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_begin_edit_settings_tag)
        reason=MT_NOTICE("Edit could not start; drain TX and check storage before retrying");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_reboot_seconds_tag&&host_.editing())
        reason=MT_NOTICE("Commit pending settings before reboot");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_channel_tag)
        reason=MT_NOTICE("Channel setup rejected: unsupported or busy; Public mute edits allowed. Drain queue and retry.");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_config_tag) {
        reason=host_.editing()&&admin_.set_config.which_payload_variant==meshtastic_Config_lora_tag&&admin_.set_config.payload_variant.lora.tx_enabled?
          MT_NOTICE("Keep TX disarmed while editing; commit, then explicitly re-enable LoRa TX"):
          admin_.set_config.which_payload_variant==meshtastic_Config_position_tag?
          MT_NOTICE("Position config rejected: only GPS-absent and current fixed-position state supported; use set/remove fixed position for coordinates"):
          admin_.set_config.which_payload_variant==meshtastic_Config_display_tag?
          MT_NOTICE("Display config rejected: timeout (0/default, 1-86400 or always-on), flip, carousel (0-86400) only"):
          admin_.set_config.which_payload_variant==meshtastic_Config_device_tag?
          MT_NOTICE("Device config rejected: supported role/rebroadcast/heartbeat/NodeInfo only; serial on, others default"):
          admin_.set_config.which_payload_variant==meshtastic_Config_bluetooth_tag?
          MT_NOTICE("Bluetooth config rejected: supported pairing mode and six-digit PIN required; random PIN needs display; finish settings edit"):
          MT_NOTICE("Config rejected: unsupported Device/Display/Position/US LoRa fields; hop limit 0-7");
      }
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_owner_tag)
        reason=MT_NOTICE("Owner update rejected: only local long/short names may change");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_add_contact_tag)
        reason=MT_NOTICE("Contact import refused: require canonical local request, valid non-self contact, idle settings, and durable host support");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_fixed_position_tag)
        reason=MT_NOTICE("Fixed position rejected: require bounded canonical E7 coordinates; optional altitude only, no time/fix/quality");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_remove_fixed_position_tag)
        reason=MT_NOTICE("Fixed-position removal rejected: require one canonical true remove operation");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_send_input_event_tag)
        reason=error==meshtastic_Routing_Error_NO_INTERFACE?
          MT_NOTICE("Display input unavailable"):
          MT_NOTICE("Display input rejected: one canonical local SELECT/UP/DOWN/LEFT/RIGHT/BACK/USER_PRESS; no keyboard or touch");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_module_config_tag)
        reason=MT_NOTICE("Module config rejected: only canonical disabled-default echo for one excluded module; it remains unavailable");
      else if(admin_.which_payload_variant==meshtastic_AdminMessage_set_favorite_node_tag||admin_.which_payload_variant==meshtastic_AdminMessage_remove_favorite_node_tag||
              admin_.which_payload_variant==meshtastic_AdminMessage_set_ignored_node_tag||admin_.which_payload_variant==meshtastic_AdminMessage_remove_ignored_node_tag||
              admin_.which_payload_variant==meshtastic_AdminMessage_toggle_muted_node_tag||
              admin_.which_payload_variant==meshtastic_AdminMessage_remove_by_nodenum_tag)
        reason=admin_.which_payload_variant==meshtastic_AdminMessage_toggle_muted_node_tag?
          MT_NOTICE("Mute toggle unconfirmed: need canonical nonzero request, existing remote node and storage; TX off, queues/settings idle"):
          MT_NOTICE("NodeDB edit unconfirmed: need valid remote node and storage; TX off, queues/settings idle, max 14 protected peers");
      reject(p.id,error,reason);
    } else if(reply_.which_payload_variant) {
      const bool sensitiveUiResponse=reply_.which_payload_variant==
        meshtastic_AdminMessage_get_ui_config_response_tag;
      adminReply(p.id);
      if(sensitiveUiResponse) {
        volatile uint8_t *outputBytes=reinterpret_cast<volatile uint8_t *>(&output_);
        for(size_t i=0;i<sizeof(output_);++i)outputBytes[i]=0;
      }
    } else ack(p.id,error);
  }
  void notification(uint32_t id,const char *message,bool error) {
    fresh(meshtastic_FromRadio_clientNotification_tag);auto &n=output_.clientNotification;
    n.has_reply_id=true;n.reply_id=id;n.level=error?meshtastic_LogRecord_Level_ERROR:meshtastic_LogRecord_Level_INFO;
    companion::notice::copy(n.message,sizeof(n.message),message);emit();
  }
  void reject(uint32_t id,meshtastic_Routing_Error code,const char *reason) {
    notification(id,reason,true);ack(id,code);
  }
  void sendEndpoint(const meshtastic_MeshPacket &p,uint32_t now) {
    if(service_.normalMode()) {sendMessage(p,now);return;}
    if(p.to!=0xffffffffu||p.pki_encrypted||p.decoded.portnum!=meshtastic_PortNum_TEXT_MESSAGE_APP||!p.id) {
      ack(p.id,p.pki_encrypted?meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST);return;
    }
    if(!service_.endpointArmed()) {
      notification(p.id,service_.endpointCount()?MT_NOTICE("Bench RF quota used; no more transmissions this boot"):MT_NOTICE("RF TX is disarmed; enable LoRa TX explicitly for the approved single message"),true);
      ack(p.id,meshtastic_Routing_Error_NO_INTERFACE);++deniedTx;return;
    }
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.payload=p.decoded.payload;data.has_bitfield=true;
    uint8_t encoded[239],frame[255];auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));size_t frameLength=0;
    if(!pb_encode(&stream,meshtastic_Data_fields,&data)||!service_.codec.encodeZeroHopData(service_.route.identity(),p.to,p.id,encoded,stream.bytes_written,frame,sizeof(frame),frameLength)) {
      ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;
    }
    const int ret=service_.submitEndpoint(owner_,p.id,frame,frameLength,now);
    if(ret){ack(p.id,meshtastic_Routing_Error_NO_INTERFACE);++deniedTx;}
  }
  unsigned originHop(const meshtastic_MeshPacket &p)const {
    // Pinned Router::sendLocal applies the user's reliable-hop-zero fallback
    // before selecting an application port or Public/private transport.
    return p.want_ack&&!p.hop_limit?service_.config.hopLimit():p.hop_limit;
  }
  enum class PeerTransport:uint8_t {Channel,Private,MissingKey,Reject};
  static meshtastic_Routing_Error peerTransportRefusal(PeerTransport transport) {
    return transport==PeerTransport::MissingKey?
      meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
      meshtastic_Routing_Error_PKI_FAILED;
  }
  PeerTransport peerTransport(const meshtastic_MeshPacket &p,uint32_t port,
                              bool applicationEligible=true)const {
    // Stock makes PKI a caller transport choice only through pki_encrypted.
    // A bounded public_key field without that bit is only a hint and cannot
    // change ordinary pin lookup or channel-selection policy.
    const bool explicitKey=p.pki_encrypted&&p.public_key.size!=0;
    const bool explicitPki=p.pki_encrypted;
    if(p.to==0xffffffffu)return explicitPki?PeerTransport::Reject:PeerTransport::Channel;
    if(service_.config.licensedOperation())return explicitPki?PeerTransport::Reject:PeerTransport::Private;
    const bool stockExcluded=port==meshtastic_PortNum_POSITION_APP||
      port==meshtastic_PortNum_NODEINFO_APP||port==meshtastic_PortNum_ROUTING_APP||
      port==meshtastic_PortNum_TRACEROUTE_APP;
    if(explicitPki) {
      if(stockExcluded)return PeerTransport::Reject;
      // Stock resolves the destination first. An absent key is the distinct
      // send-fail-public-key result even when a malformed hint accompanied an
      // explicit PKI request; hints are validated only for a resolved peer.
      const auto destination=service_.privateDestinationDisposition(p.to);
      if(destination==pki::DestinationDisposition::UnknownNoKey)
        return service_.privateConfigured()?PeerTransport::MissingKey:PeerTransport::Reject;
      if(destination!=pki::DestinationDisposition::PinnedActive||
         (p.public_key.size&&p.public_key.size!=32))return PeerTransport::Reject;
      const auto hinted=service_.privateDestinationDisposition(p.to,
        explicitKey?p.public_key.bytes:nullptr,explicitKey?p.public_key.size:0);
      return hinted==pki::DestinationDisposition::PinnedActive?
        PeerTransport::Private:PeerTransport::Reject;
    }
    if(p.to==service_.route.identity()||!applicationEligible||stockExcluded||
       service_.stockChannelExcludesAutoPki(p.channel))
      return PeerTransport::Channel;
    // A remote NodeInfo/cache key is not a private-channel configuration.
    // The stock selector needs our local private identity before automatic
    // PKI can apply; without it these otherwise eligible requests stay on the
    // configured shared channel regardless of cached destination metadata.
    if(!service_.privateConfigured())return PeerTransport::Channel;
    switch(service_.privateDestinationDisposition(p.to)) {
    case pki::DestinationDisposition::PinnedActive:return PeerTransport::Private;
    case pki::DestinationDisposition::UnknownNoKey:
      // Stock sends the initial key-verification exchange on the shared
      // channel when no destination key exists. Other eligible unicast
      // applications with a configured local private authority fail with the
      // stock missing-public-key result instead of silently downgrading.
      if(port==meshtastic_PortNum_KEY_VERIFICATION_APP)return PeerTransport::Channel;
      return service_.privateConfigured()?PeerTransport::MissingKey:PeerTransport::Channel;
    case pki::DestinationDisposition::KnownUnpinned:
    case pki::DestinationDisposition::Revoked:
    case pki::DestinationDisposition::KeyMismatch:
    case pki::DestinationDisposition::Unavailable:return PeerTransport::Reject;
    }
    return PeerTransport::Reject;
  }
  void sendNodeInfo(const meshtastic_MeshPacket &p,uint32_t now) {
    if(p.pki_encrypted) {
      reject(p.id,meshtastic_Routing_Error_PKI_FAILED,MT_NOTICE("Private messaging currently accepts text to an explicitly pinned peer"));return;
    }
    if(!p.id||p.to<=1||p.to==0xfffffffeu||p.to==service_.route.identity()||p.hop_limit>HopPolicy::Maximum||
      p.decoded.payload.size||p.decoded.request_id||p.decoded.reply_id||p.decoded.emoji||
      p.decoded.xeddsa_signature.size) {
      reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("NodeInfo needs empty payload, remote target and hop 0-7; owner data is local"));return;
    }
    const unsigned hop=originHop(p);
    const bool wantResponse=p.decoded.want_response||(p.decoded.has_bitfield&&(p.decoded.bitfield&2));
    const int ret=service_.submitNodeInfo(owner_,p.id,p.to,hop,p.want_ack,wantResponse,now);
    queue(p.id);
    if(ret) {
      const auto code=(ret==-EAGAIN||ret==-ENOBUFS)?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        (ret==-EINVAL||ret==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE;
      reject(p.id,code,code==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED?
        MT_NOTICE("NodeInfo is pending or rate limited; at most one new discovery operation per minute"):
        code==meshtastic_Routing_Error_BAD_REQUEST?MT_NOTICE("NodeInfo request ID or destination is invalid"):
        MT_NOTICE("NodeInfo requires an available radio and explicit LoRa TX arm"));++deniedTx;
    }
  }
  void sendTraceRoute(const meshtastic_MeshPacket &p,uint32_t now) {
    if(!canonicalInput()||!p.id||p.from||p.to<=1||p.to>=0xfffffffeu||p.to==service_.route.identity()||
      p.hop_limit>HopPolicy::Maximum||p.hop_start||p.want_ack||p.pki_encrypted||
      detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT||
      p.decoded.portnum!=meshtastic_PortNum_TRACEROUTE_APP||!p.decoded.want_response||
      p.decoded.dest||p.decoded.source||p.decoded.request_id||p.decoded.reply_id||
      p.decoded.emoji||p.decoded.xeddsa_signature.size||
      p.decoded.has_bitfield) {
      reject(p.id,p.pki_encrypted||p.decoded.xeddsa_signature.size?
        meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Traceroute needs canonical Public unicast, hop 0-7, no ACK/private claims or caller response metadata"));return;
    }
    if(!service_.endpointArmed()) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,
        service_.txAllowed()?MT_NOTICE("Traceroute requires explicit LoRa TX arm"):MT_NOTICE("Traceroute is unavailable in this RX-only build"));++deniedTx;return;
    }
    uint8_t data[239]{},frame[255]{};size_t dataSize=0,frameSize=0;
    if(!TraceRoute::requestData(p.decoded.payload.bytes,p.decoded.payload.size,data,sizeof(data),dataSize)||
       !service_.encodeTraceRouteQuery(p.to,p.id,data,dataSize,frame,sizeof(frame),frameSize)) {
      reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Traceroute payload is malformed, noncanonical, oversized or exceeds the bounded route/SNR lists"));return;
    }
    frame[12]=uint8_t(p.hop_limit|(p.hop_limit<<5));
    const int ret=service_.submitTraceRoute(owner_,frame,frameSize,now);queue(p.id);
    if(ret) {
      const auto code=(ret==-EINVAL||ret==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:
        ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      reject(p.id,code,code==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED?
        MT_NOTICE("Traceroute queue is full"):MT_NOTICE("Traceroute requires a ready armed radio and a fresh request ID"));++deniedTx;
    }
  }
  void sendPositionQuery(const meshtastic_MeshPacket &p,uint32_t now) {
    const auto transport=peerTransport(p,meshtastic_PortNum_POSITION_APP);
    const bool privateRequest=transport==PeerTransport::Private;
    if(transport==PeerTransport::Reject||transport==PeerTransport::MissingKey) {
      reject(p.id,peerTransportRefusal(transport),MT_NOTICE("Position query is stock-excluded from PKI or the destination is not an active pinned peer"));return;
    }
    if(!canonicalInput()||!p.id||p.from||p.to<=1||p.to>=0xfffffffeu||p.to==service_.route.identity()||
      p.hop_limit>HopPolicy::Maximum||p.hop_start||(!privateRequest&&p.want_ack)||
      detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT||
      !p.decoded.want_response||p.decoded.payload.size||p.decoded.dest||p.decoded.source||
      p.decoded.request_id||p.decoded.reply_id||p.decoded.emoji||p.decoded.xeddsa_signature.size||p.decoded.has_bitfield) {
      reject(p.id,p.pki_encrypted||p.decoded.xeddsa_signature.size?
        meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Position query needs canonical empty unicast want_response, hop 0-7; RF ACK needs pinned private peer"));return;
    }
    if(!service_.endpointArmed()) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Position query requires a ready armed radio and a TX-enabled build"));++deniedTx;return;
    }
    if(privateRequest) {
      constexpr uint8_t data[]={8,3,24,1,72,2};
      const int ret=service_.submitPrivate(owner_,p.to,p.id,data,sizeof(data),originHop(p),p.want_ack,
        p.pki_encrypted&&p.public_key.size?p.public_key.bytes:nullptr,
        p.pki_encrypted?p.public_key.size:0,p.pki_encrypted,now);queue(p.id);
      if(ret){reject(p.id,ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        ret==-EACCES||ret==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_PKI_FAILED,
        MT_NOTICE("Private Position needs identity, pinned key, nonce storage and radio"));++deniedTx;}
      return;
    }
    const int ret=service_.submitChannelPositionQuery(owner_,p.to,p.id,p.hop_limit,now);queue(p.id);
    if(ret){reject(p.id,(ret==-EINVAL||ret==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:
      ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE,
      MT_NOTICE("Position query rejected: require a fresh ID, available queue and permitted peer/radio"));++deniedTx;}
  }
  void sendNeighborQuery(const meshtastic_MeshPacket &p,uint32_t now) {
    const auto transport=peerTransport(p,meshtastic_PortNum_NEIGHBORINFO_APP);
    const bool privateRequest=transport==PeerTransport::Private;
    if(transport==PeerTransport::Reject||transport==PeerTransport::MissingKey) {
      reject(p.id,peerTransportRefusal(transport),MT_NOTICE("Neighbor query destination is not an active pinned peer or its private runtime is unavailable"));return;
    }
    if(!canonicalInput()||!p.id||p.from||p.to<=1||p.to>=0xfffffffeu||p.to==service_.route.identity()||
      p.hop_limit>HopPolicy::Maximum||p.hop_start||p.xeddsa_signed||(!privateRequest&&p.want_ack)||
      detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT||
      !p.decoded.want_response||p.decoded.payload.size||p.decoded.dest||p.decoded.source||
      p.decoded.request_id||p.decoded.reply_id||p.decoded.emoji||p.decoded.xeddsa_signature.size||p.decoded.has_bitfield) {
      reject(p.id,p.pki_encrypted||p.xeddsa_signed||p.decoded.xeddsa_signature.size?
        meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Neighbor query needs canonical empty unicast want_response, hop 0-7; RF ACK needs pinned private peer"));return;
    }
    if(!service_.endpointArmed()) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Neighbor query requires a ready armed radio and a TX-enabled build"));++deniedTx;return;
    }
    if(privateRequest) {
      constexpr uint8_t data[]={8,71,24,1,72,2};
      const int ret=service_.submitPrivate(owner_,p.to,p.id,data,sizeof(data),originHop(p),p.want_ack,
        p.pki_encrypted&&p.public_key.size?p.public_key.bytes:nullptr,
        p.pki_encrypted?p.public_key.size:0,p.pki_encrypted,now);queue(p.id);
      if(ret){reject(p.id,ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        ret==-EACCES||ret==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_PKI_FAILED,
        MT_NOTICE("Private Neighbor query requires the active identity, exact pinned peer and available durable nonce/radio state"));++deniedTx;}
      return;
    }
    uint8_t frame[22+bench::ChannelAead::TagSize]{};size_t size=0;
    if(!service_.encodeNeighborQuery(p.to,p.id,frame,sizeof(frame),size)) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Neighbor query encoding failed"));return;
    }
    frame[12]=uint8_t(p.hop_limit|(p.hop_limit<<5));
    const int ret=service_.submitNeighborQuery(owner_,frame,size,now);queue(p.id);
    if(ret){reject(p.id,(ret==-EINVAL||ret==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:
      ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE,
      MT_NOTICE("Neighbor query rejected: require a fresh ID, available queue and permitted peer/radio"));++deniedTx;}
  }
  void localRead(const meshtastic_MeshPacket &p) {
    if(!canonicalInput()||!p.id||!p.decoded.want_response||p.decoded.payload.size||
      p.decoded.dest||p.decoded.source||p.decoded.request_id||p.decoded.reply_id||
      p.decoded.emoji||p.decoded.has_bitfield||p.hop_start) {
      reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Local NodeInfo/position read needs empty want_response; publish unsupported"));return;
    }
    const bool isPosition=p.decoded.portnum==meshtastic_PortNum_POSITION_APP;
    if(isPosition&&!service_.config.fixedPosition()) {
      reject(p.id,meshtastic_Routing_Error_NO_RESPONSE,MT_NOTICE("No local fixed position is configured"));return;
    }
    responsePacket(p.id,p.decoded.portnum);auto &data=output_.packet.decoded;
    auto stream=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));bool encoded;
    if(isPosition) {
      // Same local readback as the configuration handshake, not a Public RF
      // response. Sharing consent and RF precision remain in PositionQuery.
      meshtastic_Position value{};position(value);
      encoded=pb_encode(&stream,meshtastic_Position_fields,&value);
    } else {
      meshtastic_User value{};user(value);
      encoded=pb_encode(&stream,meshtastic_User_fields,&value);
    }
    if(encoded){data.payload.size=stream.bytes_written;emit();}
    else ack(p.id,meshtastic_Routing_Error_TOO_LARGE);
  }
  void localNeighbors(const meshtastic_MeshPacket &p){
    if(!canonicalInput()||!p.id||p.from||!p.decoded.want_response||p.decoded.payload.size||
       p.decoded.dest||p.decoded.source||p.decoded.request_id||p.decoded.reply_id||
       p.decoded.emoji||p.decoded.has_bitfield||p.hop_start){
      reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Local snapshot requires an empty want_response request"));return;
    }
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    if(p.decoded.portnum==meshtastic_PortNum_PAXCOUNTER_APP){
      meshtastic_Paxcount count{};
      if(!service_.localPaxSnapshot(count)){ack(p.id,meshtastic_Routing_Error_NO_RESPONSE);return;}
      responsePacket(p.id,p.decoded.portnum);auto &data=output_.packet.decoded;
      auto out=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
      if(!pb_encode(&out,meshtastic_Paxcount_fields,&count)){ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;}
      data.payload.size=out.bytes_written;emit();return;
    }
#endif
    meshtastic_NeighborInfo value{};const int result=host_.localNeighbors(value);
    if(result){reject(p.id,result==-ENOTSUP?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_NO_RESPONSE,MT_NOTICE("Local neighbor observations are unavailable or disabled"));return;}
    responsePacket(p.id,meshtastic_PortNum_NEIGHBORINFO_APP);
    auto &data=output_.packet.decoded;auto out=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(pb_encode(&out,meshtastic_NeighborInfo_fields,&value)){data.payload.size=out.bytes_written;emit();}
    else ack(p.id,meshtastic_Routing_Error_TOO_LARGE);
  }
  void localTelemetry(const meshtastic_MeshPacket &p) {
    if(!canonicalInput()||!p.id||!p.decoded.want_response||
      !(TelemetryQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)||
        EnvironmentQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)||
        PowerQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)||
        LocalStatsQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)||
        bench::MtCodec::supplementalTelemetryVariant(p.decoded.payload.bytes,p.decoded.payload.size))||
      p.decoded.dest||p.decoded.source||p.decoded.request_id||p.decoded.reply_id||
      p.decoded.emoji||p.decoded.has_bitfield||p.hop_start) {
      reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Local telemetry requires a canonical supported metrics selector"));return;
    }
    uint32_t seconds=0;
    if(!EnvironmentQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)&&
       !PowerQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)&&!host_.uptimeSeconds(seconds)) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Local monotonic uptime is unavailable"));return;
    }
    uint8_t encoded[128]{};size_t size=0;
    if(TelemetryQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)) {
      if(!TelemetryQuery::uptimeResponse(seconds,p.id,service_.airUtilTx(),service_.channelUtilization(),encoded,sizeof(encoded),size,service_.hardware(),host_.wallTime())) {
        ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;
      }
    } else if(const auto extra=bench::MtCodec::supplementalTelemetryVariant(p.decoded.payload.bytes,p.decoded.payload.size)) {
      if(!host_.extraTelemetry(extra,p.id,encoded,sizeof(encoded),size)){reject(p.id,meshtastic_Routing_Error_NO_RESPONSE,MT_NOTICE("Local sensor readings are unavailable or disabled"));return;}
    } else if(PowerQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)) {
      zt::telemetry::PowerReadings value{};const int result=host_.localPower(value);
      if(result){reject(p.id,result==-ENOTSUP?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_NO_RESPONSE,MT_NOTICE("Local power readings are unavailable or disabled"));return;}
      if(!PowerQuery::response(value,p.id,encoded,sizeof(encoded),size,host_.wallTime())){ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;}
    } else if(EnvironmentQuery::request(p.decoded.payload.bytes,p.decoded.payload.size)) {
      meshtastic_EnvironmentMetrics value{};const int result=host_.localEnvironment(value);
      if(result){reject(p.id,result==-ENOTSUP?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_NO_RESPONSE,MT_NOTICE("Local environment readings are unavailable or disabled"));return;}
      if(!EnvironmentQuery::responseMetrics(value,p.id,encoded,sizeof(encoded),size,host_.wallTime())){ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;}
    } else {
      // Exact LocalStats selector (32 00): same local-only, zero-RF reply path.
      const auto observed=service_.localStats();
      if(!LocalStatsQuery::uptimeResponse(seconds,p.id,service_.airUtilTx(),service_.channelUtilization(),encoded,sizeof(encoded),size,&observed,host_.wallTime())) {
        ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;
      }
    }
    responsePacket(p.id,meshtastic_PortNum_TELEMETRY_APP);
    auto stream=pb_istream_from_buffer(encoded,size);
    if(!pb_decode(&stream,meshtastic_Data_fields,&output_.packet.decoded)) {
      ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;
    }
    emit();
  }
  void sendTelemetryQuery(const meshtastic_MeshPacket &p,uint32_t now) {
    const auto transport=peerTransport(p,meshtastic_PortNum_TELEMETRY_APP);
    const bool privateRequest=transport==PeerTransport::Private;
    if(transport==PeerTransport::Reject||transport==PeerTransport::MissingKey) {
      reject(p.id,peerTransportRefusal(transport),MT_NOTICE("Telemetry query destination is not an active pinned peer or its private runtime is unavailable"));return;
    }
    const uint8_t variant=RemoteTelemetryQuery::requestVariant(p.decoded.payload.bytes,p.decoded.payload.size);
    const bool localStats=variant==6;
    if(!canonicalInput()||!p.id||p.from||p.to<=1||p.to>=0xfffffffeu||p.to==service_.route.identity()||
      p.hop_limit>HopPolicy::Maximum||p.hop_start||(!privateRequest&&p.want_ack)||
      detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT||
      !p.decoded.want_response||!variant||p.decoded.dest||p.decoded.source||
      p.decoded.request_id||p.decoded.reply_id||p.decoded.emoji||p.decoded.xeddsa_signature.size||p.decoded.has_bitfield) {
      reject(p.id,p.pki_encrypted||p.decoded.xeddsa_signature.size?
        meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Telemetry query needs canonical unicast selector, want_response, hop 0-7; RF ACK needs pinned private peer"));return;
    }
    if(!service_.endpointArmed()) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Telemetry query requires a ready armed radio and a TX-enabled build"));++deniedTx;return;
    }
    if(privateRequest) {
      uint8_t data[]={8,67,18,2,18,0,24,1,72,2};
      data[4]=uint8_t((variant<<3)|2);
      const int ret=service_.submitPrivate(owner_,p.to,p.id,data,sizeof(data),originHop(p),p.want_ack,
        p.pki_encrypted&&p.public_key.size?p.public_key.bytes:nullptr,
        p.pki_encrypted?p.public_key.size:0,p.pki_encrypted,now);queue(p.id);
      if(ret){reject(p.id,ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        ret==-EACCES||ret==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_PKI_FAILED,
        MT_NOTICE("Private Telemetry needs identity, pinned key, nonce storage and radio"));++deniedTx;}
      return;
    }
    uint8_t frame[26+bench::ChannelAead::TagSize]{};size_t size=0;
    if(!service_.encodeTelemetryQuery(p.to,p.id,frame,sizeof(frame),size,localStats,variant)) {
      reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Telemetry query encoding failed"));return;
    }
    frame[12]=uint8_t(p.hop_limit|(p.hop_limit<<5));
    const int ret=service_.submitTelemetryQuery(owner_,frame,size,now,localStats,variant);queue(p.id);
    if(ret){reject(p.id,(ret==-EINVAL||ret==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:
      ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE,
      MT_NOTICE("Telemetry query rejected: require a fresh ID, available queue and permitted peer/radio"));++deniedTx;}
  }
  void sendGpioRequest(const meshtastic_MeshPacket &p,uint32_t now) {
#if defined(CONFIG_MT_GPIO_MODULES)
    if(p.pki_encrypted||p.xeddsa_signed||p.decoded.xeddsa_signature.size||
       detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT) {
      reject(p.id,meshtastic_Routing_Error_PKI_FAILED,
        MT_NOTICE("RemoteHardware commands cannot request PKI, attach a peer key, signature or caller trust proof on the stock gpio channel"));return;
    }
    if(p.to==service_.route.identity()||!PublicApplication::gpioClient(p)||!canonicalInput()) {
      reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("RemoteHardware commands require canonical READ/WRITE/WATCH data, a remote unicast target and a fresh packet identity"));return;
    }
    uint8_t data[239]{};size_t size=0;
    if(!PublicApplication::encodeGpioRequest(p,data,sizeof(data),size)) {
      bench::wipeKeyBuffer(data,sizeof(data));reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("RemoteHardware command encoding failed canonical validation"));return;
    }
    const int result=service_.submitGpioRequest(owner_,p.to,p.id,data,size,originHop(p),p.want_ack,now);
    bench::wipeKeyBuffer(data,sizeof(data));queue(p.id);
    if(result) {
      const auto code=(result==-EINVAL||result==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:
        result==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EPERM?meshtastic_Routing_Error_NO_CHANNEL:meshtastic_Routing_Error_NO_INTERFACE;
      reject(p.id,code,MT_NOTICE("RemoteHardware request needs an available named gpio channel, fresh ID lease and armed radio queue"));++deniedTx;
    }
#else
    (void)now;reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("RemoteHardware module is disabled"));
#endif
  }
  void sendApplication(const meshtastic_MeshPacket &p,uint32_t now) {
#if defined(CONFIG_MT_GPIO_MODULES)
    if(detail::enumNumber(p.decoded.portnum)==meshtastic_PortNum_REMOTE_HARDWARE_APP&&
       modules::GpioModuleChannels::gpio(service_.config,p.channel)) {
      sendGpioRequest(p,now);return;
    }
#endif
    const bool local=p.to==service_.route.identity();
    const auto transport=local?PeerTransport::Channel:
      peerTransport(p,detail::enumNumber(p.decoded.portnum));
    if(transport==PeerTransport::Reject||transport==PeerTransport::MissingKey) {
      reject(p.id,peerTransportRefusal(transport),transport==PeerTransport::MissingKey?
        MT_NOTICE("Recipient public key is missing; exchange NodeInfo before sending"):
        MT_NOTICE("Private application needs an active identity and matching recipient key"));return;
    }
    const bool privateRequest=transport==PeerTransport::Private;
    if(!PublicApplication::client(p,privateRequest)||!canonicalInput()||(privateRequest&&local)) {
      reject(p.id,p.pki_encrypted||p.decoded.xeddsa_signature.size||p.xeddsa_signed||
        detail::enumNumber(p.ack_proof_status)?meshtastic_Routing_Error_PKI_FAILED:meshtastic_Routing_Error_BAD_REQUEST,
        MT_NOTICE("Application transport needs canonical data, fresh ID, permitted destination, no caller signature/routing/response claims"));return;
    }
    uint8_t data[239]{};size_t size=0;
    if(!PublicApplication::encode(detail::enumNumber(p.decoded.portnum),p.decoded.payload.bytes,p.decoded.payload.size,
       p.decoded.reply_id,p.decoded.emoji,data,sizeof(data),size,p.decoded.has_bitfield?uint8_t(p.decoded.bitfield):0,!local)) {
      bench::wipeKeyBuffer(data,sizeof(data));
      ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;
    }
    if(privateRequest&&size>227){bench::wipeKeyBuffer(data,sizeof(data));ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;}
    if(p.to==service_.route.identity()) {
      // Local delivery reserves no RF ID. Accepted waypoint data also reaches
      // the same per-device local history as received waypoints. It reaches the requesting USB/BLE session like local text.
      fresh(meshtastic_FromRadio_packet_tag);auto &out=output_.packet;
      out.from=out.to=service_.route.identity();out.id=p.id;out.priority=p.priority;
      out.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      auto wire=pb_istream_from_buffer(data,size);
      const bool decoded=pb_decode(&wire,meshtastic_Data_fields,&out.decoded);
      bench::wipeKeyBuffer(data,sizeof(data));
      if(!decoded){ack(p.id,meshtastic_Routing_Error_BAD_REQUEST);return;}
      if(out.decoded.portnum==meshtastic_PortNum_WAYPOINT_APP)
        host_.waypointPacket(out.from,out.to,out.id,p.channel,false,true,out.decoded.payload.bytes,out.decoded.payload.size);
      timestamp(out);host_.preview(out.from,out.to,out.id,out.decoded.portnum,out.decoded.payload.bytes,out.decoded.payload.size,0,0,now,true);
      emit();ack(p.id,meshtastic_Routing_Error_NONE);++localMessages;return;
    }
    const int result=privateRequest?service_.submitPrivate(owner_,p.to,p.id,data,size,originHop(p),p.want_ack,
      p.pki_encrypted&&p.public_key.size?p.public_key.bytes:nullptr,
      p.pki_encrypted?p.public_key.size:0,p.pki_encrypted,now):
      service_.submitPublicApplication(owner_,p.to,p.id,data,size,originHop(p),p.want_ack,now);
    if(!result && p.decoded.portnum==meshtastic_PortNum_WAYPOINT_APP){
      const bool encryptedPeer=privateRequest&&!service_.config.licensedOperation();
      host_.waypointPacket(service_.route.identity(),p.to,p.id,encryptedPeer?0:p.channel,encryptedPeer,true,
                           p.decoded.payload.bytes,p.decoded.payload.size);
    }
    bench::wipeKeyBuffer(data,sizeof(data));queue(p.id);
    if(result) {
      const auto code=privateRequest?(result==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EACCES||result==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_PKI_FAILED):
        result==-EINVAL||result==-EALREADY?meshtastic_Routing_Error_BAD_REQUEST:
        result==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      reject(p.id,code,MT_NOTICE("Application transport requires an available armed radio, fresh packet ID and ready durable identity provider when configured"));++deniedTx;
    }
  }
  void sendMessage(const meshtastic_MeshPacket &p,uint32_t now) {
    const auto transport=peerTransport(p,meshtastic_PortNum_TEXT_MESSAGE_APP);
    const bool privateRequest=transport==PeerTransport::Private;
    if(transport==PeerTransport::Reject||transport==PeerTransport::MissingKey) {
      reject(p.id,peerTransportRefusal(transport),transport==PeerTransport::MissingKey?
        MT_NOTICE("Recipient public key is missing; exchange NodeInfo before sending"):
        MT_NOTICE("Private text needs an active identity and matching recipient key"));return;
    }
    if(privateRequest&&p.decoded.portnum!=meshtastic_PortNum_TEXT_MESSAGE_APP){reject(p.id,meshtastic_Routing_Error_PKI_FAILED,MT_NOTICE("Private messaging currently accepts text to an explicitly pinned peer"));return;}

    if(p.decoded.portnum!=meshtastic_PortNum_TEXT_MESSAGE_APP){reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Only text and bounded NodeInfo requests are supported by this endpoint"));return;}
    if(!p.id||p.to<=1||p.to==0xfffffffeu||p.hop_limit>HopPolicy::Maximum){reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Message requires a nonzero ID, valid destination and hop limit 0 through 7"));return;}
    if(p.decoded.want_response||p.decoded.request_id||p.decoded.xeddsa_signature.size){reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Response requests and caller signatures are unsupported"));return;}
    if(!service_.endpointArmed()) {
      service_.noteOriginReject(4,DiagnosticDomain::Routing,4);
      notification(p.id,service_.txAllowed()?MT_NOTICE("RF TX is disarmed; explicit LoRa TX arm required"):MT_NOTICE("RF TX is disarmed; this release has the hardware TX gate disabled"),true);
      ack(p.id,meshtastic_Routing_Error_NO_INTERFACE);++deniedTx;return;
    }
    // Like stock MeshService, local origination never trusts a client's `from`.
    // A stale phone envelope is harmless: both RF source and nonce use our ID.
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.payload=p.decoded.payload;data.has_bitfield=true;
    data.reply_id=p.decoded.reply_id;data.emoji=p.decoded.emoji;
    uint8_t encoded[239],frame[255];size_t n=0;auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
    if(!pb_encode(&stream,meshtastic_Data_fields,&data)) {ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;}
    const unsigned privateHop=originHop(p);
    if(privateRequest) {
      const int ret=service_.submitPrivate(owner_,p.to,p.id,encoded,stream.bytes_written,privateHop,p.want_ack,
      p.pki_encrypted&&p.public_key.size?p.public_key.bytes:nullptr,
      p.pki_encrypted?p.public_key.size:0,p.pki_encrypted,now);
      bench::wipeKeyBuffer(encoded,sizeof(encoded));queue(p.id);
      if(ret) {
        const auto code=ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
          ret==-EACCES||ret==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_PKI_FAILED;
        reject(p.id,code,MT_NOTICE("Private send requires identity, pinned key, nonce storage and an armed radio"));++deniedTx;
      }
      return;
    }
    if(!service_.encodePublicMessage(p.to,p.id,encoded,stream.bytes_written,frame,255,n)) {
      ack(p.id,meshtastic_Routing_Error_TOO_LARGE);return;
    }
    // Broadcast reliability uses an observed relay, never the wire want_ack bit.
    const unsigned hop=originHop(p);
    frame[12]=uint8_t(hop|(hop<<5)|((p.want_ack&&p.to!=0xffffffffu)?8:0));
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    // Stock loops local broadcasts through module dispatch before its RF send
    // attempt. Preserve that ordering even when the subsequent local/RF queue
    // refuses the packet. This branch is only reached after the Companion's
    // decoded public TEXT checks and canonical frame encoding succeeded.
    if(service_.storeForwardLocalDispatchReady()&&p.to==UINT32_MAX&&
       !p.pki_encrypted&&!p.xeddsa_signed){
      const uint32_t transport=detail::enumNumber(p.transport_mechanism);
      if(transport>=_meshtastic_MeshPacket_TransportMechanism_MIN&&
         transport<=_meshtastic_MeshPacket_TransportMechanism_MAX){
        modules::StoreForwardArchive::MessageView message{};
        message.from=service_.route.identity();message.to=UINT32_MAX;message.id=p.id;
        storeForwardTimestamp(message.time,message.hasRxTime);
        message.channel=p.channel;message.hopLimit=uint8_t(p.hop_limit);message.hopStart=uint8_t(p.hop_start);
        message.transportMechanism=uint8_t(transport);message.viaMqtt=p.via_mqtt;
        message.rxSnr=p.rx_snr;message.hasRxRssi=p.has_rx_rssi;message.rxRssi=p.rx_rssi;
        message.replyId=p.decoded.reply_id;message.emoji=p.decoded.emoji!=0;
        message.payload=p.decoded.payload.bytes;message.payloadSize=p.decoded.payload.size;
        (void)service_.archiveStoreForwardLocalText(message);
      }
    }
#endif
    const int ret=service_.submitMessage(owner_,frame,n,p.want_ack,now);
    queue(p.id);
    if(ret) {
      const auto code=(ret==-EINVAL||ret==-EALREADY)?meshtastic_Routing_Error_BAD_REQUEST:
        ret==-ENOBUFS?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:meshtastic_Routing_Error_NO_INTERFACE;
      reject(p.id,code,code==meshtastic_Routing_Error_BAD_REQUEST?MT_NOTICE("Message ID already used with different content or expired from retry history; compose a new message"):
        code==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED?MT_NOTICE("Local message queue is full"):MT_NOTICE("Radio is unavailable or TX is disarmed"));++deniedTx;
    }
  }
  void handleMqttClientProxy(uint32_t now,DeferredProxy &deferred) {
#if defined(CONFIG_MT_M119_TLS_TEST) || defined(CONFIG_MT_T096_ROUTER_ONLY)
    (void)now;
    (void)deferred;
    ++unsupported;
    return; // Reject phone-proxy ingress before policy/decode/downlink admission.
#else
    const auto &message=input_.mqttClientProxyMessage;
    Host::MqttProxyPolicy policy{};
    if(!connected_||!canonicalInput()||!host_.mqttProxyPolicy(policy)||
       !policy.mqttEnabled||!policy.proxyToClientEnabled||!policy.brokerConfigured||
       message.retained||
       message.which_payload_variant!=meshtastic_MqttClientProxyMessage_data_tag) {
      ++unsupported;return;
    }
    if(!mqtt::client_proxy::dispatchProtobuf(message,
          [&](const char *topic,const uint8_t *payload,size_t length) {
            return mqtt::MqttServiceEnvelopeDecoder::decode(payload,length,&deferred.envelope)&&
                   mqtt::MqttDownlinkAdmission::topicMatches(policy.root,topic,strlen(topic),deferred.envelope);
          })) {
      bench::wipeKeyBuffer(&deferred,sizeof(deferred));++unsupported;return;
    }
    const int gatewayLength=snprintf(deferred.gateway,sizeof(deferred.gateway),"!%08x",
                                     unsigned(service_.route.identity()));
    if(gatewayLength!=9) {
      bench::wipeKeyBuffer(&deferred,sizeof(deferred));++unsupported;return;
    }
    auto &context=deferred.context;
    context.mqttEnabled=policy.mqttEnabled;
    context.brokerConfigured=policy.brokerConfigured;
    context.transportReady=connected_&&policy.proxyToClientEnabled;
    context.proxyModeEnabled=policy.proxyToClientEnabled;
    context.phoneProxyMode=true;
    context.tlsEnabled=policy.tlsEnabled;
    // This is deliberately false: the device did not verify the phone's
    // broker TLS session. Admission treats phoneProxyMode as explicit policy
    // delegation, separate from the direct MQTT TLS/trust path.
    context.tlsTrustReady=false;
    context.encryptionEnabled=policy.encryptionEnabled;
    deferred.now=now;
    deferred.random=uint32_t(now)^service_.route.identity()^sequence_;
    deferred.active=true;
#endif
  }
  void dispatchDeferredProxy(DeferredProxy &deferred) {
    if(deferred.active)
      (void)service_.processMqttDownlink(deferred.envelope,deferred.context,
          deferred.gateway,deferred.now,deferred.random);
  }
  void pollMqttProxyPublish() {
#if defined(CONFIG_MT_M119_TLS_TEST)
    service_.setMqttProxyBridgeEnabled(false);
    return; // No phone-proxy publish bridge in the TLS-only image.
#else
    Host::MqttProxyPolicy policy{};
    const bool enabled=host_.mqttProxyPolicy(policy)&&policy.mqttEnabled&&
        policy.proxyToClientEnabled&&policy.brokerConfigured;
    service_.setMqttProxyBridgeEnabled(enabled);
    if(!connected_||!enabled)return;
    output_={};
    if(!host_.takeMqttProxyPublish(output_.mqttClientProxyMessage))return;
    if(!mqtt::client_proxy::validProtobuf(output_.mqttClientProxyMessage)) {
      bench::wipeKeyBuffer(&output_,sizeof(output_));++unsupported;return;
    }
    output_.id=++sequence_;
    output_.which_payload_variant=meshtastic_FromRadio_mqttClientProxyMessage_tag;
    (void)emit();
    bench::wipeKeyBuffer(&output_,sizeof(output_));
#endif
  }
  // Keep the large decoded packet dispatch out of handle's frame/code body.
  // The caller still holds the shared workspace lease and ClearInput guard.
  [[gnu::noinline]] void handlePacket(uint32_t now) {
    auto &p=input_.packet;
    // A policy transition requires a new canonical USB epoch before this
    // owner can enter recovery-only mode. Other Companion owners are untouched.
    if(owner_==0&&!host_.consoleAllowed()&&!recoveryOnly_)return;
    static constexpr char preferencesResume[]="zt_prefs_resume_v1";
    const bool preferencesAttempt=p.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      p.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&
      p.decoded.payload.size==sizeof(preferencesResume)-1&&
      !memcmp(p.decoded.payload.bytes,preferencesResume,sizeof(preferencesResume)-1);
    if(preferencesAttempt) {
      const bool local=owner_==0&&connected_&&canonicalInput()&&p.id&&p.to==service_.route.identity()&&
        !p.from&&!p.want_ack&&!p.hop_limit&&!p.hop_start&&!p.channel&&!p.pki_encrypted&&!p.public_key.size&&
        !p.decoded.xeddsa_signature.size&&!p.decoded.want_response&&!p.decoded.request_id&&!p.decoded.reply_id&&
        !p.decoded.emoji&&!p.decoded.dest&&!p.decoded.source&&!p.decoded.bitfield&&
        detail::enumNumber(p.ack_proof_status)==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT;
      const int result=local?host_.resumePreferences(now):-EACCES;
      if(recoveryOnly_){RecoveryReplyScope scope(recoveryReply_);
        if(result)reject(p.id,result==-EINVAL||result==-ENOENT||result==-ENOTSUP?
          meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE,
          MT_NOTICE("Preference restore resume refused; radio remains held"));
        else ack(p.id,meshtastic_Routing_Error_NONE);
      } else if(result)reject(p.id,result==-EINVAL||result==-ENOENT||result==-ENOTSUP?
          meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE,
          MT_NOTICE("Preference restore resume refused; radio remains held"));
      else ack(p.id,meshtastic_Routing_Error_NONE);
      return;
    }
    static constexpr char freshPrefix[]="zt_pki_fresh_v1 ";
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
    const bool importCommand=p.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      p.decoded.payload.size>=sizeof("zt_pki_import_v1 ")-1&&
      !memcmp(p.decoded.payload.bytes,"zt_pki_import_v1 ",sizeof("zt_pki_import_v1 ")-1);
#endif
    const bool freshAttempt=p.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      p.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&
      ((p.decoded.payload.size>=sizeof(freshPrefix)-1&&!memcmp(p.decoded.payload.bytes,freshPrefix,sizeof(freshPrefix)-1))
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
       ||importCommand
#endif
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
       ||(p.decoded.payload.size>=sizeof("zt_pki_contacts_expand_v1 ")-1&&
          !memcmp(p.decoded.payload.bytes,"zt_pki_contacts_expand_v1 ",sizeof("zt_pki_contacts_expand_v1 ")-1))
#endif
      );
    if(freshAttempt) {
      const uint32_t request=p.id;
      const bool local=
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
        (!importCommand||acceptedUsbFrame_)&&
#endif
        owner_==0&&connected_&&canonicalInput()&&request&&p.to==service_.route.identity()&&
        !p.from&&!p.want_ack&&!p.hop_limit&&!p.hop_start&&!p.channel&&!p.pki_encrypted&&!p.public_key.size&&
        !p.decoded.xeddsa_signature.size&&!p.decoded.want_response&&!p.decoded.request_id&&!p.decoded.reply_id&&
        !p.decoded.emoji&&!p.decoded.dest&&!p.decoded.source&&!p.decoded.bitfield&&
        detail::enumNumber(p.ack_proof_status)==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT;
      char response[233]{};detail::VolatileWipe wipeResponse{response,sizeof(response)};
      const int size=local?host_.freshProvision(p.decoded.payload.bytes,p.decoded.payload.size,frameEpoch_,response,sizeof(response)):-EACCES;
      bench::wipeKeyBuffer(&p,sizeof(p));bench::wipeKeyBuffer(frame_,sizeof(frame_));
      if(size<=0||size>=int(sizeof(response))){
        if(recoveryOnly_){RecoveryReplyScope scope(recoveryReply_);reject(request,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Local USB fresh setup refused"));}
        else reject(request,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Local USB fresh setup refused"));return;}
      fresh(meshtastic_FromRadio_packet_tag);auto &out=output_.packet;
      out.from=out.to=service_.route.identity();out.id=sequence_;
      out.which_payload_variant=meshtastic_MeshPacket_decoded_tag;out.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
      out.decoded.request_id=request;out.decoded.has_bitfield=true;out.decoded.payload.size=pb_size_t(size);
      memcpy(out.decoded.payload.bytes,response,size_t(size));timestamp(out);
      if(recoveryOnly_){RecoveryReplyScope scope(recoveryReply_);emit();}else emit();return;
    }
    static constexpr char restorePrefix[]="zt_pki_restore_v1 ";
    const size_t restorePrefixLength=sizeof(restorePrefix)-1;
    const bool restoreAttempt=p.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      p.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&
      // Detect the reserved command from its fixed prefix even if a malformed
      // decoded size exceeds the payload array; otherwise it could fall into
      // the ordinary text path. The fixed generated payload array is larger
      // than this prefix, so this bounded prefix comparison is safe.
      p.decoded.payload.size>=restorePrefixLength&&
      !memcmp(p.decoded.payload.bytes,restorePrefix,restorePrefixLength);
    if(restoreAttempt) {
      const uint32_t requestId=p.id;
      const bool localShape=owner_==0&&connected_&&canonicalInput()&&requestId&&p.to==service_.route.identity()&&
        !p.from&&!p.want_ack&&!p.hop_limit&&!p.hop_start&&!p.channel&&!p.pki_encrypted&&!p.public_key.size&&
        !p.decoded.xeddsa_signature.size&&!p.decoded.want_response&&!p.decoded.request_id&&!p.decoded.reply_id&&
        !p.decoded.emoji&&!p.decoded.dest&&!p.decoded.source&&!p.decoded.bitfield&&
        detail::enumNumber(p.ack_proof_status)==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT;
      const int result=localShape?host_.restoreExistingIdentity(p.decoded.payload.bytes,
        p.decoded.payload.size,frameEpoch_):-EACCES;
      // Clear both Companion-owned decoded and serialized copies immediately.
      // Preserve only the request ID needed for its numeric ACK.
      bench::wipeKeyBuffer(&p,sizeof(p));bench::wipeKeyBuffer(frame_,sizeof(frame_));
      if(recoveryOnly_){RecoveryReplyScope scope(recoveryReply_);
        if(result)reject(requestId,result==-EINVAL||result==-ENOTSUP?
            meshtastic_Routing_Error_BAD_REQUEST:
            result==-EACCES||result==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:
            meshtastic_Routing_Error_PKI_FAILED,
            MT_NOTICE("Local USB identity restore refused; no private input was returned"));
        else ack(requestId,meshtastic_Routing_Error_NONE);
      } else if(result)reject(requestId,result==-EINVAL||result==-ENOTSUP?
            meshtastic_Routing_Error_BAD_REQUEST:
            result==-EACCES||result==-EBUSY?meshtastic_Routing_Error_NO_INTERFACE:
            meshtastic_Routing_Error_PKI_FAILED,
            MT_NOTICE("Local USB identity restore refused; no private input was returned"));
      else ack(requestId,meshtastic_Routing_Error_NONE);
      return;
    }
    if(recoveryOnly_) {
      // Only the three reserved local recovery commands above are admissible
      // in this session. No generic local-read, Admin, endpoint or radio path.
      return;
    }
    queue(p.id);
    if(!host_.ready()){reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Local config/radio unavailable"));return;}
    if(p.which_payload_variant!=meshtastic_MeshPacket_decoded_tag){reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("Companion requests must contain decoded application data"));return;}
    if(p.channel>=8){reject(p.id,meshtastic_Routing_Error_NO_CHANNEL,MT_NOTICE("Channel index must be 0 through 7"));return;}
#if defined(CONFIG_MT_POWER_STRESS)
    if(p.decoded.portnum==meshtastic_PortNum_POWERSTRESS_APP){
      const bool local=owner_==0 && connected_ && p.to==service_.route.identity() && p.id && !p.from &&
        !p.channel && !p.pki_encrypted && !p.public_key.size && !p.decoded.xeddsa_signature.size &&
        !p.decoded.source && !p.decoded.dest && !p.decoded.request_id && !p.decoded.reply_id &&
        !p.decoded.want_response && !p.decoded.emoji && !p.via_mqtt;
      const int result=local ? host_.powerStress(p.id,p.decoded.payload.bytes,p.decoded.payload.size) : -EACCES;
      ack(p.id,result==0 ? meshtastic_Routing_Error_NONE :
        result==-EBUSY ? meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED :
        result==-EINVAL ? meshtastic_Routing_Error_BAD_REQUEST : meshtastic_Routing_Error_NO_INTERFACE);
      return;
    }
#endif
    if(p.to==service_.route.identity()&&p.decoded.portnum==meshtastic_PortNum_ADMIN_APP) {
      if(p.channel){reject(p.id,meshtastic_Routing_Error_NO_CHANNEL,MT_NOTICE("Local admin uses channel0"));return;}
      // Stock Python Node._sendAdmin sets PKI even for local USB/BLE admin.
      // The existing local transport policy grants no packet key claims;
      // discard hints and reconstruct the reply through existing admin handling.
      p.pki_encrypted=false;p.public_key={};p.decoded.xeddsa_signature={};
      p.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT;
      handleAdmin(p,now);return;
    }
#ifdef CONFIG_MT_T096_ROUTER_ONLY
    reject(p.id,meshtastic_Routing_Error_NO_INTERFACE,MT_NOTICE("Router USB supports local administration only"));
#else
    if(service_.config.licensedOperation()&&p.pki_encrypted) {
      reject(p.id,meshtastic_Routing_Error_PKI_FAILED,
        MT_NOTICE("Licensed operation sends public channel packets; encrypted private requests are unavailable"));return;
    }
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(p.decoded.portnum==meshtastic_PortNum_ADMIN_APP) {sendPrivateAdminGet(p,now);return;}
#endif
    if(p.channel) {PrimaryChannel::Selection key;const bool configured=service_.config.channelSelection(p.channel,key);PrimaryChannel::clear(key);if(!configured){reject(p.id,meshtastic_Routing_Error_NO_CHANNEL,MT_NOTICE("Selected channel is disabled or unavailable"));return;}}
    if(p.to==service_.route.identity()&&(p.pki_encrypted||p.decoded.xeddsa_signature.size||
      detail::enumNumber(p.ack_proof_status)!=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT)) {
      reject(p.id,meshtastic_Routing_Error_PKI_FAILED,MT_NOTICE("Local requests cannot carry caller-supplied private or authenticated metadata"));return;
    }

    if(p.to==service_.route.identity()&&(p.decoded.portnum==meshtastic_PortNum_NODEINFO_APP||
      p.decoded.portnum==meshtastic_PortNum_POSITION_APP)) {
      localRead(p);return;
    }
    if(p.to==service_.route.identity()&&(p.decoded.portnum==meshtastic_PortNum_NEIGHBORINFO_APP
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
      ||p.decoded.portnum==meshtastic_PortNum_PAXCOUNTER_APP
#endif
      )){localNeighbors(p);return;}
    if(p.to==service_.route.identity()&&p.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP) {
      localTelemetry(p);return;
    }
    // A USB message addressed to this node is local endpoint delivery, never RF.
    if(p.to==service_.route.identity()&&p.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP) {
#if defined(CONFIG_MT_PKI_STOCK_TRIAL)
      if(pki::UsbProvisionParser::handles(p.decoded.payload.bytes,p.decoded.payload.size)) {
        if(owner_!=0||!connected_||!canonicalInput()||!p.id||p.from||p.want_ack||p.hop_limit||p.hop_start||
           p.decoded.want_response||p.decoded.request_id||p.decoded.reply_id||p.decoded.emoji||
           p.decoded.dest||p.decoded.source||p.decoded.bitfield) {
          reject(p.id,meshtastic_Routing_Error_BAD_REQUEST,MT_NOTICE("PKI provisioning requires a canonical connected USB-local command"));return;
        }
        char response[233]{};
        const int size=host_.provision(p.decoded.payload.bytes,p.decoded.payload.size,frameEpoch_,response,sizeof(response));
        if(size<=0||size>int(sizeof(response))){reject(p.id,meshtastic_Routing_Error_PKI_FAILED,MT_NOTICE("PKI provisioning refused; no retry was performed"));return;}
        const auto request=p.id;fresh(meshtastic_FromRadio_packet_tag);auto &out=output_.packet;
        out.from=out.to=service_.route.identity();out.id=sequence_;
        out.which_payload_variant=meshtastic_MeshPacket_decoded_tag;out.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
        out.decoded.request_id=request;out.decoded.has_bitfield=true;out.decoded.payload.size=pb_size_t(size);
        memcpy(out.decoded.payload.bytes,response,size_t(size));timestamp(out);emit();return;
      }
#endif
      host_.preview(service_.route.identity(),p.to,p.id,p.decoded.portnum,p.decoded.payload.bytes,p.decoded.payload.size,0,0,now,true);
      fresh(meshtastic_FromRadio_packet_tag);output_.packet=p;output_.packet.from=service_.route.identity();
      output_.packet.public_key={}; // An ignored caller hint is not local identity metadata.
      output_.packet.want_ack=false;timestamp(output_.packet);emit();ack(p.id,meshtastic_Routing_Error_NONE);++localMessages;return;
    }
    if(service_.normalMode()&&PublicApplication::allowed(detail::enumNumber(p.decoded.portnum))&&p.to==service_.route.identity()) {
      sendApplication(p,now);return;
    }
    Service::ChannelScope channel(service_,p.channel);
    if(!channel.valid()){reject(p.id,meshtastic_Routing_Error_NO_CHANNEL,MT_NOTICE("Selected channel is disabled or unavailable"));return;}
    // Keep the signer call chain out of the text encoder's large stack frame.
#if defined(CONFIG_MT_HOST_METRICS_BRIDGE)
    // Portduino producer runs on the attached Linux host. The USB companion
    // supplies only a HostMetrics broadcast; other telemetry retains its query dispatcher.
    if(owner_==0 && connected_ && service_.normalMode() && p.to==UINT32_MAX && !p.decoded.want_response &&
       p.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP &&
       PublicApplication::payloadAllowed(p.decoded.portnum,p.decoded.payload.bytes,p.decoded.payload.size)){
      sendApplication(p,now);return;
    }
#endif
    if(service_.normalMode()&&p.decoded.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&p.decoded.want_response)sendNeighborQuery(p,now);
    else if(service_.normalMode()&&PublicApplication::allowed(detail::enumNumber(p.decoded.portnum)))sendApplication(p,now);
    else if(service_.normalMode()&&p.decoded.portnum==meshtastic_PortNum_NODEINFO_APP)sendNodeInfo(p,now);
    else if(service_.normalMode()&&p.decoded.portnum==meshtastic_PortNum_TRACEROUTE_APP)sendTraceRoute(p,now);
    else if(service_.normalMode()&&p.decoded.portnum==meshtastic_PortNum_POSITION_APP)sendPositionQuery(p,now);
    else if(service_.normalMode()&&p.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP)sendTelemetryQuery(p,now);
    else sendEndpoint(p,now);
#endif
    return;
  }
  void handle(uint32_t now,DeferredProxy &deferred) {
    struct ClearInput {
      meshtastic_ToRadio &input;uint8_t *frame;size_t size;
      ~ClearInput(){bench::wipeKeyBuffer(&input,sizeof(input));bench::wipeKeyBuffer(frame,size);}
    } clearInput{input_,frame_,sizeof(frame_)};
    input_={};auto s=pb_istream_from_buffer(frame_,length_);
    if(!pb_decode(&s,meshtastic_ToRadio_fields,&input_)){++parseErrors;return;}
    if(owner_==0&&(recoveryOnly_||!host_.consoleAllowed())) {
      switch(input_.which_payload_variant) {
      case meshtastic_ToRadio_want_config_id_tag:handshake(input_.want_config_id,now);break;
      case meshtastic_ToRadio_disconnect_tag:resetStream();break;
      case meshtastic_ToRadio_packet_tag:handlePacket(now);break;
      default:++unsupported;break;
      }
      return;
    }
    switch(input_.which_payload_variant) {
    case meshtastic_ToRadio_want_config_id_tag:handshake(input_.want_config_id,now);break;
    case meshtastic_ToRadio_disconnect_tag:resetStream();break;
#if defined(CONFIG_MT_PHONE_FILES)
    case meshtastic_ToRadio_xmodemPacket_tag:
      if(configComplete_&&appFilesAllowed()){
        if(!host_.appFileSubmit(owner_,input_.xmodemPacket,now)){
          fresh(meshtastic_FromRadio_xmodemPacket_tag);
          output_.xmodemPacket.control=meshtastic_XModem_Control_NAK;emit(false);
        }
      }
      break;
#endif
    case meshtastic_ToRadio_heartbeat_tag:
      if(connected_){
        if(input_.heartbeat.nonce!=1)queue();
        else if(host_.ready()&&canonicalInput())host_.nodeInfoPing(now);
      }
      break;
    case meshtastic_ToRadio_mqttClientProxyMessage_tag:handleMqttClientProxy(now,deferred);break;
    case meshtastic_ToRadio_packet_tag:handlePacket(now);break;
    default:++unsupported;break;
    }
  }
  // Run the completed frame's parser under the one shared workspace lease,
  // then hand any admitted MQTT proxy packet to Service only after release.
  // That Service call can synchronously reenter receive() on both companions.
  void dispatchFrame(uint32_t now,companion::CompanionWorkspace::OuterLockHeldProof proof,
                     bool boundary=false,uint32_t nonce=0) {
    if(!proof.held())return;
    DeferredProxy deferred{};
    detail::VolatileWipe deferredWipe{&deferred,sizeof(deferred)};
    {
      auto lease=workspace_.acquire(proof);
      if(!lease){bench::wipeKeyBuffer(frame_,sizeof(frame_));length_=used_=0;return;}
      if(boundary) {
        const uint32_t epoch=frameEpoch_;
        resetStream();host_.completeUnmeasuredHandshakeBoundary(epoch);handshake(nonce,now);
      }
      else handle(now,deferred);
    }
    dispatchDeferredProxy(deferred);
  }
#if defined(CONFIG_MT_RF_ADMIN_GET)
  static bool adminEntropy(void *context,uint8_t *out,size_t size) {
    auto *host=static_cast<Host*>(context);
    return host&&host->adminPasskey(out,size);
  }
  static meshtastic_Routing_Error remoteStorageResult(int result) {
    return !result?meshtastic_Routing_Error_NONE:
      result==-EINVAL||result==-ENOENT?meshtastic_Routing_Error_BAD_REQUEST:
      result==-EBUSY||result==-EAGAIN||result==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
      meshtastic_Routing_Error_NO_INTERFACE;
  }
  meshtastic_Routing_Error applyRemoteNodeEdit(admin::mutation_protocol::Operation operation,
      uint32_t id,uint32_t request,uint32_t from,uint32_t now) {
    using Op=admin::mutation_protocol::Operation;
    if(id<=1||id>=0xfffffffeu||id==service_.route.identity())return meshtastic_Routing_Error_BAD_REQUEST;
    if((operation==Op::Unignore||operation==Op::ToggleMute)&&service_.nodes.find(id)<0)
      return meshtastic_Routing_Error_NONE;
    const uint8_t replayOperation=operation==Op::ToggleMute?MuteToggleReplay:0;
    if(operation==Op::ToggleMute) {
      for(const auto &seen:muteReplay_)if(seen.request==request&&seen.from==from) {
        return seen.operation==replayOperation&&seen.target==id?
          meshtastic_Routing_Error_NONE:meshtastic_Routing_Error_BAD_REQUEST;
      }
    }
    int result=-ENOTSUP;
    if(operation==Op::Favorite||operation==Op::Unfavorite)
      result=host_.editRemoteFavorite(id,operation==Op::Favorite,now);
    else {
      NodeCache::Edit edit=NodeCache::Edit::Remove;
      switch(operation) {
      case Op::Ignore:edit=NodeCache::Edit::Ignore;break;
      case Op::Unignore:edit=NodeCache::Edit::Unignore;break;
      case Op::ToggleMute:edit=NodeCache::Edit::ToggleMute;break;
      default:return meshtastic_Routing_Error_BAD_REQUEST;
      }
      result=host_.editNode(id,edit,now);
    }
    const bool committedDegraded=result==Host::EditCommittedDegraded;
    if(operation==Op::ToggleMute&&(result==0||committedDegraded))
      muteReplay_[nextMuteReplay_++%8]={request,id,from,replayOperation,committedDegraded};
    return committedDegraded?meshtastic_Routing_Error_NONE:remoteStorageResult(result);
  }
  meshtastic_Routing_Error executeRemoteAdminSet(const admin::mutation_protocol::Mutation &mutation,
      uint32_t request,uint32_t from,uint32_t now) {
    using Op=admin::mutation_protocol::Operation;
    meshtastic_AdminMessage setter=admin_;detail::VolatileWipe setterWipe{&setter,sizeof(setter)};
    setter.session_passkey={};
    uint8_t setterWire[admin::remote_protocol::MaxPayloadBytes]{};
    detail::VolatileWipe setterWireWipe{setterWire,sizeof(setterWire)};
    auto encoded=pb_ostream_from_buffer(setterWire,sizeof(setterWire));
    if(!pb_encode(&encoded,meshtastic_AdminMessage_fields,&setter))return meshtastic_Routing_Error_TOO_LARGE;
    const size_t setterSize=encoded.bytes_written;
    switch(mutation.operation) {
    case Op::Owner:return applyOwner(admin_.set_owner,setterWire,setterSize,false);
    case Op::Config:{
      bool deferred=false;
      return applyConfig(admin_.set_config,now,setterWire,setterSize,request,false,true,deferred,false);
    }
    case Op::ModuleConfig:
      if(!(host_.supportedModules()&moduleBit(admin_.set_module_config.which_payload_variant)))
        return meshtastic_Routing_Error_NO_INTERFACE;
      if(!canonicalModuleConfig(setterWire,setterSize))return meshtastic_Routing_Error_BAD_REQUEST;
      return remoteStorageResult(host_.saveRemoteModuleConfig(admin_.set_module_config,now));
    case Op::Channel:return applyChannel(admin_.set_channel,setterWire,setterSize,now,true);
    case Op::Favorite:case Op::Unfavorite:case Op::Ignore:case Op::Unignore:case Op::ToggleMute:
      return applyRemoteNodeEdit(mutation.operation,mutation.node,request,from,now);
    case Op::SetTimeOnly:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_time_only_tag,setterWire,setterSize)||
         !admin_.set_time_only)return meshtastic_Routing_Error_BAD_REQUEST;
      return remoteStorageResult(host_.setWallTime(admin_.set_time_only));
    case Op::StoreUiConfig:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_store_ui_config_tag,setterWire,setterSize)||
         !ui::UiConfigSettings::valid(admin_.store_ui_config))return meshtastic_Routing_Error_BAD_REQUEST;
      return remoteStorageResult(host_.saveUiConfig(admin_.store_ui_config,now));
    case Op::SetCannedMessages:{
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_canned_message_module_messages_tag,
                                  setterWire,setterSize))return meshtastic_Routing_Error_BAD_REQUEST;
      size_t length=0;
      if(!validCannedMessageList(admin_.set_canned_message_module_messages,
                                 sizeof(admin_.set_canned_message_module_messages),length))
        return meshtastic_Routing_Error_BAD_REQUEST;
      return length?cannedStoreError(host_.saveCannedMessageMessages(
        admin_.set_canned_message_module_messages,length,now)):meshtastic_Routing_Error_NONE;
    }
    case Op::SetRingtone:{
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_set_ringtone_message_tag,
                                  setterWire,setterSize))return meshtastic_Routing_Error_BAD_REQUEST;
      const size_t length=strnlen(admin_.set_ringtone_message,sizeof(admin_.set_ringtone_message));
      if(length==sizeof(admin_.set_ringtone_message))return meshtastic_Routing_Error_BAD_REQUEST;
      return remoteStorageResult(host_.saveRingtone(admin_.set_ringtone_message,length,now));
    }
    case Op::FixedPosition:
      return applyFixedPosition(admin_.set_fixed_position,setterWire,setterSize,true,now);
    case Op::RemoveFixedPosition:
      return applyRemoveFixedPosition(admin_.remove_fixed_position,setterWire,setterSize,true,now);
    case Op::Reboot:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_reboot_seconds_tag,setterWire,setterSize)||
         (host_.editing()&&admin_.reboot_seconds>=0)||!host_.scheduleReboot(admin_.reboot_seconds))
        return meshtastic_Routing_Error_BAD_REQUEST;
      return meshtastic_Routing_Error_NONE;
    case Op::Shutdown:
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_shutdown_seconds_tag,setterWire,setterSize)||
         (host_.editing()&&admin_.shutdown_seconds>=0)||!host_.scheduleShutdown(admin_.shutdown_seconds))
        return meshtastic_Routing_Error_BAD_REQUEST;
      return meshtastic_Routing_Error_NONE;
    case Op::Ota: {
      const auto &request=admin_.ota_request;
      const auto mode=detail::enumNumber(request.reboot_ota_mode);
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_ota_request_tag,setterWire,setterSize)||
         (mode!=meshtastic_OTAMode_OTA_BLE&&mode!=meshtastic_OTAMode_OTA_WIFI)||
         request.ota_hash.size!=32||host_.editing())
        return meshtastic_Routing_Error_BAD_REQUEST;
      const int result=host_.scheduleFirmwareUpdate(request,now);
      return result?(result==-EBUSY||result==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EINVAL||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE):
        meshtastic_Routing_Error_NONE;
    }
    case Op::Dfu: {
      if(!canonicalAdminOperation(admin_,meshtastic_AdminMessage_enter_dfu_mode_request_tag,setterWire,setterSize)||
         !admin_.enter_dfu_mode_request||host_.editing())return meshtastic_Routing_Error_BAD_REQUEST;
      const int result=host_.enterDfu(now);
      return result?(result==-EBUSY||result==-ENOSPC?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        result==-EINVAL||result==-ENOTSUP?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE):
        meshtastic_Routing_Error_NONE;
    }
    default:return meshtastic_Routing_Error_BAD_REQUEST;
    }
  }
#endif
public:
  uint32_t handshakes=0,parseErrors=0,encodeErrors=0,outputDrops=0,unsupported=0,deniedTx=0,localMessages=0,receivedMessages=0;
  using WorkspaceProof=companion::CompanionWorkspace::OuterLockHeldProof;
  Companion(Service &s,Host &h,uint8_t model,companion::CompanionWorkspace &workspace,uint8_t owner=0)
    :service_(s),host_(h),workspace_(workspace),model_(model),owner_(owner),
     input_(workspace_.scratch().input),output_(workspace_.scratch().output),
     admin_(workspace_.scratch().admin),reply_(workspace_.scratch().reply) {}
#if defined(CONFIG_MT_RF_ADMIN_GET)
private:
  bool adminDeviceInfo(const admin::remote_protocol::Query &query) {
    using Operation=admin::remote_protocol::Operation;
    switch(query.operation){
    case Operation::DeviceMetadata:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_device_metadata_response_tag;
      metadata(reply_.get_device_metadata_response);return true;
    case Operation::ConnectionStatus:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_device_connection_status_response_tag;
      connectionStatus(reply_.get_device_connection_status_response);return true;
    case Operation::HardwarePins:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag;
      return host_.hardwarePinInventory(reply_.get_node_remote_hardware_pins_response)==0;
    default:return false;
    }
  }
public:
  // Main calls after a staged remote Config has actually been applied.
  // This publishes the normal owner-scoped local node snapshot; it
  // does not originate a radio packet or bypass position cadence/privacy.
  void remoteConfigurationApplied(bool clearPosition,WorkspaceProof proof) {
    auto lease=workspace_.acquire(proof);if(!lease)return;
    if(!connected_||!configComplete_||(owner_==0&&!host_.consoleAllowed()))return;
    localNode(clearPosition);
  }
  // Trusted Service/PkiRuntime synchronous callback, under the service lock.
  // This does not dispatch local Admin, emit client data, write settings, or
  // grant authority to a packet supplied over USB/BLE/TCP.
  bool privateAdminReplyData(uint32_t local,uint32_t from,const uint8_t key[32],
                            uint32_t request,const uint8_t *data,size_t length,
                            uint8_t *out,size_t capacity,size_t &size,WorkspaceProof proof) {
    size=0;
    auto lease=workspace_.acquire(proof);if(!lease)return false;
    if(!out||!capacity||local!=service_.route.identity()||!host_.ready()||host_.editing())return false;
    admin::remote_protocol::Query query;
    if(!admin::remote_protocol::canonicalGetRequest(data,length,query))return false;
    meshtastic_Config_SecurityConfig security{};
    admin::remote_protocol::Wipe sw{&security,sizeof(security)};
    if(!host_.adminKeys(security)||admin::AdminAuthorization::authorizeCurrentIngress(
        from,local,request,key,length,local,security)!=admin::Authorization::Authorized||
       !service_.privateAdminPeerCurrent(from,key))return false;
    admin_={};reply_={};
    admin::remote_protocol::Wipe aw{&admin_,sizeof(admin_)},rw{&reply_,sizeof(reply_)};
    switch(query.operation){
    case admin::remote_protocol::Operation::Owner:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
      user(reply_.get_owner_response);break;
    case admin::remote_protocol::Operation::Config: {
      reply_.which_payload_variant=meshtastic_AdminMessage_get_config_response_tag;
      auto &c=reply_.get_config_response;
      if(!config(c,admin::remote_protocol::configResponseTag(query.selector)))return false;
      if(c.which_payload_variant==meshtastic_Config_network_tag&&
         !admin::remote_redaction::redactRemoteWifiPsk(c))return false;
      if(c.which_payload_variant==meshtastic_Config_security_tag&&
         !admin::remote_redaction::redactRemoteSecurityPrivateKey(c))return false;
      break;
    }
    case admin::remote_protocol::Operation::ModuleConfig: {
      reply_.which_payload_variant=meshtastic_AdminMessage_get_module_config_response_tag;
      auto &c=reply_.get_module_config_response;
      if(!moduleConfig(c,admin::remote_protocol::moduleConfigResponseTag(query.selector)))return false;
      if(c.which_payload_variant==meshtastic_ModuleConfig_mqtt_tag&&
         !admin::remote_redaction::redactRemoteMqttPassword(c))return false;
      break;
    }
    case admin::remote_protocol::Operation::Channel:
      reply_.which_payload_variant=meshtastic_AdminMessage_get_channel_response_tag;
      channel(reply_.get_channel_response,unsigned(query.selector)-1);break;
    case admin::remote_protocol::Operation::CannedMessages: {
      char messages[201]{};size_t length=0;
      if(host_.cannedMessageMessages(messages,sizeof(messages))||
         !validCannedMessageList(messages,sizeof(messages),length))return false;
      reply_.which_payload_variant=meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
      memcpy(reply_.get_canned_message_module_messages_response,messages,length+1);break;
    }
    case admin::remote_protocol::Operation::Ringtone: {
      reply_.which_payload_variant=meshtastic_AdminMessage_get_ringtone_response_tag;
      if(host_.ringtone(reply_.get_ringtone_response,sizeof(reply_.get_ringtone_response))||
         strnlen(reply_.get_ringtone_response,sizeof(reply_.get_ringtone_response))==sizeof(reply_.get_ringtone_response))return false;
      break;
    }
    case admin::remote_protocol::Operation::UiConfig: {
      reply_.which_payload_variant=meshtastic_AdminMessage_get_ui_config_response_tag;
      meshtastic_DeviceUIConfig value{};detail::VolatileWipe valueWipe{&value,sizeof(value)};
      if(host_.uiConfig(value)||!ui::UiConfigSettings::valid(value))return false;
      reply_.get_ui_config_response=value;break;
    }
    default:if(!adminDeviceInfo(query))return false;break;
    }
    if(!adminPasskeys_.issueAfterCallerAuthorization(from,key,host_.adminSessionNow(),
          &adminEntropy,&host_,reply_.session_passkey.bytes))return false;
    reply_.session_passkey.size=8;
    meshtastic_Data response{};admin::remote_protocol::Wipe dw{&response,sizeof(response)};
    response.portnum=meshtastic_PortNum_ADMIN_APP;response.request_id=request;
    // Stock zero-hop header admission requires the modern Data bitfield even
    // when neither MQTT consent nor a further application reply is requested.
    response.has_bitfield=true;response.bitfield=0;
    auto payload=pb_ostream_from_buffer(response.payload.bytes,sizeof(response.payload.bytes));
    if(!pb_encode(&payload,meshtastic_AdminMessage_fields,&reply_))return false;
    response.payload.size=payload.bytes_written;
    auto wire=pb_ostream_from_buffer(out,capacity<227?capacity:227);
    if(!pb_encode(&wire,meshtastic_Data_fields,&response)||
       !admin::remote_protocol::canonicalGetResponse(out,wire.bytes_written,query,request)){
      bench::wipeKeyBuffer(out,capacity<227?capacity:227);return false;
    }
    size=wire.bytes_written;return true;
  }

  // Service calls this synchronously only for an admitted public request on
  // the configured keyed Admin channel. Possession of the shared-channel
  // session token is not PKI identity or a private-peer claim.
  bool legacyAdminRequest(const admin::LegacyAdminRequest &request,uint32_t now,
      uint8_t *out,size_t capacity,size_t &size,WorkspaceProof proof) {
    size=0;
    auto lease=workspace_.acquire(proof);if(!lease)return false;
    if(owner_!=0||!out||!capacity||!host_.ready()||host_.editing()||
       !service_.currentLegacyAdminRequest(request))return false;
    const uint32_t domain=request.domainEpoch();
    if(legacyAdminScopeKnown_&&(domain!=legacyAdminDomainEpoch_||
       request.channelIndex()!=legacyAdminChannelIndex_||request.channelHash()!=legacyAdminChannelHash_))
      legacyAdminSession_.reset();
    legacyAdminScopeKnown_=true;legacyAdminDomainEpoch_=domain;
    legacyAdminChannelIndex_=request.channelIndex();legacyAdminChannelHash_=request.channelHash();

    const uint8_t *data=nullptr;size_t dataSize=0;
    if(!request.requestData(data,dataSize)||!data||!dataSize||dataSize>admin::remote_protocol::MaxDataBytes)
      return false;
    meshtastic_Data envelope{};admin::remote_protocol::Wipe envelopeWipe{&envelope,sizeof(envelope)};
    uint8_t canonical[admin::remote_protocol::MaxDataBytes]{};
    admin::remote_protocol::Wipe canonicalWipe{canonical,sizeof(canonical)};
    if(!admin::remote_protocol::decodeCanonicalData(data,dataSize,envelope,canonical)||
       !admin::remote_protocol::validDataEnvelope(envelope,true)||
       envelope.portnum!=meshtastic_PortNum_ADMIN_APP)return false;

    if(request.kind()==admin::LegacyAdminRequest::Kind::Get) {
      admin::remote_protocol::Query query{};
      if(!admin::remote_protocol::canonicalGetRequest(data,dataSize,query))return false;
      uint8_t payload[admin::remote_protocol::MaxPayloadBytes]{};
      admin::remote_protocol::Wipe payloadWipe{payload,sizeof(payload)};
      size_t payloadSize=0;
      if(!admin::remote_protocol::extractCanonicalPayload(data,dataSize,true,0,payload,payloadSize))return false;
      admin_={};reply_={};
      admin::remote_protocol::Wipe adminWipe{&admin_,sizeof(admin_)},replyWipe{&reply_,sizeof(reply_)};
      uint8_t canonicalAdmin[admin::remote_protocol::MaxPayloadBytes]{};
      admin::remote_protocol::Wipe canonicalAdminWipe{canonicalAdmin,sizeof(canonicalAdmin)};
      if(!admin::remote_protocol::decodeCanonicalAdmin(payload,payloadSize,admin_,canonicalAdmin))return false;
      switch(query.operation) {
      case admin::remote_protocol::Operation::Owner:
        reply_.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
        user(reply_.get_owner_response);break;
      case admin::remote_protocol::Operation::Config: {
        reply_.which_payload_variant=meshtastic_AdminMessage_get_config_response_tag;
        auto &configResponse=reply_.get_config_response;
        if(!config(configResponse,admin::remote_protocol::configResponseTag(query.selector)))return false;
        if(configResponse.which_payload_variant==meshtastic_Config_network_tag&&
           !admin::remote_redaction::redactRemoteWifiPsk(configResponse))return false;
        if(configResponse.which_payload_variant==meshtastic_Config_security_tag&&
           !admin::remote_redaction::redactRemoteSecurityPrivateKey(configResponse))return false;
        break;
      }
      case admin::remote_protocol::Operation::ModuleConfig: {
        reply_.which_payload_variant=meshtastic_AdminMessage_get_module_config_response_tag;
        auto &module=reply_.get_module_config_response;
        if(!moduleConfig(module,admin::remote_protocol::moduleConfigResponseTag(query.selector)))return false;
        if(module.which_payload_variant==meshtastic_ModuleConfig_mqtt_tag&&
           !admin::remote_redaction::redactRemoteMqttPassword(module))return false;
        break;
      }
      case admin::remote_protocol::Operation::Channel:
        reply_.which_payload_variant=meshtastic_AdminMessage_get_channel_response_tag;
        channel(reply_.get_channel_response,unsigned(query.selector)-1);
        redactChannelPsk(reply_.get_channel_response);break;
      case admin::remote_protocol::Operation::CannedMessages: {
        char messages[201]{};size_t length=0;
        if(host_.cannedMessageMessages(messages,sizeof(messages))||
           !validCannedMessageList(messages,sizeof(messages),length))return false;
        reply_.which_payload_variant=meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
        memcpy(reply_.get_canned_message_module_messages_response,messages,length+1);break;
      }
      case admin::remote_protocol::Operation::Ringtone:
        reply_.which_payload_variant=meshtastic_AdminMessage_get_ringtone_response_tag;
        if(host_.ringtone(reply_.get_ringtone_response,sizeof(reply_.get_ringtone_response))||
           strnlen(reply_.get_ringtone_response,sizeof(reply_.get_ringtone_response))==sizeof(reply_.get_ringtone_response))return false;
        break;
      case admin::remote_protocol::Operation::UiConfig: {
        reply_.which_payload_variant=meshtastic_AdminMessage_get_ui_config_response_tag;
        meshtastic_DeviceUIConfig value{};detail::VolatileWipe valueWipe{&value,sizeof(value)};
        if(host_.uiConfig(value)||!ui::UiConfigSettings::valid(value))return false;
        reply_.get_ui_config_response=value;break;
      }
      default:if(!adminDeviceInfo(query))return false;break;
      }
      if(!legacyAdminSession_.issue(now,&adminEntropy,&host_,reply_.session_passkey.bytes))return false;
      reply_.session_passkey.size=admin::LegacyAdminSession::TokenBytes;
      meshtastic_Data response{};admin::remote_protocol::Wipe responseWipe{&response,sizeof(response)};
      response.portnum=meshtastic_PortNum_ADMIN_APP;response.request_id=request.packetId();
      response.has_bitfield=true;response.bitfield=0;
      auto payloadOut=pb_ostream_from_buffer(response.payload.bytes,sizeof(response.payload.bytes));
      if(!pb_encode(&payloadOut,meshtastic_AdminMessage_fields,&reply_))return false;
      response.payload.size=payloadOut.bytes_written;
      auto output=pb_ostream_from_buffer(out,capacity<admin::remote_protocol::MaxDataBytes?
                                           capacity:admin::remote_protocol::MaxDataBytes);
      if(!pb_encode(&output,meshtastic_Data_fields,&response)||
         !admin::remote_protocol::canonicalGetResponse(out,output.bytes_written,query,request.packetId())) {
        bench::wipeKeyBuffer(out,capacity<admin::remote_protocol::MaxDataBytes?
                                 capacity:admin::remote_protocol::MaxDataBytes);
        return false;
      }
      size=output.bytes_written;return true;
    }
    if(request.kind()!=admin::LegacyAdminRequest::Kind::Set)return false;
    using namespace admin::mutation_protocol;
    Mutation mutation{};
    if(!canonicalSetRequest(data,dataSize,mutation))return false;
    uint8_t payload[admin::remote_protocol::MaxPayloadBytes]{};
    admin::remote_protocol::Wipe payloadWipe{payload,sizeof(payload)};
    size_t payloadSize=0;
    if(!admin::remote_protocol::extractCanonicalPayload(data,dataSize,true,0,payload,payloadSize))return false;
    admin_={};admin::remote_protocol::Wipe adminWipe{&admin_,sizeof(admin_)};
    uint8_t canonicalAdmin[admin::remote_protocol::MaxPayloadBytes]{};
    admin::remote_protocol::Wipe canonicalAdminWipe{canonicalAdmin,sizeof(canonicalAdmin)};
    if(!admin::remote_protocol::decodeCanonicalAdmin(payload,payloadSize,admin_,canonicalAdmin))return false;
    meshtastic_Routing_Error error=meshtastic_Routing_Error_BAD_REQUEST;
    if(!legacyAdminSession_.validate(admin_.session_passkey.bytes,admin_.session_passkey.size,now))
      error=meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY;
    else if(!host_.ready())error=meshtastic_Routing_Error_NO_INTERFACE;
    else if(host_.editing())error=meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;
    else if(service_.currentLegacyAdminRequest(request))
      error=executeRemoteAdminSet(mutation,request.packetId(),request.from(),now);
    else return false;

    meshtastic_Routing routing{};admin::remote_protocol::Wipe routingWipe{&routing,sizeof(routing)};
    routing.which_variant=meshtastic_Routing_error_reason_tag;routing.error_reason=error;
    meshtastic_Data response{};admin::remote_protocol::Wipe responseWipe{&response,sizeof(response)};
    response.portnum=meshtastic_PortNum_ROUTING_APP;response.request_id=request.packetId();
    response.has_bitfield=true;response.bitfield=0;
    auto routingOut=pb_ostream_from_buffer(response.payload.bytes,sizeof(response.payload.bytes));
    if(!pb_encode(&routingOut,meshtastic_Routing_fields,&routing))return false;
    response.payload.size=routingOut.bytes_written;
    auto dataOut=pb_ostream_from_buffer(out,capacity<admin::remote_protocol::MaxDataBytes?
                                           capacity:admin::remote_protocol::MaxDataBytes);
    if(!pb_encode(&dataOut,meshtastic_Data_fields,&response)||
       !admin::legacy_protocol::canonicalResponse(out,dataOut.bytes_written,request.packetId())) {
      bench::wipeKeyBuffer(out,capacity<admin::remote_protocol::MaxDataBytes?
                               capacity:admin::remote_protocol::MaxDataBytes);
      return false;
    }
    size=dataOut.bytes_written;return true;
  }

  // Called by every local transport before an Admin-key allowlist write;
  // production also fans invalidation out to the trusted provider instance.
  void clearPrivateAdminSessions() { adminPasskeys_.clearAll(); }

  // Trusted Service/PkiRuntime synchronous owner-loop callback, after its RX
  // event and storage state are restored. This path applies only the narrow
  // typed mutation slice and never sends a client/RF response itself.
  bool privateAdminMutation(uint32_t local,uint32_t from,const uint8_t key[32],
                            uint32_t request,const uint8_t *data,size_t length,
                            uint32_t now,uint32_t &routingError,WorkspaceProof proof) {
    routingError=meshtastic_Routing_Error_BAD_REQUEST;
    auto lease=workspace_.acquire(proof);if(!lease)return false;
    using namespace admin::mutation_protocol;
    Mutation mutation{};
    if(!canonicalSetRequest(data,length,mutation))return false;
    if(local!=service_.route.identity()||!key||!request)return false;
    meshtastic_Config_SecurityConfig security{};
    admin::remote_protocol::Wipe securityWipe{&security,sizeof(security)};
    if(!host_.adminKeys(security)||admin::AdminAuthorization::authorizeCurrentIngress(
        from,local,request,key,length,local,security)!=admin::Authorization::Authorized)return false;

    uint8_t payload[admin::remote_protocol::MaxPayloadBytes]{};
    admin::remote_protocol::Wipe payloadWipe{payload,sizeof(payload)};
    size_t payloadSize=0;
    if(!admin::remote_protocol::extractCanonicalPayload(data,length,true,0,payload,payloadSize))return false;
    admin_={};
    admin::remote_protocol::Wipe adminWipe{&admin_,sizeof(admin_)};
    uint8_t canonical[admin::remote_protocol::MaxPayloadBytes]{};
    admin::remote_protocol::Wipe canonicalWipe{canonical,sizeof(canonical)};
    if(!admin::remote_protocol::decodeCanonicalAdmin(payload,payloadSize,admin_,canonical)||
       admin_.session_passkey.size!=admin::AdminSessionPasskeys::kTokenBytes)return false;
    if(!host_.ready()) {routingError=meshtastic_Routing_Error_NO_INTERFACE;return true;}
    if(host_.editing()) {routingError=meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;return true;}
    if(!adminPasskeys_.validateAfterCallerAuthorization(from,key,host_.adminSessionNow(),
                                                        admin_.session_passkey.bytes)) {
      routingError=meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY;return true;
    }
    // Check the current authenticated peer pin immediately before a typed
    // setter; the allowlist snapshot above is also fresh for this request.
    if(!service_.privateAdminPeerCurrent(from,key))return false;
    routingError=executeRemoteAdminSet(mutation,request,from,now);return true;
  }
#endif
  // The caller owns persistence and the original transport epoch/admission.
  // Local SET completes with one Routing result, never an Admin GET response.
  void completeBluetoothConfig(uint32_t request,int result,WorkspaceProof proof) {
    auto lease=workspace_.acquire(proof);if(!lease)return;ack(request,bluetoothConfigResult(result));
  }
  // Local verified NodeDB removal: both transports call this under serviceLock.
  // Only received duplicate records are forgotten; outbound safety is separate.
  void forgetPeer(uint32_t id){for(auto &seen:seen_)if(seen.from==id)seen={};
#if defined(CONFIG_MT_RF_ADMIN_GET)
    adminPasskeys_.revokePeer(id);
#endif
  }
  void packet(const uint8_t *p,size_t n,uint32_t now,WorkspaceProof proof) {
    if(!proof.held())return;
    if(owner_==0&&(recoveryOnly_||!host_.consoleAllowed()))return;
    {
      auto lease=workspace_.acquire(proof);if(!lease)return;
      if(!p||!n||n>sizeof(frame_)){++parseErrors;return;}
      memcpy(frame_,p,n);length_=n;
    }
    dispatchFrame(now,proof);
  }
#if defined(CONFIG_MT_POWER_STRESS) || defined(CONFIG_MT_GENERIC_THREAD)
  bool localModuleLog(const char *source,const char *message,WorkspaceProof proof){
    if(owner_ || !connected_ || !configComplete_ || !host_.consoleAllowed())return false;
    auto lease=workspace_.acquire(proof);if(!lease)return false;
    fresh(meshtastic_FromRadio_log_record_tag);
    auto &record=output_.log_record;
    record.level=meshtastic_LogRecord_Level_INFO;record.time=host_.wallTime();
#if defined(CONFIG_MT_LOCAL_CLOCK)
    record.time=service_.config.localClock(record.time);
#endif
    snprintf(record.source,sizeof(record.source),"%s",source);
    snprintf(record.message,sizeof(record.message),"%s",message);
    return emit(false,true);
  }
#endif
  // This path writes only FromRadio on the admitted owner session. Keep it
  // separate from mesh announcements, their display fanout and MQTT.
  bool sendLocalPosition(uint32_t now,WorkspaceProof proof){
    if(owner_>1||!connected_||!configComplete_||handshakeNonce_==OnlyConfig||
       !host_.ready()||host_.editing()||!service_.normalMode()||
       (positionSentToPhone_&&uint32_t(now-lastPositionToPhoneMs_)<60000))return false;
    if(owner_==0){
      if(recoveryOnly_||!host_.consoleAllowed())return false;
    }else{
      const auto link=host_.bluetoothSecurity();
      if(!link.connected||link.pending||link.error||link.level<2)return false;
    }
    auto lease=workspace_.acquire(proof);if(!lease)return false;
    meshtastic_Position p{};
    if(!service_.localPosition(p)||(!p.latitude_i&&!p.longitude_i))return false;
    // Live fixes already have the configured field projection. Fixed Position
    // stores a measured MSL altitude independently of its output field flags.
    if(service_.config.fixedPosition()&&(service_.config.gnssSettings().positionFlags&3)!=3){
      p.has_altitude=false;p.altitude=0;
    }
    p.precision_bits=32;
    p.time=host_.wallTime();
    fresh(meshtastic_FromRadio_packet_tag);
    auto &packet=output_.packet;
    packet.from=service_.route.identity();packet.to=UINT32_MAX;packet.id=sequence_;
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.priority=meshtastic_MeshPacket_Priority_BACKGROUND;
    packet.decoded.portnum=meshtastic_PortNum_POSITION_APP;
    auto payload=pb_ostream_from_buffer(packet.decoded.payload.bytes,sizeof(packet.decoded.payload.bytes));
    if(!pb_encode(&payload,meshtastic_Position_fields,&p))return false;
    packet.decoded.payload.size=payload.bytes_written;timestamp(packet,p.time);
    if(!emit(false,true))return false;
    positionSentToPhone_=true;lastPositionToPhoneMs_=now;
    return true;
  }
  bool modulePacket(const uint8_t*data,size_t size,uint32_t id,unsigned channel,WorkspaceProof proof,bool idleOnly=false,bool background=false){
    auto lease=workspace_.acquire(proof);if(!lease)return false;
    if(!connected_||(idleOnly&&!configComplete_)||(owner_==0&&(recoveryOnly_||!host_.consoleAllowed()))||
       !data||!size||size>239||channel>=8)return false;
    fresh(meshtastic_FromRadio_packet_tag);auto&packet=output_.packet;
    packet.from=service_.route.identity();packet.to=0xffffffffu;packet.id=id;packet.channel=channel;
    if(background)packet.priority=meshtastic_MeshPacket_Priority_BACKGROUND;
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;auto input=pb_istream_from_buffer(data,size);
    if(!pb_decode(&input,meshtastic_Data_fields,&packet.decoded)||input.bytes_left)return false;
    timestamp(packet,host_.wallTime());return emit(!idleOnly,idleOnly);
  }
  void poll(WorkspaceProof proof) {
    auto lease=workspace_.acquire(proof);if(!lease)return;
#if defined(CONFIG_MT_PHONE_FILES)
    if(!appFilesAllowed())host_.appFileDisconnect(owner_);
    else if(configComplete_){
      meshtastic_XModem packet{};
      if(host_.appFileResponse(owner_,packet)){
        fresh(meshtastic_FromRadio_xmodemPacket_tag);output_.xmodemPacket=packet;
        if(emit(false))host_.appFileSent(owner_);
      }
    }
#endif
#if defined(CONFIG_MT_RF_ADMIN_GET)
    if(legacyAdminScopeKnown_&&legacyAdminDomainEpoch_!=service_.legacyAdminDomainEpoch()) {
      legacyAdminSession_.reset();legacyAdminScopeKnown_=false;
    }
    legacyAdminSession_.poll(host_.adminSessionNow());
#endif
    if(owner_==0&&(recoveryOnly_||!host_.consoleAllowed())){
      if(apiLogSessionActive_){host_.safeApiLogSession(false,false);apiLogSessionActive_=false;}
      return;
    }
#if defined(CONFIG_MT_RF_ADMIN_GET)
    adminPasskeys_.sweep(host_.adminSessionNow());
#endif
#if defined(CONFIG_MT_PEER_DIRECTORY)
    if(favoriteRequest_){
      int result=-ESTALE;
      if(connected_&&service_.normalMode()&&!service_.config.isManaged()&&host_.ready()&&!host_.editing()&&
         favoriteRevision_==host_.directoryRevision())
        result=host_.promoteDirectoryFavorite(owner_,favoriteNode_,favoriteAt_);
      if(result==-EAGAIN)return;
      const uint32_t request=favoriteRequest_;favoriteRequest_=0;
      if(!result||result==Host::EditCommittedDegraded){
        const int slot=service_.nodes.find(favoriteNode_);
        if(slot>=0){fresh(meshtastic_FromRadio_node_info_tag);cachedNode(output_.node_info,*service_.nodes.get(unsigned(slot)));emit();}
        if(result)notification(request,MT_NOTICE("NodeDB saved; radio RX restore failed; interface faulted"),true);
        ack(request,meshtastic_Routing_Error_NONE);
      }else ack(request,result==-ENOSPC||result==-EBUSY?meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
                        result==-ESTALE||result==-ENOENT?meshtastic_Routing_Error_BAD_REQUEST:meshtastic_Routing_Error_NO_INTERFACE);
    }
#endif
    if(handshakePhase_!=HandshakePhase::Idle) {
      progressHandshake();
      if(handshakePhase_!=HandshakePhase::Idle)return;
    }
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS))
    if(owner_==0){
      uint32_t id=0;int result=0;
      if(host_.sensorAdminResult(sensorSession_,id,result)&&connected_)ack(id,bluetoothConfigResult(result));
    }
#endif
    pollMqttProxyPublish();
    replaySatellites();
    if(service_.normalMode()) {
      Endpoint::Result r;
      while(service_.takeMessageResult(owner_,r)) {
        if(!connected_)continue;
        const char *message=r.kind==Endpoint::Kind::Authenticated?(r.code?MT_NOTICE("Authenticated peer Routing error received"):MT_NOTICE("Authenticated peer Routing ACK received")):
          r.kind==Endpoint::Kind::PrivateResponse?MT_NOTICE("Authenticated app response received; not a Routing proof ACK"):
          r.kind==Endpoint::Kind::MqttEcho?MT_NOTICE("Own MQTT echo matched locally; destination delivery unconfirmed"):
          r.kind==Endpoint::Kind::Peer?MT_NOTICE("Public-channel peer ACK matched; unauthenticated"):
          r.kind==Endpoint::Kind::Response?MT_NOTICE("Public app response correlated; not authenticated"):
          r.kind==Endpoint::Kind::Relay?MT_NOTICE("Rebroadcast matched; relay evidence only, not recipient receipt"):
          r.kind==Endpoint::Kind::Local?MT_NOTICE("Local radio TX complete; no peer receipt requested"):
          MT_NOTICE("No confirmed delivery; bounded retry or radio policy ended");
        notification(r.id,message,r.kind==Endpoint::Kind::Failed||r.code!=0);queue(r.id);
        ack(r.id,static_cast<meshtastic_Routing_Error>(r.code),r.peer,r.kind==Endpoint::Kind::Authenticated);
      }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
      replayLocalHistory();
#endif
      pollSafeApiLog();
      return;
    }
    uint32_t id;int result;
    if(service_.takeEndpointResult(owner_,id,result)) {
      if(result==0){notification(id,MT_NOTICE("Radio TX complete: zero hop, no retries; delivery unconfirmed"),false);ack(id,meshtastic_Routing_Error_NONE);}
      else {notification(id,MT_NOTICE("Radio send failed; no retry; bench quota consumed"),true);ack(id,meshtastic_Routing_Error_NO_INTERFACE);}
    }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    replayLocalHistory();
#endif
    pollSafeApiLog();
  }
  void resetStream() {
#if defined(CONFIG_MT_PEER_DIRECTORY)
    host_.directoryDisconnect(owner_);favoriteRequest_=0;
#endif
#if defined(CONFIG_MT_PHONE_FILES)
    host_.appFileDisconnect(owner_);
#endif
#if (defined(CONFIG_MT_SCD4X_ADMIN) || defined(CONFIG_MT_ACCESSORY_SENSORS))
    ++sensorSession_;
#endif
    if(owner_==0){host_.provisionDisconnect();host_.freshProvisionDisconnect();}
    state_=0;length_=used_=0;frameEpoch_=0;bench::wipeKeyBuffer(frame_,sizeof(frame_));connected_=false;
    recoveryOnly_=false;recoveryReply_=false;
    handshakePhase_=HandshakePhase::Idle;handshakeIndex_=0;handshakeNonce_=handshakeNow_=0;
    configComplete_=false;
    if(apiLogSessionActive_){host_.safeApiLogSession(owner_==1,false);apiLogSessionActive_=false;}
    for(auto &entry:muteReplay_)entry={};
    nextMuteReplay_=0;statusReplayNext_=statusReplayCount_=0;replayKind_=Satellite::Done;service_.disconnect(owner_);
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    localHistory_.clear();
#endif
  }
  // Timeout abandons incomplete messages. Invalid length never allocates memory.
  void byte(uint8_t b,uint32_t now,uint32_t epoch,WorkspaceProof proof) {
    if(!proof.held())return;
    if(state_&&(uint32_t(now-lastByte_)>2000||frameEpoch_!=epoch)){
      state_=0;length_=used_=0;bench::wipeKeyBuffer(frame_,sizeof(frame_));
    }
    lastByte_=now;
    switch(state_) {
    case 0:if(b==0x94){state_=1;frameEpoch_=epoch;}break;
    case 1:state_=b==0xc3?2:(b==0x94?1:0);break;
    case 2:length_=size_t(b)<<8;state_=3;break;
    case 3:length_|=b;used_=0;if(!length_||length_>sizeof(frame_)){state_=0;++parseErrors;}else state_=4;break;
    case 4:frame_[used_++]=b;if(used_==length_){
      state_=0;
      if(host_.acceptTransportFrame(frameEpoch_)) {
        uint32_t nonce=0;
        // Only a complete, top-level, byte-canonical frame can delimit the
        // unmeasured XIAO stream. In-frame payload bytes are never rescanned.
        bool boundary=false;
        {
          auto lease=workspace_.acquire(proof);
          if(!lease){bench::wipeKeyBuffer(frame_,sizeof(frame_));length_=used_=0;break;}
          boundary=canonicalHandshakeBoundary(frame_,length_,nonce)&&
                   host_.beginUnmeasuredHandshakeBoundary();
        }
        acceptedUsbFrame_=true;dispatchFrame(now,proof,boundary,nonce);acceptedUsbFrame_=false;
      }
      else {bench::wipeKeyBuffer(frame_,sizeof(frame_));length_=used_=0;}
      break;
    }
    }
  }
  void receive(const uint8_t *p,size_t n,int16_t rssi,int8_t snr,uint32_t now,WorkspaceProof proof) {
    receive(p,n,rssi,snr,now,host_.wallTime(),proof);
  }
  // A single RF dequeue supplies one captured UTC value to every local
  // transport, including zero for unknown, so fanout cannot straddle seconds.
  void receive(const uint8_t *p,size_t n,int16_t rssi,int8_t snr,uint32_t now,uint32_t receivedAt,WorkspaceProof proof) {
    receive(p,n,rssi,snr,now,receivedAt,false,0,true,proof);
  }
  // Packet status is copied with the payload at native callback/dequeue time.
  // Only protobuf packet SNR uses the fractional value. Routing, cache,
  // traceroute and preview retain the existing integer-dB input.
  void receive(const uint8_t *p,size_t n,int16_t rssi,int8_t snr,uint32_t now,uint32_t receivedAt,
               bool snrKnown,uint8_t snrRaw,bool radioReceive,WorkspaceProof proof) {
    auto lease=workspace_.acquire(proof);if(!lease)return;
    if(owner_==0&&(recoveryOnly_||!host_.consoleAllowed()))return;
    if(!service_.receivingFrame(p,n,radioReceive)||!p||n<17||n>255)return;
    const float packetSnr=snrKnown?packetSnrDb(snrRaw):float(snr);
    uint32_t from=bench::le32(p+4),to=bench::le32(p),id=bench::le32(p+8);
    const unsigned hop=p[12]&7,start=(p[12]>>5)&7;
    if(from<=1||from>=0xfffffffeu||!id||from==service_.route.identity()||(to!=0xffffffffu&&to!=service_.route.identity())||(start&&hop>start))return;
    if(service_.config.ignoresIncoming(from)||service_.nodes.ignored(from))return; // Before duplicate state, cache and UI.
    if(p[13]==0&&to==service_.route.identity()&&!service_.config.licensedOperation()&&
       !service_.sharedChannelZeroReceive(p,n)) {
      pki::PrivateEvent event;
      if(!service_.takePrivateEvent(owner_,p,n,event))return;
      // The service committed replay and restored healthy RX before publishing.
      // Decode only its immutable authenticated event; no transport decrypt.
      fresh(meshtastic_FromRadio_packet_tag);auto &out=output_.packet;
      auto stream=pb_istream_from_buffer(event.data,event.size);
      out.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      if(!pb_decode(&stream,meshtastic_Data_fields,&out.decoded)) {bench::wipeKeyBuffer(&event,sizeof(event));return;}
      if(!service_.receivePortAllowed(out.decoded.portnum)
#if defined(CONFIG_MT_RF_ADMIN_GET)
         &&!event.adminOwners
#endif
      ) {
        bench::wipeKeyBuffer(&event,sizeof(event));bench::wipeKeyBuffer(&output_,sizeof(output_));return;
      }
#if defined(CONFIG_MT_RF_ADMIN_GET)
      // Retain only the exact canonical remote-hardware GET response whose
      // private request was already matched to this owner by PkiRuntime and
      // Service. This callback only stages typed RAM metadata; it performs no
      // storage I/O and never handles a SET or GPIO operation.
      if(event.adminOwners&&out.decoded.portnum==meshtastic_PortNum_ADMIN_APP) {
        admin::remote_protocol::Query remotePinQuery{
          admin::remote_protocol::Operation::ModuleConfig,
          static_cast<uint8_t>(meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG)};
        if(admin::remote_protocol::canonicalGetResponse(event.data,event.size,
            remotePinQuery,out.decoded.request_id)) {
          meshtastic_AdminMessage response{};
          admin::remote_protocol::Wipe wipeResponse{&response,sizeof(response)};
          uint8_t canonicalAdmin[admin::remote_protocol::MaxPayloadBytes]{};
          admin::remote_protocol::Wipe wipeCanonicalAdmin{canonicalAdmin,sizeof(canonicalAdmin)};
          if(admin::remote_protocol::decodeCanonicalAdmin(out.decoded.payload.bytes,
              out.decoded.payload.size,response,canonicalAdmin)&&
             response.which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag&&
             response.get_module_config_response.which_payload_variant==
               meshtastic_ModuleConfig_remote_hardware_tag) {
            (void)host_.stageRemoteHardwarePins(event.from,event.publicKey,
              response.get_module_config_response.payload_variant.remote_hardware,now);
          }
        }
      }
#endif
      if(Endpoint::positionResponsePort(out.decoded.portnum)&&out.decoded.request_id) {
        uint8_t payload[233]{};size_t size=0;
        const bool admitted=service_.takePrivatePosition(owner_,event.from,event.to,event.id,out.decoded.request_id,now,payload,sizeof(payload),size,uint8_t(out.decoded.portnum))&&
          size==out.decoded.payload.size&&!memcmp(payload,out.decoded.payload.bytes,size);
        if(admitted) {
          PeerPosition position;
          if(out.decoded.portnum==meshtastic_PortNum_POSITION_APP&&cachedPosition(payload,size,position))
            privateSatellite(event.from,Satellite::Position,service_.nodes.observePosition(event.from,position));
        }
        bench::wipeKeyBuffer(payload,sizeof(payload));
        if(!admitted) {
          bench::wipeKeyBuffer(&event,sizeof(event));bench::wipeKeyBuffer(&output_,sizeof(output_));return;
        }
      }
      if(out.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&out.decoded.request_id) {
        uint8_t payload[233]{};size_t size=0;PeerDeviceMetrics metrics;
        if(!service_.takePrivateTelemetry(owner_,event.from,event.to,event.id,out.decoded.request_id,now,payload,sizeof(payload),size)||
           size!=out.decoded.payload.size||memcmp(payload,out.decoded.payload.bytes,size)) {
          bench::wipeKeyBuffer(&event,sizeof(event));bench::wipeKeyBuffer(&output_,sizeof(output_));return;
        }
        if(TelemetryQuery::cachedMetrics(payload,size,metrics))
          privateSatellite(event.from,Satellite::Device,service_.nodes.observeDeviceMetrics(event.from,metrics));
        else if(!RemoteTelemetryQuery::responsePayload(payload,size)) {
          bench::wipeKeyBuffer(&event,sizeof(event));bench::wipeKeyBuffer(&output_,sizeof(output_));return;
        }
      }
      if(out.decoded.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&out.decoded.request_id) {
        uint8_t payload[233]{};size_t size=0;
        const bool admitted=service_.takePrivateNeighbor(owner_,event.from,event.to,event.id,out.decoded.request_id,now,payload,sizeof(payload),size)&&
          size==out.decoded.payload.size&&!memcmp(payload,out.decoded.payload.bytes,size);
        bench::wipeKeyBuffer(payload,sizeof(payload));
        if(!admitted) {
          bench::wipeKeyBuffer(&event,sizeof(event));bench::wipeKeyBuffer(&output_,sizeof(output_));return;
        }
      }
#if defined(CONFIG_MT_RF_ADMIN_GET)
      // Admin data belongs only to the recorded requesting companion.
      if(!event.adminOwners) {
#endif
      service_.nodes.observeReceivedPacket(event.from,receivedAt,(p[12]&0x10u)!=0);
      observeRadioPacket(event.from,start>=hop?start-hop:0,snrKnown,packetSnr,now,
        out.decoded.portnum,out.decoded.payload.bytes,out.decoded.payload.size,radioReceive,(p[12]&0x10u)!=0,p[15],receivedAt,start||out.decoded.has_bitfield);
#if defined(CONFIG_MT_RF_ADMIN_GET)
      }
#endif
      out.from=event.from;out.to=event.to;out.id=event.id;out.pki_encrypted=true;
      out.via_mqtt=(p[12]&0x10u)!=0;
      timestamp(out,receivedAt);
      out.public_key.size=32;memcpy(out.public_key.bytes,event.publicKey,32);
      out.rx_snr=packetSnr;out.has_rx_rssi=snrKnown;out.rx_rssi=rssi;out.hop_limit=hop;out.hop_start=start;
#if defined(CONFIG_MT_RF_ADMIN_GET)
      if(!event.adminOwners) {
#endif
      if(out.decoded.portnum==meshtastic_PortNum_POSITION_APP)
        host_.positionForGeofence(from,out.to,0,true,out.decoded.payload.bytes,out.decoded.payload.size);
      if(out.decoded.portnum==meshtastic_PortNum_WAYPOINT_APP)
        host_.waypointPacket(from,out.to,id,0,true,false,out.decoded.payload.bytes,out.decoded.payload.size);
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
      if(out.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP && !out.decoded.want_response){
        meshtastic_Telemetry report{};
        if(RemoteTelemetryQuery::decodeCanonical(out.decoded.payload.bytes,out.decoded.payload.size,0,report))host_.displayReport(from,id,false,report);
      }
#endif
      host_.peripheralPacket(from,out.to,id,out.decoded.portnum,0,out.decoded.payload.bytes,out.decoded.payload.size,false);
      host_.preview(from,out.to,id,out.decoded.portnum,out.decoded.payload.bytes,out.decoded.payload.size,rssi,snr,now,false);
#if defined(CONFIG_MT_RF_ADMIN_GET)
      }
#endif
      if(NodeUser::coerceId(out.decoded,event.from))out.decoded.xeddsa_signature.size=0;
      if(connected_&&emit())++receivedMessages;
      bench::wipeKeyBuffer(&event,sizeof(event));bench::wipeKeyBuffer(&output_,sizeof(output_));return;
    }
    if(!service_.receiveSourceAllowed(from)||!service_.routingPacketAllowed(false,p[13],from,to))return;
    bench::RouteMetadata m;
    unsigned channelIndex=0;if(!service_.decodeChannel(p,n,m,channelIndex))return;
    Service::ChannelScope channel(service_,channelIndex);if(!channel.valid())return;
    if(!service_.receivePortAllowed(m.portnum)){bench::wipeKeyBuffer(&m,sizeof(m));return;}
    if(!service_.privatePeerMetadata(from,m))return;
    service_.applyPublicSignature(p,n,m,now);
    if(!service_.packetSignatureAllowed(p,m,now)||m.signatureQuarantined)return;
    if(m.portnum==meshtastic_PortNum_TRACEROUTE_APP)m.signatureLength=0;
    const bool neighborResponse=m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&m.request_id;
    // Only an immutable Endpoint approval admits a transaction response.
    // Signed unsolicited Neighbor announcements keep their ordinary path.
#if defined(CONFIG_MT_REPLY_EXAMPLE)
    const bool stockExample=Service::stockExampleRequest(service_.codec,p,n,m);
#else
    constexpr bool stockExample=false;
#endif
    if(PublicApplication::allowed(m.portnum)&&!stockExample&&!neighborResponse&&
       !(Endpoint::positionResponsePort(m.portnum)&&m.request_id)&&
       !PublicApplication::received(service_.codec,p,n,m))return;
    const bool signedModule=m.signatureVerified&&bench::publicSignedModulePort(m.portnum);
    const bool verifiedEnvelope=m.signatureVerified&&m.signatureLength==64&&
      (signedModule||m.portnum==meshtastic_PortNum_POSITION_APP||m.portnum==meshtastic_PortNum_NODEINFO_APP||service_.codec.licensedPlaintext());
    uint8_t signerKey[32]{};
    if(verifiedEnvelope&&!service_.signature.publicIdentity(from,now,signerKey))return;
    // Only normalized endpoint results can acknowledge client requests. Passing
    // an unmatched raw Routing/request_id through lets stock clients bypass the
    // engine's peer/destination checks.
    if(service_.normalMode()&&(m.portnum==meshtastic_PortNum_ROUTING_APP||hop>start||(!start&&!m.hasBitfield)))return;
    for(auto &x:seen_)if(x.from==from&&x.id==id&&uint32_t(now-x.at)<600000)return;
    uint8_t tracePayload[TraceRoute::MaxPayload]{};size_t traceSize=0;
    const bool traceResponse=m.portnum==meshtastic_PortNum_TRACEROUTE_APP;
    if(traceResponse&&!service_.takeTraceRoute(owner_,from,to,id,m,now,tracePayload,sizeof(tracePayload),traceSize))return;
    const bool positionResponse=Endpoint::positionResponsePort(m.portnum)&&m.request_id;
    if(positionResponse&&!service_.takePositionQuery(owner_,from,to,id,m,now,tracePayload,sizeof(tracePayload),traceSize))return;
    const bool telemetryResponse=m.portnum==meshtastic_PortNum_TELEMETRY_APP&&m.request_id;
    if(telemetryResponse&&!service_.takeTelemetryQuery(owner_,from,to,id,m,now,tracePayload,sizeof(tracePayload),traceSize))return;
    if(neighborResponse&&!service_.takeNeighborQuery(owner_,from,to,id,m,now,tracePayload,sizeof(tracePayload),traceSize))return;
    const bool gpioResponse=m.portnum==meshtastic_PortNum_REMOTE_HARDWARE_APP&&m.request_id;
    const bool applicationResponse=traceResponse||positionResponse||telemetryResponse||neighborResponse||gpioResponse;
    const bool transformedResponse=traceResponse||positionResponse||telemetryResponse||neighborResponse;
    seen_[nextSeen_++%64]={from,id,now};
    if(m.portnum==meshtastic_PortNum_ADMIN_APP)return; // RF never enters USB admin dispatch.
    if(m.portnum==meshtastic_PortNum_NODEINFO_APP) {
      if(!m.nodeInfoValid)return;
      if(service_.refusesUnsignedIdentity(from,channelIndex,m.signatureVerified))return;
      {
        // Same complete admission as Service, before response completion. An
        // unchanged entry still emits one event to each connected transport.
        const int slot=NodeUser::accept(service_.nodes,service_.route.identity(),from,
          m.payload,m.payloadLength,snr,start>=hop?start-hop:0);
        if(slot<0)return;
        service_.nodes.observeReceivedPacket(from,receivedAt,(p[12]&0x10u)!=0);
#if defined(CONFIG_MT_PEER_DIRECTORY)
        const uint32_t sharedUtc=to==UINT32_MAX&&!m.wantResponse&&!m.request_id?receivedAt:0;
#if defined(CONFIG_MT_PAGED_SATELLITES)
        service_.satellites.publicHeader(service_.nodes,from,sharedUtc);
#endif
        host_.directoryObserved(*service_.nodes.get(unsigned(slot)),sharedUtc);
#endif
        // An unchanged shared entry still belongs in each client's event stream.
        if(connected_){fresh(meshtastic_FromRadio_node_info_tag);cachedNode(output_.node_info,*service_.nodes.get(unsigned(slot)));emit();}
      }
    } else service_.nodes.observeReceivedPacket(from,receivedAt,(p[12]&0x10u)!=0);
    observeRadioPacket(from,start>=hop?start-hop:0,snrKnown,packetSnr,now,
      static_cast<meshtastic_PortNum>(m.portnum),m.payload,m.payloadLength,radioReceive,(p[12]&0x10u)!=0,p[15],receivedAt,start||m.hasBitfield);
    // Only matched, canonical Public query responses enter the RAM cache.
    // Unknown peers and unsolicited telemetry remain forwarded as before.
    if(telemetryResponse){
      PeerDeviceMetrics metrics;
      if(TelemetryQuery::cachedMetrics(tracePayload,traceSize,metrics))
        privateSatellite(from,Satellite::Device,service_.nodes.observeDeviceMetrics(from,metrics));
    }
    // Correlated replies use the immutable Service-approved body. Ordinary
    // canonical Position announcements may annotate known peers after the
    // existing channel/signature/ignore/duplicate admission above. Neither
    // path creates an identity or changes local coordinates, clock or storage.
    const bool sharedBroadcast=to==UINT32_MAX&&!m.wantResponse&&!m.request_id;
    const Satellite::Observation observation{receivedAt,int16_t(packetSnr*4),
      uint8_t((start>=hop?8u+start-hop:0u)|((p[12]&0x10u)?16u:0u))};
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
    // Only canonical, non-query broadcasts are projected. A canonical directed
    // report withdraws this sender's previous public display value.
    if(!sharedBroadcast && to==service_.route.identity() && !m.wantResponse && m.portnum==meshtastic_PortNum_TELEMETRY_APP){
      meshtastic_Telemetry report{};
      if(RemoteTelemetryQuery::decodeCanonical(m.payload,m.payloadLength,0,report))host_.displayReport(from,id,false,report);
    }
#endif
    if(sharedBroadcast&&m.portnum==meshtastic_PortNum_TELEMETRY_APP){
      meshtastic_Telemetry t{};
      if(RemoteTelemetryQuery::decodeCanonical(m.payload,m.payloadLength,0,t)){
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
        host_.displayReport(from,id,true,t);
#endif
        if(t.which_variant==meshtastic_Telemetry_device_metrics_tag){
          PeerDeviceMetrics dm;
          if(dm.assign(t.variant.device_metrics)){
            if(service_.satellites.remember(service_.nodes,from,Satellite::Device,channelIndex,nullptr,0,observation))
              (void)service_.nodes.observeDeviceMetrics(from,dm,true);
#if defined(CONFIG_MT_PAGED_SATELLITES)
            uint8_t body[32];size_t len=0;if(companion::SatelliteWire::device(dm,body,len))
              host_.satelliteReceived(from,Satellite::Device,channelIndex,observation,body,len);
#endif
          }
        } else if(t.which_variant==meshtastic_Telemetry_environment_metrics_tag){
          if(service_.satellites.remember(service_.nodes,from,Satellite::Environment,channelIndex,m.payload,m.payloadLength,observation))
            service_.nodes.environmentReplay(from,true);
#if defined(CONFIG_MT_PAGED_SATELLITES)
          host_.satelliteReceived(from,Satellite::Environment,channelIndex,observation,m.payload,m.payloadLength);
#endif
        }
      }
    }
    if(m.portnum==meshtastic_PortNum_POSITION_APP){
      PeerPosition position;
      const auto *payload=positionResponse?tracePayload:m.payload;
      const size_t size=positionResponse?traceSize:m.payloadLength;
      if(cachedPosition(payload,size,position)){
        const bool sameChannel=service_.satellites.channel(from,Satellite::Position)==channelIndex;
        const bool replay=sharedBroadcast&&service_.satellites.remember(service_.nodes,from,Satellite::Position,channelIndex,nullptr,0,observation);
        const bool updated=service_.nodes.observePosition(from,position,replay,sameChannel);
        if(!sharedBroadcast)privateSatellite(from,Satellite::Position,updated);
#if defined(CONFIG_MT_PAGED_SATELLITES)
        if(sharedBroadcast){
          uint8_t body[32];size_t len=0;const int slot=service_.nodes.find(from);
          const auto *hot=slot<0?nullptr:service_.nodes.get(unsigned(slot));
          if(companion::SatelliteWire::position(hot&&hot->replayPosition?hot->position:position,body,len))
            host_.satelliteReceived(from,Satellite::Position,channelIndex,observation,body,len);
        }
#endif
        host_.positionForGeofence(from,to,channelIndex,false,payload,size);
      }
    }
    if(m.portnum==meshtastic_PortNum_NODE_STATUS_APP){
      auto setNodeStatus=[&](uint32_t source,const PeerStatus &status) {
        const bool replay=sharedBroadcast&&service_.satellites.remember(service_.nodes,source,Satellite::Status,channelIndex,nullptr,0,observation);
#if defined(CONFIG_MT_PAGED_SATELLITES)
        if(sharedBroadcast)host_.satelliteReceived(source,Satellite::Status,channelIndex,observation,
            reinterpret_cast<const uint8_t*>(status.text()),strlen(status.text()));
#endif
        const bool updated=service_.nodes.observeStatus(source,status,replay);
        if(!sharedBroadcast)privateSatellite(source,Satellite::Status,updated);
        return updated;
      };
      stock::StatusMessageModule::handleReceived({from,m.payload,m.payloadLength},stock::StatusMessageReceiver(setNodeStatus));
    }
    if(m.portnum==meshtastic_PortNum_RANGE_TEST_APP)host_.rangeObservation(from,id,m.payload,m.payloadLength,rssi,packetSnr,snrKnown,uint8_t(hop),receivedAt);
    if(!applicationResponse){
      if(m.portnum==meshtastic_PortNum_WAYPOINT_APP)
        host_.waypointPacket(from,to,id,channelIndex,false,false,m.payload,m.payloadLength);
      host_.peripheralPacket(from,to,id,m.portnum,channelIndex,m.payload,m.payloadLength,false);
      host_.preview(from,to,id,m.portnum,m.payload,m.payloadLength,rssi,snr,now,false);
    }
    if(!connected_)return;
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
    // The archive already delivers an admitted public chat to each normal phone
    // session. A missing server, failed append, or non-replayable packet still
    // uses normal live delivery.
    if(radioReceive&&m.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&configComplete_&&handshakeNonce_!=OnlyConfig&&
       !m.signatureLength&&(to==UINT32_MAX||to==service_.route.identity())) {
      const auto *archive=service_.storeForwardArchive();
      const auto *row=archive&&archive->size()?archive->recordAt(archive->size()-1):nullptr;
      if(row&&row->time&&row->from==from&&row->to==to&&row->id==id&&row->channel==channelIndex&&
         row->payloadSize==m.payloadLength&&!memcmp(row->payload,m.payload,m.payloadLength))return;
    }
#endif
    fresh(meshtastic_FromRadio_packet_tag);auto &out=output_.packet;
    out.from=from;out.to=to;out.id=id;out.channel=channelIndex;out.rx_snr=packetSnr;out.has_rx_rssi=snrKnown;out.rx_rssi=rssi;
    out.via_mqtt=(p[12]&0x10u)!=0;
    timestamp(out,receivedAt);
    out.hop_limit=p[12]&7;out.hop_start=(p[12]>>5)&7;out.relay_node=p[15];out.next_hop=p[14];
    out.which_payload_variant=meshtastic_MeshPacket_decoded_tag;auto &d=out.decoded;
    d.portnum=static_cast<meshtastic_PortNum>(m.portnum);d.payload.size=transformedResponse?traceSize:m.payloadLength;
    memcpy(d.payload.bytes,transformedResponse?tracePayload:m.payload,d.payload.size);d.want_response=m.wantResponse;
    d.request_id=applicationResponse?m.request_id:
      ((!service_.normalMode()||service_.matchedNodeInfo(from,to,m,now))?m.request_id:0);
    d.reply_id=m.reply_id;d.emoji=m.emoji;
    d.has_bitfield=m.hasBitfield;d.bitfield=m.bitfield;
    NodeUser::coerceId(d,from);
    if(verifiedEnvelope&&d.request_id==m.request_id&&d.payload.size==m.payloadLength&&
       !memcmp(d.payload.bytes,m.payload,m.payloadLength)){
      // Preserve the verified live envelope. Cached replay below has another ID
      // and deliberately carries no signature or signer-identity claim. A
      // transformed trace response also cannot borrow the original signature.
      out.xeddsa_signed=true;out.public_key.size=32;memcpy(out.public_key.bytes,signerKey,32);
      d.xeddsa_signature.size=64;memcpy(d.xeddsa_signature.bytes,m.signature,64);
    }
    if(emit())++receivedMessages;
    // Service owns generated over-air ACKs, subject to its runtime TX gate.
  }
  // Root broadcasts device-wide verification notifications after the single
  // shared protocol owner accepts an event. Each transport takes the common
  // workspace lease before touching its FromRadio scratch message.
  bool notifyKeyVerification(const meshtastic_ClientNotification &event,WorkspaceProof proof) {
    auto lease=workspace_.acquire(proof);
    if(!lease||!connected_||(owner_==0&&(recoveryOnly_||!host_.consoleAllowed()))||!host_.ready())return false;
    fresh(meshtastic_FromRadio_clientNotification_tag);
    output_.clientNotification=event;
    const bool sent=emit();
    volatile uint8_t *bytes=reinterpret_cast<volatile uint8_t *>(&output_);
    for(size_t i=0;i<sizeof(output_);++i)bytes[i]=0;
    return sent;
  }
};
}
