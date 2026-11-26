// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <meshtastic/admin.pb.h>
#if defined(CONFIG_MT_STOCK_SHT)
#include "accessory/Accessories.h"
#else
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#endif

namespace mt::stock::telemetry {
inline int setShtAccuracy(const meshtastic_SensorConfig &cfg) {
    if(!cfg.has_shtxx_config||cfg.has_scd4x_config||cfg.has_scd30_config||cfg.has_ds248x_config||
       cfg.has_sen5x_config||cfg.has_sen6x_config||cfg.has_as3935_config)return -ENOTSUP;
    const auto &sht=cfg.shtxx_config;
    if(!sht.has_set_accuracy||sht.set_accuracy>2)return -EINVAL;
#if defined(CONFIG_MT_STOCK_SHT)
    return accessory::selectShtAccuracy(sht.set_accuracy);
#else
    static_assert(DT_NODE_HAS_COMPAT(DT_ALIAS(zt_shtxx),sensirion_sht3xd) ||
                  DT_NODE_HAS_COMPAT(DT_ALIAS(zt_shtxx),sensirion_sht4x),
                  "zt-shtxx must select a stock accuracy-capable SHT3x/SHT4x");
    const device *sensor=DEVICE_DT_GET(DT_ALIAS(zt_shtxx));
    if(!device_is_ready(sensor))return -ENODEV;
    const sensor_value accuracy{static_cast<int32_t>(sht.set_accuracy),0};
    return sensor_attr_set(sensor,SENSOR_CHAN_ALL,SENSOR_ATTR_RESOLUTION,&accuracy);
#endif
}
}
