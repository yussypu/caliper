#include "common/platform.h"

#if !CALIPER_NATIVE

#include <cstdlib>
#include <cstring>
#include <vector>

#include "rx/feed.h"
#include "rx/feed_source.h"
#include "rx/transport.h"
#include "timing/tsc.h"

// Portable transport. Generates the full feed into a contiguous buffer at
// startup, hands packets out by pointer on the path, and uses now() reads as
// the RX and TX timestamps. The wire to wire number it produces is the
// userspace loop cost, which is what is observable without a NIC. On the
// native backend these endpoints become NIC hardware timestamps.

namespace caliper {

namespace {
constexpr uint64_t kFeedCount = 2'000'000;
// Nominal arrival spacing reported for the CO correction. This backend does not
// pace, so its corrected column is nominal and for development only. The native
// backend enforces this spacing on the wire.
constexpr uint64_t kArrivalIntervalNs = 4000;
constexpr size_t kTxCap = 256;
}  // namespace

struct Transport::Impl {
  Feed feed{kFeedCount};
  FeedSource feed_file;
  std::vector<MdMsg> packets;
  uint64_t next = 0;
  alignas(CALIPER_CACHELINE) uint8_t tx_buf[kTxCap];

  Impl() {
    if (feed_file.load(feed_path())) {
      packets.assign(feed_file.data(), feed_file.data() + feed_file.count());
    } else {
      packets.resize(kFeedCount);
      for (uint64_t i = 0; i < kFeedCount; ++i) feed.at(i, packets[i]);
    }
  }
};

Transport::Transport() : impl_(new Impl()) {}
Transport::~Transport() { delete impl_; }

void Transport::warm() {
  volatile uint8_t sink = 0;
  for (auto& p : impl_->packets) sink ^= reinterpret_cast<uint8_t*>(&p)[0];
  for (size_t i = 0; i < kTxCap; ++i) impl_->tx_buf[i] = 0;
  (void)sink;
}

bool Transport::rx(RxPacket& out) {
  if (CALIPER_UNLIKELY(impl_->next >= impl_->packets.size())) return false;
  const MdMsg& m = impl_->packets[impl_->next++];
  out.data = reinterpret_cast<const uint8_t*>(&m);
  out.len = sizeof(MdMsg);
  out.rx_ts = timing::now();
  out.expected_interval = kArrivalIntervalNs;
  return true;
}

uint64_t Transport::tx(const uint8_t* data, size_t len) {
  if (len > kTxCap) len = kTxCap;
  std::memcpy(impl_->tx_buf, data, len);
  return timing::now();
}

BookBand Transport::book_band() const {
  BookBand b{};
  if (impl_->feed_file.loaded()) {
    const FeedHeader& h = impl_->feed_file.header();
    b.instruments = 1;
    b.levels = static_cast<int>(h.num_levels);
    b.price_base = h.price_base;
    std::memcpy(b.symbol, h.symbol, 8);
    return b;
  }
  b.instruments = 8;
  b.levels = 1024;
  b.price_base = 100000;
  std::memcpy(b.symbol, "SYNTH\0\0", 8);
  return b;
}

bool Transport::wire_hardware() const { return false; }

const char* Transport::backend() const { return "sim"; }

}  // namespace caliper

#endif  // !CALIPER_NATIVE
