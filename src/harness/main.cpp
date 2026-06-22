#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "book/book.h"
#include "common/platform.h"
#include "decode/decode.h"
#include "encode/encode.h"
#include "hist/hist.h"
#include "pmu/pmu.h"
#include "rx/transport.h"
#include "strategy/strategy.h"
#include "timing/tsc.h"

#if CALIPER_NATIVE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace caliper {
namespace {

constexpr uint64_t kHistMaxNs = 10'000'000;  // 10 ms ceiling
constexpr int kSigFigs = 3;

void apply_host_runtime() {
#if CALIPER_NATIVE
  mlockall(MCL_CURRENT | MCL_FUTURE);
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(2, &set);  // hot core, must match scripts/host_setup.sh isolcpus
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

  // Hold the hot core out of deep C states for the run without a reboot.
  // Writing 0 us to /dev/cpu_dma_latency caps wakeup latency while the fd is
  // open, so the core does not pay a deep sleep exit on the path.
  static int dma_latency_fd = open("/dev/cpu_dma_latency", O_WRONLY);
  if (dma_latency_fd >= 0) {
    int32_t target = 0;
    ssize_t w = write(dma_latency_fd, &target, sizeof(target));
    (void)w;  // fd stays open for the process lifetime to hold the request
  }
#endif
}

void ensure_results_dir() { mkdir("results", 0755); }

uint64_t sub_overhead(uint64_t ticks, uint64_t overhead) {
  return ticks > overhead ? ticks - overhead : 0;
}

void write_table(const char* path, const Histogram& wire,
                 const timing::Calibration& cal) {
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  const double ps[] = {50.0, 90.0, 99.0, 99.9, 99.99, 99.999};
  const char* names[] = {"p50", "p90", "p99", "p99.9", "p99.99", "p99.999"};
  std::fprintf(f, "percentile,latency_ns\n");
  for (int i = 0; i < 6; ++i) {
    double ns = timing::to_ns(wire.value_at_percentile(ps[i]), cal);
    std::fprintf(f, "%s,%.1f\n", names[i], ns);
  }
  std::fprintf(f, "max,%.1f\n", timing::to_ns(wire.max(), cal));
  std::fclose(f);
}

void print_summary(const char* label, const Histogram& h,
                   const timing::Calibration& cal) {
  std::printf("%-22s p50=%.0f p99=%.0f p99.9=%.0f p99.99=%.0f max=%.0f (ns)\n",
              label, timing::to_ns(h.value_at_percentile(50.0), cal),
              timing::to_ns(h.value_at_percentile(99.0), cal),
              timing::to_ns(h.value_at_percentile(99.9), cal),
              timing::to_ns(h.value_at_percentile(99.99), cal),
              timing::to_ns(h.max(), cal));
}

int run() {
  apply_host_runtime();
  ensure_results_dir();

  timing::Calibration cal = timing::calibrate();

  Transport transport;
  transport.warm();
  // Size the book to the feed's price band, real venue day or synthetic.
  BookBand band = transport.book_band();
  Book book(band.instruments, band.levels, band.price_base);
  // Fire on a penny wide or tighter spread; one cent is two half ticks.
  Strategy strategy(2);
  Pmu pmu;

  Histogram h_decode(kSigFigs, kHistMaxNs);
  Histogram h_book(kSigFigs, kHistMaxNs);
  Histogram h_decide(kSigFigs, kHistMaxNs);
  Histogram h_encode(kSigFigs, kHistMaxNs);
  Histogram h_wire(kSigFigs, kHistMaxNs);
  Histogram h_wire_co(kSigFigs, kHistMaxNs);

  uint64_t expected_interval_ns = 0;
  uint8_t tx_buf[64];
  uint64_t orders = 0;

  std::printf("caliper backend=%s invariant_tsc=%d ns_per_tick=%.4f overhead=%llu ticks\n",
              transport.backend(), cal.invariant_tsc ? 1 : 0, cal.ns_per_tick,
              static_cast<unsigned long long>(cal.overhead_ticks));
  std::printf("caliper symbol=%.8s book=%d levels base=%lld\n",
              band.symbol, band.levels, static_cast<long long>(band.price_base));

  pmu.start();

  RxPacket pkt;
  while (transport.rx(pkt)) {
    expected_interval_ns = pkt.expected_interval;

    uint64_t t0 = timing::now();
    DecodedMd md{};
    bool ok = decode_md(pkt.data, pkt.len, md);
    uint64_t t1 = timing::now();
    if (CALIPER_UNLIKELY(!ok)) continue;

    book.apply(md);
    uint64_t t2 = timing::now();

    OrderMsg order;
    bool fire = strategy.decide(book, md, order);
    uint64_t t3 = timing::now();

    // decode and book run on every tick, record them regardless of firing.
    h_decode.record(sub_overhead(t1 - t0, cal.overhead_ticks));
    h_book.record(sub_overhead(t2 - t1, cal.overhead_ticks));

    uint64_t tx_ts;
    if (fire) {
      size_t n = encode_order(order, tx_buf, sizeof(tx_buf));
      uint64_t t4 = timing::now();
      tx_ts = transport.tx(tx_buf, n);

      h_decide.record(sub_overhead(t3 - t2, cal.overhead_ticks));
      h_encode.record(sub_overhead(t4 - t3, cal.overhead_ticks));

      uint64_t w2w = sub_overhead(tx_ts - pkt.rx_ts, cal.overhead_ticks);
      uint64_t expected_ticks = static_cast<uint64_t>(
          static_cast<double>(expected_interval_ns) / cal.ns_per_tick);
      h_wire.record(w2w);
      h_wire_co.record_corrected(w2w, expected_ticks);
      orders++;
    }
  }

  PmuSnapshot snap = pmu.stop();

  std::printf("\nclosed loop done: %llu orders fired\n",
              static_cast<unsigned long long>(orders));
  print_summary("tick to trade", h_wire, cal);
  print_summary("tick to trade (CO)", h_wire_co, cal);
  print_summary("  decode", h_decode, cal);
  print_summary("  book", h_book, cal);
  print_summary("  decide", h_decide, cal);
  print_summary("  encode", h_encode, cal);

  if (pmu.available()) {
    double ipc = snap.cycles ? static_cast<double>(snap.instructions) /
                                   static_cast<double>(snap.cycles)
                             : 0.0;
    std::printf(
        "\npmu (whole run, userspace):\n"
        "  cycles=%llu instructions=%llu ipc=%.2f\n"
        "  branch_misses=%llu l1d_misses=%llu llc_misses=%llu dtlb_misses=%llu\n",
        static_cast<unsigned long long>(snap.cycles),
        static_cast<unsigned long long>(snap.instructions), ipc,
        static_cast<unsigned long long>(snap.branch_misses),
        static_cast<unsigned long long>(snap.l1d_misses),
        static_cast<unsigned long long>(snap.llc_misses),
        static_cast<unsigned long long>(snap.dtlb_misses));
  } else {
    std::printf("\npmu: not available on this backend\n");
  }

  write_table("results/tick_to_trade.csv", h_wire, cal);
  write_table("results/tick_to_trade_co.csv", h_wire_co, cal);
  h_wire.write_cdf("results/cdf_wire.csv", cal.ns_per_tick);
  h_wire_co.write_cdf("results/cdf_wire_co.csv", cal.ns_per_tick);
  h_decode.write_cdf("results/cdf_decode.csv", cal.ns_per_tick);
  h_book.write_cdf("results/cdf_book.csv", cal.ns_per_tick);
  h_decide.write_cdf("results/cdf_decide.csv", cal.ns_per_tick);
  h_encode.write_cdf("results/cdf_encode.csv", cal.ns_per_tick);

  std::printf("\nwrote results/ (percentile tables and CDFs)\n");
  return 0;
}

}  // namespace
}  // namespace caliper

int main() { return caliper::run(); }
