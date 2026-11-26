"""Pure CLI-boundary controls for explicit WiFi MQTT profile admission."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "build-local.py"


class BuildLocalMqttCli(unittest.TestCase):
    def run_invalid(self, target, flags, expected):
        with tempfile.TemporaryDirectory(prefix="m114-build-cli-") as temp:
            out = Path(temp) / "must-not-be-created"
            cmd = [sys.executable, "-B", str(SCRIPT), target,
                   "--zephyr", str(Path(temp) / "zephyr"),
                   "--modules", str(Path(temp) / "modules"),
                   "--sdk", str(Path(temp) / "sdk"), "--build", str(out), *flags]
            result = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected, result.stderr)
            self.assertFalse(out.exists(), "invalid profile reached build-directory creation")

    def test_mqtt_requires_explicit_wifi_tcp(self):
        self.run_invalid("xiao", ["--wifi-mqtt"], "--wifi-mqtt requires --wifi-tcp")

    def test_mqtt_profile_requires_xiao_target(self):
        self.run_invalid("t096", ["--wifi-tcp", "--wifi-mqtt"],
                         "--wifi-mqtt requires the WiFi-capable xiao target")

    def test_tcp_target_guard_remains(self):
        self.run_invalid("t096", ["--wifi-tcp"],
                         "--wifi-tcp requires the WiFi-capable xiao target")

    def test_explicit_overlay_and_provenance_are_wired(self):
        source = SCRIPT.read_text()
        self.assertIn("overlays.append(app/'boards/xiao-wifi-mqtt.conf')", source)
        self.assertIn("'wifi_tcp':a.wifi_tcp,'wifi_mqtt':a.wifi_mqtt", source)
        self.assertIn("p.add_argument('--wifi-mqtt'", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
