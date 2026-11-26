# Meshtastic 2.8.2 signing and identity admission

The stock comparison uses firmware commit `d5f16191ca28b01b5024f590f04a67f25d66f1b1`. The native suite compiles the actual receive policy, transmit predicate and unsigned identity predicate against controlled crypto and NodeDB outcomes. Separate cases use real Monocypher signatures, the channel codec and PhoneAPI to send and repeat a directed Position query. Run both enum modes with distinct output directories.

```sh
python3 mt_only/scripts/test-module-signing.py --stock /path/to/firmware.git --output /tmp/zt-signing-default
python3 mt_only/scripts/test-module-signing.py --stock /path/to/firmware.git --short-enums --output /tmp/zt-signing-short
python3 mt_only/tests/stock_text_verification/run.py --stock /path/to/firmware.git
python3 mt_only/experimental/pki/test.py --output /tmp/zt-session-crypto
```

Compatible sends unsigned public NodeInfo, including the owner's public key. AES-CCM also suppresses signing. Balanced admits unsigned ordinary traffic while protecting previously signed identities. Strict exempts routing, key verification and traceroute on ordinary channels. A received invalid signature on a known key is rejected. A traceroute signature carries no identity evidence. ZT preserves the original radio envelope and its existing canonical relay constraints.

The normal encrypted first-contact path still requires a locally authorized pin or an admitted possession proof. An unsigned NodeInfo or shared-channel CCM tag alone cannot enroll a private contact. A verified signer retained in the phone directory or warm store can block an unsigned update. On a cold lookup, the next eligible NodeInfo can be admitted once the page worker verifies the absence of prior signing. Correlated directed requests can still receive a response. A known changed name or role may produce one phone warning per hour using the retained name.

Licensed mode refuses key verification and reports the reason locally. The two-role transcript covers an already pinned pair and a pair absent from both stores. Both participants require the matching final exchange, nonce and local confirmation. An absent peer can become manually verified only through the existing durable contact transaction.

The low-level X25519 primitive takes clamped inputs. ZT's identity journal also checks the derived public key and node binding. Ordinary Admin has no raw private-key import. A recovery record must validate as stored. Directed traffic retains one derived secret per live fixed-pair session in RAM. Each new operation checks the handle and matching scalar. Session close, cache eviction, invalid handle and freeze clear the derived key. Explicit standalone crypto callers derive independently on every operation.

Stock uses a cache of eight shared secrets on nRF52 and ten on ESP32. Stock's nRF52840 build can accelerate X25519 and Ed25519 verification with its Adafruit CC310 library. The pinned Zephyr T096 board leaves its CryptoCell node disabled, the build does not select PSA Montgomery255, and the pinned HAL and Zephyr crypto drivers do not supply the donor's CC310 curve provider. These profiles use the existing software curve implementation.
