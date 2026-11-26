// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
struct NRF_POWER_Type {uint32_t gpregret[2];};
extern NRF_POWER_Type test_power;
#define NRF_POWER (&test_power)
void nrf_power_gpregret_set(NRF_POWER_Type *,uint32_t,uint32_t);
