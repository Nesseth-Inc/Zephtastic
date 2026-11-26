// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "HotNodeMetadataJournal.h"

#include <string.h>

namespace mt { namespace mqtt {

enum class HotNodeMetadataStartupState : uint8_t {
    Unavailable, PublicRamOnly, JournalLoaded, JournalRecovered,
    JournalInitialized, JournalInitialWriteUncertain
};
struct HotNodeMetadataStartupResult {
    HotNodeMetadataStartupState state=HotNodeMetadataStartupState::Unavailable;
    bool countAvailable() const { return state!=HotNodeMetadataStartupState::Unavailable; }
};

// Runs only during serialized boot. A private profile may persist metadata only
// when PkiStorage exposes the already-prepared identity binding for this node;
// this public snapshot does not expose a private-key handle. Empty/public-only
// profiles get a coherent RAM window but never touch the metadata journal.
template<class Storage, class Binding, class Seed>
HotNodeMetadataStartupResult startHotNodeMetadata(
    Storage &storage, bool privatePrepared, bool privateEmpty,
    uint32_t self, Binding &binding, uint8_t publicKey[32],
    HotNodeMetadataJournal &journal, SettingsBackend &settings,
    OnlineNodeWindow &window, Seed seed) {
    binding=Binding{};
    memset(publicKey,0,32);
    if(!self||privatePrepared==privateEmpty) return {};

    if(privateEmpty) {
        if(!seed(window,self)) return {};
        return {HotNodeMetadataStartupState::PublicRamOnly};
    }

    if(!storage.preparedBinding(binding)||binding.node!=self) {
        binding=Binding{};
        return {};
    }
    memcpy(publicKey,binding.publicKey,32);
    if(journal.load(settings,window,self,publicKey)!=0) {
        binding=Binding{};
        memset(publicKey,0,32);
        return {};
    }
    if(journal.state()==HotNodeMetadataJournal::State::Empty) {
        if(!seed(window,self)) return {};
        if(journal.save(settings,window,self,publicKey)!=0)
            return {HotNodeMetadataStartupState::JournalInitialWriteUncertain};
        return {HotNodeMetadataStartupState::JournalInitialized};
    }
    if(journal.state()==HotNodeMetadataJournal::State::Loaded)
        return {HotNodeMetadataStartupState::JournalLoaded};
    if(journal.state()==HotNodeMetadataJournal::State::Recovered)
        return {HotNodeMetadataStartupState::JournalRecovered};
    return {};
}

}} // namespace mt::mqtt
