// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Arduino.h"
#include <array>
#include <cassert>
#include <map>
#include <vector>

struct Registers {
  std::map<uint16_t, uint16_t> memory;
  std::vector<uint16_t> writes;
  uint16_t failed = 0xffff;
  unsigned phase = 1, reads = 0;
  bool ready = true, busy = false;
  unsigned address() const { return 0x3a; }
  bool word(uint16_t reg, uint16_t &value) {
    ++reads;
    if (reg == failed)
      return false;
    value = reg == 0x3fff
                ? ((phase << 2) | unsigned(ready) | (busy ? 0x200 : 0))
                : memory[reg];
    return true;
  }
  bool wide(uint16_t reg, int32_t &value) {
    uint16_t lo, hi;
    if (!word(reg, lo) || !word(reg + 1, hi))
      return false;
    value = static_cast<int32_t>((uint32_t(hi) << 16) | lo);
    return true;
  }
  bool set(uint16_t reg, uint16_t value) {
    assert(reg == 0x3001 || reg == 0x3fff);
    writes.push_back(reg);
    if (reg == failed)
      return false;
    memory[reg] = value;
    return true;
  }
  void coefficient(uint16_t reg, int32_t value) {
    memory[reg] = uint32_t(value) & 0xffff;
    memory[reg + 1] = uint32_t(value) >> 16;
  }
  Registers() {
    memory[0x24d5] = address() >> 1;
    coefficient(0x2424, 200 * 65536);
    coefficient(0x2426, 1000 * 256);
    coefficient(0x2428, 900000000);
    coefficient(0x242a, -16000);
    coefficient(0x242c, 600000);
    memory[0x242e] = 256;
    memory[0x242f] = 512;
    memory[0x2481] = 16384;
    memory[0x2482] = uint16_t(-8);
    memory[0x4005] = 1600;
    memory[0x4008] = 20000;
  }
};

struct TwoWire {
  Registers registers;
  std::array<uint8_t, 4> bytes{};
  unsigned used = 0, cursor = 0;
  uint16_t reg = 0;
  void beginTransmission(uint8_t addr) {
    assert(addr == registers.address());
    used = 0;
  }
  void write(uint8_t b) { bytes.at(used++) = b; }
  int endTransmission(bool = true) {
    reg = (bytes[0] << 8) | bytes[1];
    assert(used == 2 || used == 4);
    return used == 4 && !registers.set(reg, (bytes[2] << 8) | bytes[3]);
  }
  void requestFrom(uint8_t addr, uint8_t count) {
    assert(addr == registers.address() && (count == 2 || count == 4));
    cursor = used = 0;
    for (unsigned i = 0; i < count / 2; ++i) {
      uint16_t value;
      if (!registers.word(reg + i, value)) {
        used = 0;
        return;
      }
      bytes[used++] = value >> 8;
      bytes[used++] = value & 255;
    }
  }
  int available() const { return used - cursor; }
  uint8_t read() { return bytes.at(cursor++); }
};
inline TwoWire Wire;
