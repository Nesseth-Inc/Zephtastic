# Telemetry report display

With the four telemetry screen settings enabled, open Telemetry and hold the button to browse reports. A tap advances and a double tap goes back. Hold again to return to the local summary. Only enabled families appear.

Environment offers Mesh, Local and Favorite views. Mesh keeps the last eligible public peer report and uses local measurements when there is no such report. Local reads the existing sensor snapshot independently of RF transmission. Favorite accepts only a current favorite peer. Power, Air quality and Health show the latest accepted report. A source's short name or node ID appears with its monotonic receipt age. A local sensor sample is labeled separately. After five minutes a report shows its age and waits for an update. The source may have a different wall clock.

Reachable fields match the stock Environment, Power, AirQuality and Health frame writers at Meshtastic commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. ZT uses its own font and splits long frames into three-row pages. IAQ values include the stock severity label. The popup and notification beep are separate behaviors. Public history requires a known, current contact. A later directed report withdraws that peer's previous value for its family. Its private payload stays out of the public report pages. Settings or identity changes invalidate the affected history.

The host tests accept a checkout of the stock firmware. The device tests use the normal companion mock radio and require no live equipment.

```sh
python3 mt_only/tests/stock_report_frames/run.py --stock /path/to/meshtastic
python3 mt_only/scripts/test-companion.py --display-reports --output /tmp/zt-report-rf
python3 mt_only/scripts/test-companion.py --display-reports --short-enums --output /tmp/zt-report-rf-short
CXX='c++ -DCONFIG_MT_TELEMETRY_REPORT_FRAMES=1' python3 mt_only/scripts/test-display.py --output /tmp/zt-report-ui
```
