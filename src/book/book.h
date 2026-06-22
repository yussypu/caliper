#pragma once

#include <cstdint>
#include <vector>

#include "common/platform.h"
#include "decode/decode.h"

// Order book. Bounded, allocation free on the path, sized at startup from the
// feed's price band. Each instrument holds a price ladder per side over a fixed
// window of half tick levels, and the best bid and ask are maintained
// incrementally: an update touches one level and adjusts the best in O(1),
// rescanning only the rare case where the best level empties. The band and
// level count come from the feed header, so a real symbol's day maps onto a
// dense ladder around its traded range. The book runs on the hot core alone, so
// there is no false sharing today. Build with -DCALIPER_BOOK_FULLSCAN=ON to
// revert to the O(n) rescan the hunt writeup measures the incremental best
// against.

namespace caliper {

struct Top {
  int64_t bid_price;
  uint32_t bid_qty;
  int64_t ask_price;
  uint32_t ask_qty;
};

class Book {
 public:
  // levels is the ladder width in half ticks; price_base is the half tick price
  // at ladder level 0. Prices outside the band clamp to the edges.
  Book(int instruments, int levels, int64_t price_base);

  CALIPER_ALWAYS_INLINE void apply(const DecodedMd& m) {
    if (CALIPER_UNLIKELY(m.instrument >= instruments_)) return;
    int li = level_index(m.price);
    size_t row = static_cast<size_t>(m.instrument) * levels_;
    uint32_t* bid = bids_.data() + row;
    uint32_t* ask = asks_.data() + row;
    uint32_t* ladder = m.side == Side::Bid ? bid : ask;

    uint32_t cur = ladder[li];
    uint32_t v = cur;
#if CALIPER_BOOK_BRANCHLESS
    uint32_t trade_val = cur > m.qty ? cur - m.qty : 0;
    bool is_del = m.type == MdType::Delete;
    bool is_trade = m.type == MdType::Trade;
    v = is_del ? 0u : (is_trade ? trade_val : m.qty);
#else
    switch (m.type) {
      case MdType::Add:
      case MdType::Modify:
        v = m.qty;
        break;
      case MdType::Delete:
        v = 0;
        break;
      case MdType::Trade:
        v = cur > m.qty ? cur - m.qty : 0;
        break;
    }
#endif
    ladder[li] = v;

#if CALIPER_BOOK_FULLSCAN
    refresh(m.instrument);
#else
    if (m.side == Side::Bid)
      update_best_bid(m.instrument, bid, li, v);
    else
      update_best_ask(m.instrument, ask, li, v);
#endif
    seq_[m.instrument] = m.seq;
  }

  CALIPER_ALWAYS_INLINE const Top& top(uint16_t instrument) const {
    return top_[instrument];
  }

  int levels() const { return levels_; }
  int64_t price_base() const { return price_base_; }

 private:
  CALIPER_ALWAYS_INLINE int level_index(int64_t price) const {
    int64_t l = price - price_base_;
    if (l < 0) l = 0;
    if (l >= levels_) l = levels_ - 1;
    return static_cast<int>(l);
  }

  // Incremental best bid: a nonzero at or above the best moves it up at once; a
  // clear of the best level scans down for the next resting level, which in an
  // active book is a step or two. best_bid_ is -1 when the side is empty.
  CALIPER_ALWAYS_INLINE void update_best_bid(uint16_t inst, const uint32_t* bid,
                                             int li, uint32_t v) {
    int best = best_bid_[inst];
    if (v != 0) {
      if (li > best) best = li;
    } else if (li == best) {
      int j = li - 1;
      while (j >= 0 && bid[j] == 0) --j;
      best = j;
    }
    best_bid_[inst] = best;
    Top& t = top_[inst];
    t.bid_price = best >= 0 ? price_base_ + best : 0;
    t.bid_qty = best >= 0 ? bid[best] : 0;
  }

  // Incremental best ask: mirror of the bid. best_ask_ is levels_ when empty.
  CALIPER_ALWAYS_INLINE void update_best_ask(uint16_t inst, const uint32_t* ask,
                                             int li, uint32_t v) {
    int best = best_ask_[inst];
    if (v != 0) {
      if (li < best) best = li;
    } else if (li == best) {
      int j = li + 1;
      while (j < levels_ && ask[j] == 0) ++j;
      best = j;
    }
    best_ask_[inst] = best;
    Top& t = top_[inst];
    t.ask_price = best < levels_ ? price_base_ + best : 0;
    t.ask_qty = best < levels_ ? ask[best] : 0;
  }

  void refresh(uint16_t instrument);  // full scan, used under CALIPER_BOOK_FULLSCAN

  int instruments_;
  int levels_;
  int64_t price_base_;
  std::vector<uint32_t> bids_;  // instruments_ rows of levels_ each
  std::vector<uint32_t> asks_;
  std::vector<Top> top_;
  std::vector<uint64_t> seq_;
  std::vector<int> best_bid_;  // level of best bid per instrument, -1 if empty
  std::vector<int> best_ask_;  // level of best ask per instrument, levels_ if empty
};

}  // namespace caliper
