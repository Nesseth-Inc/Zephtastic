// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
struct device {};
constexpr int K_NO_WAIT=0;
static unsigned drops,beaconOffProfileRx,powerActivityCalls,radioRxGood,radioRxBad,agcLastGood;
static unsigned now;
#define CONFIG_MT_NORMAL_ENDPOINT 1
static unsigned k_uptime_get_32(){return now;}
static void atomic_set(unsigned *value,unsigned n){*value=n;}
static int sx126x_bench_health(const device*){return 1;}
static unsigned atomic_get(const unsigned *value){return *value;}
static void notifyPowerActivity(){++powerActivityCalls;}
static int rxQueue;
static void atomic_inc(unsigned *value){++*value;}
static int k_msgq_put(int *,const void *,int);
#include "queue_callback.h"
static std::deque<Rx> queue;
static int k_msgq_put(int *identity,const void *bytes,int wait){
  assert(identity==&rxQueue&&wait==K_NO_WAIT);if(queue.size()==8)return -1;
  queue.push_back(*static_cast<const Rx *>(bytes));return 0;
}
int main(){
  device dev;uint8_t bytes[255]{};sx126x_packet_status status{};
  for(unsigned i=0;i<9;++i){bytes[0]=uint8_t(i);status.snr_raw=uint8_t(i+1);status.snr_known=true;
    receive(&dev,bytes,1,-90,int8_t(i),&status,nullptr);}
  assert(queue.size()==8&&drops==1&&powerActivityCalls==9);bytes[0]=255;status.snr_raw=255;
  for(unsigned i=0;i<8;++i){const Rx event=queue.front();queue.pop_front();
    assert(event.data[0]==i&&event.snrRaw==i+1&&event.snrKnown&&event.snr==int8_t(i));}
  std::cout<<"PASS actual callback copies metadata/payload atomically; queue saturation drops complete ninth event\n";
  receive(&dev,bytes,1,-90,-1,nullptr,nullptr);assert(!queue.back().snrKnown&&!queue.back().snrRaw);queue.clear();
  receive(&dev,nullptr,0,-90,-1,&status,nullptr);assert(!queue.back().snrKnown&&!queue.back().snrRaw);queue.clear();
  const auto before=drops;receive(&dev,nullptr,1,-90,-1,&status,nullptr);
  receive(&dev,bytes,256,-90,-1,&status,nullptr);assert(queue.empty()&&drops==before+2);
  const auto activityBefore=powerActivityCalls,dropsBefore=drops;beaconOffProfileRx=1;
  receive(&dev,bytes,1,-90,-1,&status,nullptr);
  assert(queue.empty()&&drops==dropsBefore&&powerActivityCalls==activityBefore);
  beaconOffProfileRx=0;receive(&dev,bytes,1,-90,-1,&status,nullptr);
  assert(queue.size()==1&&powerActivityCalls==activityBefore+1);queue.clear();
  std::cout<<"PASS unknown/malformed metadata handling; off-profile callbacks neither queue data nor signal home power activity\n";
  const auto good=radioRxGood,bad=radioRxBad;
  now=100;receive(&dev,bytes,15,-90,4,&status,nullptr);
  assert(radioRxGood==good&&radioRxBad==bad+1&&agcLastGood==0);
  now=200;receive(&dev,bytes,16,-90,4,&status,nullptr);
  assert(radioRxGood==good+1&&agcLastGood==200);
  beaconOffProfileRx=1;now=300;receive(&dev,bytes,16,-90,4,&status,nullptr);
  assert(radioRxGood==good+1&&agcLastGood==200);
  beaconOffProfileRx=0;
  while(queue.size()<8)receive(&dev,bytes,1,-90,4,&status,nullptr);
  const auto fullDrops=drops;
  now=400;receive(&dev,bytes,16,-90,4,&status,nullptr);
  assert(drops==fullDrops+1&&radioRxGood==good+2&&agcLastGood==400);
  std::cout<<"RX_EVENT_BYTES "<<sizeof(Rx)<<'\n';
}
