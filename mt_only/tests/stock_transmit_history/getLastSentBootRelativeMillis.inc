uint32_t TransmitHistory::getLastSentBootRelativeMillis(uint32_t storedSeconds) const
{
    if (getRTCQuality() != RTCQualityNone) {
        return 0;
    }

    uint32_t now = getTime();

    if (storedSeconds <= now) {
        uint32_t secondsAgo = now - storedSeconds;
        if (secondsAgo > BOOT_RELATIVE_RECOVERY_WINDOW_SEC) {
            return 0;
        }
        return Time::skipZero(Time::getMillis() - (secondsAgo * 1000));
    }

    uint32_t secondsAhead = storedSeconds - now;
    if (secondsAhead > BOOT_RELATIVE_RECOVERY_WINDOW_SEC) {
        return 0;
    }

    return Time::skipZero(Time::getMillis());
}
