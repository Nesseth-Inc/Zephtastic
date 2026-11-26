// SPDX-License-Identifier: GPL-3.0-only
#include "peripheral.h"
#include "ZephyrUpdateBackend.h"
#include <zephyr/sys/reboot.h>
#include <cassert>
#include <cerrno>

using namespace mt::update;
Registers hardware{0x12,0x34};
Uicr uicr{};
static uint32_t vectors[2]{0x20040000,0xf4101};
static unsigned writes,resets,reads;
static bool accept=true;
const volatile uint32_t *test_vectors(uint32_t address) {
    assert(address==0xf4000);++reads;return vectors;
}
void nrf_power_gpregret_set(Registers *regs,uint32_t index,uint32_t value) {
    assert(regs==&hardware&&index==0);++writes;
    if(accept) regs->GPREGRET=value;
}
void sys_reboot(int kind) {assert(kind==SYS_REBOOT_COLD);++resets;}

int main() {
    meshtastic_Config_NetworkConfig network{};
    assert(!available(Mode::NordicBleDfu)&&!reads&&!writes);
    uicr.NRFFW[0]=0xf4000;rebootPrepared(Mode::NordicBleDfu);assert(!resets);
#if defined(CONFIG_MT_NORDIC_DFU) && (CONFIG_MT_HARDWARE_MODEL == 0 || CONFIG_MT_HARDWARE_MODEL == 127)
    uint8_t hash[32]{};
    assert(available(Mode::NordicBleDfu)&&available(Mode::NordicUf2)&&writes==0);
    vectors[1]&=~1u;
    assert(prepare(Mode::NordicBleDfu,nullptr,network)==-ENOTSUP&&!writes);
    vectors[1]|=1;
    assert(prepare(Mode::NordicBleDfu,hash,network)==-EINVAL&&!writes);
    accept=false;assert(prepare(Mode::NordicBleDfu,nullptr,network)==-EIO);
    rebootPrepared(Mode::NordicBleDfu);assert(!resets);
    accept=true;assert(!prepare(Mode::NordicBleDfu,nullptr,network));
    assert(hardware.GPREGRET==0xa8&&hardware.GPREGRET2==0x34&&!resets);
    assert(prepare(Mode::NordicUf2,nullptr,network)==-EBUSY);
    rebootPrepared(Mode::NordicUf2);assert(!resets);
    rebootPrepared(Mode::NordicBleDfu);rebootPrepared(Mode::NordicBleDfu);
    assert(resets==1);assert(!prepare(Mode::NordicUf2,nullptr,network));
    assert(hardware.GPREGRET==0x57&&hardware.GPREGRET2==0x34);
    rebootPrepared(Mode::NordicUf2);assert(resets==2);
#else
    assert(!available(Mode::NordicBleDfu)&&!available(Mode::NordicUf2));
    assert(prepare(Mode::NordicBleDfu,nullptr,network)==-ENOTSUP);
    rebootPrepared(Mode::NordicBleDfu);
    assert(!writes&&!reads&&!resets&&hardware.GPREGRET==0x12&&hardware.GPREGRET2==0x34);
#endif
}
