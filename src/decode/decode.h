#pragma once

#include <cstddef>
#include <cstring>

#include "common/platform.h"
#include "common/wire.h"

// Wire bytes to internal message. The internal form is the same packed struct,
// so decode is a length checked load with no branch on message type. The type
// dispatch the hunt writeup tracks for branch mispredict lives in book.apply,
// not here. decode reads the frame the receive path just wrote. Under a paced
// feed the frame is warm by the time decode reads it, so the cold load the
// synthetic bursts exposed is off the path; an explicit prefetch measured as no
// further help and ships off, see the hunt writeup finding A.

namespace caliper {

struct DecodedMd {
  MdType type;
  Side side;
  uint16_t instrument;
  uint64_t seq;
  int64_t price;
  uint32_t qty;
  uint32_t order_id;
};

CALIPER_ALWAYS_INLINE bool decode_md(const uint8_t* buf, size_t len,
                                     DecodedMd& out) {
  if (CALIPER_UNLIKELY(len < sizeof(MdMsg))) return false;
  MdMsg m;
  std::memcpy(&m, buf, sizeof(m));
  out.type = static_cast<MdType>(m.type);
  out.side = static_cast<Side>(m.side);
  out.instrument = m.instrument;
  out.seq = m.seq;
  out.price = m.price;
  out.qty = m.qty;
  out.order_id = m.order_id;
  return true;
}

}  // namespace caliper
