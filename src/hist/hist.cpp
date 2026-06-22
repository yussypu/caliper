#include "hist/hist.h"

#include <cmath>
#include <cstdio>

namespace caliper {

namespace {

int count_leading_zeros(uint64_t v) {
  if (v == 0) return 64;
  return __builtin_clzll(v);
}

int ceil_log2(double x) {
  return static_cast<int>(std::ceil(std::log2(x)));
}

}  // namespace

Histogram::Histogram(int significant_figures, uint64_t max_value) {
  sig_figs_ = significant_figures;
  unit_magnitude_ = 0;
  int sub_bucket_count_magnitude =
      ceil_log2(2.0 * std::pow(10.0, significant_figures));
  sub_bucket_half_count_magnitude_ = sub_bucket_count_magnitude - 1;
  sub_bucket_count_ = static_cast<size_t>(1) << sub_bucket_count_magnitude;
  sub_bucket_half_count_ = sub_bucket_count_ / 2;
  sub_bucket_mask_ = (sub_bucket_count_ - 1) << unit_magnitude_;

  uint64_t smallest = sub_bucket_count_ << unit_magnitude_;
  int count = 1;
  while (smallest < max_value && count < 64) {
    smallest <<= 1;
    count++;
  }
  bucket_count_ = count;
  counts_len_ = (bucket_count_ + 1) * sub_bucket_half_count_;
  counts_.assign(counts_len_, 0);
}

size_t Histogram::index_for(uint64_t value) const {
  int pow2ceiling = 64 - count_leading_zeros(value | sub_bucket_mask_);
  int bucket_index =
      pow2ceiling - unit_magnitude_ - (sub_bucket_half_count_magnitude_ + 1);
  if (bucket_index < 0) bucket_index = 0;
  size_t sub_bucket_index = value >> (bucket_index + unit_magnitude_);
  size_t base = static_cast<size_t>(bucket_index + 1)
                << sub_bucket_half_count_magnitude_;
  size_t idx = base + (sub_bucket_index - sub_bucket_half_count_);
  if (idx >= counts_len_) idx = counts_len_ - 1;
  return idx;
}

uint64_t Histogram::value_for(size_t index) const {
  int bucket_index =
      static_cast<int>(index >> sub_bucket_half_count_magnitude_) - 1;
  int64_t sub_bucket_index =
      static_cast<int64_t>(index & (sub_bucket_half_count_ - 1)) +
      sub_bucket_half_count_;
  if (bucket_index < 0) {
    sub_bucket_index -= sub_bucket_half_count_;
    bucket_index = 0;
  }
  return static_cast<uint64_t>(sub_bucket_index)
         << (bucket_index + unit_magnitude_);
}

void Histogram::record_corrected(uint64_t value, uint64_t expected_interval) {
  record(value);
  if (expected_interval == 0 || value <= expected_interval) return;
  for (uint64_t missing = value - expected_interval;
       missing >= expected_interval; missing -= expected_interval) {
    record(missing);
  }
}

void Histogram::reset() {
  std::fill(counts_.begin(), counts_.end(), 0);
  count_ = 0;
  max_seen_ = 0;
}

void Histogram::add(const Histogram& other) {
  if (other.counts_len_ != counts_len_) return;
  for (size_t i = 0; i < counts_len_; ++i) counts_[i] += other.counts_[i];
  count_ += other.count_;
  if (other.max_seen_ > max_seen_) max_seen_ = other.max_seen_;
}

uint64_t Histogram::value_at_percentile(double percentile) const {
  if (count_ == 0) return 0;
  double clamped = percentile < 0.0 ? 0.0 : (percentile > 100.0 ? 100.0 : percentile);
  uint64_t target = static_cast<uint64_t>(
      std::ceil((clamped / 100.0) * static_cast<double>(count_)));
  if (target == 0) target = 1;
  uint64_t running = 0;
  for (size_t i = 0; i < counts_len_; ++i) {
    running += counts_[i];
    if (running >= target) {
      uint64_t low = value_for(i);
      int bucket_index =
          static_cast<int>(i >> sub_bucket_half_count_magnitude_) - 1;
      if (bucket_index < 0) bucket_index = 0;
      uint64_t size = static_cast<uint64_t>(1) << (bucket_index + unit_magnitude_);
      return low + size - 1;
    }
  }
  return max_seen_;
}

void Histogram::write_cdf(const char* path, double ns_per_tick) const {
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  std::fprintf(f, "value_ns,percentile,cumulative_count\n");
  uint64_t running = 0;
  for (size_t i = 0; i < counts_len_; ++i) {
    if (counts_[i] == 0) continue;
    running += counts_[i];
    double pct = 100.0 * static_cast<double>(running) /
                 static_cast<double>(count_ == 0 ? 1 : count_);
    uint64_t low = value_for(i);
    int bucket_index =
        static_cast<int>(i >> sub_bucket_half_count_magnitude_) - 1;
    if (bucket_index < 0) bucket_index = 0;
    uint64_t size = static_cast<uint64_t>(1) << (bucket_index + unit_magnitude_);
    double value_ns = static_cast<double>(low + size - 1) * ns_per_tick;
    std::fprintf(f, "%.1f,%.6f,%llu\n", value_ns, pct,
                 static_cast<unsigned long long>(running));
  }
  std::fclose(f);
}

}  // namespace caliper
