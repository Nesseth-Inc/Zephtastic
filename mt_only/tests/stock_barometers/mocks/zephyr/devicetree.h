// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define DT_ALIAS(n) n
#define DT_NODE_HAS_STATUS(n,s) status(n)
#define status(n) status_##n
#define DT_REG_ADDR(n) addr(n)
#define addr(n) addr_##n
#define DT_NODE_HAS_COMPAT(n,c) 1
#define DT_FOREACH_STATUS_OKAY(c,fn)
#define DT_ON_BUS(n,b) 0
#define addr_zt_aht 0x38
#define addr_zt_bmp085 0x77
#define addr_zt_dps310 0x76
#define addr_zt_bmp3xx 0x77
#ifdef BMP3_PROFILE
#define status_zt_bmp3xx 1
#else
#define status_zt_bmp085 1
#define status_zt_aht 1
#define status_zt_dps310 1
#endif
#define status_zt_scd30 0
#define status_zt_sen5x 0
#define status_zt_sen6x 0
#define status_zt_as3935 0
#define status_zt_nau7802 0
