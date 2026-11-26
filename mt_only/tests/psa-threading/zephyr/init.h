// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define SYS_INIT(fn, level, priority) \
    int (*host_install)(void) = fn; \
    const char *host_init_level = #level; \
    const unsigned host_init_priority = priority
