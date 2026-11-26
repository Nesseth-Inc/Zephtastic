# XIAO broadcast cooldowns

The normal XIAO image retains eight independent broadcast timestamps. The pinned Meshtastic firmware uses the same eight producers in its transmit history. They are NodeInfo, Position, MeshBeacon, DeviceMetrics, EnvironmentMetrics, PowerMetrics, AirQualityMetrics and HealthMetrics.

Run the suite with a local checkout containing Meshtastic commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`.

```sh
python3 -B mt_only/tests/stock_transmit_history/run.py \
  --stock /path/to/meshtastic/firmware --output /tmp/zt-broadcast-history-results
```

The runner compiles the two donor recovery functions without changing their bodies. It exercises ZT's configured producer, scheduler, crypto-independent send admission and Settings paths with ASan and UBSan under both enum ABIs. The AirQuality and Health cases use the complete firmware polling method with synthetic sensor snapshots. The factory test exercises interrupted erases. The storage test injects a truncated inactive bank at every byte boundary, a reported failure after a complete write and a future record version.

## Storage and timing

Two 82-byte native Settings records contain the local node number, the enrolled public owner key where applicable, a generation, a checksum, and the eight timestamps with their clock types. Normal radio and channel edits keep cooldowns for the same owner. Factory configuration reset erases both records. Changing the identity prevents the latest record from restoring a previous owner's timestamps. PKI builds start persistence only after enrollment supplies a valid binding.

An empty store leaves the first eligible send immediately available. An absolute timestamp recovers its age from the local wall clock with the donor's one-day window. Boot-relative entries recover only without a set wall clock and within the donor's two-minute window. A new local admission takes precedence over recovered timing. The Position module restores just its last-send timestamp at startup, following the donor constructor. It starts with unknown previous coordinates.

The first dirty record checkpoints at the next radio-safe opportunity. Further updates coalesce for five minutes. An orderly software reset or real power-off attempts a final checkpoint. Sudden power loss can discard changes since the last successful checkpoint. Timer sleep on XIAO retains the running scheduler in RAM. A MeshBeacon wave requires a successful, verified checkpoint before queuing its packets and folds other dirty timestamps into that write. Existing ZT MeshBeacon settings remain readable and govern the first migration wave. An older firmware can read its previous beacon marker on downgrade.

NodeInfo stamps once its packet is accepted into the sender's queue. Position stamps successful scheduler sends. Device, Environment, Power and Health follow the stock opportunity semantics. AirQuality stamps the accepted measurement packet. A missing health sensor supplies no measurement. A lightning-triggered Environment report retains its separate urgent admission.

The host tests do not exercise physical sensor acquisition or provide a remote RF receipt.
