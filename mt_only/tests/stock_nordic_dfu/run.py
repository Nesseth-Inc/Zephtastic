#!/usr/bin/env python3
"""Compile the real update backend, substituting only the memory-mapped peripherals."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
SOURCE = APP/'src/update/ZephyrUpdateBackend.cpp'

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    # The peripheral values use their nRF52 address range; flash is backed by
    # an array so the same real vector checks run on Darwin and Linux.
    code = SOURCE.read_text()
    raw = 'reinterpret_cast<const volatile uint32_t *>(uintptr_t(start))'
    assert code.count(raw) == 1
    (root/'backend.cpp').write_text(code.replace(raw, 'test_vectors(start)'))
    proto=APP.parent/'zephcore/bench/upstream-differential/source'
    (root/'meshtastic').symlink_to(proto,target_is_directory=True)
    for filename, content in {
        'zephyr/devicetree.h': '#define DT_NODELABEL(x) 1\n#define DT_NODE_EXISTS(x) 1\n#define DT_REG_ADDR(x) 0xf4000u\n#define DT_REG_SIZE(x) 0xc000u\n',
        'hal/nrf_power.h': '#include "peripheral.h"\n',
    }.items():
        dest=root/filename;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(content)
    for flags in ([],['-fshort-enums']):
        for enabled, model in [(False,0),(True,0),(True,127),(True,69)]:
            opt=['-DCONFIG_MT_NORDIC_DFU=1'] if enabled else []
            cmd=['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
                 '-fsanitize=address,undefined',*flags,*opt,'-DCONFIG_SOC_SERIES_NRF52=1',
                 '-DCONFIG_BOARD_HELTEC_T096=1',f'-DCONFIG_MT_HARDWARE_MODEL={model}','-I'+str(proto/'nanopb'),
                 '-I'+str(root),'-I'+str(HERE),'-I'+str(APP/'src/update'),
                 '-I'+str(APP/'tests/usb_bootloader_stubs'),str(root/'backend.cpp'),str(HERE/'cases.cpp'),'-o',str(root/'case')]
            subprocess.run(cmd,check=True)
            subprocess.run([str(root/'case')],check=True)
print('PASS T096 default-disabled and opt-in Nordic BLE/USB selector, vector validation, readback and cold reset in both enum ABIs')
