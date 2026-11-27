// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Inject independent time, per-source storage and channel precision into the
// unchanged stock calculations. Each scenario owns a new module instance.
namespace donor {
#include "defaults.inc"
using NodeNum = uint32_t;
using ChannelIndex = uint8_t;
struct DisplayFormatters {
    static const char *getModemPresetDisplayName(meshtastic_Config_LoRaConfig_ModemPreset, bool, bool);
};
struct Channels {
    meshtastic_Channel table[8]{};
    meshtastic_Channel &getByIndex(unsigned i) { return table[i]; }
    const char *getName(size_t);
    bool isWellKnownChannel(ChannelIndex);
    bool isEventChannel(ChannelIndex) const { return false; }
    bool usesPublicKey(ChannelIndex) const { return true; } // All case channels expand to a public key.
};
static Channels channels;
static struct { meshtastic_Config_LoRaConfig lora{}; } config;
uint32_t getPositionPrecisionForChannel(uint8_t channelIndex);
namespace concurrency { struct LockGuard { explicit LockGuard(int *) {} }; }
struct UnifiedCacheEntry {
    uint8_t pos_fingerprint=0, pos_time=0, rate_count=0, rate_time=0;
    uint8_t getRateCount() const { return rate_count; }
    uint8_t getRateTime() const { return rate_time; }
    void setRateCount(uint8_t count) { rate_count=count; }
    void setRateTime(uint8_t time) { rate_time=time; }
};
struct TrafficManagementModule {
    struct { meshtastic_ModuleConfig_TrafficManagementConfig traffic_management{}; } moduleConfig;
    int cacheLock=0;
    static inline uint32_t now=0;
    static uint32_t clockMs() { return now; }
    #include "ticks.inc"
    std::map<NodeNum,UnifiedCacheEntry> entries;
    UnifiedCacheEntry *findOrCreateEntry(NodeNum from, bool *isNew) {
        auto [entry, fresh] = entries.try_emplace(from);
        *isNew=fresh;
        return &entry->second;
    }
    meshtastic_Config_DeviceConfig_Role resolveSenderRole(NodeNum, UnifiedCacheEntry *, bool) {
        return meshtastic_Config_DeviceConfig_Role_CLIENT;
    }
    static uint8_t computePositionFingerprint(int32_t, int32_t, uint8_t);
    bool shouldDropPosition(const meshtastic_MeshPacket *, const meshtastic_Position *, uint32_t);
    bool isRateLimited(NodeNum, uint32_t);
};
#define TRAFFIC_MANAGEMENT_CACHE_SIZE 16
#define TM_LOG_DEBUG(...) ((void)0)
#define TM_LOG_TRACE(...) ((void)0)
#define PRESET(name) meshtastic_Config_LoRaConfig_ModemPreset_##name
#include "donor.inc"
#undef PRESET
#undef TM_LOG_TRACE
#undef TM_LOG_DEBUG
#undef TRAFFIC_MANAGEMENT_CACHE_SIZE
}
