// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d src/mqtt/MQTT.cpp, MQTT::applyRegionRootTopic.
#pragma once
#include <stddef.h>
#include <string.h>
namespace mt::stock::MQTT {
template<class Result>
Result applyRegionRootTopic(bool isDefaultServer, bool isDefaultRootTopic,
                            const char *root, size_t rootLength,
                            const char *regionName, size_t regionLength,
                            char *output, size_t outputCapacity, size_t maximumRootLength)
{
    if (!isDefaultServer)
        return Result::Unchanged;
    if (!isDefaultRootTopic)
        return Result::Unchanged;
    // ZT settings views may lack a trailing NUL. Stage only changed roots
    // and let the settings owner decide when to persist a change.
    constexpr size_t prefixLength = sizeof("msh/") - 1;
    const size_t length = prefixLength + regionLength;
    if (length > maximumRootLength || length >= outputCapacity)
        return Result::Invalid;
    if (length == rootLength && !memcmp(root, "msh/", prefixLength) &&
        !memcmp(root + prefixLength, regionName, regionLength))
        return Result::Unchanged;
    memcpy(output, "msh/", prefixLength);
    memcpy(output + prefixLength, regionName, regionLength);
    output[length] = '\0';
    return Result::Changed;
}
}
