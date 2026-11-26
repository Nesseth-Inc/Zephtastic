/* Scratch host test only: replace this translation unit's wrappers, never the global allocator. */
#include "tf_psa_crypto_common.h"
#include <mbedtls/platform.h>
#include <stddef.h>
void *zt_probe_calloc(size_t,size_t);
void zt_probe_free(void *);
#undef mbedtls_calloc
#undef mbedtls_free
#define mbedtls_calloc zt_probe_calloc
#define mbedtls_free zt_probe_free
