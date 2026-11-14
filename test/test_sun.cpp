// Host-side check of the sunrise/sunset maths:
//   g++ -std=c++17 -o /tmp/test_sun test/test_sun.cpp && /tmp/test_sun
#include "../src/sun.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
static void check(const char *name, int y, int m, int d, double lat, double lon, int tz, int expRise, int expSet) {
  int r = 0, s = 0;
  bool ok = sun::times(y, m, d, lat, lon, tz, r, s);
  bool pass = ok && abs(r - expRise) <= 4 && abs(s - expSet) <= 4;
  printf("%-22s rise %02d:%02d set %02d:%02d  %s\n", name, r / 60, r % 60, s / 60, s % 60, pass ? "ok" : "FAIL");
  if (!pass) fails++;
}

int main() {
  // Expected values from published almanac tables (tolerance 4 min).
  check("Hyderabad 14 Nov", 2026, 11, 14, 17.385, 78.4867, 330, 6 * 60 + 21, 17 * 60 + 40);
  check("Chandigarh 21 Jun", 2026, 6, 21, 30.7333, 76.7794, 330, 5 * 60 + 20, 19 * 60 + 28);
  check("London 21 Dec", 2026, 12, 21, 51.5074, -0.1278, 0, 8 * 60 + 4, 15 * 60 + 53);
  check("Melbourne 1 Jan", 2026, 1, 1, -37.8136, 144.9631, 660, 6 * 60 + 1, 20 * 60 + 46);
  int r, s;
  if (sun::times(2026, 6, 21, 78.0, 15.0, 120, r, s)) { printf("polar day should have no sunset FAIL\n"); fails++; }
  return fails ? 1 : 0;
}
