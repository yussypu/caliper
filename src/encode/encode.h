#pragma once

#include <cstddef>
#include <cstring>

#include "common/platform.h"
#include "common/wire.h"

// Internal order to wire bytes. A copy into the preallocated tx buffer.

namespace caliper {

CALIPER_ALWAYS_INLINE size_t encode_order(const OrderMsg& o, uint8_t* buf,
                                          size_t cap) {
  if (CALIPER_UNLIKELY(cap < sizeof(OrderMsg))) return 0;
  std::memcpy(buf, &o, sizeof(o));
  return sizeof(o);
}

}  // namespace caliper
