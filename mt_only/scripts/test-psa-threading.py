#!/usr/bin/env python3
"""Offline shared PSA threading integration: real pinned crypto and POSIX kernel semantic double."""
import argparse, hashlib, json, os, re, shlex, shutil, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT));import host_test_toolchain as host
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--tf-psa',required=True,type=Path)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--t096-commands',type=Path)
p.add_argument('--xiao-commands',type=Path)
a=p.parse_args();dep=a.tf_psa.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'RESULTS.json').unlink(missing_ok=True)
test=ROOT/'mt_only/tests/psa-threading';port=ROOT/'mt_only/src/pki/psa-threading'
pin=next(x['commit'] for x in json.loads((ROOT/'mt_only/dependency-lock.json').read_text()) if x['path']=='modules/crypto/tf-psa-crypto')
assert subprocess.check_output(['git','-C',str(dep),'rev-parse','HEAD'],text=True).strip()==pin
subprocess.run(['git','-C',str(dep),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
files=sorted(list(test.rglob('*'))+list(port.rglob('*'))+[Path(__file__).resolve(),ROOT/'mt_only/Kconfig',ROOT/'mt_only/pki-groundwork.cmake',ROOT/'mt_only/PKI-PSA-THREADING-M11.md',ROOT/'mt_only/boards/xiao-wifi-mqtt.conf'])
def hashes():return {str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files if f.is_file()}
before=hashes();commands=[]

# Guard the production selection contract without pretending that a host
# harness is a Zephyr Kconfig/CMake evaluation. The target build remains the
# integration proof for these conditions.
kconfig=(ROOT/'mt_only/Kconfig').read_text()
cmake=(ROOT/'mt_only/pki-groundwork.cmake').read_text()
wifi=(ROOT/'mt_only/boards/xiao-wifi-mqtt.conf').read_text()
assert 'select MT_SHARED_MBEDTLS_THREADING' in kconfig
gateway_block=kconfig.split('config MT_MQTT_GATEWAY\n',1)[1].split('\nconfig ',1)[0]
assert 'select MT_SHARED_MBEDTLS_THREADING' in gateway_block
assert 'default "mt_pki_psa_threading_config.h" if MT_SHARED_MBEDTLS_THREADING' in kconfig
assert 'default n' in kconfig[kconfig.index('config MT_SHARED_MBEDTLS_THREADING'):]
assert 'CONFIG_MT_SHARED_MBEDTLS_THREADING' not in wifi  # selected by MT_MQTT_GATEWAY, not a board override
assert 'CONFIG_MBEDTLS_USER_CONFIG_ENABLE' in cmake
assert 'CONFIG_MBEDTLS_USER_CONFIG_FILE STREQUAL "mt_pki_psa_threading_config.h"' in cmake
assert 'CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE STREQUAL "mt_pki_psa_threading_config.h"' in cmake
assert 'CONFIG_KERNEL_INIT_PRIORITY_DEFAULT LESS_EQUAL 39' in cmake
assert 'CONFIG_MULTITHREADING' in cmake and 'CONFIG_MBEDTLS_INIT' in cmake
assert 'CONFIG_MT_SHARED_MBEDTLS_THREADING' in (port/'mt_pki_psa_threading_config.h').read_text()

def run(cmd,log):
    commands.append(cmd)
    with (out/log).open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=180,
                    # LeakSanitizer cannot enumerate sibling threads when the
                    # runner's /proc access is restricted. AddressSanitizer
                    # and UBSan stay enabled; only the unsupported leak pass is
                    # disabled for this deliberately multithreaded executable.
                    env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1',
                         'ASAN_OPTIONS':'halt_on_error=1:detect_leaks=0'})
    if r.returncode:raise RuntimeError((out/log).read_text()[-10000:])
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
       '-DCONFIG_MT_SHARED_MBEDTLS_THREADING=1','-DCONFIG_MULTITHREADING=1','-DCONFIG_MBEDTLS_INIT=1','-DCONFIG_KERNEL_INIT_PRIORITY_DEFAULT=40',
       '-I'+str(test),'-I'+str(port)]
build=out/'psa-library-make';config=test/'config.h'
run(['cmake','-S',str(dep),'-B',str(build),'-G','Unix Makefiles','-DENABLE_TESTING=OFF','-DENABLE_PROGRAMS=OFF','-DGEN_FILES=OFF',
     '-DCMAKE_C_COMPILER='+shutil.which(host.cc()[0]),'-DTF_PSA_CRYPTO_CONFIG_FILE='+str(config),'-DCMAKE_BUILD_TYPE=Debug',
     '-DCMAKE_C_FLAGS='+shlex.join(flags)],'configure.log')
run(['cmake','--build',str(build),'--parallel','4'],'build.log')
lib=build/'core/libtfpsacrypto.a'
flags+=['-DTF_PSA_CRYPTO_CONFIG_FILE="'+str(config)+'"','-I'+str(dep/'include'),'-I'+str(dep/'drivers/builtin/include'),'-I'+str(dep/'dispatch/include')]
run([*host.cc(),'-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',*flags,
     str(port/'ZephyrPsaThreading.c'),str(test/'kernel.c'),str(test/'tests.c'),str(lib),'-o',str(out/'tests')],'compile.log')
