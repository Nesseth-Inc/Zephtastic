// SPDX-License-Identifier: GPL-3.0-only
#include "network/MqttRegionRootPolicy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {
using Policy=mt::network::MqttRegionRootPolicy;
using Result=Policy::Result;

void testDefaultBrokerAndAutomaticRoots(){
  char out[32];memset(out,0x5a,sizeof(out));
  assert(Policy::apply("",0,"",0,"US",2,out,sizeof(out))==Result::Changed);
  assert(!strcmp(out,"msh/US"));

  memset(out,0x5a,sizeof(out));
  const char host[]="mqtt.meshtastic.org:8883";
  const char oldRoot[]="msh/EU_868";
  assert(Policy::apply(host,sizeof(host)-1,oldRoot,sizeof(oldRoot)-1,
                       "US",2,out,sizeof(out))==Result::Changed);
  assert(!strcmp(out,"msh/US"));

  memset(out,0x5a,sizeof(out));
  assert(Policy::apply("mqtt.meshtastic.org",19,"msh",3,"UNSET",5,
                       out,sizeof(out))==Result::Changed);
  assert(!strcmp(out,"msh/UNSET"));
}

void testCustomBrokerAndRootStayUserOwned(){
  char out[32];memset(out,0x5a,sizeof(out));
  assert(Policy::apply("broker.example",14,"msh/EU_868",10,"US",2,
                       out,sizeof(out))==Result::Unchanged);
  assert(uint8_t(out[0])==0x5a);

  assert(Policy::apply("mqtt.meshtastic.org",19,"msh/custom",10,"US",2,
                       out,sizeof(out))==Result::Unchanged);
  assert(uint8_t(out[0])==0x5a);
  assert(Policy::apply("mqtt.meshtastic.org",19,"msh/US",6,"US",2,
                       out,sizeof(out))==Result::Unchanged);
  assert(uint8_t(out[0])==0x5a);
}

void testExactStockSuffixAndBounds(){
  assert(Policy::stockDefaultRoot("msh/ITU3_70CM",13));
  assert(Policy::stockDefaultRoot("msh/UNSET",9));
  assert(!Policy::stockDefaultRoot("msh/US-extra",12));
  assert(!Policy::stockDefaultRoot("msh/custom",10));

  char out[8];memset(out,0x5a,sizeof(out));
  assert(Policy::apply("",0,"",0,"US",2,out,6)==Result::Invalid);
  assert(uint8_t(out[0])==0x5a);
  char full[32];memset(full,0x5a,sizeof(full));
  assert(Policy::apply("",0,"msh/custom",10,"US",2,full,sizeof(full))==Result::Unchanged);
  assert(uint8_t(full[0])==0x5a);
  assert(Policy::apply("",0,"",0,"CUSTOM",6,full,sizeof(full))==Result::Invalid);
  assert(uint8_t(full[0])==0x5a);
}
}

int main(){
  testDefaultBrokerAndAutomaticRoots();
  testCustomBrokerAndRootStayUserOwned();
  testExactStockSuffixAndBounds();
  puts("PASS regional MQTT root policy: default-only, exact stock suffixes, unchanged roots untouched");
}
