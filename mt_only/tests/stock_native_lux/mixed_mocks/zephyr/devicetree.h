// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define DT_ALIAS(node) node
#define DT_NODE_HAS_STATUS(node,s) 1
#define MODEL_zt_environment NATIVE_FIRST
#define MODEL_zt_environment_2 NATIVE_SECOND
#define MODEL_rohm_bh1750 6
#define MODEL_ams_tsl2561 5
#define MODEL_ti_opt3001 4
#define MODEL_ams_tsl2591 3
#define MODEL_vishay_veml7700 2
#define SENSOR_MODEL(n) MODEL_##n
#define DT_NODE_HAS_COMPAT(n,c) (SENSOR_MODEL(n)==SENSOR_MODEL(c))
