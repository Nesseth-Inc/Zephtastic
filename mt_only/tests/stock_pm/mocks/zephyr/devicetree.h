// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define DT_ALIAS(n) n
#define DT_NODE_HAS_STATUS(n,s) pm_status(n)
#define pm_status(n) status_##n
#define status_zt_scd30 0
#define status_zt_sen5x 0
#define status_zt_sen6x 0
#define status_zt_as3935 0
#define status_zt_nau7802 0
#define status_zt_radsens 0
#define status_zt_rcwl9620 0
#define status_zt_sfa30 0
#define status_zt_hm330x 0
#ifdef CONFIG_MT_PMSA003I
#define status_air_quality 1
#define status_air_quality_2 0
#else
#define status_air_quality 0
#define status_air_quality_2 1
#endif
#define status_zt_pmsa003i 0
#define status_air_quality_3 0
#define status_body_temperature 0
#define status_health_sensor 0
#define DT_NODE_HAS_COMPAT(n,c) pm_compatible(n,c)
#define pm_compatible(n,c) compat_##n##_##c
#define compat_air_quality_2_seeed_hm330x 1
#define compat_air_quality_2_zephtastic_hm330x 0
#define DT_REG_ADDR(n) pm_address(n)
#define pm_address(n) address_##n
#define address_air_quality_2 0x40
#define DT_NODE_HAS_PROP(n,p) 1
#define compat_air_quality_plantower_pmsa003i 1
#define compat_air_quality_zephtastic_pmsa003i 0
#define address_air_quality 0x12
#define DT_SAME_NODE(a,b) 1
