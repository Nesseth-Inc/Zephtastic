# M64 packet-bound fractional SNR and M65 received RSSI presence

The native SX126x driver keeps the signed raw quarter-dB SNR octet from its
existing successful `GET_PACKET_STATUS` read. The standard `lora_recv_cb`
continues to receive the original integer dB value. The new mutually exclusive
`sx126x_recv_async_packet` registration shares the same state, cancellation,
configuration and callback-generation checks, and provides a callback-local
`{snr_raw, snr_known}` sample. There is no second SPI read or last-value getter.

The application copies that sample with its payload into the bounded `Rx`
queue. Both USB and BLE companion exports use the same queued sample for
`MeshPacket.rx_snr`. Service/routing/backoff, preview, NodeInfo cache SNR and
traceroute continue using the existing integer SNR. Extending those fields is
outside this change. Failed status reads retain legacy behavior and mark the
fractional sample unknown; CRC/header/FIFO/restart failures cannot credit a
sample, and stale callback generations cannot publish one.

`src/PeerSnr.h` is the unchanged Qwen helper, SHA256
`21477ae65369eda769b139e482baadcb9cc631ceed51ed3a4ea864e5539db03f`.
It maps all 256 octets exactly to `-32..31.75` dB in quarter-dB steps. Unknown
samples and existing companion overloads preserve the legacy integer export.
The protobuf SNR field is not optional, so this adds no wire-level distinction
between zero SNR and absent/unknown metadata.

Run each mode with a new output folder outside this source worktree:

```sh
python3 -B mt_only/tests/packet_snr/run.py --output /absolute/new/default
python3 -B mt_only/tests/packet_snr/run.py --short-enums --output /absolute/new/short
```

The runner honors shell-split `CC`/`CXX`, freezes and hashes its actual inputs,
uses ASAN/UBSAN, bounds and reaps each process group, and refuses output reuse.
It extracts actual native read/registration/RX bodies and the actual application
queue callback. The export fixture uses actual Service/Companion/MtCodec and
nanopb for both board models and local transports. Fake HAL/queue/private-event
providers prove attribution and admitted-event boundaries; they do not establish
Zephyr scheduling, cryptography, target builds or physical reception.

The native extraction regression at `../channel_metrics/native/run.py` retains
the frozen M60d2 baseline and compares unchanged legacy HAL/callback/state traces
in default and short-enum modes. Its signal-quality stub supports both old and
new call signatures; the new packet suite separately tests the real status read.

M64 left the optional nanopb `MeshPacket.rx_rssi` presence false. The M65
fixture requires presence only when `snrKnown` says the shared native
`GET_PACKET_STATUS` read succeeded. All original known-status 256 SNR cases
retain their `-90` RSSI input and require present `-90` after the real nanopb
roundtrip. Unknown-status and legacy-overload cases require absent RSSI and a
decoded value of zero, even with the fixture's synthetic `-90` input. This is a
status-validity gate, not an `rssi!=0` gate: valid zero must remain present.

The existing public and admitted-private export groups also cover RSSI `-90`,
`0`, and `10` for both board models81/127 and both USB/BLE transports. Each case
requires exactly one exported packet per transport, the expected RSSI value,
presence even at zero, unchanged fractional SNR, no radio send, no settings save
and no node-edit call. For each model and public/private boundary, the same
fixture then alternates known negative and known zero with unknown-status zero.
Every unknown export must clear presence after the preceding known packet,
retain the legacy SNR fallback, and still emit exactly once without side effects.
These cases remain within the existing three export PASS groups; native and
queue test semantics/counts are unchanged. The private
provider still proves only an already-admitted event boundary, not cryptography.
This fixture does not establish physical reception or separately qualify RSSI
absence on local synthesized completions or replayed history.
