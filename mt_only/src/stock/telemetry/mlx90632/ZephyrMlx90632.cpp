// SPDX-License-Identifier: GPL-3.0-only
#include "ObjectTemperature.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>

namespace mt::stock::telemetry::mlx90632 {
namespace {
const i2c_dt_spec port = I2C_DT_SPEC_GET(DT_ALIAS(zt_mlx90632));
static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_mlx90632), zephtastic_mlx90632));
static_assert(DT_REG_ADDR(DT_ALIAS(zt_mlx90632)) == 0x3a ||
              DT_REG_ADDR(DT_ALIAS(zt_mlx90632)) == 0x3b);
struct RegisterBus {
  unsigned address() const { return port.addr; }
  bool read(uint16_t reg, uint8_t *bytes, size_t count) {
    uint8_t index[2];
    sys_put_be16(reg, index);
    return i2c_is_ready_dt(&port) &&
           !i2c_write_read_dt(&port, index, sizeof(index), bytes, count);
  }
  bool word(uint16_t reg, uint16_t &value) {
    uint8_t raw[2];
    if (!read(reg, raw, sizeof(raw)))
      return false;
    value = sys_get_be16(raw);
    return true;
  }
  bool wide(uint16_t reg, int32_t &value) {
    uint8_t raw[4];
    if (!read(reg, raw, sizeof(raw)))
      return false;
    value = static_cast<int32_t>(uint32_t(sys_get_be16(raw + 2)) << 16 |
                                 sys_get_be16(raw));
    return true;
  }
  bool set(uint16_t reg, uint16_t value) {
    uint8_t raw[4];
    sys_put_be16(reg, raw);
    sys_put_be16(value, raw + 2);
    return !i2c_write_dt(&port, raw, sizeof(raw));
  }
} bus;
Sensor<RegisterBus> sensor(bus);
} // namespace
void poll(bool enabled, uint64_t now) { sensor.poll(enabled, now); }
zt::Observed<float> snapshot(uint64_t now) { return sensor.snapshot(now); }
} // namespace mt::stock::telemetry::mlx90632
