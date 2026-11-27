# SPDX-License-Identifier: GPL-3.0-only
"""Assign every upstream module inventory row to its qualification job."""
from pathlib import Path
import re

STOCK = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
EXISTING = set("""
AdminModule TrafficManagementModule HopScalingModule RoutingModule NodeInfoModule
MeshBeaconBroadcastModule PositionModule PositionPrecision DeviceTelemetryModule EnvironmentTelemetryModule
""".split())
GROUPS = {
    "wire": """
        TextMessageModule KeyVerificationModule TraceRouteModule NeighborInfoModule
        WaypointModule GeofenceModule StoreForwardModule MQTT RangeTestModule StatusMessageModule AtakPluginModule""",
    "peripherals": """
        InputBroker SystemCommandsModule BuzzerFeedbackThread StatusLEDModule ReplyBotModule
        DropzoneModule GenericThreadModule RemoteHardwareModule PowerStressModule CannedMessageModule
        GamesModule SerialModule AudioModule PaxcounterModule ExternalNotificationModule DetectionSensorModule
        WatchdogThread OnScreenKeyboardModule ReplyModule Breakout ChirpyRunner Snake Tetris AmbientLightingThread""",
    "reports": "AirQualityTelemetryModule HealthTelemetryModule PowerTelemetryModule HostMetricsModule UnitConversions",
    "core": "MeshBeaconListenerModule MeshBeaconTxHook DMShellModule DMShellTestModule",
}
KINDS = {"differential", "behavior", "profile", "source", "hardware", "partial"}


def verify_module_evidence(group, receipt):
    # The native inventory check separately derives the registered set and
    # translation-unit set directly from each of the two donor Git objects.
    owners = set(EXISTING)
    for labels in GROUPS.values():
        for name in labels.split():
            if name in owners:
                raise ValueError(f"Duplicate module assignment {name}")
            owners.add(name)
    inventory = Path(__file__).resolve().parents[1] / "docs/UPSTREAM-MODULE-INVENTORY.md"
    expected = set(re.findall(r"^\| `(\w+)` \| (?:registered|companion|support) \|", inventory.read_text(), re.M))
    expected.update(("DMShellModule", "DMShellTestModule"))
    if owners != expected:
        raise ValueError(f"Unaccounted module rows: {sorted(owners ^ expected)}")
    rows = receipt["modules"]
    wanted = set(GROUPS[group].split())
    if receipt["stock_revision"] != STOCK or set(rows) != wanted:
        raise ValueError(f"Incorrect donor or module evidence for {group}: {sorted(set(rows) ^ wanted)}")
    for name, item in rows.items():
        if item.get("kind") not in KINDS or not isinstance(item.get("evidence"), str) or not item["evidence"].strip():
            raise ValueError(f"Missing evidence type or description for {name}")
