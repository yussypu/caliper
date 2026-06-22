#pragma once

#include <cstdint>

// Wire formats shared by the feed, decode, and encode. Fixed size, packed,
// little endian on the wire. gavel emits md_msg over UDP multicast, caliper
// replies with order_msg.

namespace caliper {

enum class MdType : uint8_t {
  Add = 1,
  Modify = 2,
  Delete = 3,
  Trade = 4,
};

enum class Side : uint8_t {
  Bid = 0,
  Ask = 1,
};

enum class OrderType : uint8_t {
  New = 1,
  Cancel = 2,
};

#pragma pack(push, 1)

struct MdMsg {
  uint8_t type;
  uint8_t side;
  uint16_t instrument;
  uint64_t seq;
  int64_t price;
  uint32_t qty;
  uint32_t order_id;
};

struct OrderMsg {
  uint8_t type;
  uint8_t side;
  uint16_t instrument;
  int64_t price;
  uint32_t qty;
  uint32_t client_id;
};

#pragma pack(pop)

static_assert(sizeof(MdMsg) == 28, "MdMsg wire size");
static_assert(sizeof(OrderMsg) == 20, "OrderMsg wire size");

}  // namespace caliper
