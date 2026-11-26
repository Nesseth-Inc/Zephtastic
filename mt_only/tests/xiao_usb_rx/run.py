#!/usr/bin/env python3
"""Run the production boot/interrupt paths with the pinned Zephyr driver and a synthetic USB endpoint."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DRIVER_SHA = '28d9302f672bae5daf30fafe402d34db6d03964c87656d0204024f6497591c12'

def function(source, name):
    match = re.search(r'^static (?:int|void|bool) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise ValueError(name)
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('driver', type=Path, help='pinned zephyr/drivers/serial/serial_esp32_usb.c')
    args = parser.parse_args()
    driver = args.driver.read_bytes()
    if hashlib.sha256(driver).hexdigest() != DRIVER_SHA:
        raise SystemExit('USB driver does not match the pinned revision')
    app = (HERE.parents[1]/'src/main.cpp').read_text()
    assert 'pollUsbTxLocked(usb);' in app[app.index('static UsbBatch takeUsbBytes('):app.index('static int commitVerifiedPeer(')]
    names = ('fifo_read','fifo_fill','irq_rx_enable','irq_rx_ready','irq_tx_ready','irq_is_pending','irq_update')
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        (out/'driver.inc').write_text('\n'.join(function(driver.decode(), 'serial_esp32_usb_'+s) for s in names))
        (out/'app.inc').write_text('\n'.join(function(app,s) for s in ('serviceUsbIrqOnce','pollUsbTxLocked','usbInterrupt','startApplicationUsbRx')))
        for flags in (['-DCONFIG_SOC_ESP32S3=1','-DUSB_CHOSEN=1'], ['-DUSB_CHOSEN=0'],
                      ['-DCONFIG_SOC_ESP32S3=1','-DUSB_CHOSEN=0']):
            exe = out/'test'
            subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',
                            '-fsanitize=address,undefined',*flags,'-I',str(out),str(HERE/'endpoint.cpp'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
    print('PASS pre-arm and late RX, stalled and partially writable TX with ordered frames, original session admission and unaffected transports')

if __name__ == '__main__':
    main()
