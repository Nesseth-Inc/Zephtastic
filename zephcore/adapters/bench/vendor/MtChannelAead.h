// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Bounded Meshtastic channel AES-CCM adapter. The CCM block construction
// follows the pinned upstream src/mesh/aes-ccm.cpp implementation (see
// docs/consolidation/M187-CHANNEL-AEAD-CRYPTO.md). AES itself is supplied by
// the caller through the existing AES-ECB-via-CTR block callback; no cipher is
// implemented here.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bench {

#if defined(__GNUC__) || defined(__clang__)
#define MT_CHANNEL_AEAD_NOINLINE __attribute__((noinline))
#else
#define MT_CHANNEL_AEAD_NOINLINE
#endif

enum class ChannelAeadResult : uint8_t {
  Success,
  AuthenticationFailed,
  BackendFailed,
};

class ChannelAead {
public:
  static constexpr size_t TagSize = 12;
  static constexpr size_t MaxPlain = 227;
  static constexpr size_t MaxWire = MaxPlain + TagSize;
  using BlockCipher = bool (*)(void *context, const uint8_t *key, size_t keyLen,
                               const uint8_t nonce[16], uint8_t *bytes, size_t len);

private:
  static void wipe(void *ptr, size_t size) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(ptr);
    while (size--) *p++ = 0;
  }
  static void write32le(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
  }
  static void write16be(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value >> 8); p[1] = uint8_t(value);
  }
  // MtAesCtr's firmware/host implementations expose AES-ECB by applying one
  // CTR block to zeros. The supplied nonce is the complete AES input block.
  static MT_CHANNEL_AEAD_NOINLINE bool aesBlock(BlockCipher cipher, void *context, const uint8_t *key,
                       size_t keyLen, const uint8_t block[16], uint8_t out[16]) {
    uint8_t encrypted[16]{};
    const bool ok = cipher && cipher(context, key, keyLen, block, encrypted, sizeof(encrypted));
    if (ok) memcpy(out, encrypted, sizeof(encrypted));
    wipe(encrypted, sizeof(encrypted));
    return ok;
  }
  static bool macStep(BlockCipher cipher, void *context, const uint8_t *key,
                      size_t keyLen, uint8_t x[16], const uint8_t block[16]) {
    uint8_t mixed[16];
    for (size_t i = 0; i < sizeof(mixed); ++i) mixed[i] = uint8_t(x[i] ^ block[i]);
    const bool ok = aesBlock(cipher, context, key, keyLen, mixed, x);
    wipe(mixed, sizeof(mixed));
    return ok;
  }
  static MT_CHANNEL_AEAD_NOINLINE bool computeMac(BlockCipher cipher, void *context, const uint8_t *key,
                         size_t keyLen, const uint8_t nonce[16], const uint8_t aad[8],
                         const uint8_t *plain, size_t plainLen, uint8_t mac[16]) {
    uint8_t block[16]{};
    // CCM parameters from stock: L=2, 12-byte tag (M'=5), 13-byte nonce.
    block[0] = uint8_t(0x40u | (((TagSize - 2) / 2) << 3) | 1u);
    memcpy(block + 1, nonce, 13);
    write16be(block + 14, uint16_t(plainLen));
    bool ok = aesBlock(cipher, context, key, keyLen, block, mac);
    if (!ok) { wipe(block, sizeof(block)); return false; }

    // Stock AAD is exactly two little-endian node IDs (8 bytes); CCM encodes
    // its length as a two-byte big-endian prefix and zero pads the block.
    memset(block, 0, sizeof(block));
    write16be(block, 8);
    memcpy(block + 2, aad, 8);
    ok = macStep(cipher, context, key, keyLen, mac, block);
    for (size_t offset = 0; ok && offset < plainLen; offset += sizeof(block)) {
      memset(block, 0, sizeof(block));
      const size_t count = plainLen - offset < sizeof(block) ? plainLen - offset : sizeof(block);
      memcpy(block, plain + offset, count);
      ok = macStep(cipher, context, key, keyLen, mac, block);
    }
    wipe(block, sizeof(block));
    return ok;
  }
  static MT_CHANNEL_AEAD_NOINLINE bool cryptPayload(BlockCipher cipher, void *context, const uint8_t *key,
                           size_t keyLen, const uint8_t nonce[16], const uint8_t *input,
                           size_t size, uint8_t *output) {
    uint8_t counter[16]{}, stream[16]{};
    counter[0] = 1; // CCM A_i flags for L=2.
    memcpy(counter + 1, nonce, 13);
    bool ok = true;
    size_t offset = 0;
    for (uint16_t index = 1; ok && offset < size; ++index) {
      write16be(counter + 14, index);
      ok = aesBlock(cipher, context, key, keyLen, counter, stream);
      const size_t count = size - offset < sizeof(stream) ? size - offset : sizeof(stream);
      if (ok) for (size_t i = 0; i < count; ++i) output[offset + i] = uint8_t(input[offset + i] ^ stream[i]);
      offset += count;
    }
    wipe(counter, sizeof(counter)); wipe(stream, sizeof(stream));
    return ok;
  }
  static MT_CHANNEL_AEAD_NOINLINE bool makeTag(BlockCipher cipher, void *context, const uint8_t *key,
                      size_t keyLen, const uint8_t nonce[16], const uint8_t mac[16],
                      uint8_t tag[TagSize]) {
    uint8_t counter[16]{}, mask[16]{};
    counter[0] = 1; memcpy(counter + 1, nonce, 13); // A_0
    const bool ok = aesBlock(cipher, context, key, keyLen, counter, mask);
    if (ok) for (size_t i = 0; i < TagSize; ++i) tag[i] = uint8_t(mac[i] ^ mask[i]);
    wipe(counter, sizeof(counter)); wipe(mask, sizeof(mask));
    return ok;
  }
  static bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t size) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < size; ++i) diff = uint8_t(diff | (a[i] ^ b[i]));
    return diff == 0;
  }
  static void makeContext(uint32_t from, uint32_t to, uint32_t id,
                          uint8_t nonce[16], uint8_t aad[8]) {
    memset(nonce, 0, 16); write32le(nonce, id); write32le(nonce + 8, from);
    write32le(aad, from); write32le(aad + 4, to);
  }

