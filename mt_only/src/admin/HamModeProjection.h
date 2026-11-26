// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "meshtastic/admin.pb.h"

namespace mt::admin {

// Pure projection of the stock SET_HAM_MODE request. These are intents only:
// the caller owns licensed-mode policy, channel plaintext mutation, persistence,
// radio reconfiguration, and any platform-specific authorization.
class HamModeProjection {
 public:
  struct Capabilities {
    bool licensedModePath = false;
    bool plaintextChannelPath = false;
    int32_t minTxPower = 0;
    int32_t maxTxPower = 0;
    float minFrequencyMHz = 0.0f;
    float maxFrequencyMHz = 0.0f;
  };

  struct Result {
    char longName[40]{};
    char shortName[5]{};
    int32_t txPower = 0;
    float frequencyMHz = 0.0f;
    bool isLicensed = false;
    bool overrideDutyCycle = false;
    uint32_t nodeInfoBroadcastSeconds = 0;
    uint8_t rebroadcastMode = 0;
    bool disableTx = false;
    bool clearEncryptedChannelPsks = false;
    bool disableAdminChannel = false;
  };

  static constexpr uint8_t LocalOnlyRebroadcast = 2;
  static constexpr size_t StockLongNameBytes = 24;

 private:
  static bool terminated(const char *s, size_t capacity) {
    return s && capacity && memchr(s, '\0', capacity) != nullptr;
  }
  static bool blank(const char *s) {
    if (!s) return true;
    for (; *s; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (!(c == ' ' || (c >= '\t' && c <= '\r'))) return false;
    }
    return true;
  }
  static size_t boundedLength(const char *s, size_t capacity) {
    const char *end = static_cast<const char *>(memchr(s, '\0', capacity));
    return end ? static_cast<size_t>(end - s) : 0;
  }
 public:
  static void copyStockName(char *out, size_t outSize, const char *in,
                            size_t inCapacity, size_t contentLimit) {
    size_t n = boundedLength(in, inCapacity);
    if (n > contentLimit) n = contentLimit;
    if (n >= outSize) n = outSize - 1;
    if (n) memcpy(out, in, n);
    out[n] = '\0';
    // Match stock sanitizeUtf8 behavior: malformed or truncated UTF-8 bytes
    // become '?'. This is intentionally applied after the stock byte clamp.
    size_t i = 0;
    while (i < n) {
      const uint8_t lead = static_cast<uint8_t>(out[i]);
      size_t width = 0;
      uint32_t cp = 0, minimum = 0;
      if (lead <= 0x7f) { ++i; continue; }
      if ((lead & 0xe0) == 0xc0) { width = 2; cp = lead & 0x1f; minimum = 0x80; }
      else if ((lead & 0xf0) == 0xe0) { width = 3; cp = lead & 0x0f; minimum = 0x800; }
      else if ((lead & 0xf8) == 0xf0) { width = 4; cp = lead & 0x07; minimum = 0x10000; }
      else { out[i++] = '?'; continue; }
      if (i + width > n) {
        while (i < n) out[i++] = '?';
        break;
      }
      bool valid = true;
      for (size_t j = 1; j < width; ++j) {
        const uint8_t next = static_cast<uint8_t>(out[i + j]);
        if ((next & 0xc0) != 0x80) { valid = false; break; }
        cp = (cp << 6) | (next & 0x3f);
      }
      if (valid && (cp < minimum || cp > 0x10ffff ||
                    (cp >= 0xd800 && cp <= 0xdfff))) valid = false;
      if (valid) i += width;
      else out[i++] = '?';
    }
  }

 public:
  // `currentShortName` is the bounded current owner buffer. Blank
  // request.short_name preserves the current short name exactly as stock does.
  // Capability ranges must come from the target platform/region policy.
  static bool project(const meshtastic_HamParameters &request,
                      const char *currentShortName, size_t currentShortNameSize,
                      const Capabilities &capabilities, Result &out) {
    if (!terminated(request.call_sign, sizeof(request.call_sign)) ||
        !terminated(request.long_name, sizeof(request.long_name)) ||
        !terminated(request.short_name, sizeof(request.short_name)) ||
        !terminated(currentShortName, currentShortNameSize) ||
        !capabilities.licensedModePath || !capabilities.plaintextChannelPath ||
        request.tx_power < capabilities.minTxPower ||
        request.tx_power > capabilities.maxTxPower ||
        !isfinite(request.frequency) || !isfinite(capabilities.minFrequencyMHz) ||
        !isfinite(capabilities.maxFrequencyMHz) ||
        capabilities.minFrequencyMHz <= 0.0f ||
        capabilities.maxFrequencyMHz < capabilities.minFrequencyMHz ||
        (request.frequency != 0.0f &&
         (request.frequency < capabilities.minFrequencyMHz ||
          request.frequency > capabilities.maxFrequencyMHz)) || blank(request.call_sign))
      return false;

    Result projected{};
    char composed[sizeof(projected.longName)]{};
    if (!blank(request.long_name))
      (void)snprintf(composed, sizeof(composed), "%s//%s", request.call_sign,
                     request.long_name);
    else
      (void)snprintf(composed, sizeof(composed), "%s", request.call_sign);
    copyStockName(projected.longName, sizeof(projected.longName), composed,
                  sizeof(composed), StockLongNameBytes);
    if (!blank(request.short_name))
      copyStockName(projected.shortName, sizeof(projected.shortName),
                    request.short_name, sizeof(request.short_name),
                    sizeof(projected.shortName) - 1);
    else
      copyStockName(projected.shortName, sizeof(projected.shortName),
                    currentShortName, currentShortNameSize,
                    sizeof(projected.shortName) - 1);

    projected.txPower = request.tx_power;
    projected.frequencyMHz = request.frequency;
    projected.isLicensed = true;
    projected.overrideDutyCycle = true;
    projected.nodeInfoBroadcastSeconds = 600;
    projected.rebroadcastMode = LocalOnlyRebroadcast;
    projected.disableTx = strcmp(request.call_sign, "N0CALL") == 0;
    projected.clearEncryptedChannelPsks = true;
    projected.disableAdminChannel = true;
    out = projected;
    return true;
  }
};

}  // namespace mt::admin
