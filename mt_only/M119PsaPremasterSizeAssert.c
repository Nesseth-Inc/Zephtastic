#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#ifndef PSA_RAW_KEY_AGREEMENT_OUTPUT_MAX_SIZE
#error "PSA raw agreement output bound is unavailable"
#endif

_Static_assert(sizeof(union mbedtls_ssl_premaster_secret) >=
                   PSA_RAW_KEY_AGREEMENT_OUTPUT_MAX_SIZE,
               "TLS premaster storage must fit the configured PSA raw agreement output");
