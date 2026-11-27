"""Extract the matched 2.8.2 game host methods; hardware and screen sinks live in the C++ fixture."""
import re


def generate(work, donor, method):
    def game(filename):
        return donor("src/modules/games/" + filename)

    broker = donor("src/input/InputBroker.h")
    header = game("GamesModule.h")
    scores = game("HighScoreTable.h")
    start = scores.index("class HighScoreTableBase")
    base = scores[start:scores.index("\n};", start) + 3]
    state = header.index("enum GamesUiState")
    constants = next(line for line in header.splitlines() if " INACTIVITY_TIMEOUT_MS =" in line)
    banner = donor("src/graphics/Screen.h").split("struct BannerOverlayOptions {", 1)[1].split("};", 1)[0]
    duration = re.search(r"uint32_t durationMs = (\d+);", banner).group(1)
    renderer = donor("src/graphics/draw/NotificationRenderer.cpp")
    options = renderer.index("optionWidths[i] = display->getStringWidth")
    first = renderer.index("        if (inEvent.inputEvent == INPUT_BROKER_UP ", options)
    last = renderer.index("\n    } else {", first)
    expiry = renderer.index("    // 0 means", renderer.index("void NotificationRenderer::drawBannercallback"))
    end = renderer.index("    // Compact panels:", expiry)
    (work / "game-banner-input.inc").write_text(renderer[expiry:end] + renderer[first:last])
    throttle = donor("src/mesh/Throttle.h")
    start = throttle.index("    static bool deadlinePassedAt(")
    (work / "game-deadline.inc").write_text(throttle[start:throttle.index("\n    }", start)+6])
    (work / "game-types.inc").write_text("\n".join([
        broker[broker.index("enum input_broker_event"):broker.index("#ifdef ARCH_PORTDUINO")],
        header[state:header.index("};", state)+2], base, constants,
        "constexpr uint32_t confirmDuration = " + duration + ";",
    ]))
    first = scores.index("    uint32_t scoreAt(uint8_t i) const override")
    clear = scores.index("    void clear() override", first)
    (work / "score-methods.inc").write_text(scores[first:scores.index("\n", clear)])
    adapter = game("Tetris.h")
    first = adapter.index("    void start(uint32_t seed) override")
    (work / "tetris-adapter.inc").write_text(adapter[first:adapter.index("    int32_t tickIntervalMs()", first)])
    tetris = game("Tetris.cpp")
    (work / "tetris-input.inc").write_text(method(tetris, "int32_t Tetris::tickIntervalMs(") + "\n"
                                            + method(tetris, "void Tetris::handleInput("))
    host = game("GamesModule.cpp")
    methods = ["void GamesModule::" + name + "(" for name in
               ("launchGame", "startPlaying", "goHome", "enterGameOver", "promptForInitials",
                "recordHighScore", "exitToIdle", "kickTick")]
    methods += ["int32_t GamesModule::runOnce(", "int GamesModule::handleInputEvent("]
    (work / "games-methods.inc").write_text("\n".join(method(host, name) for name in methods))
