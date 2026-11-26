// SPDX-License-Identifier: GPL-3.0-only
#include "M119TlsTestCa.h"
#if !defined(CONFIG_MT_M119_TLS_TEST)
#error "M119 test CA source must be excluded unless its explicit test Kconfig is enabled"
#endif
#include "MqttBootSnapshot.h"
#include <errno.h>
#include <stdint.h>
#include <zephyr/net/tls_credentials.h>
#include "M119TestCa.generated.h"
#ifndef MT_M119_TEST_CA_MAX_BYTES
#error "Missing Root-generated M119 public CA fixture contract"
#endif
static_assert(sizeof(kM119TestCaDer)>0 && sizeof(kM119TestCaDer)<=MT_M119_TEST_CA_MAX_BYTES,
              "M119 test CA DER size outside reviewed bound");
namespace mt::mqtt {
bool installM119TestCa() {
  // Refuse to mix the test CA with any pre-existing client cert/key or PSK
  // material sharing this security tag. M118 has no such provisioning path.
  constexpr tls_credential_type types[]={TLS_CREDENTIAL_CA_CERTIFICATE,
    TLS_CREDENTIAL_PUBLIC_CERTIFICATE,TLS_CREDENTIAL_PRIVATE_KEY,
    TLS_CREDENTIAL_PSK,TLS_CREDENTIAL_PSK_ID,TLS_CREDENTIAL_PRIVATE_KEY_PSA};
  uint8_t probe=0;
  for(const auto type:types) {
    size_t length=0;
    const int existing=tls_credential_get(static_cast<sec_tag_t>(BootSnapshot::SecurityTag),
                                          type,&probe,&length);
    if(existing!=-ENOENT||length!=0)return false;
  }
  return tls_credential_add(static_cast<sec_tag_t>(BootSnapshot::SecurityTag),
                            TLS_CREDENTIAL_CA_CERTIFICATE,kM119TestCaDer,sizeof(kM119TestCaDer))==0;
}
}
