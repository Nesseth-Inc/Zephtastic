// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Diagnostic only. Installation is exclusively the ordered SYS_INIT hook.
bool mt_psa_threading_ready(void);
#ifdef __cplusplus
}
#endif
