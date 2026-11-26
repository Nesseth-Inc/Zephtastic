"""Owned local fixture: actual production headers and unchanged pinned stock."""
if not __debug__:
    raise RuntimeError("Optimized driver refused")
from pathlib import Path
import argparse, datetime, hashlib, json, os, re, signal, subprocess, shlex, shutil, sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0,str(HERE.parents[1]/"airtime_quality"))
from bounded_process import run_group

SOURCE = HERE.parents[2] / "src"
ORACLE = HERE.parent / "stock_reference"
STOCK_PINS = {
    "airtime.cpp": "95c8402594cbc1534b06d4be178cdc5757cba752d7e606da3a3bb9ed7e6078e0",
    "airtime.h": "c9a3388e8cedca600438e23d32d1cbbfd4a0a24bda621c5bff12b6e576d04fa2",
}

def require(ok, text):
    if not ok:
        raise RuntimeError(text)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

class Interrupted(BaseException):
    pass

def interrupted(signum, frame):
    raise Interrupted()

def run(argv, log, seconds, env):
    with log.open("w") as output:
        result=run_group(argv,timeout=seconds,stdout=output,env=env,cwd=HERE)
    return {"argv":argv,"exit":result.returncode,"log_sha256":sha(log),"output":log.read_text()}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    out = args.output.resolve()
    require(not out.is_relative_to(HERE.parents[3]), "Output must be outside source tree")
    out.mkdir(exist_ok=False)
    (out / "tmp").mkdir()
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "TMPDIR": str(out / "tmp"), "LC_ALL": "C",
           "ASAN_OPTIONS": "halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}
    compiler_argv = shlex.split(args.compiler)
    compiler = Path(shutil.which(compiler_argv[0])).resolve(strict=True)
    compiler_argv[0] = str(compiler)
    full = (SOURCE / "ChannelBuckets.h").is_file()
    require(full, "Production bucket header required")
    paths = [SOURCE / name for name in ("ChannelQuality.h", "ChannelWindow.h", "Observed.h")]
    if full:
        paths.append(SOURCE / "ChannelBuckets.h")
    paths += [HERE / "test_quality.cpp", Path(__file__).resolve(), compiler,
              HERE.parents[1]/"airtime_quality/bounded_process.py", ORACLE / "STOCK-PIN.json", *(ORACLE / "stock" / n for n in STOCK_PINS),
              *sorted((ORACLE / "stubs").rglob("*.h"))]
    inputs = {str(p): sha(p) for p in paths}
    require(all(sha(ORACLE / "stock" / n) == pin for n, pin in STOCK_PINS.items()), "Stock source drift")
    summary = {"status": "RUNNING", "full_window_header_present": full,
               "stock_commit": "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c",
               "source_sha256": inputs, "variants": [], "hardware": False,
               "preserved_quality_and_old_window_groups":17,"added_logged_window_groups":3}
    (out / "INPUTS.json").write_text(json.dumps(summary, indent=2) + "\n")
    try:
        for name, extra in [("default", []), ("short-enums", ["-fshort-enums"])]:
            require(all(sha(Path(p)) == pin for p, pin in inputs.items()), "Input changed before compile")
            binary = out / name
            argv = [*compiler_argv, "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", *extra, "-I", str(SOURCE),
                    "-I", str(ORACLE / "stock"), "-I", str(ORACLE / "stubs")]
            if not full:
                argv += ["-DQUALITY_ONLY"]
            argv += [str(HERE / "test_quality.cpp")]
            if full:
                argv += [str(ORACLE / "stock/airtime.cpp")]
            argv += ["-o", str(binary)]
            compiled = run(argv, out / (name + "-compile.log"), 45, env)
            require(compiled["exit"] == 0, "Compilation failed; inspect log")
            result = run([str(binary)], out / (name + "-test.log"), 10, env)
            matches = re.findall(r"^SUMMARY groups=(\d+) checks=(\d+) failures=(\d+)$", result["output"], re.M)
            require(len(matches) == 1, "Missing unique terminal summary")
            groups, checks, failures = map(int, matches[0])
            summary["variants"].append({"name": name, "compile": compiled, "test": result,
                                        "groups": groups, "checks": checks, "failures": failures,
                                        "binary_sha256": sha(binary)})
            require(result["exit"] == 0 and failures == 0 and groups == (20 if full else 6), "Fixture failure")
        require(all(sha(Path(p)) == pin for p, pin in inputs.items()), "Input changed during qualification")
        summary["status"] = "PASS" if full else "PASS_QUALITY_ONLY_WINDOW_PENDING_BUCKET_HEADER"
        summary["source_unchanged"] = True
    except BaseException as error:
        summary.update(status="FAIL", error_type=type(error).__name__)
        raise
    finally:
        summary["finished_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out / "SUMMARY.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({k: summary[k] for k in ("status", "full_window_header_present", "source_unchanged")}))

if __name__ == "__main__":
    for number in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, interrupted)
    main()
