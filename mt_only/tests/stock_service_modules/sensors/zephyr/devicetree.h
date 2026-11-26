#pragma once
#define DT_HAS_ALIAS(name) test_alias_##name
#define test_alias_air_quality 1
#define test_alias_health_sensor 1
#define test_alias_air_quality_2 0
#define test_alias_air_quality_3 0
#define test_alias_body_temperature 0
#define DT_ALIAS(name) name
#define DT_TEST_OK(node) test_alias_##node
#define DT_NODE_HAS_STATUS(node,s) DT_TEST_OK(node)
#define DT_PROP_OR(node, property, fallback) test_offset_##property
enum { test_offset_heart_rate_channel_offset=0, test_offset_spo2_channel_offset=1,
       test_offset_body_temperature_channel_offset=2 };
