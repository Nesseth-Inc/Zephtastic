// SPDX-License-Identifier: GPL-3.0-only
// Synthetic state and failure vectors, never receiver/hardware measurements.
#include "gnss/GnssAcquisitionGate.h"
#include "gnss/GnssPromotionProjection.h"
#include "gnss/T096GnssPower.h"
#include <assert.h>
using namespace zt::gnss;
namespace power=zt::gnss::t096;
static PromotionEvent event(){return {12345678900LL,-23456789000LL,1000,12345,5,true};}
static bool same(const PromotionEvent&a,const PromotionEvent&b){return a.latitudeNdeg==b.latitudeNdeg&&a.longitudeNdeg==b.longitudeNdeg&&a.capturedUptimeMs==b.capturedUptimeMs&&a.altitudeMm==b.altitudeMm&&a.satellites==b.satellites&&a.valid==b.valid;}
struct Changes {
  bool actual[3]{};int channel[12]{},count=0,failCall=0,rollbackFailCall=0;
  bool value[12]{};int error=-101;
  int set(unsigned n,bool v){assert(count<12);actual[n]=v;channel[count]=int(n);value[count]=v;++count;return count==failCall?error:count==rollbackFailCall?-77:0;}
  static int rail(void*p,bool v){return static_cast<Changes*>(p)->set(0,v);}
  static int reset(void*p,bool v){return static_cast<Changes*>(p)->set(1,v);}
  static int uart(void*p,bool v){return static_cast<Changes*>(p)->set(2,v);}
  power::Ops ops(){return {this,rail,reset,uart};}
};
static power::State state(bool rail,bool reset,bool uart){return {true,rail,true,reset,true,uart};}
static void powerCases(){
  Changes c;c.actual[0]=true;c.actual[1]=true;
  auto r=power::start(c.ops(),state(true,true,false));
  assert(r.validation==power::Validation::kOk&&!r.op_error&&!r.rollback_error&&r.final.allKnown());
  assert(c.count==2&&c.channel[0]==1&&!c.value[0]&&c.channel[1]==2&&c.value[1]);
  c={};c.actual[0]=true;c.actual[1]=true;c.failCall=1;c.error=-100;
  r=power::start(c.ops(),state(true,true,false));
  assert(r.validation==power::Validation::kOk&&r.op_error==-100&&!r.rollback_error&&r.final.allKnown());
  assert(c.count==2&&c.channel[0]==1&&c.channel[1]==1&&c.actual[0]&&c.actual[1]&&!c.actual[2]);
  c={};c.actual[1]=true;c.failCall=3;
  r=power::start(c.ops(),state(false,true,false));
  assert(r.validation==power::Validation::kOk&&r.op_error==-101&&!r.rollback_error&&r.final.allKnown());
  assert(c.count==6&&c.channel[3]==2&&c.channel[4]==1&&c.channel[5]==0);
  assert(!c.actual[0]&&c.actual[1]&&!c.actual[2]);
  c={};c.actual[1]=true;c.failCall=3;c.rollbackFailCall=4;
  r=power::start(c.ops(),state(false,true,false));
  assert(r.op_error==-101&&r.rollback_error==-77&&!r.final.uart_known&&r.final.rail_known&&r.final.reset_known);
  c={};auto unknown=state(false,true,false);unknown.rail_known=false;
  r=power::start(c.ops(),unknown);assert(r.validation==power::Validation::kInvalidState&&c.count==0);
  auto missing=c.ops();missing.setPassiveUart=nullptr;
  r=power::start(missing,state(false,true,false));assert(r.validation==power::Validation::kMissingCallback&&c.count==0);
  c={};c.actual[0]=c.actual[2]=true;
  r=power::stop(c.ops(),state(true,false,true));
  assert(!r.op_error&&r.final.allKnown()&&c.count==3&&c.channel[0]==2&&c.channel[1]==1&&c.channel[2]==0);
  c={};c.actual[1]=true;r=power::stop(c.ops(),state(false,true,false));assert(!r.op_error&&c.count==0);
}
static void acquisitionCases(){
  AcquisitionGate gate;PendingPromotion slot{};PromotionEvent out=event(),e=event();
  assert(!takePromotion(slot,out)&&same(out,PromotionEvent{}));
  assert(!gate.consider(e,true,slot)&&gate.goodFixCount()==1&&!slot.pending);
  auto bad=e;bad.valid=false;assert(!gate.consider(bad,true,slot)&&gate.goodFixCount()==0);
  assert(!gate.consider(e,true,slot)&&!gate.consider(e,true,slot));
  assert(gate.consider(e,true,slot)&&gate.goodFixCount()==3&&same(slot.event,e));
  auto newer=e;newer.latitudeNdeg++;assert(!gate.consider(newer,true,slot)&&same(slot.event,e));
  assert(takePromotion(slot,out)&&same(out,e)&&same(slot.event,PromotionEvent{})&&!slot.pending);
  assert(gate.consider(newer,true,slot));
  bad=e;bad.satellites=3;assert(!gate.consider(bad,true,slot)&&gate.goodFixCount()==0&&same(slot.event,newer));
  assert(!gate.consider(e,false,slot)&&!slot.pending&&same(slot.event,PromotionEvent{})&&gate.goodFixCount()==0);
  gate.consider(e,true,slot);gate.reset();assert(gate.goodFixCount()==0);
  const int64_t latitudes[]={-90000000000LL,90000000000LL};
  for(int64_t lat:latitudes){e=event();e.latitudeNdeg=lat;assert(publishPromotion(slot,e));assert(takePromotion(slot,out));e.latitudeNdeg+=lat<0?-1:1;assert(!publishPromotion(slot,e));}
  const int64_t longitudes[]={-180000000000LL,180000000000LL};
  for(int64_t lon:longitudes){e=event();e.longitudeNdeg=lon;assert(publishPromotion(slot,e));assert(takePromotion(slot,out));e.longitudeNdeg+=lon<0?-1:1;assert(!publishPromotion(slot,e));}
}
int main(){powerCases();acquisitionCases();}
