// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace mt::mqtt {
// One-shot test-only install of Root's public CA at the existing MQTT tag.
// False means do not start the M119 TLS test; never fall back to plaintext.
bool installM119TestCa();
}
