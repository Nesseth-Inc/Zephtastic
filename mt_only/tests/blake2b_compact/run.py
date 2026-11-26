#!/usr/bin/env python3
"""Sanitized differential test for Monocypher's fixed-round BLAKE2b loop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

MONO = ROOT / "zephcore/lib/monocypher"
SOURCE = MONO / "monocypher.c"
HEADER = MONO / "monocypher.h"
SHA512_SOURCE = MONO / "monocypher-ed25519.c"
TEST = HERE.parent / "blake2b_compact_vectors.c"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, env, capture=False):
    return subprocess.run(command, env=env, check=True, text=True,
                          capture_output=capture)


def verify_corpus(output):
    message_sizes = {0, 1, 127, 128, 129, 255, 256, 257, 1024}
    message = bytes((i * 73 + 19) & 0xff for i in range(1024))
    key = bytes((i * 11 + 7) & 0xff for i in range(32))
    checks = 0
    sha_sizes = set()
    for line in output.splitlines():
        if line.startswith("kat-") or line.startswith("PASS "):
            continue
        fields = line.split()
        if len(fields) != 2:
            raise RuntimeError(f"Malformed test output: {line!r}")
        kind, actual = fields
        try:
            method, digest_size_s, message_size_s = kind.rsplit("-", 2)
            digest_size, message_size = int(digest_size_s), int(message_size_s)
        except ValueError as exc:
            raise RuntimeError(f"Malformed case label: {kind!r}") from exc
        if method == "sha512":
            if digest_size != 64 or hashlib.sha512(message[:message_size]).hexdigest() != actual:
                raise RuntimeError(f"SHA-512 mismatch: {kind}")
            sha_sizes.add(message_size)
            continue
        if method not in {"unkeyed", "unkeyed-stream", "keyed", "keyed-stream"}:
            raise RuntimeError(f"Unexpected case label: {method!r}")
        if digest_size not in range(1, 65) or message_size not in message_sizes:
            raise RuntimeError(f"Out-of-range test case: {kind!r}")
        data = message[:message_size]
        has_key = method.startswith("keyed")
        expected = hashlib.blake2b(data, digest_size=digest_size,
                                   key=key if has_key else b"").hexdigest()
        if actual != expected:
            raise RuntimeError(f"Independent hashlib oracle mismatch: {kind}")
        checks += 1
    if sha_sizes != {0, 1, 111, 112, 127, 128, 129, 255, 256, 257, 1024}:
        raise RuntimeError("Incomplete SHA-512 corpus")
    expected_checks = 64 * len(message_sizes) * 4
    if checks != expected_checks:
        raise RuntimeError(f"Expected {expected_checks} corpus checks, got {checks}")
    return checks


def check_output_guard():
    with tempfile.TemporaryDirectory(prefix="blake2b-output-guard-") as tmp:
        root = Path(tmp)
        out = root / "occupied-output"
        out.mkdir()
        marker = out / "keep.marker"
        marker_bytes = b"caller-owned output must remain untouched\n"
        marker.write_bytes(marker_bytes)
        compiler_called = root / "compiler-was-called"
        compiler = root / "compiler-sentinel"
        compiler.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            f"Path({str(compiler_called)!r}).write_text('called')\n"
            "raise SystemExit(99)\n"
        )
        compiler.chmod(0o700)
        env = {**os.environ, "CC": str(compiler)}
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), "--output", str(out)],
            text=True, capture_output=True, env=env
        )
        if result.returncode == 0:
            raise RuntimeError("Existing output directory was unexpectedly accepted")
        if marker.read_bytes() != marker_bytes:
            raise RuntimeError("Existing output marker changed")
        if compiler_called.exists():
            raise RuntimeError("Compiler ran before existing-output refusal")
        if "must be new" not in result.stderr:
            raise RuntimeError("Runner did not report the fresh-output requirement")
    print("PASS existing-output negative control: marker preserved, compiler not invoked")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--self-test-output-guard", action="store_true",
                        help="verify existing output directories are refused without writes")
    args = parser.parse_args()
    if args.self_test_output_guard:
        check_output_guard()
        return
    out = args.output.expanduser().resolve()
    if out.exists() or out.is_symlink():
        raise SystemExit("Output directory must be new; existing paths are never modified")
    try:
        out.mkdir(parents=True, exist_ok=False)
    except FileExistsError as exc:
        raise SystemExit("Output directory must be new; existing paths are never modified") from exc

    tracked = {"monocypher.c": sha(SOURCE), "monocypher-ed25519.c": sha(SHA512_SOURCE), "monocypher.h": sha(HEADER),
               "blake2b_compact_vectors.c": sha(TEST), "runner": sha(Path(__file__))}
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
              f"-I{MONO}"]
    cc = host.cc()
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0",
           "UBSAN_OPTIONS": "halt_on_error=1"}
    commands = {}
    outputs = {}
    variants = {
        "default": [],
        "fixed_round_loop": ["-DBLAKE2_NO_UNROLLING=1"],
        "t096_factored": ["-DBLAKE2_NO_UNROLLING=1", "-DMT_SMALL_CRYPTO_ROUNDS=1"],
    }
    for name, defines in variants.items():
        exe = out / f"blake2b-{name}"
        command = [*cc, *common, *defines, str(TEST), str(SOURCE), str(SHA512_SOURCE), "-o", str(exe)]
        commands[name] = command
        run(command, env)
        result = run([str(exe)], env, capture=True)
        outputs[name] = result.stdout
        (out / f"{name}.log").write_text(result.stdout + result.stderr)

    if any(value != outputs["default"] for value in outputs.values()):
        raise RuntimeError("Default and compact-loop outputs differ")
    checks = verify_corpus(outputs["default"])
    stable = tracked == {"monocypher.c": sha(SOURCE), "monocypher-ed25519.c": sha(SHA512_SOURCE), "monocypher.h": sha(HEADER),
                         "blake2b_compact_vectors.c": sha(TEST), "runner": sha(Path(__file__))}
    if not stable:
        raise RuntimeError("Pinned crypto/test inputs changed while tests ran")
    (out / "transcript.txt").write_text(outputs["default"])
    result = {
        "status": "PASS",
        "hardware": False,
        "source_head": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
        "source_tree": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD^{tree}"], text=True).strip(),
        "source_hashes_before_after_match": stable,
        "monocypher_version": "4.0.3",
        "monocypher_c_sha256": tracked["monocypher.c"],
        "monocypher_header_sha256": tracked["monocypher.h"],
        "test_sha256": tracked["blake2b_compact_vectors.c"],
        "runner_sha256": tracked["runner"],
        "compiler": cc,
        "sanitizers": ["address", "undefined"],
        "variants": variants,
        "published_known_answer_vectors": ["BLAKE2b-512 empty", "BLAKE2b-512 abc", "BLAKE2b-256 empty", "BLAKE2b-256 abc"],
        "differential_corpus_cases": checks,
        "independent_python_hashlib_checks": checks,
        "parameters": {"digest_sizes": "1 through 64", "message_lengths": [0, 1, 127, 128, 129, 255, 256, 257, 1024], "key_bytes": 32, "message_pattern": "(i*73+19) mod 256", "key_pattern": "(i*11+7) mod 256"},
        "curve_known_answers": ["RFC 7748 6.1 public and agreed key", "RFC 8032 7.1 test 1 and changed signature rejection"],
        "independent_sha512_checks": 11,
        "commands": commands,
        "limitations": ["No target build, size measurement, or constant-time/assembly audit was performed.", "Pinned TF-PSA source/library is not available in this local checkout, so no real PSA integration flow was run."],
    }
    (out / "RESULTS.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS default and both compact variants: {checks} corpus outputs each, ASan/UBSan; four fixed BLAKE2b KATs")


if __name__ == "__main__":
    main()
