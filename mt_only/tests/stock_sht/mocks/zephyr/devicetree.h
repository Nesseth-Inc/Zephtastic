// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define DT_ALIAS(n) n
#define DT_NODE_HAS_STATUS(n,s) status(n)
#define status(n) status_##n
#define DT_REG_ADDR(n) addr(n)
#define addr(n) addr_##n
#define DT_NODE_HAS_COMPAT(n,c) 1
#define DT_FOREACH_STATUS_OKAY(c,fn)
#define DT_SAME_NODE(a,b) 1
#define DT_BUS(n) 0
#define DT_ON_BUS(n,b) 0
#define addr_zt_sht40 0x40
#define addr_zt_sht44 0x44
#define addr_zt_sht45 0x45
#define addr_zt_sht70 0x70
#define status_zt_sht40 1
#define status_zt_sht44 1
#define status_zt_sht45 1
#define status_zt_sht70 1
#define status_zt_scd30 0
#define status_zt_sen5x 0
#define status_zt_sen6x 0
#define status_zt_as3935 0
#define status_zt_nau7802 0
