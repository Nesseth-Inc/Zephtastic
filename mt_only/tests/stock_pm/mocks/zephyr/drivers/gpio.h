// SPDX-License-Identifier: GPL-3.0-only
#pragma once
struct gpio_dt_spec { unsigned dt_flags; };
enum { GPIO_ACTIVE_LOW=1, GPIO_OUTPUT_ACTIVE=2, GPIO_OUTPUT_INACTIVE=3 };
#define GPIO_DT_SPEC_GET(n,p) {0}
bool gpio_is_ready_dt(const gpio_dt_spec *);
int gpio_pin_configure_dt(const gpio_dt_spec *,int);
