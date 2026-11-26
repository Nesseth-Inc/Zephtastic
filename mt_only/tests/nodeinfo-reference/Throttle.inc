bool Throttle::isWithinTimespanMs(uint32_t lastExecutionMs, uint32_t timeSpanMs)
{
    uint32_t now = Time::getMillis();
    return (now - lastExecutionMs) < timeSpanMs;
}
