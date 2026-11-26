// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "../src/modules/GpioModuleBackend.h"

gpio_dt_spec mt_fake_gpio_spec_PIN0{&mt_fake_gpio_device,2,0};
gpio_dt_spec mt_fake_gpio_spec_PIN1{&mt_fake_gpio_device,MT_FAKE_PIN1,0};
gpio_dt_spec mt_fake_gpio_spec_PIN2{&mt_fake_gpio_device,6,0};

namespace {
constexpr uint64_t Bit0=uint64_t(1)<<0;
constexpr uint64_t Bit4=uint64_t(1)<<4;
constexpr uint64_t Bit63=uint64_t(1)<<63;

void check(bool condition,const char *message){
  if(!condition){fprintf(stderr,"FAIL %s\n",message);exit(1);}
}

[[maybe_unused]] void testEmptyBackend(){
  auto &backend=mt::modules::zephyrGpioModuleBackend();
  const auto caps=backend.capabilities();
  check(!caps.exported&&!caps.readable&&!caps.writable&&!caps.pullup,
        "no binding exports zero capabilities");
  const unsigned configure=mt_fake_gpio_device.configureCalls;
  const unsigned reads=mt_fake_gpio_device.getCalls;
  const unsigned writes=mt_fake_gpio_device.setCalls;
  uint64_t value=0xfeedbeef;
  check(backend.configureInput(0,false)==-ENODEV,"empty input reports unavailable");
  check(backend.read(Bit0,false,value)==-ENODEV,"empty read reports unavailable");
  check(value==0xfeedbeef,"empty read leaves output unchanged");
  check(backend.write(Bit0,Bit0)==-ENODEV,"empty write reports unavailable");
  check(configure==mt_fake_gpio_device.configureCalls&&reads==mt_fake_gpio_device.getCalls&&
        writes==mt_fake_gpio_device.setCalls,"empty backend makes no GPIO calls");
}

[[maybe_unused]] void testUnavailableAtInitialization(){
  mt_fake_gpio_device.ready=false;
  auto &backend=mt::modules::zephyrGpioModuleBackend();
  check(!backend.capabilities().exported,"initially unready table exports zero capabilities");
  mt_fake_gpio_device.ready=true;
  check(!backend.capabilities().exported,"initially unready table does not widen later");
  const unsigned configure=mt_fake_gpio_device.configureCalls;
  check(backend.configureInput(0,false)==-ENODEV,"unready input fails");
  check(configure==mt_fake_gpio_device.configureCalls&&
        mt_fake_gpio_device.operationsAfterUnready==0,"unready initialization makes no GPIO calls");
}

[[maybe_unused]] void testInvalidTable(){
  auto &backend=mt::modules::zephyrGpioModuleBackend();
  const auto caps=backend.capabilities();
  check(!caps.exported&&!caps.readable&&!caps.writable&&!caps.pullup,
        "invalid dedicated table fails closed");
  const unsigned configure=mt_fake_gpio_device.configureCalls;
  const unsigned reads=mt_fake_gpio_device.getCalls;
  const unsigned writes=mt_fake_gpio_device.setCalls;
  uint64_t value=0x12345678;
  check(backend.configureInput(0,false)==-EINVAL,"invalid table input rejected");
  check(backend.read(Bit0,false,value)==-EINVAL,"invalid table read rejected");
  check(value==0x12345678,"invalid table read output unchanged");
  check(backend.write(Bit0,Bit0)==-EINVAL,"invalid table write rejected");
  check(configure==mt_fake_gpio_device.configureCalls&&reads==mt_fake_gpio_device.getCalls&&
        writes==mt_fake_gpio_device.setCalls,"invalid table makes no GPIO calls");
}

[[maybe_unused]] void testValidTableAndOperations(){
  using mt::modules::zephyrGpioModuleBackend;
  // main.cpp obtains the reference during static construction. That must not
  // latch pre-device-init readiness before the first operational access.
  mt_fake_gpio_device.ready=false;
  (void)zephyrGpioModuleBackend();
  mt_fake_gpio_device.ready=true;
  auto &backend=zephyrGpioModuleBackend();
  const auto caps=backend.capabilities();
  check(caps.exported==(Bit0|Bit4|Bit63),"wire IDs 0, 4 and 63 map explicitly");
  check(caps.readable==(Bit0|Bit4),"readable permissions exact");
  check(caps.writable==(Bit0|Bit63),"writable permissions exact");
  check(caps.pullup==Bit0,"pullup permission exact");

  unsigned configure=mt_fake_gpio_device.configureCalls;
  unsigned reads=mt_fake_gpio_device.getCalls;
  unsigned writes=mt_fake_gpio_device.setCalls;
  check(backend.configureInput(0,true)==0,"permitted pull-up input config succeeds");
  check(mt_fake_gpio_device.configureCalls==configure+1,"input config reaches driver");
  check(mt_fake_gpio_device.lastConfigureFlags[2]==(GPIO_INPUT|GPIO_PULL_UP),
        "requested pull-up is configured");
  check(backend.configureInput(63,false)==-EACCES,"write-only wire ID cannot be configured as input");
  check(backend.configureInput(4,true)==-ENOTSUP,"undeclared pull-up is rejected");
  check(backend.configureInput(64,false)==-EINVAL,"wire ID 64 is out of range");
  check(mt_fake_gpio_device.configureCalls==configure+1,"invalid input requests make no GPIO calls");

  mt_fake_gpio_device.values[2]=1;
  mt_fake_gpio_device.values[6]=0;
  uint64_t result=0xdeadbeef;
  check(backend.read(Bit0|Bit4,false,result)==0,"multi-pin read succeeds");
  check(result==Bit0,"read returns selected values only");
  check(mt_fake_gpio_device.configureCalls==configure+3&&mt_fake_gpio_device.getCalls==reads+2,
        "read configures all selected pins then samples");
  check(backend.read(Bit0|Bit63,false,result)==-EACCES,"mixed readable/write-only mask rejected");
  check(backend.read(Bit0|Bit4,true,result)==-ENOTSUP,"unsupported mixed pull-up mask rejected");
  check(backend.read(Bit0|(uint64_t(1)<<1),false,result)==-EINVAL,"unexported mask bit rejected");
  check(mt_fake_gpio_device.configureCalls==configure+3&&mt_fake_gpio_device.getCalls==reads+2,
        "invalid complete masks are rejected before any IO");

  mt_fake_gpio_device.getError[6]=-EIO;
  result=0xabcdef01;
  check(backend.read(Bit0|Bit4,false,result)==-EIO,"read driver error propagates");
  check(result==0xabcdef01,"failed read leaves output unchanged");
  mt_fake_gpio_device.getError[6]=0;
  mt_fake_gpio_device.configureError[6]=-EBUSY;
  const unsigned readsBeforeConfigError=mt_fake_gpio_device.getCalls;
  result=0x87654321;
  check(backend.read(Bit0|Bit4,false,result)==-EBUSY,"input configuration error propagates");
  check(result==0x87654321&&mt_fake_gpio_device.getCalls==readsBeforeConfigError,
        "configuration failure leaves output and sampling untouched");
  mt_fake_gpio_device.configureError[6]=0;

  check(backend.write(Bit0|Bit63,Bit63)==0,"permitted multi-pin write succeeds");
  check(mt_fake_gpio_device.values[2]==0&&mt_fake_gpio_device.values[5]==1,
        "writes set requested levels");
  check(mt_fake_gpio_device.setCalls==writes+2,"both selected lines written");
  configure=mt_fake_gpio_device.configureCalls;
  writes=mt_fake_gpio_device.setCalls;
  check(backend.write(Bit0|Bit4,Bit0)==-EACCES,"mixed writable/read-only mask rejected");
  check(backend.write(Bit0,Bit63)==-EINVAL,"value outside write mask rejected");
  check(mt_fake_gpio_device.configureCalls==configure&&mt_fake_gpio_device.setCalls==writes,
        "invalid write masks are rejected before any IO");

  // A physical multi-pin write is deliberately not claimed atomic. A driver
  // error on the later pin is returned after the earlier line was applied.
  mt_fake_gpio_device.setError[5]=-EIO;
  const int partial=backend.write(Bit0|Bit63,Bit0|Bit63);
  check(partial==-EIO&&mt_fake_gpio_device.values[2]==1,
        "later write failure propagates without claiming rollback");
  mt_fake_gpio_device.setError[5]=0;

  mt_fake_gpio_device.ready=false;
  const unsigned opCount=mt_fake_gpio_device.configureCalls+mt_fake_gpio_device.getCalls+
                         mt_fake_gpio_device.setCalls;
  check(!backend.capabilities().exported,"runtime-unready device removes all capabilities");
  check(backend.configureInput(0,false)==-ENODEV,"runtime-unready input fails");
  check(backend.read(Bit0,false,result)==-ENODEV,"runtime-unready read fails");
  check(backend.write(Bit0,0)==-ENODEV,"runtime-unready write fails");
  check(opCount==mt_fake_gpio_device.configureCalls+mt_fake_gpio_device.getCalls+
        mt_fake_gpio_device.setCalls&&mt_fake_gpio_device.operationsAfterUnready==0,
        "readiness checked before all driver IO");
  mt_fake_gpio_device.ready=true;
  check(!backend.capabilities().exported,"capabilities do not widen after readiness loss");
}
}

int main(){
#if defined(FAKE_GPIO_NO_NODE)
  testEmptyBackend();
  puts("PASS M138 empty Zephyr GPIO backend: zero exports and no device calls");
#elif defined(FAKE_GPIO_START_UNREADY)
  testUnavailableAtInitialization();
  puts("PASS M138 initially unready GPIO table remains empty after readiness changes");
#elif defined(FAKE_GPIO_BAD_WIRE_ID)||defined(FAKE_GPIO_DUPLICATE_WIRE_ID)|| \
      defined(FAKE_GPIO_DUPLICATE_PHYSICAL_PIN)||defined(FAKE_GPIO_BAD_PULLUP)
  testInvalidTable();
  puts("PASS M138 invalid dedicated GPIO table fails closed before GPIO calls");
#else
  testValidTableAndOperations();
  puts("PASS M138 explicit GPIO export map, full-mask preflight, error propagation and readiness latching");
#endif
  return 0;
}
