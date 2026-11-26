// SPDX-License-Identifier: GPL-3.0-only
#include "stock/peripherals/SerialWeather.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
struct GeoCoord {static double toDegrees(double r){return r*180/3.1415926535897932384626433832795;}};
meshtastic_Telemetry oracle(float velSum,int velCount,double dir_sum_sin,double dir_sum_cos,
                            int dirCount,float temperatureF,float capVoltageF,float batVoltageF,
                            float gust,int rainSum,float rain,float lull)
{
    uint32_t lastAveraged;
    auto millis=[] { return 300000u; };
#include "donor_ws85.inc"
    (void)lastAveraged;
    return m;
}
struct Capture {
    bool permit;
    meshtastic_Telemetry wire{};
    static bool receive(void *ctx,const mt_serial::WeatherReport &r){
        auto &self=*static_cast<Capture*>(ctx);
        mt::stock::serial::weatherTelemetry(r,self.wire);
        return self.permit;
    }
};
}
void weatherCases()
{
    mt_serial::Ws85Engine engine;
    auto feed=[&](const char *s){return engine.feed(reinterpret_cast<const uint8_t*>(s),strlen(s));};
    Capture capture{};
    // A whole report delivered in fragments, spanning north and including a genuine zero.
    assert(feed("WindDir = 345\nWin")&&feed("dDir = 10\r\nWindSpeed=1.2\nWindSpeed=2.0\nWindGust=3.5\nBatVoltage=3.0\nCapVoltage=4.1\nGXTS04Temp=-3.1\nRain=0\nRainIntSum=12.9\n"));
    const double pi=3.1415926535897932384626433832795;
    auto expected=oracle(3.2f,2,sin(345*pi/180)+sin(10*pi/180),cos(345*pi/180)+cos(10*pi/180),2,-3.1f,4.1f,3,3.5f,12,0,1.2f);
    assert(!engine.poll(299999,Capture::receive,&capture));
    assert(!engine.poll(300000,Capture::receive,&capture)); // rejected queue admission retains data
    capture.permit=true;
    assert(engine.poll(300001,Capture::receive,&capture));
    auto &v=capture.wire.variant.environment_metrics;
    auto &d=expected.variant.environment_metrics;
    assert(v.has_wind_direction && v.wind_direction==d.wind_direction && v.wind_speed==d.wind_speed);
    assert(v.has_wind_gust && v.wind_gust==d.wind_gust && v.has_wind_lull && v.wind_lull==d.wind_lull);
    assert(v.has_voltage && v.voltage==d.voltage && v.has_temperature && v.temperature==d.temperature);
    assert(v.has_rainfall_24h && v.rainfall_24h==d.rainfall_24h && v.has_rainfall_1h && v.rainfall_1h==d.rainfall_1h);
    mt_serial::Ws85Engine partial;
    const char *wind="WindDir=0\nWindSpeed=0\n";
    assert(partial.feed(reinterpret_cast<const uint8_t*>(wind),strlen(wind)));
    assert(partial.poll(300000,Capture::receive,&capture));
    assert(v.has_wind_speed && v.wind_speed==0 && v.has_wind_direction && v.wind_direction==0);
    assert(!v.has_temperature && !v.has_wind_gust && !v.has_voltage && !v.has_rainfall_24h && !v.has_rainfall_1h);
    puts("PASS WS85 wind aggregation, quantities, partial input, missing readings, unsigned cadence and queue retry");
}