run([str(out/'tests')],'tests.log')
probes={}
for name,path in [('t096',a.t096_commands),('xiao',a.xiao_commands)]:
    if not path:continue
    entries=json.loads(path.read_text());dest=out/(name+'-target');dest.mkdir(exist_ok=True)
    records=[]
    suffixes=['platform/threading.c','core/psa_crypto_slot_management.c','bt_crypto_psa.c','host/ecc.c','zephyr_init.c']
    initentry=next(x for x in entries if x['file'].endswith('zephyr_init.c'))
    for suffix in suffixes+['ZephyrPsaThreading.c']:
        entry=initentry if suffix=='ZephyrPsaThreading.c' else next(x for x in entries if x['file'].endswith(suffix))
        args=entry.get('arguments') or shlex.split(entry['command']);base=[];skip=False
        for x in args:
            if skip:skip=False;continue
            if x=='-o':skip=True;continue
            if x=='-c' or x==entry['file']:continue
            base.append(x)
        # Compile-only overlay on immutable prior build commands. Actual final
        # CMake/Kconfig propagation and linked init order need root full builds.
        original=next(Path(x) for x in base if x.endswith('/zephyr/autoconf.h'))
        shadow=dest/'generated/zephyr/autoconf.h';shadow.parent.mkdir(parents=True,exist_ok=True)
        configtext=original.read_text()
        def define(text,symbol,value):
            line=f'#define {symbol} {value}'
            pattern=rf'(?m)^(?:#define {re.escape(symbol)} .*|/\* #undef {re.escape(symbol)} \*/)$'
            if re.search(pattern,text):return re.sub(pattern,lambda _:line,text)
            return text+'\n'+line+'\n'
        for symbol in ('CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE','CONFIG_MBEDTLS_USER_CONFIG_FILE'):
            configtext=define(configtext,symbol,'"mt_pki_psa_threading_config.h"')
        for symbol in ('CONFIG_MT_SHARED_MBEDTLS_THREADING','CONFIG_MBEDTLS_USER_CONFIG_ENABLE','CONFIG_TF_PSA_CRYPTO_USER_CONFIG'):
            configtext=define(configtext,symbol,'1')
        shadow.write_text(configtext)
        base=[str(shadow) if x==str(original) else x for x in base]
        base[1:1]=['-I'+str(dest/'generated'),'-I'+str(port)]
        source=port/suffix if suffix=='ZephyrPsaThreading.c' else Path(entry['file'])
        obj=dest/(source.stem+'.o');cmd=[*base,'-Werror','-fstack-usage','-c',str(source),'-o',str(obj)]
        commands.append(cmd)
        r=subprocess.run(cmd,cwd=entry['directory'],text=True,capture_output=True,timeout=60)
        obj.with_suffix('.log').write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError(r.stderr)
        cmd=[*base,'-E','-dM',str(source)];commands.append(cmd)
        macros=subprocess.check_output(cmd,cwd=entry['directory'],text=True,timeout=60)
        assert '#define MBEDTLS_THREADING_C ' in macros and '#define MBEDTLS_THREADING_ALT ' in macros
        assert '#define MBEDTLS_THREADING_PTHREAD ' not in macros
        obj.with_suffix('.macros.txt').write_text(macros)
        record={'source':str(source),'object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),
                'macros_sha256':hashlib.sha256(macros.encode()).hexdigest(),
                'stack_usage':obj.with_suffix('.su').read_text().splitlines()}
        if source.stem in ('ZephyrPsaThreading','zephyr_init'):
            readelf=base[0].rsplit('-',1)[0]+'-readelf'
            sections=subprocess.check_output([readelf,'-SW',str(obj)],text=True)
            obj.with_suffix('.sections.txt').write_text(sections)
            priority=39 if source.stem=='ZephyrPsaThreading' else 40
            assert '.z_init_POST_KERNEL_P_'+str(priority)+'_' in sections
            record['init_priority']=priority
        records.append(record)
    probes[name]={'status':'COMPILE_ONLY','compile_commands_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                  'configuration':'explicit overlay selecting the identical shared Zephyr Mbed TLS and TF-PSA user config; final Kconfig/CMake/link validation pending',
                  'objects':records}
assert before==hashes(),'source changed during evidence run'
result={'status':'PASS','scope':'shared Zephyr Mbed TLS/TF-PSA source selection contract plus production ALT and actual pinned PSA; POSIX kernel semantic double, no target timing/physical qualification',
        'target_probes':probes,'source_stable':True,'source_hashes':before,'psa_commit':pin,'psa_library_sha256':hashlib.sha256(lib.read_bytes()).hexdigest(),
        'summary':(out/'tests.log').read_text().strip(),'no_devices':True,'no_persistent_credentials':True,'no_rng_fallback':True}
(out/'RESULTS.json').write_text(json.dumps(result,indent=2)+'\n');(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
print(result['summary'])
