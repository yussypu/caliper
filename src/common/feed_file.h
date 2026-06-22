#pragma once

#include <cstdint>

#include "common/wire.h"

// On disk feed format produced by the gavel bridge and replayed by caliper.
// A fixed header names the symbol and the price band, then a flat array of
// MdMsg records in arrival order. Prices are rebased to half ticks above
// price_base so the book ladder is a small dense window, and the bridge clamps
// anything outside the band and reports how much. One symbol per file.

namespace caliper {

#pragma pack(push, 1)

struct FeedHeader {
  char magic[8];        // kFeedMagic
  char symbol[8];       // null padded ticker, e.g. "AAPL"
  uint64_t day;         // yyyymmdd of the source ITCH day
  uint64_t msg_count;   // number of MdMsg records following the header
  uint32_t num_levels;  // book band width in half ticks, price - price_base
  int64_t price_base;   // half tick price mapped to ladder level 0
  uint32_t tick_units;  // price units per half tick on the source venue
};

#pragma pack(pop)

inline constexpr char kFeedMagic[8] = {'C', 'A', 'L', 'F', 'E', 'E', 'D', '1'};

static_assert(sizeof(FeedHeader) == 48, "FeedHeader wire size");

}  // namespace caliper
