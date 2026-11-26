// Independent host tests of the actual production headers; no window model.
#include "ChannelQuality.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#ifndef QUALITY_ONLY
#include "ChannelWindow.h"
#include "airtime.h"
#include "UptimeClock.h"
#include <cmath>
#endif

static unsigned checks=0,groups=0,failures=0;
static void check(bool value,const char *text,int line){++checks;if(!value){std::fprintf(stderr,"line %d: %s\n",line,text);throw std::runtime_error(text);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
static void group(const char *name,void(*fn)()){++groups;try{fn();std::printf("PASS %s\n",name);}catch(const std::exception&){++failures;std::printf("FAIL %s\n",name);}}

static void quality_preanchor_highwater(){zt::ChannelQuality q;CHECK(q.availableAt(50000));CHECK(!q.observe(49999));CHECK(!q.availableAt(50000));CHECK(!q.availableAt(109999));CHECK(q.availableAt(110000));}
static void quality_invalid_clock_first_good(){zt::ChannelQuality q;CHECK(!q.observe(UINT64_MAX));CHECK(!q.availableAt(100000));CHECK(!q.availableAt(159999));CHECK(q.availableAt(160000));}
static void quality_regression_first_good(){zt::ChannelQuality q;CHECK(q.observe(100000));CHECK(!q.observe(99999));CHECK(!q.availableAt(120000));CHECK(!q.availableAt(179999));CHECK(q.availableAt(180000));}
static void quality_repeated_bad_extends(){zt::ChannelQuality q;q.invalidate(1000);CHECK(!q.availableAt(2000));CHECK(!q.observe(1999));CHECK(!q.availableAt(4000));CHECK(!q.availableAt(63999));CHECK(q.availableAt(64000));}
static void quality_repeated_uncertainty_extends(){zt::ChannelQuality q;q.invalidate(1000);CHECK(!q.availableAt(2000));q.invalidate(3000);CHECK(!q.availableAt(61000));CHECK(!q.availableAt(62999));CHECK(q.availableAt(63000));}
static void quality_u64_limits(){zt::ChannelQuality q;q.invalidate(UINT64_MAX-60001);CHECK(!q.availableAt(UINT64_MAX-2));CHECK(q.availableAt(UINT64_MAX-1));CHECK(!q.availableAt(UINT64_MAX));CHECK(!q.availableAt(UINT64_MAX-1));CHECK(!q.availableAt(UINT64_MAX-1));}

#ifndef QUALITY_ONLY
template<class T> static void absent(zt::Observed<T> v){CHECK(!v.has_value());}
static void sum(zt::ChannelWindow &w,uint64_t now,uint64_t amount){auto v=w.windowSumMs(now);CHECK(v.has_value());CHECK(*v==amount);}
static void value(zt::ChannelWindow &w,uint64_t now,float expected){auto v=w.percentAt(now);CHECK(v.has_value());CHECK(std::fabs(*v-expected)<=1e-6f*std::fmax(1.0f,std::fabs(expected)));}
static void window_reads_do_not_anchor(){zt::ChannelWindow w;absent(w.windowSumMs(50000));absent(w.percentAt(60000));CHECK(!w.anchored());CHECK(!w.record(59999,1));CHECK(!w.anchored());CHECK(w.record(70000,0));CHECK(w.anchored());absent(w.windowSumMs(129999));sum(w,130000,0);}
static void window_quality_keeps_credits(){zt::ChannelWindow w;CHECK(w.record(1000,1000));w.invalidate(2000);CHECK(w.record(30000,6000));absent(w.windowSumMs(61999));sum(w,62000,6000);value(w,62000,10.0f);}
static void window_lost_clock_first_good(){zt::ChannelWindow w;CHECK(w.record(10000,1000));absent(w.windowSumMs(UINT64_MAX));CHECK(w.record(20000,0));CHECK(w.record(70000,6000));absent(w.percentAt(79999));sum(w,80000,6000);value(w,80000,10.0f);}
static void window_regression_first_good(){zt::ChannelWindow w;CHECK(w.record(100000,1000));CHECK(!w.record(99999,9000));CHECK(w.record(120000,0));CHECK(w.record(170000,6000));absent(w.windowSumMs(179999));sum(w,180000,6000);}
static void window_repeated_bad_clock(){zt::ChannelWindow w;CHECK(w.record(10000,1000));absent(w.windowSumMs(9000));CHECK(w.record(20000,1000));absent(w.windowSumMs(19000));CHECK(w.record(40000,6000));CHECK(w.record(80000,3333));absent(w.windowSumMs(99999));sum(w,100000,3333);}
static void window_healthy_aged_idle(){zt::ChannelWindow w;CHECK(w.record(9999,6000));sum(w,59999,6000);sum(w,60000,0);sum(w,600000000,0);value(w,600000000,0);CHECK(w.anchored());}
static void window_phase_boundaries(){zt::ChannelWindow w;CHECK(w.record(9999,1000));CHECK(w.record(10000,2000));sum(w,59999,3000);sum(w,60000,2000);sum(w,69999,2000);sum(w,70000,0);}
static void window_bucket_overflow(){zt::ChannelWindow w;CHECK(w.record(1000,UINT32_MAX));CHECK(!w.record(1001,1));CHECK(!w.record(1002,0));absent(w.windowSumMs(1002));CHECK(w.record(10000,6000));absent(w.windowSumMs(59999));sum(w,60000,6000);}
static void window_aggregate_overflow(){zt::ChannelWindow w;CHECK(w.record(1000,UINT32_MAX-10));CHECK(w.record(10000,11));absent(w.windowSumMs(10000));absent(w.percentAt(59999));sum(w,60000,11);}
static void window_u64_time(){zt::ChannelWindow w;const uint64_t base=((UINT64_MAX-300000)/10000)*10000;CHECK(w.record(base,6000));sum(w,base+59999,6000);sum(w,base+60000,0);sum(w,UINT64_MAX-1,0);absent(w.percentAt(UINT64_MAX));absent(w.percentAt(UINT64_MAX-1));zt::ChannelWindow high;CHECK(high.record(uint64_t(1)<<63,6000));value(high,(uint64_t(1)<<63)+1000,10.0f);}

static void healthy_stock_differential(){
  // Only representable, monotonic stock clock values. This does not qualify
  // stock millis carry, counter quality, EMA, RF observation, or integration.
  const uint64_t starts[]={0,1,9999,10000,59999,60000,4294967295ULL,4294967295000ULL-1000000};
  for(uint64_t start:starts){
    zt::ChannelWindow w;Time::syntheticNowMs=start;AirTime stock;
    CHECK(w.record(start,0));stock.logAirtime(RX_LOG,0);
    const uint64_t offsets[]={0,1,9999,10000,10001,29999,30000,59999,60000,69999,70000,120000,900000};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i){
      const uint64_t now=start+offsets[i];Time::syntheticNowMs=now;
      if(i<9){const uint32_t amount=i==0?1U:(i==1?6000U:unsigned(i*11));CHECK(w.record(now,amount));stock.logAirtime(reportTypes(i%3),amount);}
      const float expected=stock.channelUtilizationPercent();CHECK(std::isfinite(expected));auto got=w.percentAt(now);CHECK(got.has_value());
      CHECK(std::fabs(*got-expected)<=1e-6f*std::fmax(1.0f,std::fabs(expected)));
      if(i==0)CHECK(expected>0.0f);
    }
  }
}
static void logged_startup_unknown_and_aging(){
  zt::ChannelWindow w;CHECK(!w.anchored());auto zero=w.loggedPercentAt(0);CHECK(zero&&*zero==0);CHECK(w.anchored());
  CHECK(w.record(1000,6000));auto old=w.percentAt(1000),logged=w.loggedPercentAt(1000);CHECK(old&&logged&&*old==10&&*logged==*old);
  w.invalidate(1100);absent(w.percentAt(1100));logged=w.loggedPercentAt(1100);CHECK(logged&&*logged==10);absent(w.percentAt(1100));
  logged=w.loggedPercentAt(59999);CHECK(logged&&*logged==10);logged=w.loggedPercentAt(60000);CHECK(logged&&*logged==0);
  absent(w.percentAt(60000));value(w,61100,0);
}
static void logged_clock_refusal_and_recovery(){
  zt::ChannelWindow w;absent(w.loggedPercentAt(UINT64_MAX));CHECK(!w.anchored());
  auto zero=w.loggedPercentAt(0);CHECK(zero&&*zero==0);absent(w.percentAt(0));CHECK(w.record(1000,6000));
  absent(w.loggedPercentAt(UINT64_MAX));auto retained=w.loggedPercentAt(1000);CHECK(retained&&*retained==10);
  absent(w.loggedPercentAt(999));absent(w.loggedPercentAt(998));retained=w.loggedPercentAt(1000);CHECK(retained&&*retained==10);
  absent(w.percentAt(1000));
}
static void logged_overflow_and_raw_range(){
  zt::ChannelWindow bucket;CHECK(bucket.record(0,UINT32_MAX));CHECK(bucket.loggedPercentAt(0));CHECK(!bucket.record(0,1));
  absent(bucket.loggedPercentAt(0));bucket.invalidate(1);absent(bucket.loggedPercentAt(1));auto aged=bucket.loggedPercentAt(60000);CHECK(aged&&*aged==0);
  zt::ChannelWindow aggregate;CHECK(aggregate.record(0,UINT32_MAX-100));CHECK(aggregate.record(10000,100));CHECK(aggregate.loggedPercentAt(10000));
  CHECK(aggregate.record(20000,1));absent(aggregate.loggedPercentAt(20000));auto retained=aggregate.loggedPercentAt(60000);CHECK(retained&&*retained>0&&*retained<1);sum(aggregate,60000,101);
  zt::ChannelWindow raw;CHECK(raw.record(0,60001));auto above=raw.loggedPercentAt(0);CHECK(above&&*above>100); // Encoder0..100 guard is separate.
}
#endif

int main(){
  group("quality_preanchor_highwater",quality_preanchor_highwater);
  group("quality_invalid_clock_first_good",quality_invalid_clock_first_good);
  group("quality_regression_first_good",quality_regression_first_good);
  group("quality_repeated_bad_extends",quality_repeated_bad_extends);
  group("quality_repeated_uncertainty_extends",quality_repeated_uncertainty_extends);
  group("quality_u64_limits",quality_u64_limits);
#ifndef QUALITY_ONLY
  group("window_reads_do_not_anchor",window_reads_do_not_anchor);
  group("window_quality_keeps_credits",window_quality_keeps_credits);
  group("window_lost_clock_first_good",window_lost_clock_first_good);
  group("window_regression_first_good",window_regression_first_good);
  group("window_repeated_bad_clock",window_repeated_bad_clock);
  group("window_healthy_aged_idle",window_healthy_aged_idle);
  group("window_phase_boundaries",window_phase_boundaries);
  group("window_bucket_overflow",window_bucket_overflow);
  group("window_aggregate_overflow",window_aggregate_overflow);
  group("window_u64_time",window_u64_time);
  group("healthy_stock_differential",healthy_stock_differential);
  group("logged_startup_unknown_and_aging",logged_startup_unknown_and_aging);
  group("logged_clock_refusal_and_recovery",logged_clock_refusal_and_recovery);
  group("logged_overflow_and_raw_range",logged_overflow_and_raw_range);
#endif
  std::printf("SUMMARY groups=%u checks=%u failures=%u\n",groups,checks,failures);return failures?1:0;
}
