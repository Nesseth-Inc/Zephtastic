// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Service.h"
#include "GnssView.h"
#include "Model.h"
#include "stock/message/TextMessageModule.h"
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
namespace mt::update { class FirmwareUpdateController; }
#endif
namespace mt::ui {
void init(const Config &config=Config{});
bool ready();
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
void showFirmwareUpdate(const update::FirmwareUpdateController &owner);
#endif
#if defined(CONFIG_MT_BASEUI_GAMES)
stock::ui::GamesModule &localGames(); // Accessed by the settings owner under the UI/service lock.
#endif
// Main owner publishes measured snapshots; missing/stale values remain absent.
#if defined(CONFIG_MT_TELEMETRY_REPORT_FRAMES)
// Accessed while the common service/display lock is held.
ReportFrames &telemetryReports();
#endif
void telemetry(const TelemetrySnapshot &snapshot,uint64_t nowMs);
// Main owner snapshots selection before taking Service ownership, projects it
// through CompletionTelemetry under the Service lock, then publishes afterward.
bool selectedPeerForTelemetry(SelectedPeerForTelemetry &out);
void peerTelemetry(const PeerTelemetryView &snapshot);
// Main/service-owner only. Ephemeral random PIN, never a log or stored record.
void pairing(bool active,uint32_t passkey,uint32_t now);
int input(uint32_t eventCode,uint32_t now);
// Main/service-owner only. Route candidates are projected from the node cache;
// no radio or companion queues are consumed by updating the display model.
void setCannedRoutes(uint8_t validChannelMask,uint32_t configurationGeneration);
void setCannedContent(const char *stableList,bool ready,bool enabled);
// Legacy combined bridge remains available to existing callers.
void setCannedMessages(const char *stableList,bool ready,bool enabled,uint32_t destination=0xffffffffu,uint8_t channel=0);
bool takePendingCannedSend(PendingCannedSend &out);
void reportCannedResult(int result);
void reportCannedCompletion(bool relayed,bool success);
void preview(const Service &service,uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *payload,size_t n,int16_t rssi,int8_t snr,uint32_t now,bool local,const stock::text::Settings &settings);
void showGeofence(uint32_t from,const char *name,bool entered,uint32_t now);
void showWaypoint(const char *name,uint32_t now);
void clearWaypoint();
void setSystemCommandOwner(int (*run)(stock::ui::SystemCommandsModule::Action,uint32_t),bool deviceReset=false);
void setButtonConsumer(bool (*consume)());
#if defined(CONFIG_MT_LOCAL_CLOCK) && defined(CONFIG_MT_XIAO_EXPANSION_OLED)
void setClockUtc(uint32_t utc);
#endif
void poll(const Service &service,bool radioReady,bool bleReady,bool bleConnected,bool usbConnected,uint32_t now,bool settingsReady=true,const GnssView &gnss={});
}
