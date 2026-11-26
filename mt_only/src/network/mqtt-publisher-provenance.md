# MQTT transport adaptation provenance

The bounded publisher state-machine interface was informed by the public MIT
ZephCore donor at
`adapters/mqtt/ZephyrMQTTPublisher.h`
and `.c`. Their SHA-256 hashes at adaptation time:

- Header: `e24c93077d3a1e7917b394d819b3cf8ca7e6223d4b9ab11df0d6536cc108f3e3`
- Source: `38fa19da551c55bc2280e41ff1d055bda57733d1baef6e2b004db390e92a4785`

The donor's Zephyr thread, global credential include, hidden WiFi event
dependency, silent queue drop, and TLS peer-verification disable are not copied.
This module is an explicit, caller-configured bounded state machine. The native
Zephyr adapter draft uses the pinned MQTT/socket API subset recorded below.
The mock fixture validates the state machine only; it does not claim target
Zephyr compilation, broker compatibility, downlink support, or device runtime.
`MqttRuntime` adds a dedicated cooperative worker and one-producer/one-consumer
bounded queue in external RAM, compiled only by `MT_MQTT_GATEWAY` for the
XIAO ESP32-S3 WiFi/PSRAM profile. Ordinary builds compile disabled stubs and
allocate no MQTT queue or worker stack. The queue is eight slots of 160-byte
topic plus 1024-byte payload capacity; its accepted items are copied before
enqueue returns and backpressure is returned to the producer.

Target dependency lock pins Zephyr commit
`1ee3b93134be23c16ab16b9eb4183bfef63447a6` in `mt_only/dependency-lock.json`.
The adapter draft was checked against these public files at the locked Zephyr revision:

- `include/zephyr/net/mqtt.h`: `52246910be49fa93a3c649f4f4b98136d8b2aa3a2706d47d0f90cead71eba0bf`
- `include/zephyr/net/socket.h`: `6ea44e0fbbdde7656e6e89f9d3c60123d6cfd5196da69f0b1f20eda7e857f253`
- `include/zephyr/net/net_ip.h`: `3858c98231a7edec70d0ca204b8613ed40106ad03a66d9c8c6595932acdc2126`
- `include/zephyr/net/socket_poll.h`: `d4056eac5ece4b67fdb81dfda86c7cf3002cfd0cd456a45b0ebde5f022938988`
- `include/zephyr/net/tls_credentials.h`: `44d3d1ec07971779235ed13687e28241b26e296015ccc67ac515045ef841b5e5`
- `subsys/net/lib/mqtt/mqtt.c`: `fb3989c7ff95aa4c45af79a367b415bd573d6be52af8023ec9f369962fd4a2c9`
- `subsys/net/lib/mqtt/mqtt_transport_socket_tcp.c`: `3ef13131dd0867cd11ea6a5a982ded751d610756019a467f4c57cea5512851ef`
- `subsys/net/lib/mqtt/mqtt_transport_socket_tls.c`: `398021882ea9ceb32f19e08bc7e1585765d06d2025bbaa7c86b1b652cf67c45c`
- `subsys/net/lib/tls_credentials/tls_credentials_volatile.c`: `5f53aeb976b4e211a8853d9b1ed69ec754b0f38c1d84e01445344a40e8cd3db0`
- `subsys/net/lib/tls_credentials/tls_credentials_protected_storage.c`: `cd53598b00978168ca8fa749a11cb08561034118ec3f65b414bae60e9bd11f34`

TLS preflight calls `tls_credential_get` for `TLS_CREDENTIAL_CA_CERTIFICATE`
with a one-byte destination and zero capacity. The pinned volatile and protected
storage implementations return `-EFBIG` plus the positive required length before
copying any certificate bytes; missing tags/types and backend failures return
other errors. Only a positive-length registered CA type is admitted. These reads
establish API and preflight semantics only. The adapter has not been target
compiled; integration must compile it against the full pinned SDK and validate
Kconfig/API guards.
