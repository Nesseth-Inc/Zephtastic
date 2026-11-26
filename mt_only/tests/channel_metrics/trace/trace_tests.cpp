// SPDX-License-Identifier: GPL-3.0-only
// Exact API/counter contract against the actual final ChannelTrace header.
#include "ChannelTrace.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <type_traits>
#ifdef NDEBUG
#error Assertions are required
#endif
using E=zt::ChannelEvent;using T=zt::ChannelTrace;
static auto fields(const T &s) {
  return std::tie(s.rxKnown,s.rxUnknown,s.uncertain,s.loss,s.limit,s.support,s.tx,
    s.overlap,s.other,s.lastMs,s.irq,s.len,s.reason,s.known);
}
static auto metadata(const T &s){return std::tie(s.lastMs,s.irq,s.len,s.known);}
static T seeded() {
  T s;s.rxKnown=11;s.rxUnknown=22;s.uncertain=33;s.loss=44;s.limit=55;
  s.support=66;s.tx=77;s.overlap=88;s.other=99;s.lastMs=1234;s.irq=0xabcd;
  s.len=47;s.reason=E::Support;s.known=false;return s;
}
static constexpr uint32_t T::* counters[]={&T::rxKnown,&T::rxUnknown,&T::uncertain,
  &T::loss,&T::limit,&T::support,&T::tx,&T::overlap,&T::other};
static constexpr uint32_t T::* selected[]={&T::rxKnown,&T::rxUnknown,&T::loss,
  &T::limit,&T::support,&T::tx,&T::overlap,&T::other};
static void contract() {
  static_assert(std::is_same<std::underlying_type_t<E>,uint8_t>::value);
  static_assert(sizeof(E)==1);
  static_assert(unsigned(E::None)==0&&unsigned(E::RxKnown)==1&&unsigned(E::RxUnknown)==2);
  static_assert(unsigned(E::NativeLoss)==3&&unsigned(E::DrainLimit)==4&&unsigned(E::Support)==5);
  static_assert(unsigned(E::TxUncertain)==6&&unsigned(E::PendingOverlap)==7&&unsigned(E::Other)==8);
  const T empty;for(auto counter:counters)assert(empty.*counter==0);
  assert(empty.lastMs==0&&empty.irq==0&&empty.len==0&&empty.reason==E::None&&!empty.known);
  std::cout<<"PASS exact enum IDs/types and zero defaults\n";
  for(unsigned event=0;event<=255;++event)if(event==0||event>8) {
    T s=seeded(),before=s;zt::noteChannelTrace(s,static_cast<E>(event),999,0xffff,255);
    assert(fields(s)==fields(before));
  }
  std::cout<<"PASS None and all247 undefined enum IDs leave every field untouched\n";
  for(unsigned event=1;event<=8;++event) {
    T s=seeded(),before=s;const E kind=static_cast<E>(event);zt::noteChannelTrace(s,kind,678,0x1234,255);
    const auto target=selected[event-1];
    for(auto counter:counters) {
      const bool increment=counter==target||(counter==&T::uncertain&&kind!=E::RxKnown);
      assert(s.*counter==before.*counter+(increment?1u:0u));
    }
    if(kind==E::RxKnown||kind==E::RxUnknown) {
      assert(s.lastMs==678&&s.irq==0x1234&&s.len==255&&s.known==(kind==E::RxKnown));
    } else assert(metadata(s)==metadata(before));
    assert(s.reason==(kind==E::RxKnown?before.reason:kind));
  }
  std::cout<<"PASS exact single-event counter/metadata/reason changes; knownRX preserves history\n";
  for(unsigned event=1;event<=8;++event) {
    T s=seeded();for(auto counter:counters)s.*counter=UINT32_MAX-1;
    for(unsigned repeat=0;repeat<6;++repeat)zt::noteChannelTrace(s,static_cast<E>(event),repeat,0xffff,255);
    const auto target=selected[event-1];
    for(auto counter:counters) {
      const bool increment=counter==target||(counter==&T::uncertain&&event!=1);
      assert(s.*counter==(increment?UINT32_MAX:UINT32_MAX-1));
    }
  }
  std::cout<<"PASS all touched counters saturate independently; no wrap or unrelated reset\n";
  T s=seeded();zt::noteChannelTrace(s,E::RxKnown);assert(s.lastMs==0&&s.irq==0&&s.len==0&&s.known);
  const auto known=s;zt::noteChannelTrace(s,E::NativeLoss);assert(metadata(s)==metadata(known));
  zt::noteChannelTrace(s,E::RxUnknown);assert(!s.known&&s.lastMs==0&&s.irq==0&&s.len==0);
  std::cout<<"PASS default metadata and historical metadata retention\n";
}
int main(){contract();}
