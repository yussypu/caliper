#pragma once

#include <cstdint>

#include "common/platform.h"

#if CALIPER_X86_64
#include <x86intrin.h>
#elif CALIPER_APPLE
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

// Cycle accurate timing. now() returns a raw counter read at the cheapest
// available cost. On x86_64 this is rdtscp, which serializes against earlier
// loads and reads the core counter. Elsewhere it is the platform monotonic
// counter so the loop runs for development. to_ns turns a tick delta into
// nanoseconds using the calibrated rate, and the calibrated rdtscp overhead
// is subtracted from every stage sample by the caller.

namespace caliper {
namespace timing {

CALIPER_ALWAYS_INLINE uint64_t now() {
#if CALIPER_X86_64
  unsigned aux;
  uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#elif CALIPER_APPLE
  return mach_absolute_time();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
#endif
}

struct Calibration {
  double ns_per_tick;
  uint64_t overhead_ticks;
  bool invariant_tsc;
};

// Checks for invariant TSC where it applies, calibrates ns per tick against
// the wall clock over a fixed window, and measures the cost of a now() pair.
Calibration calibrate();

CALIPER_ALWAYS_INLINE double to_ns(uint64_t ticks, const Calibration& c) {
  return static_cast<double>(ticks) * c.ns_per_tick;
}

}  // namespace timing
}  // namespace caliper
