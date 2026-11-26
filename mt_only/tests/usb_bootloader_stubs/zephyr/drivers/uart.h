// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
struct device {};
constexpr uint32_t UART_LINE_CTRL_BAUD_RATE=1;
int uart_line_ctrl_get(const device *,uint32_t,uint32_t *);
