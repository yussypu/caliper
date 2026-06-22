#include <cstdint>
#include <cstdio>
#include <vector>

#include "book/book.h"
#include "common/wire.h"

// Shadow model test for the incremental best bid and ask. Drives the book with
// a long deterministic sequence of adds, modifies, deletes, and trades, and
// after every operation recomputes the best by brute force over an independent
// shadow ladder. If the O(1) incremental best ever disagrees with the full
// scan of the shadow, the test fails. This is the guarantee that the fast path
// is correct, the same shadow book idea gavel uses in its own harness.

namespace {

using namespace caliper;

int g_failures = 0;

}  // namespace

int main() {
  const int levels = 512;
  const int64_t base = 1000;
  Book book(1, levels, base);
  std::vector<uint32_t> sbid(levels, 0);
  std::vector<uint32_t> sask(levels, 0);

  uint64_t s = 0x9e3779b97f4a7c15ull;
  auto rnd = [&]() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 33);
  };

  for (int it = 0; it < 500000 && g_failures < 5; ++it) {
    DecodedMd m{};
    m.instrument = 0;
    m.seq = static_cast<uint64_t>(it);
    bool bid = (rnd() & 1) != 0;
    m.side = bid ? Side::Bid : Side::Ask;
    int lvl = static_cast<int>(rnd() % levels);
    m.price = base + lvl;
    uint32_t qty = 1 + rnd() % 100;
    m.qty = qty;

    std::vector<uint32_t>& sh = bid ? sbid : sask;
    uint32_t cur = sh[lvl];
    switch (rnd() % 4) {
      case 0: m.type = MdType::Add; sh[lvl] = qty; break;
      case 1: m.type = MdType::Modify; sh[lvl] = qty; break;
      case 2: m.type = MdType::Delete; sh[lvl] = 0; break;
      default: m.type = MdType::Trade; sh[lvl] = cur > qty ? cur - qty : 0; break;
    }

    book.apply(m);

    int64_t ebp = 0;
    uint32_t ebq = 0;
    for (int i = levels - 1; i >= 0; --i) {
      if (sbid[i] != 0) { ebp = base + i; ebq = sbid[i]; break; }
    }
    int64_t eap = 0;
    uint32_t eaq = 0;
    for (int i = 0; i < levels; ++i) {
      if (sask[i] != 0) { eap = base + i; eaq = sask[i]; break; }
    }

    const Top& t = book.top(0);
    if (t.bid_price != ebp || t.bid_qty != ebq || t.ask_price != eap ||
        t.ask_qty != eaq) {
      std::printf(
          "FAIL it=%d: book bid %lld x %u ask %lld x %u, expected bid %lld x %u "
          "ask %lld x %u\n",
          it, (long long)t.bid_price, t.bid_qty, (long long)t.ask_price,
          t.ask_qty, (long long)ebp, ebq, (long long)eap, eaq);
      g_failures++;
    }
  }

  if (g_failures == 0) {
    std::printf("incremental best matches the shadow over 500000 operations\n");
    return 0;
  }
  std::printf("%d book mismatches\n", g_failures);
  return 1;
}
