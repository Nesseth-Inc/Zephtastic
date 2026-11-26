int16_t Channels::generateHash(ChannelIndex channelNum)
{
    auto k = getKey(channelNum);
    if (k.length < 0)
        return -1; // invalid
    else {
        const char *name = getName(channelNum);
        uint8_t h = xorHash((const uint8_t *)name, strlen(name));

        h ^= xorHash(k.bytes, k.length);

        // Differentiate AEAD channels in routing so AEAD and non-AEAD
        // channels with the same PSK have different hashes
        auto &ch = getByIndex(channelNum);
        if (ch.has_settings && ch.settings.use_aead)
            h ^= 0xAE;

        return h;
    }
}

