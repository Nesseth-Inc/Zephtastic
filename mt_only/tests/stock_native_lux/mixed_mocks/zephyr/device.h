// SPDX-License-Identifier: GPL-3.0-only
#pragma once
struct device {unsigned id;};
inline constexpr device zt_environment{0}, zt_environment_2{1};
#define DEVICE_DT_GET(n) (&n)
bool device_is_ready(const device *);
