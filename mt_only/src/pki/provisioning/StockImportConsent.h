// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FreshDomain.h"
#include <stdio.h>

namespace mt::pki {

template<class Session> class StockImportConsent {
public:
  static constexpr char Prefix[] = "zt_pki_import_v1 ";
  static constexpr uint64_t LifetimeMs = 90000;

  StockImportConsent(PkiFreshProvisioning &core, Session &usb, EntropySource &entropy)
      : core_(core), usb_(usb), entropy_(entropy) {}
  ~StockImportConsent() { eraseIdentityBytes(challenge_, sizeof(challenge_)); }
  StockImportConsent(const StockImportConsent &) = delete;
  StockImportConsent &operator=(const StockImportConsent &) = delete;

  void cancel() {
    armed_ = false;
    staged_ = false;
    epoch_ = 0;
    bleEpoch_ = 0;
    resumeOnly_ = false;
    started_ = 0;
    candidate_ = {};
    eraseIdentityBytes(challenge_, sizeof(challenge_));
    (void)core_.clearImportSession();
    usb_.clear();
  }

  void tick() { if (armed_ && !live()) cancel(); }
  bool active() { tick(); return armed_; }

  int stage(const uint8_t scalar[32], const uint8_t pub[32], bool usbPacket,
            uint32_t frameEpoch) {
    tick();
    if (!armed_ || resumeOnly_ || (usbPacket && frameEpoch != epoch_)) return -EACCES;
    if (staged_) return -EBUSY;
    if (!usbPacket) {
      if (!frameEpoch || !usb_.currentImportBle(frameEpoch)) return -EACCES;
      bleEpoch_ = frameEpoch;
    }

    // No storage write precedes the independent USB confirmation. A second
    // setter cannot replace a candidate already presented to the USB owner.
    const int rc = core_.stageImported(scalar, pub, candidate_);
    if (rc || !live()) {
      cancel();
      return rc ? rc : -EACCES;
    }
    staged_ = true;
    return 0;
  }

  int command(const uint8_t *input, size_t size, uint32_t epoch, char *out,
              size_t capacity, bool canArm, bool eligibleBle, bool held = false) {
    constexpr size_t prefixSize = sizeof(Prefix) - 1;
    if (!input || !out || capacity < 175 || size < prefixSize ||
        memcmp(input, Prefix, prefixSize)) return -EINVAL;

    input += prefixSize;
    size -= prefixSize;
    tick();
    if (size == 12 && !memcmp(input, "capabilities", size)) {
      if (!held || !canArm || !usb_.current(epoch)) return -EACCES;
      return printed(snprintf(out, capacity, "pki_import_v1 profile=mt-zephyr-xiao held=1"), capacity);
    }
    if (size == 3 && !memcmp(input, "arm", size)) return arm(epoch, canArm, held, out, capacity);
    if (!armed_ || epoch != epoch_ || !usb_.current(epoch)) {
      cancel();
      return -EACCES;
    }
    if (size == 6 && !memcmp(input, "cancel", size)) {
      cancel();
      return printed(snprintf(out, capacity, "pki_import_v1 rc=0 armed=0"), capacity);
    }
    if (size == 6 && !memcmp(input, "status", size)) {
      char pub[65]{};
      if (staged_) hex(candidate_.publicKey, 32, pub);
      return printed(snprintf(out, capacity,
          "pki_import_v1 rc=0 armed=1 staged=%u node=%08x pub=%s ble_eligible=%u",
          unsigned(staged_), unsigned(candidate_.node), pub, unsigned(eligibleBle)), capacity);
    }
    if (held != resumeOnly_) { cancel(); return -EACCES; }
    return confirm(input, size, out, capacity);
  }

private:
  PkiFreshProvisioning &core_;
  Session &usb_;
  EntropySource &entropy_;
  uint32_t epoch_ = 0;
  uint32_t bleEpoch_ = 0;
  uint64_t started_ = 0;
  bool armed_ = false;
  bool staged_ = false;
  bool resumeOnly_ = false;
  uint8_t challenge_[16]{};
  IdentityBinding candidate_{};

  bool live() const {
    return armed_ && usb_.current(epoch_) && usb_.millis() - started_ < LifetimeMs &&
           (!bleEpoch_ || usb_.currentImportBle(bleEpoch_));
  }

  int arm(uint32_t epoch, bool allowed, bool held, char *out, size_t capacity) {
    if (!allowed || armed_ || !core_.clearImportSession() || !usb_.bind(epoch)) return -EACCES;
    if (held) {
      const int rc = core_.stageSavedImport(candidate_);
      if (rc || !usb_.prepareHeldImportEntropy()) { cancel(); return rc ? rc : -EIO; }
      staged_ = true;
      resumeOnly_ = true;
    }
    if (!entropy_.fill(challenge_, sizeof(challenge_)) || !ImportedDomain::hasSalt(challenge_) ||
        !usb_.current(epoch)) {
      cancel();
      return -EIO;
    }
    started_ = usb_.millis();
    if (started_ > UINT64_MAX - LifetimeMs) {
      cancel();
      return -EIO;
    }
    usb_.limitUntil(started_ + LifetimeMs);
    epoch_ = epoch;
    armed_ = true;

    // S3 Serial/JTAG does not observe every unplug. Only this response gives
    // the USB owner the capability to confirm; a later status read omits it.
    char token[33];
    hex(challenge_, sizeof(challenge_), token);
    const int rc = printed(snprintf(out, capacity, "pki_import_v1 rc=0 armed=1 challenge=%s", token), capacity);
    eraseIdentityBytes(token, sizeof(token));
    if (rc < 0) cancel();
    return rc;
  }

  int confirm(const uint8_t *input, size_t size, char *out, size_t capacity) {
    const char *verb = resumeOnly_ ? "resume " : "confirm ";
    const size_t nodeOffset = strlen(verb);
    const size_t publicOffset = nodeOffset + 8 + 1;
    const size_t proofOffset = publicOffset + 64 + 1;
    if (size != proofOffset + 32 || memcmp(input, verb, nodeOffset) ||
        input[publicOffset - 1] != ' ' || input[proofOffset - 1] != ' ') return -EINVAL;

    uint8_t node[4]{}, pub[32]{}, proof[16]{};
    const bool parsed = decode(input + nodeOffset, node, sizeof(node)) &&
                        decode(input + publicOffset, pub, sizeof(pub)) &&
                        decode(input + proofOffset, proof, sizeof(proof));
    const uint32_t expectedNode = uint32_t(node[0]) << 24 | uint32_t(node[1]) << 16 |
                                  uint32_t(node[2]) << 8 | node[3];
    const bool match = parsed && staged_ && expectedNode == candidate_.node &&
                       crypto_verify32(candidate_.publicKey, pub) == 0 &&
                       crypto_verify16(challenge_, proof) == 0;
    eraseIdentityBytes(proof, sizeof(proof));
    if (!match) {
      cancel();
      return -EACCES;
    }
    const int rc = core_.confirm(candidate_);
    const bool current = live();
    cancel();
    if (rc || !current) return rc ? rc : -EACCES;
    return printed(snprintf(out, capacity, "pki_import_v1 rc=0 committed=1 reboot=1"), capacity);
  }

  static int nibble(uint8_t value) {
    if (value >= '0' && value <= '9') return value - '0';
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
  }
  static bool decode(const uint8_t *text, uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      const int high = nibble(text[2 * i]), low = nibble(text[2 * i + 1]);
      if (high < 0 || low < 0) return false;
      bytes[i] = uint8_t(16 * high + low);
    }
    return true;
  }
  static void hex(const uint8_t *bytes, size_t count, char *out) {
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
      out[2 * i] = digits[bytes[i] >> 4];
      out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * count] = 0;
  }
  static int printed(int size, size_t capacity) {
    return size > 0 && size_t(size) < capacity ? size : -ENOSPC;
  }
};

} // namespace mt::pki
