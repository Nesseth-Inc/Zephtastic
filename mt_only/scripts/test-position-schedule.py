#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Native pure scheduling policy with a separately compiled pinned-stock oracle.

Supply the literal stock reference directories; they are inputs, never fetched.
Only a fixture clock/send and precomputed movement are modeled. No GeoCoord,
role/network scaling, channel consent, current fix or radio behavior is claimed.
"""
if not __debug__:raise RuntimeError('Optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
app=Path(__file__).resolve().parents[1];root=app.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-reference',required=True,type=Path);p.add_argument('--throttle-reference',required=True,type=Path);a=p.parse_args()
out=a.output.resolve();ref=a.stock_reference.resolve();throttle=a.throttle_reference.resolve()
if out.exists()or out.is_relative_to(root):p.error('fresh output outside source required')
sha=lambda raw:hashlib.sha256(raw).hexdigest()
expected={'PositionModule.cpp':'23df14aaabccd94fe6f337a0248ec58be96ee383b2b41aa10d2fcc95e3af7066','PositionModule.h':'0d79d2f624acefbb67ae40a2339d43173666a6ad4994462b2b55facc0d6c4fd4','Default.cpp':'3a5492e6026dcbe9d86eefb3ec58b8f69432388496bc8ce3767d594b0e369f4f','Default.h':'a4211f1e26e7696a4378e1d8aa5203571d700e5b2de7be6e55cd5dab69c96fd7'}
for name,digest in expected.items():assert sha((ref/name).read_bytes())==digest
reference_inputs={str(ref/n):h for n,h in expected.items()}
for n in('Throttle.h','Throttle.cpp'):reference_inputs[str(throttle/n)]=sha((throttle/n).read_bytes())
def function(text,start):
    begin=text.index(start);opening=text.index('{',begin);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]+'\n'
position=(ref/'PositionModule.cpp').read_text();defaults=(ref/'Default.cpp').read_text();throttle_cpp=(throttle/'Throttle.cpp').read_text()
assert 'lastGpsSend == 0 || msSinceLastSend >= effectiveIntervalMs' in position
extracts=[function(defaults,'static inline uint32_t secondsToMsClamped('),function(defaults,'uint32_t Default::getConfiguredOrDefaultMs(uint32_t configuredInterval, uint32_t defaultInterval)'),function(defaults,'uint32_t Default::getConfiguredOrDefault(uint32_t configured, uint32_t defaultValue)'),function(throttle_cpp,'bool Throttle::isWithinTimespanMs('),function(position,'uint32_t PositionModule::effectiveBroadcastIntervalMs('),function(position,'void PositionModule::trySmartBroadcast(')]
out.mkdir(parents=True);shutil.copyfile(throttle/'Throttle.h',out/'stock-Throttle.h')
prefix=r'''#include <cstdint>
#include <cmath>
#include "stock-Throttle.h"
static uint32_t fixtureNow;
namespace Time { uint32_t getMillis(){return fixtureNow;} uint32_t skipZero(uint32_t now){return now?now:1;} }
template<class... T> void fixtureLog(const char*,T...){}
#define LOG_DEBUG(...) fixtureLog(__VA_ARGS__)
#define LOG_DEBUG_GPS(...) fixtureLog(__VA_ARGS__)
namespace Default { uint32_t getConfiguredOrDefaultMs(uint32_t,uint32_t);uint32_t getConfiguredOrDefault(uint32_t,uint32_t); }
struct meshtastic_PositionLite {int32_t latitude_i=30,longitude_i=40;};
constexpr unsigned meshtastic_PortNum_POSITION_APP=3;
struct History {void setLastSentToMesh(unsigned){}};static History *transmitHistory=nullptr;
struct SmartPosition {float distanceTraveled;uint32_t distanceThreshold;bool hasTraveledOverThreshold;};
class PositionModule {
public:
 uint32_t lastGpsSend=0,minimumTimeThreshold=300000;int32_t lastGpsLatitude=0,lastGpsLongitude=0;
 struct {uint32_t timestamp=0;} localPosition;
 SmartPosition movement{};bool accepted=false;unsigned attempts=0;
 SmartPosition getDistanceTraveledSinceLastSend(meshtastic_PositionLite){return movement;}
 bool sendOurPosition(){++attempts;return accepted;}
 static uint32_t effectiveBroadcastIntervalMs(uint32_t,bool,uint32_t);
 void trySmartBroadcast(const meshtastic_PositionLite&,uint32_t);
};
'''
suffix=r'''
uint32_t stockInterval(uint32_t configured,uint32_t fallback){return Default::getConfiguredOrDefaultMs(configured,fallback);}
uint32_t stockDistanceThreshold(uint32_t configured){return Default::getConfiguredOrDefault(configured,100);}
bool stockPeriodic(uint32_t lastGpsSend,uint32_t now,uint32_t regular,bool stationary){
 const uint32_t msSinceLastSend=now-lastGpsSend;
 const uint32_t effectiveIntervalMs=PositionModule::effectiveBroadcastIntervalMs(regular,stationary,21960000);
 return lastGpsSend == 0 || msSinceLastSend >= effectiveIntervalMs;
}
bool stockSmart(uint32_t last,uint32_t now,uint32_t minimum,float moved,uint32_t threshold,bool accepted,uint32_t &stamp,int32_t &lat,int32_t &lon){
 PositionModule m;m.lastGpsSend=last;m.minimumTimeThreshold=minimum;m.accepted=accepted;
 if(last){m.lastGpsLatitude=10;m.lastGpsLongitude=20;}
 m.movement={moved,threshold,moved>=threshold};fixtureNow=now;m.trySmartBroadcast(meshtastic_PositionLite{},now);
 stamp=m.lastGpsSend;lat=m.lastGpsLatitude;lon=m.lastGpsLongitude;return m.attempts!=0;
}
'''
oracle=prefix+'\n'.join(extracts)+suffix;(out/'stock-oracle.cpp').write_text(oracle)
inputs=[app/'src/gnss/PositionSchedule.h',app/'tests/position_schedule_tests.cpp',Path(__file__).resolve()]
before={str(x.relative_to(root)):sha(x.read_bytes())for x in inputs}
cxx=shlex.split(os.environ.get('CXX','clang++'));flags=['-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I'+str(app/'src')]
rows=[];result={'status':'FAIL','hardware':False,'network':False,'ASAN_UBSAN':True,'source_sha256':before,'literal_reference_sha256':reference_inputs,'stock_ref':'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','stock_oracle_sha256':sha(oracle.encode()),'literal_extracted_function_sha256':[sha(x.encode())for x in extracts],'oracle_fixture_limits':'Clock/send/movement are modeled; literal stock smart/default/clamp/stationary/Throttle bodies execute with the new floor verified by the 2.8.2 stock_node_position differential. GeoCoord, network scaling, roles, consent and current fix are caller-owned.','variants':rows}
try:
    (out/'compiler.txt').write_text(subprocess.run([*cxx,'--version'],capture_output=True,text=True,check=True,timeout=15).stdout)
    for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
        cmd=[*cxx,*flags,*extra,str(app/'tests/position_schedule_tests.cpp'),str(out/'stock-oracle.cpp'),'-o',str(out/label)]
        with(out/(label+'.build.log')).open('x')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        execute=[str(out/label)]
        with(out/(label+'.test.log')).open('x')as log:subprocess.run(execute,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
        lines=(out/(label+'.test.log')).read_text().splitlines();assert sum(x.startswith('PASS ')for x in lines)==7 and lines[-1]=='POSITION_SCHEDULE_GROUPS 7'
        rows.append({'variant':label,'groups':7,'compile_command':cmd,'execution_command':execute,'binary_sha256':sha((out/label).read_bytes()),'test_log_sha256':sha((out/(label+'.test.log')).read_bytes())})
    assert all(sha((root/n).read_bytes())==h for n,h in before.items())and all(sha(Path(n).read_bytes())==h for n,h in reference_inputs.items())
    result.update(status='PASS',source_stable=True)
finally:(out/'results.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print(json.dumps({'status':result['status'],'executions':len(rows),'groups_per_enum':7,'hardware':False,'network':False}))