public:
  // This layer reproduces stock CCM for the caller's exact header fields and
  // does not impose node-ID/packet-ID admission; frame owners validate those.
  // Encrypt nonempty plaintext bytes and append the stock 12-byte CCM tag.
  // `inputLen` is plaintext length; `outputLen` is ciphertext-plus-tag length.
  // Exact and partial input/output overlap are safe because plaintext is
  // snapshotted.
  static MT_CHANNEL_AEAD_NOINLINE ChannelAeadResult encrypt(BlockCipher cipher, void *context,
      const uint8_t *key, size_t keyLen, uint32_t from, uint32_t to, uint32_t id,
      const uint8_t *input, size_t inputLen, uint8_t *output, size_t outputCapacity,
      size_t &outputLen) {
    outputLen = 0;
    if (!cipher || !key || (keyLen != 16 && keyLen != 32) ||
        !input || !inputLen || inputLen > MaxPlain || !output ||
        outputCapacity < inputLen + TagSize) return ChannelAeadResult::BackendFailed;

    uint8_t plain[MaxPlain]{}, nonce[16]{}, aad[8]{}, mac[16]{}, tag[TagSize]{};
    memcpy(plain, input, inputLen);
    makeContext(from, to, id, nonce, aad);
    bool ok = computeMac(cipher, context, key, keyLen, nonce, aad, plain, inputLen, mac) &&
              cryptPayload(cipher, context, key, keyLen, nonce, plain, inputLen, output) &&
              makeTag(cipher, context, key, keyLen, nonce, mac, tag);
    if (ok) {
      memcpy(output + inputLen, tag, TagSize);
      outputLen = inputLen + TagSize;
    } else {
      wipe(output, inputLen + TagSize);
    }
    wipe(plain, sizeof(plain)); wipe(nonce, sizeof(nonce)); wipe(aad, sizeof(aad));
    wipe(mac, sizeof(mac)); wipe(tag, sizeof(tag));
    return ok ? ChannelAeadResult::Success : ChannelAeadResult::BackendFailed;
  }

  // Decrypt one ciphertext-plus-tag wire value. `inputLen` includes the
  // appended 12-byte tag. Plaintext is copied to output only after the
  // constant-time tag check succeeds. After valid key/endpoint/output
  // arguments, truncated and tag-only inputs are AuthenticationFailed (never
  // a CTR fallback signal). Invalid arguments leave output untouched; a
  // backend or tag failure wipes exactly the bounded expected plaintext span.
  static MT_CHANNEL_AEAD_NOINLINE ChannelAeadResult decrypt(BlockCipher cipher, void *context,
      const uint8_t *key, size_t keyLen, uint32_t from, uint32_t to, uint32_t id,
      const uint8_t *input, size_t inputLen, uint8_t *output, size_t outputCapacity,
      size_t &outputLen) {
    outputLen = 0;
    if (!cipher || !key || (keyLen != 16 && keyLen != 32) ||
        !output || (inputLen && !input) || inputLen > MaxWire) return ChannelAeadResult::BackendFailed;
    if (inputLen <= TagSize) return ChannelAeadResult::AuthenticationFailed;

    const size_t plainLen = inputLen - TagSize;
    if (outputCapacity < plainLen) return ChannelAeadResult::BackendFailed;
    uint8_t plain[MaxPlain]{}, nonce[16]{}, aad[8]{}, mac[16]{}, expected[TagSize]{};
    makeContext(from, to, id, nonce, aad);
    bool ok = cryptPayload(cipher, context, key, keyLen, nonce, input, plainLen, plain);
    if (ok) ok = computeMac(cipher, context, key, keyLen, nonce, aad, plain, plainLen, mac);
    if (ok) ok = makeTag(cipher, context, key, keyLen, nonce, mac, expected);
    const bool authenticated = ok && constantTimeEqual(expected, input + plainLen, TagSize);
    if (authenticated) {
      memmove(output, plain, plainLen);
      outputLen = plainLen;
    } else {
      wipe(output, plainLen);
    }
    wipe(plain, sizeof(plain)); wipe(nonce, sizeof(nonce)); wipe(aad, sizeof(aad));
    wipe(mac, sizeof(mac)); wipe(expected, sizeof(expected));
    if (!ok) return ChannelAeadResult::BackendFailed;
    return authenticated ? ChannelAeadResult::Success : ChannelAeadResult::AuthenticationFailed;
  }
};

#undef MT_CHANNEL_AEAD_NOINLINE

} // namespace bench
