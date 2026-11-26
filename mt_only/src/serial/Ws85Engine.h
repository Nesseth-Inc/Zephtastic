// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mt_serial {
struct WeatherReport {
  bool has_wind_speed=false, has_wind_direction=false, has_wind_gust=false;
  bool has_wind_lull=false, has_temperature=false, has_voltage=false;
  bool has_rainfall_24h=false, has_rainfall_1h=false;
  float wind_speed=0;
  float wind_direction=0;
  float temperature=0;
  float voltage=0;
  float wind_gust=0;
  float rainfall_24h=0;
  float rainfall_1h=0;
  float wind_lull=0;
};
using WeatherEmit = bool (*)(void *context, const WeatherReport &report);

// Bounded line parser and five-minute wind accumulator corresponding to stock
// SerialModule::processWXSerial. Values are real input fields, never generated.
class Ws85Engine {
public:
  static constexpr size_t LineCapacity=128;
  static constexpr uint32_t ReportIntervalMs=300000;
  bool feed(const uint8_t *bytes, size_t size);
  bool poll(uint32_t nowMs, WeatherEmit emit, void *context);
  size_t pendingLineBytes() const { return lineUsed_; }
  uint32_t malformedLines() const { return malformedLines_; }

private:
  char line_[LineCapacity]{};
  size_t lineUsed_=0;
  uint32_t malformedLines_=0;
  uint32_t lastAveragedMs_=0;
  double directionSin_=0, directionCos_=0;
  float velocitySum_=0, gust_=0, lull_=-1;
  uint32_t velocityCount_=0, directionCount_=0;
  float batteryVoltage_=0, capacitorVoltage_=0, temperature_=0;
  float rain_=0, rainSum_=0;
  bool gotWind_=false, gustSeen_=false, discardLine_=false;
  bool haveBatteryVoltage_=false, haveCapacitorVoltage_=false;
  bool haveTemperature_=false, haveRain_=false, haveRainSum_=false;
  void lineComplete();
  void resetWindow();
};
} // namespace mt_serial
