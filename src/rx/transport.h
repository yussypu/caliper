#pragma once

#include <cstddef>
#include <cstdint>

#include "common/platform.h"

// The transport hides the receive and send path behind one interface. On
// x86_64 Linux the backend is AF_XDP with NIC hardware RX and TX timestamps.
// Elsewhere the backend is an in process simulated feed so the loop runs for
// development. rx_ts and the tx return value are the wire to wire endpoints:
// hardware timestamps on the native backend, now() reads on the sim backend.

namespace caliper {

struct RxPacket {
  const uint8_t* data;
  size_t len;
  uint64_t rx_ts;             // hardware RX timestamp, ticks
  uint64_t expected_interval; // nominal arrival spacing, ns, for the CO correction
};

// The book ladder geometry the feed needs. Comes from the feed file header for
// a real venue day, or the synthetic defaults when replaying the generator.
struct BookBand {
  int instruments;
  int levels;
  int64_t price_base;
  char symbol[8];
};

class Transport {
 public:
  Transport();
  ~Transport();

  // Blocks until the next market data packet. Returns false at end of feed.
  bool rx(RxPacket& out);

  // Sends len bytes and returns the hardware TX timestamp in ticks.
  uint64_t tx(const uint8_t* data, size_t len);

  // Touches every buffer page so there is no first touch fault on the path.
  void warm();

  // The book band for this run: from the feed file, or synthetic defaults.
  BookBand book_band() const;

  // True when rx_ts and tx_ts are NIC hardware nanoseconds, false when they are
  // rdtscp ticks. The harness uses this to record the wire histogram correctly.
  bool wire_hardware() const;

  const char* backend() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace caliper
