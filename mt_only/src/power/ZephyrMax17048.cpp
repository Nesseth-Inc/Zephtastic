// SPDX-License-Identifier: GPL-3.0-only
#include "Max17048.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>

namespace mt::power::max17048 {
namespace {
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_max17048), maxim_max17048));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_max17048)) == 0x36);
const i2c_dt_spec port = I2C_DT_SPEC_GET(DT_ALIAS(zt_max17048));
Sample sample;
struct RegisterBus {
  bool set(uint8_t address, uint16_t value) const {
    uint8_t bytes[3] = {address};
    sys_put_be16(value, bytes + 1);
    return i2c_write_dt(&port, bytes, sizeof(bytes)) == 0;
  }
  bool word(uint8_t address, uint16_t &value) const {
    uint8_t bytes[2];
    if (!i2c_is_ready_dt(&port) ||
        i2c_write_read_dt(&port, &address, 1, bytes, sizeof(bytes))) return false;
    value = sys_get_be16(bytes);
    return true;
  }
} bus;
}
void poll(uint64_t now) { sample.poll(bus, now); }
bool identified(){return sample.identified();}
Reading snapshot(uint64_t now) { return sample.get(now); }
}
