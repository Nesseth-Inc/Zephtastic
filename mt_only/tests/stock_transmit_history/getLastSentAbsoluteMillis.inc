uint32_t TransmitHistory::getLastSentAbsoluteMillis(uint32_t storedEpoch) const
{
    uint32_t now = getTime();
    if (now < 2) {
        return 0;
    }

    if (storedEpoch > now) {
        return 0;
    }

    uint32_t secondsAgo = now - storedEpoch;
    uint32_t msAgo = secondsAgo * 1000;

    if (secondsAgo > 86400 || msAgo / 1000 != secondsAgo) {
        return 0;
    }

    return Time::skipZero(Time::getMillis() - msAgo);
}
