#pragma once
#include <cstdint>
#include <cerrno>
struct device {};
inline device sensorDevice;
#define DT_PROP(n,p) 1
#define DT_ALIAS(n) sensor_token
#define DT_NODE_HAS_COMPAT(n,c) 1
#define DT_ON_BUS(n,b) 1
#define DEVICE_DT_GET(n) (&sensorDevice)
#define IS_ENABLED(k) 1
extern bool sensorReady;
inline bool device_is_ready(const device*){return sensorReady;}
inline int device_init(const device*){sensorReady=true;return 0;}
