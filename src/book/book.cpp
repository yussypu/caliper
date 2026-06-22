#include "book/book.h"

namespace caliper {

Book::Book(int instruments, int levels, int64_t price_base)
    : instruments_(instruments),
      levels_(levels),
      price_base_(price_base),
      bids_(static_cast<size_t>(instruments) * levels, 0),
      asks_(static_cast<size_t>(instruments) * levels, 0),
      top_(instruments, Top{0, 0, 0, 0}),
      seq_(instruments, 0),
      best_bid_(instruments, -1),
      best_ask_(instruments, levels) {}

// Rescans the band for the best bid and ask. A real book maintains the best
// incrementally, this bounded scan is enough to exercise the cache behavior the
// lab measures and is the place to swap in the incremental version.
void Book::refresh(uint16_t instrument) {
  const uint32_t* bids = bids_.data() + static_cast<size_t>(instrument) * levels_;
  const uint32_t* asks = asks_.data() + static_cast<size_t>(instrument) * levels_;
  Top t;
  t.bid_price = 0;
  t.bid_qty = 0;
  t.ask_price = 0;
  t.ask_qty = 0;
  for (int i = levels_ - 1; i >= 0; --i) {
    if (bids[i] != 0) {
      t.bid_price = price_base_ + i;
      t.bid_qty = bids[i];
      break;
    }
  }
  for (int i = 0; i < levels_; ++i) {
    if (asks[i] != 0) {
      t.ask_price = price_base_ + i;
      t.ask_qty = asks[i];
      break;
    }
  }
  top_[instrument] = t;
}

}  // namespace caliper
