#!/usr/bin/env python3
"""Check the stock game cores against the pinned tree and exercise native ZT display input."""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("donor", type=Path)
parser.add_argument("--release", choices=("2.8.1", "2.8.2"), default="2.8.1")
args = parser.parse_args()
pin = "d5f16191ca28b01b5024f590f04a67f25d66f1b1" if args.release == "2.8.2" else "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
app = Path(__file__).resolve().parents[2]
games = app / "src/stock/ui/games"
cxx = shlex.split(os.environ.get("CXX", "c++"))
def donor(name):
    return subprocess.check_output(["git", "-C", args.donor, "show", f"{pin}:{name}"], text=True)
def run(argv):
    subprocess.run(argv, check=True)

with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    proto = app.parent / "zephcore/bench/upstream-differential/source"
    (work / "meshtastic").mkdir()
    for f in proto.glob("*.pb.h"):
        shutil.copyfile(f, work / "meshtastic" / f.name)
    (work / "modules").mkdir()
    (work / "modules/games").symlink_to(games)
    (work / "TestUtil.h").write_text("#include <cstdlib>\nstatic void initializeTestEnvironment(){}\n")
    (work / "unity.h").write_text("""#include <cassert>
#define UNITY_BEGIN() ((void)0)
#define UNITY_END() 0
#define RUN_TEST(f) f()
#define TEST_ASSERT_TRUE(v) assert(v)
#define TEST_ASSERT_FALSE(v) assert(!(v))
#define TEST_ASSERT_EQUAL_INT16(a,b) assert((a)==(b))
#define TEST_ASSERT_EQUAL_INT(a,b) assert((a)==(b))
#define TEST_ASSERT_EQUAL_UINT8(a,b) assert((a)==(b))
#define TEST_ASSERT_EQUAL_UINT16(a,b) assert((a)==(b))
#define TEST_ASSERT_EQUAL_UINT32(a,b) assert((a)==(b))
""")
    flags = [*cxx, "-std=c++17", "-fsanitize=address,undefined", "-I"+str(work)]
    virtual=donor("src/graphics/VirtualKeyboard.cpp")
    (work/"configuration.h").write_text("#include <stdint.h>\n")
    (work/"OLEDDisplay.h").write_text("class OLEDDisplay;\n")
    (work/"VirtualKeyboard.h").write_text(donor("src/graphics/VirtualKeyboard.h").replace("  private:", "  public:"))
    core = (virtual[virtual.index("namespace graphics"):virtual.index("void VirtualKeyboard::draw(")]
            + virtual[virtual.index("char VirtualKeyboard::getCharForKey"):virtual.rindex("#endif")])
    broker = donor("src/input/InputBroker.h")
    keyboard_module = donor("src/modules/OnScreenKeyboardModule.cpp")
    def method(source, signature):
        start = source.index(signature)
        return source[start:source.index("\n}", start) + 2]
    control = ("#include <memory>\nnamespace graphics {\n" +
               broker[broker.index("enum input_broker_event"):broker.index("// Which physical joystick")] +
               "\nstruct InputEvent {unsigned inputEvent;};\n"
               "struct OnScreenKeyboardModule {std::unique_ptr<VirtualKeyboard> keyboard;bool cancelled=false;"
               "void onCancel(){cancelled=true;}void handleInput(const InputEvent &);"
               "static bool processVirtualKeyboardInput(const InputEvent &,VirtualKeyboard *);};\n" +
               method(keyboard_module, "void OnScreenKeyboardModule::handleInput(") + "\n" +
               method(keyboard_module, "bool OnScreenKeyboardModule::processVirtualKeyboardInput(") + "\n}\n")
    compare = work/"keyboard.cpp"
    compare.write_text('#include "VirtualKeyboard.h"\n#define LOG_INFO(...) ((void)0)\n'
                       'static uint32_t now=0;static uint32_t millis(){return now;}\n'
                       'namespace graphics { struct Screen{enum {FOCUS_PRESERVE};void setFrames(int){}};'
                       'static Screen *screen=nullptr;}\n' + core + control
                       + (Path(__file__).parent/"keyboard_cases.inc").read_text())
    run([*flags,"-I"+str(app/"src"),str(compare),"-o",str(work/"keyboard")])
    run([str(work/"keyboard")])
    # Verify all bitmap bytes and table labels against the pinned source, including variation selectors.
    reference=donor("src/graphics/emotes.cpp")
    source=(app/"src/stock/ui/EmoteTable.inc").read_text()
    bitmap_pattern=r"const unsigned char (\w+)\[\] PROGMEM = \{([^}]+)\}"
    original={name:re.findall(r"0x[0-9a-fA-F]+",data) for name,data in re.findall(bitmap_pattern,reference)}
    copied={name:re.findall(r"0x[0-9a-fA-F]+",data) for data,name in re.findall(r"\{(0x[^}]+)\}, // (\w+)",source)}
    assert copied==original
    run([sys.executable, str(app/"scripts/pack-emotes.py"), str(app/"src/stock/ui/EmoteTable.inc"),
         str(work/"mt_emote_table.h")])
    samples = ",\n".join("{" + ",".join(values) + "}" for values in original.values())
    check = work/"packed.cpp"
    check.write_text('#include "stock/ui/EmotePicker.h"\n#include <cassert>\n'
                     + 'static constexpr uint8_t donor[][32]={' + samples + '};\n'
                     + 'int main(){for(unsigned i=0;i<sizeof(donor)/32;++i){uint8_t bytes[32];'
                       'mt::stock::ui::emotes::bitmap(i,bytes);assert(!memcmp(bytes,donor[i],32));}}')
    run([*flags, "-DMT_PACKED_EMOTES", "-I"+str(app/"src"), str(check), "-o", str(work/"packed")])
    run([str(work/"packed")])
    entries=re.findall(r'\{"([^"]+)",\s*(\w+),\s*\w+,\s*\w+\}',reference)
    assert [label for label,_ in entries]==re.findall(r'^    "([^"]+)"',source,re.M)
    index_rows=re.findall(r'^    \{(\d+),(\d+),(\d+)\}, // (\w+)',source,re.M)
    assert len(index_rows)==len(entries)
    offset=0
    for (label,glyph),(stored,size,index,name) in zip(entries,index_rows):
        n=len(bytes(label,"ascii").decode("unicode_escape").encode("utf-8"))
        assert int(stored)==offset and int(size)==n and list(original)[int(index)]==glyph==name
        offset+=n
    print(f"Emote picker: {len(entries)} labels and {len(copied)} XBM glyphs verified")
    for mode in ["Snake", "Tetris", "ChirpyRunner", "Breakout"]:
        for ext, marker in [("h", '#include "configuration.h"'), ("cpp", "#if HAS_SCREEN && BASEUI_HAS_GAMES")]:
            reference = donor(f"src/modules/games/{mode}.{ext}").split(marker)[0].rstrip()
            # The C++ header serves only Breakout's excluded Arduino joystick adapter.
            if mode=="Breakout" and ext=="cpp":
                reference=re.sub(r"^#include <cstring> // strcmp, for the (joystick-source test in handleInput|stick-source test in isPolledStickRepeat)\(\)$", "", reference, flags=re.M)
            actual = (games / f"{mode}.{ext}").read_text().split("\n", 2)[2].rstrip()
            assert reference == actual, f"core drift in {mode}.{ext}"
    for mode, test in [("Snake","snake"),("Breakout","breakout"),("ChirpyRunner","chirpy")]:
        cases = donor(f"test/test_{test}/test_main.cpp")
        src = work / f"{test}.cpp"
        src.write_text(cases + "\nint main(){setup();}\n")
        binary = work / test
        run([*flags, str(src), str(games / f"{mode}.cpp"), "-o", str(binary)])
        run([str(binary)])
        print(f"{mode}: {len(re.findall(r'RUN_TEST[(]', cases))} pinned stock tests passed")
    body = (app / "src/ui/Display.cpp").read_text().split("namespace mt::ui {", 1)[1]
    fixture = (app / "tests/display_runtime_fixture.cpp").read_text().split("int main(){using namespace mt::ui;")[0]
    source = work / "ui.cpp"
    source.write_text(fixture.replace("// INCLUDE_ACTUAL_DISPLAY_BODY", "namespace mt::ui {"+body)
                      + (Path(__file__).parent/"display_cases.inc").read_text())
    for width in [160,240]:
        for enum_flag in [[], ["-fshort-enums"]]:
            binary = work / "ui"
            run([*flags, *enum_flag, "-Wall", "-Wextra", "-Werror", "-Wno-missing-braces", "-Wno-missing-field-initializers", "-I"+str(app/"src"), "-I"+str(proto/"nanopb"),
                 "-I"+str(app.parent/"zephcore/lib/monocypher"), "-I"+str(app.parent/"zephcore/adapters/bench/vendor"),
                 "-DCONFIG_MT_DISPLAY=1","-DCONFIG_MT_BASEUI_GAMES=1","-DCONFIG_MT_BASEUI_EMOTES=1","-DCONFIG_MT_ONSCREEN_KEYBOARD=1",
                 '-DCONFIG_MT_UI_DEVICE_LABEL="HELTEC T096"',f"-DCONFIG_MT_UI_WIDTH={width}",
                 f"-DCONFIG_MT_UI_HEIGHT={80 if width == 160 else 135}","-DCONFIG_MT_UI_MONO=0","-DCONFIG_MT_UI_STRIP_ROWS=16",
                 str(source), *map(str,sorted(games.glob("*.cpp"))), "-o", str(binary)])
            run([str(binary)])
