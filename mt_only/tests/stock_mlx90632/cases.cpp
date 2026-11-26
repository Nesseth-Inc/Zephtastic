// SPDX-License-Identifier: GPL-3.0-only
#include "oracle.h"
#include "stock/telemetry/mlx90632/ObjectTemperature.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using mt::stock::telemetry::mlx90632::Sensor;
int main() {
  Registers device;
  Sensor<Registers> sensor(device);
  MLX90632::status status;
  assert(stock.mlx.begin(device.address(), Wire, status));
  sensor.poll(false, 0);
  assert(device.reads == 0);
  uint64_t now = 1000;
  for (int n = 0; n < 256; ++n, now += 1200) {
    device.phase = Wire.registers.phase = 1 + (n & 1);
    for (unsigned i = 0; i < 6; ++i) {
      if (i == 2 || i == 5)
        continue;
      auto value = uint16_t(n * (int(i) + 1) - 500);
      device.memory[0x4003 + i] = Wire.registers.memory[0x4003 + i] = value;
    }
    sensor.poll(true, now);
    const auto count = device.reads;
    sensor.poll(true, now + 10);
    assert(count == device.reads);
    sensor.poll(true, now + 100);
    Reading message{};
    assert(stock.getMetrics(&message));
    auto actual = sensor.snapshot(now + 100);
    assert(actual && message.variant.environment_metrics.has_temperature);
    assert(std::memcmp(&*actual,
                       &message.variant.environment_metrics.temperature,
                       sizeof(float)) == 0);
  }
  assert(!sensor.snapshot(now + 91000));
  sensor.poll(true, now);
  device.failed = 0x4005;
  sensor.poll(true, now + 100);
  assert(!sensor.snapshot(now + 100));
  device.failed = 0xffff;
  device.ready = false;
  sensor.poll(true, now + 1200);
  sensor.poll(true, now + 2700);
  assert(!sensor.snapshot(now + 2700));
  device.ready = true;
  device.phase = 17;
  sensor.poll(true, now + 4000);
  sensor.poll(true, now + 4100);
  assert(!sensor.snapshot(now + 4100));
  device.phase = 2;
  sensor.poll(true, now + 5300);
  sensor.poll(true, now + 5400);
  assert(sensor.snapshot(now + 5400));
  sensor.poll(false, now + 5700);
  assert(!sensor.snapshot(now + 5700) && (device.memory[0x3001] & 6) == 2);
  for (unsigned failure : {0x24d5, 0x2425, 0x2481}) {
    Registers faulty;
    faulty.failed = failure;
    Sensor<Registers> probe(faulty);
    probe.poll(true, 1);
    probe.poll(true, 120);
    assert(!probe.snapshot(120));
    if (failure == 0x24d5)
      assert(faulty.writes.empty());
  }
  Registers stranger;
  stranger.memory[0x24d5] = 0;
  Sensor<Registers> probe(stranger);
  probe.poll(true, 1);
  probe.poll(false, 300);
  assert(stranger.writes.empty());
  stranger.memory[0x24d5] = stranger.address() >> 1;
  stranger.busy = true;
  probe.poll(true, 1200);
  assert(stranger.writes.empty());
  puts("256 object temperatures bit-identical to stock, both cycles, "
       "calibration, timeout, disabled and fault cases passed");
}
