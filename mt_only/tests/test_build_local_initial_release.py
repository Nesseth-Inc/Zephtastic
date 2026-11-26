#!/usr/bin/env python3
"""Admission and resolved-Kconfig checks for build-local.py --initial-release."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
APP = HERE.parent
SCRIPT = APP / "scripts/build-local.py"


FAKE_CMAKE = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args=sys.argv[1:]
if args and args[0]=='--build':
    Path(os.environ['FAKE_BUILD_MARKER']).write_text('called\n')
    raise SystemExit(0)
build=Path(args[args.index('-B')+1]); build.mkdir(parents=True,exist_ok=True)
sdk=Path(os.environ['FAKE_SDK'])
cache=build/'CMakeCache.txt'
cache.write_text('CMAKE_C_COMPILER:FILEPATH='+str(sdk/'bin/fake-cc')+'\nCMAKE_CXX_COMPILER:FILEPATH='+str(sdk/'bin/fake-cxx')+'\n')
zephyr=build/'zephyr'; zephyr.mkdir(exist_ok=True)
tak=os.environ['FAKE_TAK_CONFIG']
(zephyr/'.config').write_text('CONFIG_MT_TAK_TRACKER='+tak+'\n' if tak=='y' else '# CONFIG_MT_TAK_TRACKER is not set\n')
(build/'fake-configure-args.json').write_text(json.dumps(args))
'''


class InitialReleaseBuildAdmission(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zt-initial-release-test-")
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        cmake = self.bin / "cmake"
        cmake.write_text(FAKE_CMAKE)
        cmake.chmod(0o755)
        self.sdk = self.root / "sdk"
        compiler_dir = self.sdk / "bin"
        compiler_dir.mkdir(parents=True)
        for name in ("fake-cc", "fake-cxx"):
            compiler = compiler_dir / name
            compiler.write_text("#!/usr/bin/env python3\nprint('fake compiler 1')\n")
            compiler.chmod(0o755)
        self.zephyr = self.root / "zephyr"
        for name in (
            "subsys/settings/src/settings.c",
            "subsys/settings/src/settings_store.c",
            "subsys/settings/src/settings_nvs.c",
            "subsys/bluetooth/host/settings.c",
            "subsys/bluetooth/host/keys.c",
        ):
            target = self.zephyr / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(APP / "dependency-overlay/zephyr" / name, target)
        self.modules = self.root / "modules"
        self.nordic = self.root / "nordic-modules"
        self.modules.mkdir()
        self.nordic.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def env(self, tak):
        return {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
            "FAKE_SDK": str(self.sdk),
            "FAKE_TAK_CONFIG": tak,
            "FAKE_BUILD_MARKER": str(self.root / "build-called"),
            "SOURCE_DATE_EPOCH": "1794661471",
        }

    def base(self, target, build, args, tak="n"):
        command = [
            sys.executable, str(SCRIPT), target,
            "--zephyr", str(self.zephyr),
            "--modules", str(self.modules),
            "--sdk", str(self.sdk),
            "--build", str(build),
            "--source-date-epoch", "1794661471",
            *( ["--nordic-modules", str(self.nordic)] if target == "t096" else [] ),
            *args,
        ]
        result = subprocess.run(command, cwd=APP.parent, env=self.env(tak), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return result

    def test_rejects_out_of_scope_profile_families_before_configure(self):
        t096 = self.base("t096", self.root / "bad-t096", ["--initial-release"])
        self.assertEqual(t096.returncode, 2, t096.stdout)
        self.assertIn("requires --normal-tx --stock-pki-trial --lto-runtime", t096.stdout)
        xiao = self.base("xiao", self.root / "bad-xiao", ["--initial-release", "--target-private-runtime", "--wifi-tcp"])
        self.assertEqual(xiao.returncode, 2, xiao.stdout)
        self.assertIn("requires --target-private-runtime --wifi-tcp --wifi-mqtt", xiao.stdout)

    def test_t096_requires_resolved_tak_off_before_build(self):
        flags = ["--initial-release", "--normal-tx", "--stock-pki-trial", "--lto-runtime"]
        build = self.root / "t096-conflict"
        result = self.base("t096", build, flags, tak="y")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("CONFIG_MT_TAK_TRACKER must resolve to n", result.stdout)
        provenance = json.loads((build / "build-provenance-private.json").read_text())
        self.assertTrue(provenance["initial_release"])
        self.assertEqual(provenance["tak_tracker_expected"], "n")
        self.assertEqual(provenance["tak_tracker_resolved"], "y")
        self.assertEqual(provenance["configure_status"], "FAIL_RELEASE_CONFIG")
        self.assertFalse((self.root / "build-called").exists())

    def test_t096_and_xiao_overlay_order_and_provenance(self):
        t096 = self.root / "t096-pass"
        result = self.base("t096", t096,
                           ["--initial-release", "--normal-tx", "--stock-pki-trial", "--lto-runtime"])
        self.assertEqual(result.returncode, 0, result.stdout)
        t096_provenance = json.loads((t096 / "build-provenance-private.json").read_text())
        t096_cmd = json.loads((t096 / "configure-command.json").read_text())
        conf = next(item.split("=", 1)[1] for item in t096_cmd if item.startswith("-DEXTRA_CONF_FILE="))
        self.assertEqual(conf.split(";"), [str(APP / "boards/t096.conf"), str(APP / "boards/initial-release.conf")])
        self.assertEqual(t096_provenance["tak_tracker_resolved"], "n")
        self.assertEqual(t096_provenance["configure_status"], "PASS")
        self.assertFalse(t096_provenance["rx_only"])

        xiao = self.root / "xiao-pass"
        result = self.base("xiao", xiao,
                           ["--initial-release", "--target-private-runtime", "--wifi-tcp", "--wifi-mqtt"])
        self.assertEqual(result.returncode, 0, result.stdout)
        xiao_cmd = json.loads((xiao / "configure-command.json").read_text())
        xiao_conf = next(item.split("=", 1)[1] for item in xiao_cmd if item.startswith("-DEXTRA_CONF_FILE="))
        self.assertEqual(xiao_conf.split(";"), [
            str(APP / "boards/xiao.conf"),
            str(APP / "boards/xiao-wifi-tcp.conf"),
            str(APP / "boards/xiao-wifi-mqtt.conf"),
            str(APP / "boards/initial-release.conf"),
        ])
        xiao_provenance = json.loads((xiao / "build-provenance-private.json").read_text())
        self.assertTrue(xiao_provenance["initial_release"])
        self.assertEqual(xiao_provenance["tak_tracker_resolved"], "n")
        self.assertTrue(xiao_provenance["initial_release_overlay_sha256"])
        self.assertTrue(xiao_provenance["rx_only"])


if __name__ == "__main__":
    unittest.main()
