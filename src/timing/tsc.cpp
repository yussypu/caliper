#include "timing/tsc.h"

#include <algorithm>
#include <array>
#include <ctime>

#if CALIPER_X86_64 && CALIPER_LINUX
#include <cpuid.h>
#endif

namespace caliper {
namespace timing {

namespace {

bool detect_invariant_tsc() {
#if CALIPER_X86_64 && CALIPER_LINUX
  unsigned a, b, c, d;
  if (!__get_cpuid(0x80000000u, &a, &b, &c, &d)) return false;
  if (a < 0x80000007u) return false;
  __get_cpuid(0x80000007u, &a, &b, &c, &d);
  return (d & (1u << 8)) != 0;
#else
  return false;
#endif
}

uint64_t mono_ns() {
#if CALIPER_APPLE
  static mach_timebase_info_data_t tb{0, 0};
  if (tb.denom == 0) mach_timebase_info(&tb);
  return mach_absolute_time() * tb.numer / tb.denom;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
#endif
}

double measure_ns_per_tick() {
  const uint64_t window_ns = 50ull * 1000ull * 1000ull;
  uint64_t w0 = mono_ns();
  uint64_t t0 = now();
  uint64_t w1 = w0;
  while (w1 - w0 < window_ns) w1 = mono_ns();
  uint64_t t1 = now();
  double dt_ticks = static_cast<double>(t1 - t0);
  double dt_ns = static_cast<double>(w1 - w0);
  if (dt_ticks <= 0.0) return 1.0;
  return dt_ns / dt_ticks;
}

uint64_t measure_overhead() {
  std::array<uint64_t, 4096> samples;
  for (auto& s : samples) {
    uint64_t a = now();
    uint64_t b = now();
    s = b - a;
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

}  // namespace

Calibration calibrate() {
  Calibration c;
  c.invariant_tsc = detect_invariant_tsc();
  // Warm the path before measuring either rate or overhead.
  for (int i = 0; i < 16; ++i) measure_overhead();
  double best = measure_ns_per_tick();
  for (int i = 0; i < 4; ++i) best = std::min(best, measure_ns_per_tick());
  c.ns_per_tick = best;
  uint64_t ov = measure_overhead();
  for (int i = 0; i < 4; ++i) ov = std::min(ov, measure_overhead());
  c.overhead_ticks = ov;
  return c;
}

}  // namespace timing
}  // namespace caliper
