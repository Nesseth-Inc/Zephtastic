// GnssUtcTime.h - UTC conversion helpers for GNSS receiver time fields.
//
// Copyright (c) 2026 ZephCore. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#ifndef ZT_GNSS_GNSS_UTC_TIME_H_
#define ZT_GNSS_GNSS_UTC_TIME_H_

#include <stdint.h>

namespace zt::gnss {

// UTC fields as reported by GNSS receivers. `centuryYear` is 0..99 and
// denotes the year 2000..2099.
struct UtcFields {
  uint8_t centuryYear;
  uint8_t month;        // 1..12
  uint8_t day;          // 1..days_in_month
  uint8_t hour;         // 0..23
  uint8_t minute;       // 0..59
  uint16_t millisecond; // 0..59999
};

// Converts UTC fields to Unix seconds (seconds since 1970-01-01T00:00:00Z).
// `millisecond` is validated (0..59999); whole seconds within the minute are retained. Leap seconds are not supported. All valid
// dates (2000-01-01 .. 2099-12-31) fit in uint32_t. Returns false and
// leaves `out` unchanged when any field is out of range.
bool toUnixSeconds(const UtcFields &fields, uint32_t &out);

// Number of Gregorian leap years in [1..n] (proleptic calendar).
constexpr uint32_t leapCountThrough(uint32_t n) {
  return n / 4u - n / 100u + n / 400u;
}

// Days from 1970-01-01 to January 1 of `year` (valid for 1970..2100).
constexpr uint32_t daysBeforeYear(uint32_t year) {
  return (year - 1970u) * 365u +
         leapCountThrough(year - 1u) - leapCountThrough(1969u);
}

inline bool toUnixSeconds(const UtcFields &fields, uint32_t &out) {
  constexpr uint8_t kMonthDays[12] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  // `centuryYear` is contractually 0..99; reject other century encodings.
  if (fields.centuryYear > 99u) return false;
  if (fields.month < 1u || fields.month > 12u) return false;
  if (fields.hour > 23u) return false;
  if (fields.minute > 59u) return false;
  if (fields.millisecond > 59999u) return false;
  const uint32_t year = 2000u + fields.centuryYear;
  const bool leap =
      (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
  const uint8_t maxDay = kMonthDays[fields.month - 1u] +
                        (fields.month == 2u && leap ? 1u : 0u);
  if (fields.day < 1u || fields.day > maxDay) return false;
  uint32_t days = daysBeforeYear(year);
  for (uint8_t m = 1u; m < fields.month; ++m)
    days += kMonthDays[m - 1u] + (m == 2u && leap ? 1u : 0u);
  days += fields.day - 1u;
  out = days * 86400u + static_cast<uint32_t>(fields.hour) * 3600u +
      static_cast<uint32_t>(fields.minute) * 60u + fields.millisecond / 1000u;
  return true;
}

}  // namespace zt::gnss

#endif  // ZT_GNSS_GNSS_UTC_TIME_H_
