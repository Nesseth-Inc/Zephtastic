# M116 MQTT MapReport adapter

## Stock reference

The payload and scheduling rules follow Meshtastic firmware commit
`8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c` (2.8.1), specifically
`MQTT::perhapsReportToMap()` in the pinned source whose SHA-256 is
`dd9005e52ffea560c805fd40ae2fd937f96c0f5cb76ccb01083ad42854590f06`.
The matching generated `mqtt.pb.h` SHA-256 is
`c44838ebaa409d6e170ab87462895d6e88dc37738138e8d13ad73ff38052ba07`;
nanopb is 0.4.9.1. The proposal source records the additional config/default
hashes and interval normalization evidence.

## Production seam

`mt::mqtt::MqttMapReporter::poll()` consumes:

```cpp
Result poll(const network::MqttGatewaySettings &settings,
            const MqttMapReport::Source &source,
            const Identity &identity, const char *configuredRoot,
            uint32_t nowMillis, bool transportReady,
            bool suppressOnEventChannel, Sink sink, void *context);
```

`Source` is a synchronous caller snapshot of `longName`, `shortName`,
`firmwareVersion`, `role`, `hwModel`, `region`, `modemPreset`,
`hasDefaultChannel`, `latitudeI`, `longitudeI`, `altitude`, and
`numOnlineLocalNodes`. `Identity` supplies `fromNode`, `packetId`, exact primary
`channelId` plus length, and local `gatewayId` plus length. Stock map packets
leave MeshPacket.id at zero; the adapter preserves that behavior. Root/main should
populate coordinates from `Service::localPosition()` only after the explicit
`mapReportingEnabled && hasMapReportSettings && should_report_location` gates
are represented in settings. The adapter independently checks those opt-ins,
transport readiness and event-channel suppression before encoding.

The callback is synchronous and should enqueue/copy its inputs before return;
its signature is compatible with a thin wrapper around `enqueuePacket()`. This
keeps ServiceEnvelope scratch inside the existing bounded codec/runtime instead
of duplicating the 1024-byte envelope buffer on the Service stack. The adapter
builds a decoded broadcast `MAP_REPORT_APP` packet, uses topic
`<root-or-msh>/2/map/`, and supplies primary channel/gateway identity to the
sink. It advances the monotonic interval after invoking the sink regardless of
queue result, matching stock's post-`publish()` timestamp update and preventing
per-loop retries when a bounded queue is full.

The shared M116 integration now binds
`PB_BIND(meshtastic_MapReport, meshtastic_MapReport, 4)` in `Protobuf.cpp` and
calls the adapter from the Service-owner main loop using local settings,
identity and position snapshots. This component itself does not edit the
common glue files; the focused runner links the production descriptor.

## Stock channel metadata predicates

The main-loop snapshot follows the pinned stock `Channels.cpp` predicates:
`hasDefaultChannel()` requires preset mode, no verbatim frequency override,
the actual US/UNSET channel slot matching the slot resolved with channel zero
(the preset primary-name hash), and at least one active channel whose effective
name equals the selected modem display name and whose normalized key is the
Public default. The normalized-key comparison intentionally treats stock
one-byte PSK aliases and an equivalent expanded Public key alike. It does not
assume a fixed slot number. Event-channel suppression follows stock's
compile-time policy: when both `USERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL` and
`USERPREFS_CHANNEL_0_PSK` are present, it compares the active primary's
normalized effective key bytes/length with that configured 16/32-byte key;
without both compile-time settings the predicate is false. Location reporting
still requires the independent explicit map-location opt-in.

## Validation limits

`mqtt_map_report_tests.cpp` compares the emitted payload against a stock
nanopb-generated synthetic golden and decodes the actual ServiceEnvelope to
check packet/channel/gateway identity. The runner uses ASan/UBSan with default
and `-fshort-enums`. No real device identity/location, target build, broker,
network, RF transmission, or publication is tested here.
