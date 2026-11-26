// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "LarkWeather.h"
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#define MT_DROPZONE_NODE DT_ALIAS(zt_dropzone)
#define MT_DROPZONE_HAS_LARK (DT_NODE_HAS_STATUS(MT_DROPZONE_NODE,okay) && DT_NODE_HAS_PROP(MT_DROPZONE_NODE,lark) && DT_NODE_HAS_STATUS(DT_PHANDLE(MT_DROPZONE_NODE,lark),okay) && DT_NODE_HAS_STATUS(DT_BUS(DT_PHANDLE(MT_DROPZONE_NODE,lark)),okay))
#if DT_NODE_HAS_STATUS(MT_DROPZONE_NODE,okay)
#include <zephyr/drivers/adc.h>
#if MT_DROPZONE_HAS_LARK
#include <zephyr/drivers/i2c.h>
#endif
#endif
#if MT_DROPZONE_HAS_LARK && defined(CONFIG_MT_LARK_WEATHER)
#define MT_DROPZONE_SHARED_LARK (DT_SAME_NODE(DT_BUS(DT_PHANDLE(MT_DROPZONE_NODE,lark)),DT_BUS(DT_ALIAS(zt_lark))) && \
                                 DT_REG_ADDR(DT_PHANDLE(MT_DROPZONE_NODE,lark))==DT_REG_ADDR(DT_ALIAS(zt_lark)))
#include "stock/telemetry/accessory/Accessories.h"
#else
#define MT_DROPZONE_SHARED_LARK 0
#endif
namespace mt::stock::optional {
class DropzoneBoard {
    enum class Stage {Idle,Status,Weather,Complete};
    Stage stage_=Stage::Idle;
    uint32_t node_=0,epoch_=0;
    Conditions obs_{};
#if MT_DROPZONE_HAS_LARK && !MT_DROPZONE_SHARED_LARK
    LarkWeather lark_;
#endif
public:
    void request(uint32_t node,uint32_t epoch){
        stage_=Stage::Status;node_=node;epoch_=epoch;obs_={};
#if MT_DROPZONE_HAS_LARK && !MT_DROPZONE_SHARED_LARK
        lark_={};
#endif
    }
    bool take(uint32_t node,uint32_t epoch,Conditions &out) {
        if(stage_!=Stage::Complete)return false;
        stage_=Stage::Idle;
        if(node!=node_||epoch!=epoch_)return false;
        out=obs_;return true;
    }
    void hardwarePoll(uint32_t now){
#if DT_NODE_HAS_STATUS(MT_DROPZONE_NODE,okay)
        if(stage_==Stage::Status){
            static const adc_dt_spec input=ADC_DT_SPEC_GET(MT_DROPZONE_NODE);
            adc_sequence seq{};int16_t raw=0;
            if(!adc_is_ready_dt(&input)||adc_channel_setup_dt(&input)||adc_sequence_init_dt(&input,&seq)){
                stage_=Stage::Idle;return;
            }
            seq.buffer=&raw;seq.buffer_size=sizeof(raw);
            if(adc_read_dt(&input,&seq)){stage_=Stage::Idle;return;}
            obs_.open=raw<100;
            stage_=Stage::Weather;
#if MT_DROPZONE_SHARED_LARK
            telemetry::accessory::requestWeather();
#endif
        }
        if(stage_==Stage::Weather){
#if MT_DROPZONE_SHARED_LARK
            telemetry::accessory::Environment weather;
            if(!telemetry::accessory::dropzoneWeather(weather))return;
            if(weather.hasWeather){
                obs_.hasWeather=true;obs_.windMps=weather.wind;obs_.pressureHpa=weather.pressure;
                obs_.celsius=weather.temperature;obs_.bearing=weather.bearing;
            }
#elif MT_DROPZONE_HAS_LARK
            static const i2c_dt_spec bus=I2C_DT_SPEC_GET(DT_PHANDLE(MT_DROPZONE_NODE,lark));
            if(i2c_is_ready_dt(&bus)&&!lark_.poll(now,obs_,
                [](const uint8_t *p,size_t n){return i2c_write_dt(&bus,p,n)==0;},
                [](uint8_t *p,size_t n){return i2c_read_dt(&bus,p,n)==0;}))return;
#endif
            stage_=Stage::Complete;
        }
#else
        stage_=Stage::Idle; // No status pin on the standard T096 or XIAO overlays.
#endif
        (void)now;
    }
};
}
#undef MT_DROPZONE_HAS_LARK
#undef MT_DROPZONE_SHARED_LARK
#undef MT_DROPZONE_NODE
