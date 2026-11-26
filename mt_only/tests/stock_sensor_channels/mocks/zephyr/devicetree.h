#pragma once
#define DT_HAS_ALIAS(a) test_alias_##a
#define test_alias_air_quality 1
#define test_alias_air_quality_2 1
#define test_alias_air_quality_3 1
#define test_alias_health_sensor 1
#define test_alias_body_temperature 1
#define DT_ALIAS(a) a
#define TEST_STATUS(a) test_status_##a
#define DT_NODE_HAS_STATUS(a,s) TEST_STATUS(a)
#define test_status_zt_environment 1
// Present aliases to disabled nodes have no linkable device object.
#define test_status_air_quality 1
#define test_status_air_quality_2 1
#define test_status_air_quality_3 0
#define test_status_health_sensor 0
#define test_status_body_temperature 1
#define test_status_zt_environment_2 0
#define test_status_zt_one_wire_5 0

#define DT_PROP(n,p) prop_##p
#define prop_smp_sr 100
#define prop_smp_ave 4
#define prop_led_pw 411
#define prop_adc_rge 4096
#define DT_ENUM_IDX(n,p) 2
#define DT_NODE_HAS_COMPAT(n,c) 1
#define DT_PROP_BY_IDX(n,p,i) 60

#define test_status_zt_scd30 1
#define test_status_zt_sen5x 1
#define test_status_zt_sen6x 1
#define test_status_zt_as3935 1
#define test_status_zt_nau7802 1
#define test_status_zt_radsens 1
#define test_status_zt_ltr390 1
#define test_status_zt_rainfall 1
#define test_status_zt_lark 1
#define prop_clock_frequency 100000
#define test_status_zt_rcwl9620 1
