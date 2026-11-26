CryptoKey Channels::getKey(ChannelIndex chIndex)
{
    meshtastic_Channel &ch = getByIndex(chIndex);
    const meshtastic_ChannelSettings &channelSettings = ch.settings;

    CryptoKey k;
    memset(k.bytes, 0, sizeof(k.bytes)); // In case the user provided a short key, we want to pad the rest with zeros

    if (!ch.has_settings || ch.role == meshtastic_Channel_Role_DISABLED) {
        k.length = -1; // invalid
    } else {
        memcpy(k.bytes, channelSettings.psk.bytes, channelSettings.psk.size);
        k.length = channelSettings.psk.size;
        if (k.length == 0) {
            // A secondary with no PSK borrows the primary's key; the chIndex != primaryIndex check
            // prevents infinite recursion if the primary slot itself is marked SECONDARY
            if (ch.role == meshtastic_Channel_Role_SECONDARY && chIndex != primaryIndex) {
                LOG_DEBUG("Unset PSK for secondary channel %s. use primary key", ch.settings.name);
                k = getKey(primaryIndex);
            } else {
                LOG_WARN("User disabled encryption");
            }
        } else if (k.length == 1) {
            // Convert the short single byte variants of psk into variant that can be used more generally

            uint8_t pskIndex = k.bytes[0];
            LOG_DEBUG("Expand short PSK #%d", pskIndex);
            if (pskIndex == 0)
                k.length = 0; // Turn off encryption
            else {
                memcpy(k.bytes, defaultpsk, sizeof(defaultpsk));
                k.length = sizeof(defaultpsk);
                // Bump up the last byte of PSK as needed
                uint8_t *last = k.bytes + sizeof(defaultpsk) - 1;
                *last = *last + pskIndex - 1; // index of 1 means no change vs defaultPSK
            }
        } else if (k.length < 16) {
            // Error! The user specified only the first few bits of an AES128 key.  So by convention we just pad the rest of the
            // key with zeros
            LOG_WARN("User provided a too short AES128 key - padding");
            k.length = 16;
        } else if (k.length < 32 && k.length != 16) {
            // Error! The user specified only the first few bits of an AES256 key.  So by convention we just pad the rest of the
            // key with zeros
            LOG_WARN("User provided a too short AES256 key - padding");
            k.length = 32;
        }
    }

    return k;
}

