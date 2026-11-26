// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define DT_ALIAS(n) n
#define DT_NODE_HAS_STATUS(n,s) status(n)
#define status(n) status_##n
#define status_zt_veml7700 1
#define DT_NODE_HAS_COMPAT(n,c) 1
#define DT_REG_ADDR(n) 0x10
#define DT_ON_BUS(n,b) 0
#define status_zt_scd30 0
#define status_zt_sen5x 0
#define status_zt_sen6x 0
#define status_zt_as3935 0
#define status_zt_nau7802 0
#define DT_FOREACH_STATUS_OKAY(c,fn)
