// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

struct Registers { uint32_t GPREGRET, GPREGRET2; };
struct Uicr { uint32_t NRFFW[2]; };
extern Registers hardware;
extern Uicr uicr;
#define NRF_POWER (&hardware)
#define NRF_UICR (&uicr)
const volatile uint32_t *test_vectors(uint32_t address);
void nrf_power_gpregret_set(Registers *, uint32_t index, uint32_t value);
