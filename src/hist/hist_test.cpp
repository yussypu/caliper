#include <cstdint>
#include <cstdio>

#include "hist/hist.h"

// Golden tests for the histogram and the coordinated omission correction.
// No framework: each check prints on failure and the process exits nonzero so
// ctest fails. The histogram is the code every published number runs through,
// so the accuracy and the CO back fill are pinned here.

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

void check_range(uint64_t got, uint64_t lo, uint64_t hi, const char* what) {
  if (got < lo || got > hi) {
    std::printf("FAIL: %s: got %llu, want [%llu, %llu]\n", what,
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(lo),
                static_cast<unsigned long long>(hi));
    g_failures++;
  }
}

using caliper::Histogram;

constexpr int kSig = 3;
constexpr uint64_t kMax = 10'000'000;

void test_empty() {
  Histogram h(kSig, kMax);
  check(h.count() == 0, "empty count is zero");
  check(h.max() == 0, "empty max is zero");
  check(h.value_at_percentile(50.0) == 0, "empty p50 is zero");
}

void test_single_value_accuracy() {
  // Every percentile of a single repeated value lands within the histogram's
  // relative error above that value. At three significant figures the bucket
  // near 5000 ticks is a few ticks wide, so a small absolute slack covers it.
  Histogram h(kSig, kMax);
  for (int i = 0; i < 1000; ++i) h.record(5000);
  check(h.count() == 1000, "single value count");
  check_range(h.value_at_percentile(50.0), 5000, 5010, "single p50");
  check_range(h.value_at_percentile(99.0), 5000, 5010, "single p99");
  check_range(h.value_at_percentile(100.0), 5000, 5010, "single p100");
}

void test_max_is_exact() {
  // max() tracks the raw maximum, not a bucketed value, so a tail sample is
  // reported exactly even though percentiles quantize.
  Histogram h(kSig, kMax);
  h.record(1);
  h.record(123456);
  h.record(42);
  check(h.max() == 123456, "max is exact raw value");
}

void test_linear_distribution() {
  // One sample at each of 1..10000. The rank at a percentile is known, so the
  // returned value is checked against it within the relative error.
  Histogram h(kSig, kMax);
  for (uint64_t v = 1; v <= 10000; ++v) h.record(v);
  check(h.count() == 10000, "linear count");
  check_range(h.value_at_percentile(50.0), 4980, 5030, "linear p50");
  check_range(h.value_at_percentile(99.0), 9880, 9960, "linear p99");
  check(h.max() == 10000, "linear max exact");
  // Percentiles are monotonic across the spectrum.
  uint64_t p10 = h.value_at_percentile(10.0);
  uint64_t p50 = h.value_at_percentile(50.0);
  uint64_t p90 = h.value_at_percentile(90.0);
  uint64_t p99 = h.value_at_percentile(99.0);
  check(p10 <= p50 && p50 <= p90 && p90 <= p99, "percentiles monotonic");
}

void test_co_backfill_count() {
  // A 10000 tick sample against a 1000 tick expected interval back fills the
  // samples a stall would have omitted: 9000, 8000, ... 1000, nine of them,
  // plus the original. The sorted set is 1000..10000 so the median is 5000.
  Histogram h(kSig, kMax);
  h.record_corrected(10000, 1000);
  check(h.count() == 10, "co back fill count");
  check_range(h.value_at_percentile(50.0), 4980, 5030, "co median after fill");
  check(h.max() == 10000, "co keeps original max");
}

void test_co_no_backfill_when_on_time() {
  // A sample at or under the expected interval is on schedule, nothing omitted.
  Histogram h(kSig, kMax);
  h.record_corrected(500, 1000);
  check(h.count() == 1, "co no fill when under interval");
  Histogram h2(kSig, kMax);
  h2.record_corrected(1000, 1000);
  check(h2.count() == 1, "co no fill when equal to interval");
}

void test_reset() {
  Histogram h(kSig, kMax);
  for (int i = 0; i < 100; ++i) h.record(777);
  h.reset();
  check(h.count() == 0, "reset clears count");
  check(h.max() == 0, "reset clears max");
  check(h.value_at_percentile(50.0) == 0, "reset clears percentiles");
}

void test_add_merges() {
  Histogram a(kSig, kMax);
  Histogram b(kSig, kMax);
  for (int i = 0; i < 600; ++i) a.record(3000);
  for (int i = 0; i < 400; ++i) b.record(9000);
  a.add(b);
  check(a.count() == 1000, "add sums counts");
  check(a.max() == 9000, "add takes larger max");
  // 60 percent of samples are at 3000, so p50 is in that bucket and p90 above.
  check_range(a.value_at_percentile(50.0), 3000, 3010, "merged p50");
  check_range(a.value_at_percentile(90.0), 9000, 9020, "merged p90");
}

}  // namespace

int main() {
  test_empty();
  test_single_value_accuracy();
  test_max_is_exact();
  test_linear_distribution();
  test_co_backfill_count();
  test_co_no_backfill_when_on_time();
  test_reset();
  test_add_merges();

  if (g_failures == 0) {
    std::printf("all histogram tests passed\n");
    return 0;
  }
  std::printf("%d histogram checks failed\n", g_failures);
  return 1;
}
