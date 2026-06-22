#pragma once

#include <cstdint>

#include "common/wire.h"

// Deterministic market data generator. Stands in for the gavel feed on the
// portable backend. It builds a two sided book per instrument and walks the
// prices so the strategy fires on a fraction of updates. The whole sequence
// is generated into a preallocated buffer at startup, so pulling a packet on
// the path is a pointer move with no allocation and no randomness cost.

namespace caliper {

class Feed {
 public:
  explicit Feed(uint64_t count) : count_(count) {}

  // Fills msg for sequence i. Deterministic, pure function of i.
  void at(uint64_t i, MdMsg& msg) const {
    uint16_t inst = static_cast<uint16_t>(i % 8);
    uint64_t step = i / 8;
    bool ask = (step & 1) != 0;
    int64_t base = 100000 + static_cast<int64_t>((step * 7) % 64);
    MdType type;
    uint32_t r = (static_cast<uint32_t>(i) * 2654435761u) >> 28;
    if (r < 9)
      type = MdType::Add;
    else if (r < 12)
      type = MdType::Modify;
    else if (r < 14)
      type = MdType::Delete;
    else
      type = MdType::Trade;
    msg.type = static_cast<uint8_t>(type);
    msg.side = static_cast<uint8_t>(ask ? Side::Ask : Side::Bid);
    msg.instrument = inst;
    msg.seq = i;
    msg.price = ask ? base + 1 : base;
    msg.qty = 1 + static_cast<uint32_t>((i * 1103515245u) % 100);
    msg.order_id = static_cast<uint32_t>(i & 0xffff);
  }

  uint64_t count() const { return count_; }

 private:
  uint64_t count_;
};

}  // namespace caliper
