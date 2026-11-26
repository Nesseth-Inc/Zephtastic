// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ExpandedContacts.h"
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>

namespace mt::pki {
class ZephyrExpandedContactFlash final : public ExpandedContactFlash {
  static_assert(PARTITION_OFFSET(mt_pki_contacts_partition) == 0x480000);
  static_assert(PARTITION_SIZE(mt_pki_contacts_partition) == ExpandedContacts::Bytes);
  void (*checkpoint_)() = nullptr;
  template <class Operation> int access(unsigned offset, size_t n, Operation op) {
    if (offset > ExpandedContacts::Bytes || n > ExpandedContacts::Bytes - offset)
      return -EINVAL;
    const flash_area *area = nullptr;
    int rc = flash_area_open(PARTITION_ID(mt_pki_contacts_partition), &area);
    if (rc)
      return rc;
    rc = op(area);
    flash_area_close(area);
    return rc;
  }

public:
  void checkpoint(void (*callback)()) { checkpoint_ = callback; }
  void progress() override {
    if (checkpoint_)
      checkpoint_();
  }
  int read(unsigned pos, void *out, size_t n) override {
    return access(pos, n, [&](const flash_area *a) { return flash_area_read(a, pos, out, n); });
  }
  int program(unsigned pos, const void *p, size_t n) override {
    return access(pos, n, [&](const flash_area *a) { return flash_area_write(a, pos, p, n); });
  }
  int eraseBank(unsigned bank) override {
    if (bank >= ExpandedContacts::Banks)
      return -EINVAL;
    const unsigned pos = bank * ExpandedContacts::BankBytes;
    return access(pos, ExpandedContacts::BankBytes,
                  [&](const flash_area *a) { return flash_area_erase(a, pos, ExpandedContacts::BankBytes); });
  }
};
} // namespace mt::pki
