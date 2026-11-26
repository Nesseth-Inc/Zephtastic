"""Compile the real SHT/OPT producer translation unit with paired node/bus/address fixtures."""
import re
import subprocess

def check_owner(host, flags, out, app, here):
    source=app/'src/stock/telemetry/accessory/ZephyrAccessories.cpp'
    prefix=out/'owner-includes'
    (prefix/'zephyr').mkdir(parents=True,exist_ok=True)
    hdr=(here/'mocks/zephyr/devicetree.h').read_text()
    # Reproduce the compiled node enumeration and equality properties of two devicetree nodes.
    # The independent bus and node identities distinguish a second controller from a second node.
    candidates=set(re.findall(r'DT_FOREACH_STATUS_OKAY\(\s*(\w+)\s*,',source.read_text()))
    body=['#define DT_FOREACH_STATUS_OKAY(c,fn) OWN_ENUM_##c(fn)']
    for name in sorted(candidates):
        body.append('#define OWN_ENUM_'+name+'(fn) '+('fn(zt_opt3001)' if name=='zephtastic_opt3001' else ''))
    hdr=hdr.replace('#define DT_FOREACH_STATUS_OKAY(c,fn)','\n'.join(body))
    hdr=hdr.replace('#define DT_SAME_NODE(a,b) 1','#define DT_SAME_NODE(a,b) ((a)==(b))')
    hdr=hdr.replace('#define DT_BUS(n) 0','#define DT_BUS(n) (n==zt_opt3001?OPT_BUS:0)')
    for n in (40,44,45,70):
        hdr=hdr.replace(f'#define status_zt_sht{n} 1',f'#define status_zt_sht{n} (SHT_ADDR==0x{n})')
    hdr+='\nenum { zt_sht40,zt_sht44,zt_sht45,zt_sht70,zt_opt3001 };\n#define addr_zt_opt3001 OPT_ADDR\n'
    (prefix/'zephyr/devicetree.h').write_text(hdr)
    for sht,opt,bus,allowed in [(0x44,0x44,0,False),(0x45,0x45,0,False),
                                (0x44,0x45,0,True),(0x45,0x44,0,True),(0x44,0x44,1,True),(0x45,0x45,1,True)]:
        proc=subprocess.run([*host.cxx(),'-std=c++17','-fsyntax-only','-I'+str(prefix),*flags,
            '-DCONFIG_MT_OPT3001=1',f'-DSHT_ADDR={sht}',f'-DOPT_ADDR={opt}',f'-DOPT_BUS={bus}',source],
            capture_output=True,text=True)
        if allowed:
            assert proc.returncode==0,proc.stderr
        else:
            assert proc.returncode and 'Disable the other owner of this SHT address' in proc.stderr,proc.stderr
    print('PASS real SHT/OPT3001 producer rejects both shared-address conflicts and accepts separate addresses/controllers')
