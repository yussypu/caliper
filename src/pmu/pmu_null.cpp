#include "common/platform.h"

#if !CALIPER_NATIVE

#include "pmu/pmu.h"

// Portable PMU backend. No counters on this host, so the snapshot is zeros
// and available() reports false. The harness still records latencies, it
// just cannot attribute a tail to a counter without the native backend.

namespace caliper {

struct Pmu::Impl {};

Pmu::Pmu() : impl_(nullptr) {}
Pmu::~Pmu() {}

bool Pmu::available() const { return false; }
void Pmu::start() {}
PmuSnapshot Pmu::stop() { return PmuSnapshot{0, 0, 0, 0, 0, 0}; }

}  // namespace caliper

#endif  // !CALIPER_NATIVE
