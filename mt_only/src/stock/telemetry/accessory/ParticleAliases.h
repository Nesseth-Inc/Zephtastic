// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <zephyr/devicetree.h>

#if defined(CONFIG_MT_HM330X)
#define HM_AIR(alias) (DT_NODE_HAS_STATUS(DT_ALIAS(alias), okay) && DT_NODE_HAS_COMPAT(DT_ALIAS(alias), seeed_hm330x))
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_hm330x), okay)
#define MT_HM_ALIAS zt_hm330x
#elif HM_AIR(air_quality)
#define MT_HM_ALIAS air_quality
#elif HM_AIR(air_quality_2)
#define MT_HM_ALIAS air_quality_2
#elif HM_AIR(air_quality_3)
#define MT_HM_ALIAS air_quality_3
#endif
#else
#define HM_AIR(alias) 0
#endif
#if defined(CONFIG_MT_PMSA003I)
#define PMS_AIR(alias) (DT_NODE_HAS_STATUS(DT_ALIAS(alias), okay) && DT_NODE_HAS_COMPAT(DT_ALIAS(alias), plantower_pmsa003i))
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_pmsa003i), okay)
#define MT_PMS_ALIAS zt_pmsa003i
#elif PMS_AIR(air_quality)
#define MT_PMS_ALIAS air_quality
#elif PMS_AIR(air_quality_2)
#define MT_PMS_ALIAS air_quality_2
#elif PMS_AIR(air_quality_3)
#define MT_PMS_ALIAS air_quality_3
#endif
#else
#define PMS_AIR(alias) 0
#endif
#if defined(MT_HM_ALIAS)
#define HM_OWNED(alias) (HM_AIR(alias) && DT_SAME_NODE(DT_ALIAS(alias),DT_ALIAS(MT_HM_ALIAS)))
#else
#define HM_OWNED(alias) 0
#endif
#if defined(MT_PMS_ALIAS)
#define PMS_OWNED(alias) (PMS_AIR(alias) && DT_SAME_NODE(DT_ALIAS(alias),DT_ALIAS(MT_PMS_ALIAS)))
#else
#define PMS_OWNED(alias) 0
#endif
#define GENERIC_AIR(alias) (DT_NODE_HAS_STATUS(DT_ALIAS(alias), okay) && !HM_OWNED(alias) && !PMS_OWNED(alias))
