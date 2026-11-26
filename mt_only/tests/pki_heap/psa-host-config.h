// SPDX-License-Identifier: GPL-3.0-only
// Host differential tests only. No key generation/storage; external RNG always
// fails, so this test backend cannot accidentally manufacture credentials.
#pragma once
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CLIENT
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_ALG_HMAC 1
#define PSA_WANT_ALG_CCM 1
#define PSA_WANT_KEY_TYPE_AES 1
#define PSA_WANT_KEY_TYPE_HMAC 1

// Scratch target-branch model: CMAC enables generic CIPHER_C like BLE target.
#define PSA_WANT_ALG_CMAC 1
#define MBEDTLS_PSA_STATIC_KEY_SLOTS
#define MBEDTLS_PSA_KEY_SLOT_COUNT 16
#define MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE 256
#define MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
