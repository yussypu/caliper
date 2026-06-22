#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/platform.h"

// A high dynamic range histogram in the style of HdrHistogram. Values are
// bucketed so that recording is a fixed cost index with no allocation on the
// path, and a wide range is held at a bounded relative error. record() is the
// only call made from the hot core. The coordinated omission correction is
// applied off the path by record_corrected, which back fills the samples a
// stall would have omitted against the expected arrival interval.

namespace caliper {

class Histogram {
 public:
  // significant_figures sets the relative error, max_value the tracked range.
  Histogram(int significant_figures, uint64_t max_value);

  CALIPER_ALWAYS_INLINE void record(uint64_t value) {
    counts_[index_for(value)]++;
    count_++;
    if (value > max_seen_) max_seen_ = value;
  }

  // Records value, then back fills omitted samples spaced by expected_interval
  // down to value, the correction Gil Tene describes.
  void record_corrected(uint64_t value, uint64_t expected_interval);

  void reset();
  void add(const Histogram& other);

  uint64_t count() const { return count_; }
  uint64_t max() const { return max_seen_; }
  uint64_t value_at_percentile(double percentile) const;
  uint64_t total_count() const { return count_; }

  // Writes a percentile, count, value table for a CDF chart. Stored values are
  // raw ticks, so ns_per_tick converts them to the nanoseconds the column
  // claims. Pass 1.0 to write raw ticks.
  void write_cdf(const char* path, double ns_per_tick) const;

 private:
  size_t index_for(uint64_t value) const;
  uint64_t value_for(size_t index) const;

  int sig_figs_;
  int sub_bucket_half_count_magnitude_;
  size_t sub_bucket_count_;
  size_t sub_bucket_half_count_;
  uint64_t sub_bucket_mask_;
  int unit_magnitude_;
  int bucket_count_;
  size_t counts_len_;
  std::vector<uint64_t> counts_;
  uint64_t count_ = 0;
  uint64_t max_seen_ = 0;
};

}  // namespace caliper
