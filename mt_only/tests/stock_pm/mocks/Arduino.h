// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
using byte = uint8_t;
void delay(unsigned);
unsigned long millis();
enum { HIGH=1, LOW=0 };
void digitalWrite(unsigned,unsigned);
