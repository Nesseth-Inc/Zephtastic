uint8_t Default::getConfiguredOrDefaultHopLimit(uint8_t configured)
{
#if USERPREFS_EVENT_MODE
    return (configured >= eventModeHopLimit) ? eventModeHopLimit : config.lora.hop_limit;
#else
    return (configured >= HOP_MAX) ? HOP_MAX : config.lora.hop_limit;
#endif
}
