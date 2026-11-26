// SPDX-License-Identifier: GPL-3.0-only
// Production and compact implementations run identical public test vectors.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

static void hex(const uint8_t *bytes, size_t size, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    out[2 * i] = digits[bytes[i] >> 4];
    out[2 * i + 1] = digits[bytes[i] & 15];
  }
  out[2 * size] = '\0';
}

static void expect_hex(const uint8_t *bytes, size_t size, const char *expected) {
  char actual[129];
  assert(size <= 64);
  hex(bytes, size, actual);
  assert(strlen(expected) == size * 2);
  assert(strcmp(actual, expected) == 0);
}

static void emit(const char *kind, size_t digest_size, size_t message_size,
                 const uint8_t *digest) {
  char actual[129];
  hex(digest, digest_size, actual);
  printf("%s-%zu-%zu %s\n", kind, digest_size, message_size, actual);
}

static void load_vector(uint8_t *out, size_t n, const char *input) {
  assert(strlen(input) == 2 * n);
  for (size_t i = 0; i < n; ++i) {
    unsigned value;
    assert(sscanf(input + 2 * i, "%2x", &value) == 1);
    out[i] = (uint8_t)value;
  }
}

static void check_curves(void) {
  uint8_t scalar[32], peer[32], out[32], signing_key[64], signature[64];
  // RFC 7748 section 6.1, Alice's public key and the shared secret.
  load_vector(scalar, 32, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  crypto_x25519_public_key(out, scalar);
  expect_hex(out, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  load_vector(peer, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  crypto_x25519(out, scalar, peer);
  expect_hex(out, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  // RFC 8032 section 7.1, Ed25519 test 1.
  load_vector(scalar, 32, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  crypto_ed25519_key_pair(signing_key, out, scalar);
  expect_hex(out, 32, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  crypto_ed25519_sign(signature, signing_key, NULL, 0);
  expect_hex(signature, 64, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  assert(crypto_ed25519_check(signature, out, NULL, 0) == 0);
  signature[17] ^= 1;
  assert(crypto_ed25519_check(signature, out, NULL, 0) != 0);
}

static void check_sha512(void) {
  static const size_t lengths[] = {0, 1, 111, 112, 127, 128, 129, 255, 256, 257, 1024};
  uint8_t message[1024], digest[64], incremental[64];
  for (size_t i = 0; i < sizeof(message); ++i) message[i] = (uint8_t)(i * 73 + 19);
  for (size_t i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
    size_t n = lengths[i];
    crypto_sha512(digest, message, n);
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, message, n/2);
    crypto_sha512_update(&ctx, message + n/2, n - n/2);
    crypto_sha512_final(&ctx, incremental);
    assert(memcmp(incremental, digest, 64) == 0);
    emit("sha512", 64, n, digest);
  }
}

int main(void) {
  check_curves();
  check_sha512();
  uint8_t digest[64];
  static const uint8_t abc[] = {'a', 'b', 'c'};

  crypto_blake2b(digest, 64, NULL, 0);
  expect_hex(digest, 64,
             "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
             "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
  emit("kat-empty-64", 64, 0, digest);
  crypto_blake2b(digest, 64, abc, sizeof(abc));
  expect_hex(digest, 64,
             "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
             "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
  emit("kat-abc-64", 64, sizeof(abc), digest);
  crypto_blake2b(digest, 32, NULL, 0);
  expect_hex(digest, 32,
             "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
  emit("kat-empty-32", 32, 0, digest);
  crypto_blake2b(digest, 32, abc, sizeof(abc));
  expect_hex(digest, 32,
             "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
  emit("kat-abc-32", 32, sizeof(abc), digest);

  uint8_t message[1024];
  uint8_t key[32];
  for (size_t i = 0; i < sizeof(message); ++i)
    message[i] = (uint8_t)(i * 73u + 19u);
  for (size_t i = 0; i < sizeof(key); ++i)
    key[i] = (uint8_t)(i * 11u + 7u);

  static const size_t message_sizes[] = {0, 1, 127, 128, 129, 255, 256, 257, 1024};
  for (size_t digest_size = 1; digest_size <= 64; ++digest_size) {
    for (size_t j = 0; j < sizeof(message_sizes) / sizeof(message_sizes[0]); ++j) {
      const size_t message_size = message_sizes[j];
      uint8_t one_shot[64], keyed[64], streamed[64], keyed_streamed[64];
      crypto_blake2b(one_shot, digest_size, message, message_size);
      crypto_blake2b_keyed(keyed, digest_size, key, sizeof(key), message, message_size);

      crypto_blake2b_ctx ctx;
      crypto_blake2b_init(&ctx, digest_size);
      for (size_t offset = 0; offset < message_size;) {
        size_t chunk = (offset % 23u) + 1u;
        if (chunk > message_size - offset) chunk = message_size - offset;
        crypto_blake2b_update(&ctx, message + offset, chunk);
        offset += chunk;
      }
      crypto_blake2b_final(&ctx, streamed);
      assert(memcmp(one_shot, streamed, digest_size) == 0);

      crypto_blake2b_keyed_init(&ctx, digest_size, key, sizeof(key));
      for (size_t offset = 0; offset < message_size;) {
        size_t chunk = (offset % 31u) + 1u;
        if (chunk > message_size - offset) chunk = message_size - offset;
        crypto_blake2b_update(&ctx, message + offset, chunk);
        offset += chunk;
      }
      crypto_blake2b_final(&ctx, keyed_streamed);
      assert(memcmp(keyed, keyed_streamed, digest_size) == 0);

      emit("unkeyed", digest_size, message_size, one_shot);
      emit("unkeyed-stream", digest_size, message_size, streamed);
      emit("keyed", digest_size, message_size, keyed);
      emit("keyed-stream", digest_size, message_size, keyed_streamed);
    }
  }
  puts("PASS fixed BLAKE2b KATs, all output sizes, keyed/unkeyed streaming and block boundaries");
  return 0;
}
