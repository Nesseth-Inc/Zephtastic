uint8_t RoutingModule::getHopLimitForResponse(const meshtastic_MeshPacket &mp)
{
    const int8_t hopsUsed = getHopsAway(mp);
    const uint8_t responseHopLimit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    if (hopsUsed >= 0) {
        if (hopsUsed > static_cast<int32_t>(responseHopLimit)) {
// In event mode, never exceed the configured event hop limit.
#if !USERPREFS_EVENT_MODE    // This falls through to the default.
            return hopsUsed; // If the request used more hops than the limit, use the same amount of hops
#endif
        } else if (mp.hop_start == 0) {
            return 0; // The requesting node wanted 0 hops, so the response also uses a direct/local path.
        } else if (static_cast<uint8_t>(hopsUsed + 2) < responseHopLimit) {
            return hopsUsed + 2; // Use only the amount of hops needed with some margin as the way back may be different
        }
    }
    return responseHopLimit;
}
