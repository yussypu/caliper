#include "common/platform.h"

#if CALIPER_NATIVE

#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

#include "pmu/pmu.h"

// Native PMU backend on Linux via perf_event_open. The six events are opened
// in one group led by cycles, so a single ioctl resets and enables them
// together and a single grouped read pulls all counts off the hot path in
// stop(). Each event carries its perf id, so the read is matched back to the
// right field by id and an event the CPU does not expose drops out cleanly
// rather than shifting the others. On Zen the generic last level cache miss
// event is not mapped to the core PMU, so llc_misses reports unavailable here
// instead of a fabricated number.

namespace caliper {

namespace {

constexpr int kMaxEvents = 6;

long perf_open(perf_event_attr* attr, int group_fd) {
  return syscall(__NR_perf_event_open, attr, 0, -1, group_fd, 0);
}

uint64_t cache_config(int id, int op, int result) {
  return static_cast<uint64_t>(id) | (static_cast<uint64_t>(op) << 8) |
         (static_cast<uint64_t>(result) << 16);
}

struct EventDef {
  uint32_t type;
  uint64_t config;
  uint64_t PmuSnapshot::*field;
};

const EventDef kEvents[] = {
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, &PmuSnapshot::cycles},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, &PmuSnapshot::instructions},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, &PmuSnapshot::branch_misses},
    {PERF_TYPE_HW_CACHE,
     cache_config(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                  PERF_COUNT_HW_CACHE_RESULT_MISS),
     &PmuSnapshot::l1d_misses},
    {PERF_TYPE_HW_CACHE,
     cache_config(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ,
                  PERF_COUNT_HW_CACHE_RESULT_MISS),
     &PmuSnapshot::llc_misses},
    {PERF_TYPE_HW_CACHE,
     cache_config(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ,
                  PERF_COUNT_HW_CACHE_RESULT_MISS),
     &PmuSnapshot::dtlb_misses},
};

constexpr int kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

}  // namespace

struct Pmu::Impl {
  int leader = -1;
  int fds[kMaxEvents];
  uint64_t ids[kMaxEvents];
  uint64_t PmuSnapshot::*fields[kMaxEvents];
  int n = 0;
};

Pmu::Pmu() : impl_(new Impl()) {
  for (int i = 0; i < kEventCount; ++i) {
    perf_event_attr a;
    std::memset(&a, 0, sizeof(a));
    a.type = kEvents[i].type;
    a.size = sizeof(a);
    a.config = kEvents[i].config;
    a.disabled = (impl_->leader < 0) ? 1 : 0;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
                    PERF_FORMAT_TOTAL_TIME_ENABLED |
                    PERF_FORMAT_TOTAL_TIME_RUNNING;

    int fd = static_cast<int>(perf_open(&a, impl_->leader));
    if (fd < 0) continue;  // event not exposed on this CPU, drop it

    uint64_t id = 0;
    ioctl(fd, PERF_EVENT_IOC_ID, &id);

    if (impl_->leader < 0) impl_->leader = fd;
    impl_->fds[impl_->n] = fd;
    impl_->ids[impl_->n] = id;
    impl_->fields[impl_->n] = kEvents[i].field;
    impl_->n++;
  }
}

Pmu::~Pmu() {
  if (impl_) {
    for (int i = 0; i < impl_->n; ++i) close(impl_->fds[i]);
    delete impl_;
  }
}

bool Pmu::available() const { return impl_ && impl_->leader >= 0; }

void Pmu::start() {
  if (!available()) return;
  ioctl(impl_->leader, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
  ioctl(impl_->leader, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

PmuSnapshot Pmu::stop() {
  PmuSnapshot s{0, 0, 0, 0, 0, 0};
  if (!available()) return s;
  ioctl(impl_->leader, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

  // Grouped read: nr, time_enabled, time_running, then {value, id} per event.
  uint64_t buf[3 + 2 * kMaxEvents];
  ssize_t got = read(impl_->leader, buf, sizeof(buf));
  if (got <= 0) return s;

  uint64_t nr = buf[0];
  uint64_t time_enabled = buf[1];
  uint64_t time_running = buf[2];

  for (uint64_t e = 0; e < nr; ++e) {
    uint64_t value = buf[3 + 2 * e];
    uint64_t id = buf[3 + 2 * e + 1];
    // Scale for multiplexing if the group did not run the whole window.
    if (time_running > 0 && time_running < time_enabled) {
      value = value * time_enabled / time_running;
    }
    for (int i = 0; i < impl_->n; ++i) {
      if (impl_->ids[i] == id) {
        s.*(impl_->fields[i]) = value;
        break;
      }
    }
  }
  return s;
}

}  // namespace caliper

#endif  // CALIPER_NATIVE
