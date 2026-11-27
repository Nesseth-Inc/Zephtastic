#!/usr/bin/env python3
"""Run the peripheral and local UI comparisons against Meshtastic 2.8.2."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
TESTS = HERE.parent
APP = TESTS.parent
PROTO = APP.parent / "zephcore/bench/upstream-differential/source"
PIN = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"


def method(source, marker):
    start = source.index(marker)
    return source[start:source.index("\n}", start) + 2]


def capture(argv):
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            env={**os.environ, "PYTHONOPTIMIZE": "", "UBSAN_OPTIONS": "halt_on_error=1",
                                 "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"})
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    return result.stdout


def platform(stock, temp):
    def source(path):
        return subprocess.check_output(["git", "-C", stock, "show", f"{PIN}:src/{path}"], text=True)
    (temp / "meshtastic").symlink_to(PROTO, target_is_directory=True)
    events = source("input/InputBroker.h")
    (temp / "events.inc").write_text(events[events.index("enum input_broker_event"):events.index("// Which physical joystick")])
    commands = source("modules/SystemCommandsModule.cpp")
    (temp / "commands.inc").write_text(method(commands, "int SystemCommandsModule::handleInputEvent("))
    broker = source("input/InputBroker.cpp")
    start = broker.index("        userConfigNoScreen.singlePress =")
    (temp / "headless.inc").write_text(broker[start:broker.index("UserButtonThread->initButton(userConfigNoScreen)", start)])
    buzzer = source("buzz/BuzzerFeedbackThread.cpp")
    (temp / "buzzer.inc").write_text(method(buzzer, "int BuzzerFeedbackThread::handleInputEvent("))
    status = source("modules/StatusLEDModule.cpp")
    start = status.index("    if (LORA_LED_state", status.index("// End the LoRa-RX flash"))
    (temp / "led.inc").write_text(method(status, "int StatusLEDModule::handleLoRaRx(") +
        "\nvoid StatusLEDModule::poll(){int my_interval=1000;\n" + status[start:status.index("#endif", start)] + "}\n")
    duration = source("modules/StatusLEDModule.h")
    (temp / "led-duration.inc").write_text(next(line for line in duration.splitlines() if "LORA_RX_LED_FLASH_MS =" in line))
    watchdog = source("watchdog/watchdogThread.cpp")
    (temp / "watchdog.inc").write_text(method(watchdog, "void WatchdogThread::feedDog(") +
                                       "\n" + method(watchdog, "bool WatchdogThread::setup("))
    power = source("modules/PowerStressModule.cpp")
    power_methods = method(power, "bool PowerStressModule::handleReceivedProtobuf(") + "\n" + method(power, "int32_t PowerStressModule::runOnce(")
    (temp / "power.inc").write_text(power_methods)
    opcodes = re.findall(r"case (meshtastic_PowerStressMessage_Opcode_\w+):", power_methods)
    (temp / "opcodes.inc").write_text(",\n".join(op for op in opcodes if not op.endswith("_UNSET")))
    gpio = source("modules/RemoteHardwareModule.cpp")
    (temp / "gpio.inc").write_text(gpio[gpio.index("#define NUM_GPIOS"):gpio.index("RemoteHardwareModule::RemoteHardwareModule()")] +
        method(gpio, "bool RemoteHardwareModule::handleReceivedProtobuf(") + "\n" + method(gpio, "int32_t RemoteHardwareModule::runOnce("))
    output = ""
    for flags in ([], ["-fshort-enums"]):
        for name in ("platform", "input"):
            binary = temp / name
            subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", *flags, "-I", temp, "-I", PROTO / "nanopb", "-I", APP / "src",
                            HERE / (name + ".cpp"), "-o", binary], check=True)
            output += capture([binary])
    return output


def require(output, marker, times):
    if output.count(marker) != times:
        raise RuntimeError(f"Expected {times} successful runs for {marker!r}")


def collect(stock, temp):
    rows = {}
    def row(module, kind, evidence, **details):
        if module in rows:
            raise ValueError(f"duplicate row {module}")
        rows[module] = {"kind": kind, "evidence": evidence, **details}
    def suite(name, *extra):
        path = [stock] if name == "stock_ui" else ["--stock", stock]
        return capture([sys.executable, TESTS / name / "run.py", *path, "--release", "2.8.2", *extra])

    edges = platform(stock, temp)
    for name, count, detail in [
        ("BuzzerFeedbackThread", 2560, "All 256 input codes across five buzzer modes and both encoder profiles select the stock sound."),
        ("StatusLEDModule", 12, "The exported LoRa RX LED follows stock flash cutoff across clock rollover."),
        ("WatchdogThread", 2, "Stock external watchdog initialization and one feed match the recorded GPIO edge and delay sequences."),
        ("PowerStressModule", 15, "Stock and ZT request the same local action for each supported bench opcode with synthetic time and hardware."),
        ("RemoteHardwareModule", 30, "Admitted GPIO reads, writes and watch reports match stock across three masks, including pin 63 and timer rollover.")]:
        require(edges, f"{name} {count} ", 2)
        row(name, "differential", detail, cases_per_abi=count)
    peripherals = suite("stock_peripherals")
    for name, marker, detail in [
        ("SerialModule", "PASS Serial receive modes", "Stock serial multiplexing, NMEA conversion and WS85 aggregation use captured UART data."),
        ("CannedMessageModule", "PASS Canned messages", "Stock message splitting, navigation, destination, channel, ACK and BEL match ZT for configured senders."),
        ("ExternalNotificationModule", "PASS External notification", "All 64 notification flag combinations, cancellation and cutoff match stock. Desktop libnotify requires Portduino."),
        ("DetectionSensorModule", "PASS Detection sensor", "The stock GPIO trigger, polarity, heartbeat and packet outcomes match captured Zephyr reads and sends."),
        ("AmbientLightingThread", "PASS Ambient lighting", "The stock RGB thread and ZT agree on enablement, refresh and synthetic RGB values.")]:
        require(peripherals, marker, 2)
        row(name, "differential", detail)
    for marker in ("PASS NMEAWPL.cpp", "PASS WS85", "PASS production extended notification"):
        require(peripherals, marker, 2)
    optional = suite("stock_optional")
    for marker in ("ReplyBot donor outcomes", "PASS mapped Zephyr", "PASS unmapped Zephyr", "PASS optional RF endpoint"):
        require(optional, marker, 2)
    for name, detail in [
        ("ReplyBotModule", "Both ABIs compare stock command parsing, reply addressing, channel, cooldown and output text."),
        ("GenericThreadModule", "Both ABIs compare the enabled development thread log and ten-second cadence against stock."),
        ("ReplyModule", "The opt-in responder matches the example donor payload and passes the fixture endpoint dispatch."),
        ("DropzoneModule", "Stock conditions, countdown and report formatting agree for simulated ADC and weather readings on mapped and unmapped profiles.")]:
        row(name, "differential", detail)
    audio = suite("stock_audio_pax", "--output", temp / "audio")
    require(audio, "audio RX length/mode comparisons", 2)
    require(audio, "Paxcounter solicited allocation", 2)
    audio_counts = re.findall(r"(\d+) audio RX length/mode comparisons", audio)
    if len(set(audio_counts)) != 1 or int(audio_counts[0]) <= 0:
        raise ValueError("Missing or inconsistent audio comparisons")
    row("AudioModule", "differential", "Captured Codec2 frame lengths, stock PTT transitions and queued audio sends match on a synthetic codec and I²S backend.", cases_per_abi=int(audio_counts[0]))
    row("PaxcounterModule", "differential", "Stock solicited replies and scheduled count reports agree across allocation failure, refresh, disabled and stale observations.")
    ui = suite("stock_ui")
    require(ui, "PASS 256 stock keyboard module input events", 1)
    require(ui, "PASS pinned VirtualKeyboard", 1)
    require(ui, "PASS keyboard compose/routing", 4)
    row("OnScreenKeyboardModule", "differential", "The stock keyboard module matches all 256 broker events, including preserving drafts on Back. VirtualKeyboard matches 88 key actions, 5,000 navigation steps and timeout.")
    row("GamesModule", "behavior", "The production display fixture enters, ticks, pauses and resumes all four games and tests score selection and clearing on two display sizes.")
    for name in ("Breakout", "ChirpyRunner", "Snake"):
        match = re.search(rf"^{name}: (\d+) pinned stock tests passed$", ui, re.M)
        if not match or int(match[1]) <= 0:
            raise ValueError(f"Missing stock tests for {name}")
        row(name, "differential", "The extracted core matches stock source. The unmodified game assertions run in the pinned Unity framework with its Arduino startup hook replaced on the host.", cases=int(match[1]))
    row("Tetris", "source", "The core matches the pinned source and the production display fixture exercises input, drop and scoring. The donor supplies no Tetris Unity suite.")
    require(edges, "SystemCommandsModule 65536 ", 2)
    require(edges, "InputBroker 5 ", 2)
    row("SystemCommandsModule", "differential", "All 65,536 event/key pairs select the same action as the stock handler. The display fixture checks physical confirmation and pairing isolation.", cases_per_abi=65536)
    row("InputBroker", "differential", "Five headless press, hold and multiclick outcomes match the stock button map. Linux Arcade Bonnet and ThinkNode-specific routing are outside the selected profiles.", cases_per_abi=5)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    pin = subprocess.check_output(["git", "-C", args.stock, "rev-parse", PIN + "^{commit}"], text=True).strip()
    if pin != PIN:
        raise ValueError("Unexpected upstream revision")
    with tempfile.TemporaryDirectory(prefix="zt-282-peripherals-") as tmp:
        rows = collect(args.stock.resolve(), Path(tmp))
    args.output.write_text(json.dumps({"stock_revision": PIN, "modules": rows, "hardware": False,
        "framework": {"Unity": {"revision": "cbcd08fa7de711053a3deec6339ee89cad5d2697", "suites": ["Snake", "Breakout", "ChirpyRunner"]},
                      "other_modules": "Production ZT code and extracted upstream handlers, exercised by project-owned host fixtures."},
        "scope": "Host software with synthetic clocks, framebuffers, codecs, radio sinks and GPIO. Physical peripherals and Linux desktop services are outside this run."}, indent=2) + "\n")

if __name__ == "__main__":
    main()
