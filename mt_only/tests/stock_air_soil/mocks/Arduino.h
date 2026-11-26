// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <string.h>
struct Stream;
enum { WB_IO2=2, WB_IO4=4, WB_IO5=5, INPUT=0, OUTPUT=1, LOW=0, HIGH=1 };
void digitalWrite(unsigned, unsigned);
void pinMode(unsigned, unsigned);
void delay(unsigned);
unsigned long millis();
