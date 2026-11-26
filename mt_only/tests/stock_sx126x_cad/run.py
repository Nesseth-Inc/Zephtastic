#!/usr/bin/env python3
"""Run the real CAD command, wait and IRQ handoff using a small SX126x bus model."""
from pathlib import Path
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
DRIVER=HERE.parents[1]/'dependency-overlay/zephyr/drivers/lora/native/sx126x/sx126x.c'
def body(source,signature):
    start=source.index(signature+'\n{')
    pos=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[pos]=='{')-(source[pos]=='}')
        pos+=1
    return source[start:pos]
s=DRIVER.read_text()
irq=body(s,'static int sx126x_handle_irq_status(const struct device *dev, uint16_t irq_status)')
assert irq.index('sx126x_lbt_irq(dev, irq_status);')<irq.index('sx126x_handle_irq_rx_done(dev, irq_status);')
assert 'dio1_mask |= SX126X_IRQ_HEADER_ERR;' in s
assert 'const bool handoff = was_rx && data->rx_cb && !data->rx_duty_cycle_enabled;' in s
assert 'sx126x_lora_cad_mode(dev, timeout, handoff);' in s
methods=[
    'static void sx126x_lbt_irq(const struct device *dev, uint16_t flags)',
    'static int sx126x_do_cad(const struct device *dev, struct sx126x_data *data)',
    'static int sx126x_lora_cad_mode(const struct device *dev, k_timeout_t timeout, bool handoff)',
]
with tempfile.TemporaryDirectory() as tmp:
    temp=Path(tmp)
    (temp/'driver.inc').write_text('\n'.join(body(s,m) for m in methods))
    hal=DRIVER.with_name('sx126x_hal_common.c').read_text()
    (temp/'busy.inc').write_text(body(hal,'int sx126x_hal_wait_busy(const struct device *dev, uint32_t timeout_ms)'))
    def section(start, end):
        a=s.index(start);return s[a:s.index(end,a)].strip()
    (temp/'agc.inc').write_text(section('static bool sx126x_calibrate_joined', '#endif')+'\n'
        +section('static int sx126x_recv_async_common','static int sx126x_lora_recv_async')+'\n'
        +section('int sx126x_recv_after_agc', '#endif'))
    main=(HERE.parents[1]/'src/main.cpp').read_text()
    restoring=main[main.index('  int restore() override {'):main.index('  int stop() override {',main.index('  int restore() override {'))]
    paused=main[main.index('  int stopForStorage() override {'):main.index('  int health() override',main.index('  int stopForStorage() override {'))]
    (temp/'agc-owner.inc').write_text(restoring+'\n'+paused)
    for abi in ([],['-fshort-enums']):
        exe=temp/'test'
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',*abi,'-fsanitize=address,undefined','-I',tmp,str(HERE/'cases.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        for cc,src,std in [('clang','agc.c','c11'),('clang++','agc-clock.cpp','c++17'),('clang++','agc-owner.cpp','c++17')]:
            subprocess.run([cc,'-std='+std,'-Wall','-Wextra','-Werror',*abi,'-fsanitize=address,undefined',
                            '-I',tmp,str(HERE/src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
        for flags in (['-DCONFIG_MT_NORMAL_ENDPOINT=1','-DTEST_CAPPED=1'],
                      ['-DTEST_CAPPED=0'], ['-DCONFIG_MT_NORMAL_ENDPOINT=1','-DCONFIG_ZEPHCORE_DUAL_RX_BENCH=1','-DTEST_CAPPED=0']):
            compiler=['clang','-std=gnu11','-Wall','-Wextra','-Werror',*abi,'-fsanitize=address,undefined',
                      '-DCONFIG_MT_ONLY=1',*flags,'-I',tmp,str(HERE/'busy.c'),'-o',str(exe)]
            subprocess.run(compiler,check=True)
            subprocess.run([str(exe)],check=True)
print('PASS CAD command encoding, normal wait/IRQ adoption, busy/clear/damaged/timeout transitions, post-init BUSY bound, joined AGC resume and preserved calibration in both enum ABIs')
