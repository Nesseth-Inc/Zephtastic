// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace mt_security {

// Install a fail-closed printk character gate while preserving the platform's
// existing output hook. Call after the Zephyr console backend has initialized.
void initializeConsoleSecurity();

// Open raw serial console output only when the persisted setting is enabled
// and its owning settings store is ready/trusted.
void serialConsolePolicy(bool enabled, bool settingsReady);

// Shared admission predicate for ordinary USB console/API input and output.
// The dedicated Serial Module UART is intentionally outside this policy.
bool serialConsoleAllowed();

} // namespace mt_security
