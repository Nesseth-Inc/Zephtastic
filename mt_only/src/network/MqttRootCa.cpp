// SPDX-License-Identifier: GPL-3.0-only
#include "MqttRootCa.h"
#include "MqttBootSnapshot.h"
#include <errno.h>
#include <zephyr/net/tls_credentials.h>
#include "mqtt_root_der.generated.h"
namespace mt::mqtt {
bool installMqttRootCa() {
  // An occupied tag can belong to another client. Never replace its entries
  // or silently accept a different CA under the verified MQTT policy.
  static constexpr tls_credential_type types[]={TLS_CREDENTIAL_CA_CERTIFICATE,
    TLS_CREDENTIAL_PUBLIC_CERTIFICATE,TLS_CREDENTIAL_PRIVATE_KEY,TLS_CREDENTIAL_PSK,
    TLS_CREDENTIAL_PSK_ID,TLS_CREDENTIAL_PRIVATE_KEY_PSA};
  const sec_tag_t tag=static_cast<sec_tag_t>(BootSnapshot::SecurityTag);
  uint8_t unused=0;
  for(auto type:types){
    size_t length=0;
    if(tls_credential_get(tag,type,&unused,&length)!=-ENOENT||length)return false;
  }
  // The Zephyr volatile credential backend retains a reference until reset.
  return tls_credential_add(tag,TLS_CREDENTIAL_CA_CERTIFICATE,mqttRootDer,sizeof(mqttRootDer))==0;
}
}
