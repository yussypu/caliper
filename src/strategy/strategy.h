#pragma once

#include "book/book.h"
#include "common/platform.h"
#include "common/wire.h"
#include "decode/decode.h"

// The reacting logic. It reads the book top for the updated instrument and
// decides whether to fire an order. The rule here is a placeholder spread
// taker: when the book is two sided and the spread is at or below a tick
// threshold, send a New on the side that crosses. Real microstructure goes
// here. The path is kept branch light and free of allocation.

namespace caliper {

class Strategy {
 public:
  explicit Strategy(int64_t spread_threshold = 1)
      : spread_threshold_(spread_threshold) {}

  CALIPER_ALWAYS_INLINE bool decide(const Book& book, const DecodedMd& m,
                                    OrderMsg& out) {
    const Top& t = book.top(m.instrument);
    if (CALIPER_UNLIKELY(t.bid_qty == 0 || t.ask_qty == 0)) return false;
    int64_t spread = t.ask_price - t.bid_price;
    if (spread > spread_threshold_) return false;
    out.type = static_cast<uint8_t>(OrderType::New);
    out.side = static_cast<uint8_t>(Side::Bid);
    out.instrument = m.instrument;
    out.price = t.ask_price;
    out.qty = t.ask_qty;
    out.client_id = ++client_id_;
    return true;
  }

 private:
  int64_t spread_threshold_;
  uint32_t client_id_ = 0;
};

}  // namespace caliper
