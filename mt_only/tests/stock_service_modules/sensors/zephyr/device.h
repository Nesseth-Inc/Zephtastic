#pragma once
struct device { unsigned id; };
inline constexpr device air_quality{1},health_sensor{2};
#define DEVICE_DT_GET(node) (&node)
inline bool device_is_ready(const device *dev){return dev!=nullptr;}
