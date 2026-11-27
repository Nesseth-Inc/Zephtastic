// SPDX-License-Identifier: GPL-3.0-only
#include "PowerAnnouncement.h"
#include "PowerQuery.h"
#include "stock/optional/UnitConversions.h"
#include "stock/telemetry/SensorModules.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace donor {
uint32_t utc;
uint32_t getTime() {return utc;}
struct TelemetrySensor {
    unsigned kind;
    bool ok, present;
    bool hasSensor() const {return present;}
    bool getMetrics(meshtastic_Telemetry *report) {
        if (!ok) return false;
        if (kind<2) {
            auto &air=report->variant.air_quality_metrics;
            if (kind==0) {air.has_co2=true;air.co2=432;}
            else {air.has_pm25_standard=true;air.pm25_standard=0;}
        } else {
            auto &health=report->variant.health_metrics;
            if (kind==2) {health.has_heart_bpm=true;health.heart_bpm=71;health.has_spO2=true;health.spO2=98;}
            else {health.has_temperature=true;health.temperature=37.25f;}
        }
        return true;
    }
} max30102Sensor{},mlx90614Sensor{};
struct AirQualityTelemetryModule {
    std::vector<TelemetrySensor *> sensors;
    bool getAirQualityTelemetry(meshtastic_Telemetry *);
};
struct HealthTelemetryModule {static bool getHealthTelemetry(meshtastic_Telemetry *);};
struct Absent {
    bool hasSensor() {return false;}
    bool getMetrics(meshtastic_Telemetry *) {assert(false);return false;}
} ina219Sensor,ina260Sensor,max17048Sensor;
struct INA226Sensor {
    bool present=false;
    bool hasSensor() const {return present;}
    struct {float getBusVoltage(){return 3.75f;} float getCurrent_mA(){return -12.5f;}} ina226;
    bool getPowerMetrics(meshtastic_Telemetry *);
    bool getMetrics(meshtastic_Telemetry *m) {return getPowerMetrics(m);}
} ina226Sensor;
struct _INA3221Measurements {struct {float voltage,current;} measurements[3];};
struct INA3221Sensor {
    bool present=false;
    bool hasSensor() const {return present;}
    _INA3221Measurements getMeasurements() {return {{{0.f,-5.f},{4.65f,0.f},{12.f,19.f}}};}
    bool getPowerMetrics(meshtastic_Telemetry *);
    bool getMetrics(meshtastic_Telemetry *m) {return getPowerMetrics(m);}
} ina3221Sensor;
struct PowerTelemetryModule {static bool getPowerTelemetry(meshtastic_Telemetry *);};
#define HAS_TELEMETRY 1
#define LOG_DEBUG(...)
#include "reports.inc"
#undef LOG_DEBUG
#undef HAS_TELEMETRY
}
static std::vector<uint8_t> wire(const meshtastic_Telemetry &t) {
    uint8_t data[233];auto out=pb_ostream_from_buffer(data,sizeof(data));
    assert(pb_encode(&out,meshtastic_Telemetry_fields,&t));return {data,data+out.bytes_written};
}
struct Sample {
    meshtastic_AirQualityMetrics air{};
    meshtastic_HealthMetrics health{};
    bool hasAir=false,hasHealth=false;
};
using Air=meshtastic_AirQualityMetrics;
struct AirReader {
    bool present,ok;unsigned index;
    explicit operator bool() const {return present;}
    bool operator()(Air &m) const {
        if (!ok) return false;
        if (index==0) {m.has_co2=true;m.co2=432;}
        else {m.has_pm25_standard=true;m.pm25_standard=0;}
        return true;
    }
};
static void sensors(unsigned &airCount,unsigned &healthCount) {
    for (unsigned flags=0;flags<16;++flags) for (auto time:{0u,1700000000u}) {
        donor::utc=time;
        const bool first=flags&1,okFirst=flags&2,second=flags&4,okSecond=flags&8;
        donor::TelemetrySensor one{0,okFirst,first},two{1,okSecond,second};
        donor::AirQualityTelemetryModule stockAir;
        if(first)stockAir.sensors.push_back(&one);
        if(second)stockAir.sensors.push_back(&two);
        Sample sample;
        AirReader readers[]={{first,okFirst,0},{second,okSecond,1}};
        sample.hasAir=mt::stock::AirQualityTelemetryModule::getAirQualityTelemetry(sample.air,readers);
        meshtastic_Telemetry reference{},actual{};
        assert(sample.hasAir==stockAir.getAirQualityTelemetry(&reference));
        assert(mt::stock::telemetry::allocReply(meshtastic_Telemetry_air_quality_metrics_tag,sample,time,actual)==sample.hasAir);
        if(sample.hasAir)assert(wire(actual)==wire(reference));
        ++airCount;
        donor::max30102Sensor={2,okFirst,first};donor::mlx90614Sensor={3,okSecond,second};
        sample.hasHealth=mt::stock::HealthTelemetryModule::getHealthTelemetry(sample.health,first,
            [&](auto &m){if(okFirst){m.has_heart_bpm=true;m.heart_bpm=71;m.has_spO2=true;m.spO2=98;}return okFirst;},
            second,[&](auto &m){if(okSecond){m.has_temperature=true;m.temperature=37.25f;}return okSecond;});
        assert(sample.hasHealth==donor::HealthTelemetryModule::getHealthTelemetry(&reference));
        assert(mt::stock::telemetry::allocReply(meshtastic_Telemetry_health_metrics_tag,sample,time,actual)==sample.hasHealth);
        if(sample.hasHealth)assert(wire(actual)==wire(reference));
        ++healthCount;
    }
}
static unsigned power() {
    unsigned count=0;
    for(bool single:{false,true})for(bool triple:{false,true})for(auto time:{0u,1700000000u}){
        donor::utc=time;donor::ina226Sensor.present=single;donor::ina3221Sensor.present=triple;
        meshtastic_Telemetry expected{};const bool available=donor::PowerTelemetryModule::getPowerTelemetry(&expected);
        zt::telemetry::PowerReadings sample;
        if(single){sample.voltage[0]=3.75f;sample.current[0]=-12.5f;}
        if(triple){
            sample.voltage[0]=0.f;sample.voltage[1]=4.65f;sample.voltage[2]=12.f;
            sample.current[0]=-5.f;sample.current[1]=0.f;sample.current[2]=19.f;
        }
        for(auto request:{0u,0x3456u}){
            uint8_t bytes[239];size_t n=0;
            const bool emitted=request?mt::PowerQuery::response(sample,request,bytes,sizeof(bytes),n,time):
                mt::PowerAnnouncement::encode(sample,bytes,sizeof(bytes),n,time);
            assert(emitted==available);
            if(emitted){
                meshtastic_Data data{};auto in=pb_istream_from_buffer(bytes,n);
                assert(pb_decode(&in,meshtastic_Data_fields,&data)&&!in.bytes_left);
                assert(data.request_id==request&&data.portnum==meshtastic_PortNum_TELEMETRY_APP&&!data.want_response);
                assert(std::vector<uint8_t>(data.payload.bytes,data.payload.bytes+data.payload.size)==wire(expected));
            }else assert(!n);
            ++count;
        }
    }
    return count;
}
static unsigned conversions() {
    unsigned count=0;
    for(float v:{-1e10f,-1e9f,-40.f,-0.f,0.f,0.125f,37.f,1003.5f,1e9f,1e10f,
                 std::numeric_limits<float>::max(),-INFINITY,INFINITY,NAN}){
        auto same=[](float a,float b){return std::isnan(a)?std::isnan(b):(a==b&&std::signbit(a)==std::signbit(b));};
        using Adapter=mt::stock::UnitConversions;using Source=donor::UnitConversions;
        assert(same(Adapter::displaySafeFloat(v),Source::displaySafeFloat(v)));
        assert(same(Adapter::CelsiusToFahrenheit(v),Source::CelsiusToFahrenheit(v)));
        assert(same(Adapter::MetersPerSecondToKnots(v),Source::MetersPerSecondToKnots(v)));
        assert(same(Adapter::HectoPascalToInchesOfMercury(v),Source::HectoPascalToInchesOfMercury(v)));
        count+=4;
    }
    return count;
}
void testTelemetryWire();
void testSensorSchedule();
void testSensorTransports();
void phoneCases();
int main(){
    unsigned air=0,health=0;
    sensors(air,health);testTelemetryWire();testSensorSchedule();testSensorTransports();phoneCases();
    std::printf("{\"air_reports\":%u,\"health_reports\":%u,\"power_reports\":%u,\"conversion_values\":%u}\n",
                air,health,power(),conversions());
}
