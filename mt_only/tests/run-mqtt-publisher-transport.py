#!/usr/bin/env python3
import hashlib, json, os, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"

def main():
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-transport-") as tmp:
        for label, flags in (("default", []), ("short-enums", ["-fshort-enums"])):
            exe = Path(tmp) / label
            cmd = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1", *flags,
                   "-I", str(APP / "src"), str(APP / "tests/mqtt_publisher_transport_tests.cpp"), "-o", str(exe)]
            print("$", " ".join(cmd), flush=True)
            subprocess.run(cmd, check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
            print(label, flush=True)
            subprocess.run([str(exe)], check=True, env=env)

        # Compile the target branch against the pinned public Zephyr atomic
        # headers. Only unrelated utility headers omitted from this focused
        # snapshot are stubbed; the atomic API and implementation stay real.
        sdk = APP / "tests/mqtt_atomic_sdk"
        manifest = json.loads((sdk / "MANIFEST.json").read_text())
        for rel, expected in manifest["files"].items():
            if expected.get("missing"):
                continue
            body = (sdk / rel).read_bytes()
            assert len(body) == expected["bytes"]
            assert hashlib.sha256(body).hexdigest() == expected["sha256"]
        include = Path(tmp) / "zephyr-stubs"
        (include / "zephyr/sys").mkdir(parents=True)
        (include / "zephyr/toolchain.h").write_text("/* unused toolchain declarations */\n")
        (include / "zephyr/types.h").write_text("#include <stdint.h>\n#include <stddef.h>\n")
        (include / "zephyr/sys/util.h").write_text(
            "#define BITS_PER_BYTE 8\n"
            "#define BIT(n) (1UL << (n))\n"
            "#define ROUND_UP(x, y) ((((x) + (y) - 1) / (y)) * (y))\n")
        target_exe = Path(tmp) / "zephyr-atomic-branch"
        cmd = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-nostdinc++", "-D__ZEPHYR__", "-DCONFIG_ATOMIC_OPERATIONS_BUILTIN",
               "-I", str(include), "-I", str(sdk / "include"), "-I", str(APP / "src"),
               str(APP / "tests/mqtt_atomic_zephyr_compile.cpp"), "-o", str(target_exe)]
        print("$", " ".join(cmd), flush=True)
        subprocess.run(cmd, check=True)
        subprocess.run([str(target_exe)], check=True)
        print("pinned-zephyr-atomic-branch", flush=True)

if __name__ == "__main__": main()
