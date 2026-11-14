// Sunrise / sunset calculation (NOAA "Almanac for Computers" method).
// Pure C++ with no Arduino dependencies so it can be unit-tested on a PC.
#pragma once
#include <math.h>

namespace sun {

static inline double rad(double d) { return d * M_PI / 180.0; }
static inline double deg(double r) { return r * 180.0 / M_PI; }
static inline double wrap(double v, double m) {
  v = fmod(v, m);
  return v < 0 ? v + m : v;
}

static inline int dayOfYear(int year, int month, int day) {
  int n1 = (275 * month) / 9;
  int n2 = (month + 9) / 12;
  int n3 = 1 + ((year - 4 * (year / 4) + 2) / 3);
  return n1 - (n2 * n3) + day - 30;
}

// Returns the event time in UTC hours (0-24). False if the sun never
// rises / sets on that day at that latitude.
static inline bool eventUtc(int year, int month, int day, double lat, double lon, bool rise, double &utcHours) {
  const double zenith = 90.833; // official sunrise/sunset incl. refraction
  int n = dayOfYear(year, month, day);
  double lngHour = lon / 15.0;
  double t = n + ((rise ? 6.0 : 18.0) - lngHour) / 24.0;
  double m = 0.9856 * t - 3.289;
  double l = wrap(m + 1.916 * sin(rad(m)) + 0.020 * sin(rad(2 * m)) + 282.634, 360.0);
  double ra = wrap(deg(atan(0.91764 * tan(rad(l)))), 360.0);
  ra += floor(l / 90.0) * 90.0 - floor(ra / 90.0) * 90.0;
  ra /= 15.0;
  double sinDec = 0.39782 * sin(rad(l));
  double cosDec = cos(asin(sinDec));
  double cosH = (cos(rad(zenith)) - sinDec * sin(rad(lat))) / (cosDec * cos(rad(lat)));
  if (cosH > 1.0 || cosH < -1.0) return false;
  double h = rise ? 360.0 - deg(acos(cosH)) : deg(acos(cosH));
  h /= 15.0;
  double localT = h + ra - 0.06571 * t - 6.622;
  utcHours = wrap(localT - lngHour, 24.0);
  return true;
}

// Sunrise and sunset as minutes after local midnight.
static inline bool times(int year, int month, int day, double lat, double lon, int tzOffsetMin, int &riseMin, int &setMin) {
  double r, s;
  if (!eventUtc(year, month, day, lat, lon, true, r)) return false;
  if (!eventUtc(year, month, day, lat, lon, false, s)) return false;
  riseMin = (int)wrap(round(r * 60.0) + tzOffsetMin, 1440.0);
  setMin = (int)wrap(round(s * 60.0) + tzOffsetMin, 1440.0);
  return true;
}

} // namespace sun
