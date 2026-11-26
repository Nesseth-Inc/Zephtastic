// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt::usb_serial_jtag {
constexpr uint32_t PacketSize = 64;

// The S3 auto-flushes a full FIFO. Zephyr's immediate flush cannot then
// terminate the host's bulk read. Leave one byte queued to finish that
// transfer with a short packet when exactly one full packet remains.
constexpr uint32_t nextWriteSize(uint32_t queued) {
  if (queued == PacketSize) return PacketSize - 1;
  return queued < PacketSize ? queued : PacketSize;
}
}
