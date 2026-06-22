#pragma once

#include <cstdint>

#include "common/platform.h"

// PMU counter reads around the hot path. On x86_64 Linux these come from
// perf_event_open: cycles, instructions, L1d misses, LLC misses, branch
// misses, dTLB misses. When a tail moves the counters say which cause moved.
// On the portable backend the snapshot is zeros so the loop still runs.

namespace caliper {

struct PmuSnapshot {
  uint64_t cycles;
  uint64_t instructions;
  uint64_t l1d_misses;
  uint64_t llc_misses;
  uint64_t branch_misses;
  uint64_t dtlb_misses;
};

class Pmu {
 public:
  Pmu();
  ~Pmu();

  bool available() const;
  void start();
  PmuSnapshot stop();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace caliper
