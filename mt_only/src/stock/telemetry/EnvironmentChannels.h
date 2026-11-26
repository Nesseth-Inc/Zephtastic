// SPDX-License-Identifier: GPL-3.0-only
// Units used by pinned BME680, INA219, VEML and distance telemetry on the wire.
#pragma once
#include <float.h>
#include <stdint.h>
#include <meshtastic/telemetry.pb.h>

namespace mt::stock::telemetry {
// Increasing order matches the light producers in EnvironmentTelemetry::setup.
enum class LuxSource : uint8_t { Declared, Ltr390, Veml7700, Tsl2591, Opt3001, Tsl2561, Bh1750 };

struct EnvironmentChannels {
#if defined(CONFIG_MT_STOCK_INA_METRICS) && !defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS)
    enum Field { Voltage, CurrentMa, Count };
#else
    enum Field { GasKohms, Voltage, CurrentMa, Lux, IrLux, DistanceMm, Count };
#endif
    float values[Count]{};
    uint8_t fields=0;
    bool has(Field field) const { return fields & (1u<<field); }
    void record(Field field,float value) {
        if (!(value>=-FLT_MAX && value<=FLT_MAX) || (field!=CurrentMa && value<0)) return;
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || !defined(CONFIG_MT_STOCK_INA_METRICS)
        if(field==GasKohms && value==0)return;
#endif
        fields|=uint8_t(1u<<field);
        values[field]=value;
    }
#if defined(CONFIG_MT_NATIVE_LUX)
    LuxSource luxSource=LuxSource::Declared;
    void recordLux(LuxSource source,float lux) {
        if (source<luxSource || !(lux>=0 && lux<=FLT_MAX)) return;
        record(Lux,lux);
        luxSource=source;
    }
#endif
    void apply(meshtastic_EnvironmentMetrics &m) const {
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || !defined(CONFIG_MT_STOCK_INA_METRICS)
        if(has(GasKohms)){m.has_gas_resistance=true;m.gas_resistance=values[GasKohms];}
#endif
        if(has(Voltage)){m.has_voltage=true;m.voltage=values[Voltage];}
        if(has(CurrentMa)){m.has_current=true;m.current=values[CurrentMa];}
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || !defined(CONFIG_MT_STOCK_INA_METRICS)
        if(has(Lux)){m.has_lux=true;m.lux=values[Lux];}
        if(has(IrLux)){m.has_ir_lux=true;m.ir_lux=values[IrLux];}
        if(has(DistanceMm)){m.has_distance=true;m.distance=values[DistanceMm];}
#endif
    }
    void from(const meshtastic_EnvironmentMetrics &m) {
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || !defined(CONFIG_MT_STOCK_INA_METRICS)
        if(m.has_gas_resistance) record(GasKohms,m.gas_resistance);
#endif
        if(m.has_voltage) record(Voltage,m.voltage);
        if(m.has_current) record(CurrentMa,m.current);
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || !defined(CONFIG_MT_STOCK_INA_METRICS)
        if(m.has_lux) record(Lux,m.lux);
        if(m.has_ir_lux) record(IrLux,m.ir_lux);
        if(m.has_distance) record(DistanceMm,m.distance);
#endif
    }
};
}
