#!/usr/bin/env python3
"""Compare real ZT audio and Pax paths to the pinned stock worker and report methods."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess

PIN = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
BLOBS = {"AudioModule": "0dc9832f352263b8934a97e5f0b89aa1ad93b7d3",
         "PaxcounterModule": "d9135c3a2f7b343bbfad6c078aa1ce5e28521cf4"}

def section(source, start):
    begin = source.index(start)
    depth = 0
    for i in range(source.index("{", begin), len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if not depth:
                return source[begin:i+1] + "\n"
    raise ValueError(start)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    app = here.parents[1]
    output = args.output.resolve()
    if output.is_relative_to(app.parent) or output.exists():
        parser.error("select a fresh directory outside source")
    output.mkdir(parents=True)
    src = {}
    for module, blob in BLOBS.items():
        ref = f"{PIN}:src/modules/esp32/{module}.cpp"
        git = ["git", "-C", args.stock]
        assert subprocess.check_output([*git, "rev-parse", ref], text=True).strip() == blob
        src[module] = subprocess.check_output([*git, "show", ref], text=True)
    audio = src["AudioModule"]
    begin = audio.index("const int headerLen =")
    end = audio.index("} else {", begin)
    (output / "stock_audio_rx.inc").write_text(audio[begin:end] + "}\n")
    constructor = section(audio, "AudioModule::AudioModule()")
    lines = constructor.splitlines()
    (output / "stock_audio_size.inc").write_text("\n".join(x for x in lines if "encode_frame_num =" in x or "encode_frame_size =" in x) + "\n")
    (output / "stock_pax_send.inc").write_text(section(src["PaxcounterModule"], "bool PaxcounterModule::sendInfo("))
    (output / "stock_pax_reply.inc").write_text(section(src["PaxcounterModule"], "meshtastic_MeshPacket *PaxcounterModule::allocReply("))
    (output / "stock_audio_send.inc").write_text(section(audio, "void AudioModule::sendPayload("))
    ptt = audio.index("if (digitalRead(moduleConfig.audio.ptt_pin")
    end = audio.index("if (radio_state == RadioState::tx) {",audio.index("this->notifyObservers(&e);",audio.index("PTT released")))
    (output / "stock_ptt.inc").write_text(audio[ptt:end])
    proto = app.parent / "zephcore/bench/upstream-differential/source"
    (output / "meshtastic").symlink_to(proto, target_is_directory=True)
    tests = []
    for name, abi in (("normal", []), ("short", ["-fshort-enums"])):
        exe = output / name
        cmd = [*shlex.split(os.environ.get("CXX","clang++")), "-std=c++17", "-Wall", "-Wextra",
               "-Werror", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               *abi, "-I"+str(app/"src"), "-I"+str(output), "-I"+str(proto/"nanopb"),
               str(here/"cases.cpp"), str(app/"src/modules/audio/AudioModule.cpp"), "-o", str(exe)]
        subprocess.run(cmd, check=True)
        env = {**os.environ, "ASAN_OPTIONS":"detect_leaks=0:halt_on_error=1", "UBSAN_OPTIONS":"halt_on_error=1"}
        result = subprocess.run([exe], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output/(name+".log")).write_text(result.stdout)
        result.check_returncode()
        print(name, result.stdout.strip())
        tests.append({"abi":name,"cases":result.stdout.strip(),"command":cmd})
    report = {"status":"PASS","stock":PIN,"stock_blobs":BLOBS,"runs":tests,"inputs":"synthetic PCM, test codec and I2S, passive observation fixture","hardware":False,"rf":False}
    for p in (here/"cases.cpp", app/"src/modules/audio/AudioModule.cpp", app/"src/modules/audio/AudioWire.h", app/"src/modules/peripherals/PaxcounterCore.h",
              app/"src/stock/peripherals/AudioModule.h", app/"src/stock/peripherals/PaxcounterModule.h"):
        report.setdefault("source_sha256", {})[p.relative_to(app).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    (output/"results.json").write_text(json.dumps(report,indent=2)+"\n")

if __name__ == "__main__":
    main()
