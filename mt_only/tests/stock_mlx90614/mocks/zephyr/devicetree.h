// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define DT_ALIAS(n) n
#define DT_NODE_HAS_STATUS(n,s) status(n)
#define status(n) status_##n
#define status_zt_mlx90614 1
#define status_zt_scd30 0
#define status_zt_sen5x 0
#define status_zt_sen6x 0
#define status_zt_as3935 0
#define status_zt_nau7802 0
#define status_body_temperature 0
#define status_health_sensor 0
#define address_zt_mlx90614 0x5a
#define DT_NODE_HAS_COMPAT(n,c) 1
#define DT_REG_ADDR(n) 0x5a
#define DT_PROP(n,p) 100000
