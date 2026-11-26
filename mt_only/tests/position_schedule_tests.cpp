// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/PositionSchedule.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>
using Schedule=mt::PositionSchedule;
using Kind=Schedule::Kind;
using Parameters=Schedule::Parameters;
// These functions are compiled separately from literal pinned stock bodies.
uint32_t stockInterval(uint32_t,uint32_t);
uint32_t stockDistanceThreshold(uint32_t);
bool stockPeriodic(uint32_t,uint32_t,uint32_t,bool);
bool stockSmart(uint32_t,uint32_t,uint32_t,float,uint32_t,bool,uint32_t&,int32_t&,int32_t&);
static unsigned groups;
static void pass(const char *label){++groups;std::printf("PASS %s\n",label);}
int main(){
  {
    // Snapshot values come from pinned Default.cpp, not the policy helper.
    struct Snapshot{uint32_t configured,fallback,expected;};
    const Snapshot cases[]={{0,3600,3600000},{0,43200,43200000},{0,300,300000},{1,300,1000},
      {2147483,300,2147483000u},{2147484,300,2147483647u},{UINT32_MAX,300,2147483647u}};
    for(const auto &c:cases)assert(stockInterval(c.configured,c.fallback)==c.expected&&Schedule::secondsToMs(c.configured,c.fallback)==c.expected);
    for(uint32_t input:{0u,1u,99u,100u,UINT32_MAX})assert(Schedule::distanceThreshold(input)==stockDistanceThreshold(input));
    pass("literal stock default/clamp snapshots join Companion/router3600/43200s and smart300s/100m");
  }
  {
    struct Snapshot{uint32_t last,now,regular;bool stationary,expected;};
    const Snapshot cases[]={{0,0,3600000,false,true},{0,1,3600000,true,true},
      {1,3600000,3600000,false,false},{1,3600001,3600000,false,true},
      {1,3600001,3600000,true,false},{1,21600000,3600000,true,false},{1,21600001,3600000,true,false},{1,21960000,3600000,true,false},{1,21960001,3600000,true,true},
      {1,21600001,43200000,true,false},{1,43200001,43200000,true,true},
      {0xfffffff0u,983,1000,false,false},{0xfffffff0u,984,1000,false,true}};
    for(const auto &c:cases){Schedule s;if(c.last)assert(s.stampSuccess(Kind::Periodic,c.last,10,20));Parameters p;p.regularIntervalMs=c.regular;p.stationary=c.stationary;
      assert(stockPeriodic(c.last,c.now,c.regular,c.stationary)==c.expected&&s.periodicDue(c.now,p)==c.expected);
    }
    pass("first periodic is immediate; stationary6h floor, longer router interval, inclusive boundary and uint32 rollover match stock");
  }
  {
    // No previous-fix-valid flag is imposed: stock can compare first position
    // to its initial0/0 baseline through the smart-only entry point.
    Schedule initial;assert(initial.lastSendMs()==0&&initial.lastLatitude()==0&&initial.lastLongitude()==0);
    struct Snapshot{uint32_t last,now,min;float moved;uint32_t distance;bool expected;};
    const Snapshot cases[]={{0,0,300000,1000,100,false},{0,299999,300000,1000,100,false},{0,300000,300000,100,100,true},
      {1000,300999,300000,100,100,false},{1000,301000,300000,99.99f,100,false},{1000,301000,300000,100,100,true},
      {1000,301000,300000,100.01f,100,true},{1000,301000,300000,0,100,false},
      {0xfffffff0u,983,1000,100,100,false},{0xfffffff0u,984,1000,100,100,true}};
    for(const auto &c:cases){Schedule s;if(c.last)assert(s.stampSuccess(Kind::Periodic,c.last,10,20));Parameters p;p.smartEnabled=true;p.smartMinimumIntervalMs=c.min;p.smartMinimumDistanceM=c.distance;
      uint32_t stamp=123;int32_t lat=123,lon=456;const bool oracle=stockSmart(c.last,c.now,c.min,c.moved,c.distance,false,stamp,lat,lon);
      assert(oracle==c.expected&&s.smartDue(c.now,p,c.moved)==c.expected&&s.decide(c.now,p,c.moved,false)==(c.expected?Kind::Smart:Kind::None));
      assert(stamp==c.last&&s.lastSendMs()==c.last);
    }
    pass("independent literal stock smart function snapshots cover first-send, movement/time equality, stationary fixes and rollover");
  }
  {
    Schedule s;Parameters p;p.smartEnabled=true;p.smartMinimumIntervalMs=Schedule::MaxIntervalMs;p.smartMinimumDistanceM=UINT32_MAX;
    assert(s.decide(0,p,0)==Kind::Periodic);assert(s.stampSuccess(Kind::Periodic,1,10,20));
    assert(s.decide(3600001,p,0)==Kind::Periodic);p.smartEnabled=false;assert(s.decide(3600001,p,std::numeric_limits<double>::quiet_NaN())==Kind::Periodic);
    p.smartEnabled=true;p.stationary=true;p.smartMinimumIntervalMs=300000;p.smartMinimumDistanceM=100;
    assert(s.decide(300001,p,100)==Kind::Smart);assert(s.decide(21600001,p,100)==Kind::Periodic);
    pass("smart thresholds and invalid movement never gate regular periodic; stationary floor does not suppress an eligible early smart send");
  }
  {
    Schedule s;Parameters p;p.smartEnabled=true;assert(s.stampSuccess(Kind::Periodic,1000,10,20));
    uint32_t stamp=0;int32_t lat=0,lon=0;
    assert(stockSmart(1000,301000,300000,100,100,false,stamp,lat,lon)&&stamp==1000&&lat==10&&lon==20);
    for(unsigned tick=0;tick<5;++tick)assert(s.decide(301000,p,100,false)==Kind::Smart&&s.lastSendMs()==1000&&s.lastLatitude()==10&&s.lastLongitude()==20);
    assert(stockSmart(1000,301000,300000,100,100,true,stamp,lat,lon));
    assert(s.stampSuccess(Kind::Smart,301000,30,40)&&s.lastSendMs()==stamp&&s.lastLatitude()==lat&&s.lastLongitude()==lon);
    assert(s.decide(301001,p,0)==Kind::None);
    assert(!s.stampSuccess(Kind::None,999,999,999)&&!s.stampSuccess(static_cast<Kind>(255),999,999,999));
    assert(s.lastSendMs()==301000&&s.lastLatitude()==30&&s.lastLongitude()==40);
    pass("failed send and due probes never stamp time or coordinates; successful sends alone replace the baseline");
  }
  {
    Schedule periodic;Parameters p;p.smartEnabled=true;
    assert(periodic.stampSuccess(Kind::Periodic,0,30,40)&&periodic.lastSendMs()==0&&periodic.decide(1,p,0)==Kind::Periodic);
    assert(stockPeriodic(0,1,p.regularIntervalMs,false));
    // A wrapped smart send at now0 keeps stock skipZero, unlike periodic.
    Schedule smart;assert(smart.stampSuccess(Kind::Periodic,0xfffffff0u,10,20));p.smartMinimumIntervalMs=16;
    uint32_t stamp=0;int32_t lat=0,lon=0;assert(stockSmart(0xfffffff0u,0,16,100,100,true,stamp,lat,lon));
    assert(smart.stampSuccess(Kind::Smart,0,30,40)&&stamp==smart.lastSendMs()&&stamp==1);
    assert(!smart.smartDue(1,p,0));
    assert(smart.periodicDue(0,p)==stockPeriodic(1,0,p.regularIntervalMs,false));
    pass("actual stock periodic0 sentinel and smart skipZero asymmetry are explicit, including same-wrap-tick arithmetic quirk");
  }
  {
    Schedule s;Parameters p;p.smartEnabled=true;assert(s.stampSuccess(Kind::Periodic,1,10,20));
    for(double invalid:{-1.0,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})assert(s.decide(400000,p,invalid,false)==Kind::None);
    p.smartMinimumIntervalMs=0;assert(s.decide(400000,p,1000,false)==Kind::None);
    p.smartMinimumIntervalMs=0x80000000u;assert(s.decide(400000,p,1000,false)==Kind::None);
    p.smartMinimumIntervalMs=300000;p.regularIntervalMs=0;assert(!s.periodicDue(400000,p)&&s.decide(400000,p,1000)==Kind::Smart);
    p.regularIntervalMs=0x80000000u;assert(!s.periodicDue(400000,p));
    pass("invalid smart input is bounded independently; unsupported intervals fail closed without mutating cadence");
  }
  std::printf("POSITION_SCHEDULE_GROUPS %u\n",groups);
}
