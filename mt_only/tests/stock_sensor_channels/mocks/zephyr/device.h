#pragma once
struct device {unsigned id;};
inline constexpr device air_quality{1},air_quality_2{2},body_temperature{3},zt_environment{4},thermometer6{5},zt_optical{6};
#define zt_scd4x air_quality_2
#define zt_shtxx zt_environment

#define DEVICE_DT_GET(n) (&n)
#define opt_zt_one_wire_0 nullptr
#define opt_zt_one_wire_1 nullptr
#define opt_zt_one_wire_2 nullptr
#define opt_zt_one_wire_3 nullptr
#define opt_zt_one_wire_4 nullptr
#define opt_zt_one_wire_5 nullptr
#define opt_zt_one_wire_6 (&thermometer6)
#define opt_zt_one_wire_7 nullptr
#define test_dev(n) opt_##n
#define DEVICE_DT_GET_OR_NULL(n) test_dev(n)
bool device_is_ready(const device *);
