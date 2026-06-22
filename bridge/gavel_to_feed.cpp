#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gavel/engine.hpp"
#include "gavel/itch/itch.hpp"
#include "gavel/output.hpp"
#include "gavel/stream.hpp"
#include "gavel/types.hpp"

#include "common/feed_file.h"
#include "common/wire.h"

// The gavel bridge. Runs a gavel input stream through the matching engine, taps
// the published event stream, and writes a caliper feed file. This is the feed
// handler: gavel publishes id keyed venue events, caliper books by price level,
// so the bridge keeps the order id to price map and resolves every cancel,
// execute, reduce, and replace back to the price level it touches. One symbol
// per run. Prices stay in gavel half ticks; the header carries the band so
// caliper rebases to a dense ladder.

namespace {

using caliper::FeedHeader;
using caliper::MdMsg;
using caliper::MdType;
using caliper::Side;

struct Resting {
  std::int32_t price;  // half ticks
  std::uint8_t side;   // caliper Side
  std::int32_t rem;    // remaining displayed qty
};

struct Stats {
  std::uint64_t add = 0, modify = 0, del = 0, trade = 0;
  std::uint64_t skipped_nonbook = 0, unresolved = 0, market_or_peg = 0;
};

caliper::Side to_caliper_side(gavel::Side s) {
  return s == gavel::Side::buy ? caliper::Side::Bid : caliper::Side::Ask;
}

// Appends one caliper update with an absolute half tick price.
void push(std::vector<MdMsg>& out, MdType t, std::uint8_t side, std::int64_t price,
          std::uint32_t qty, std::uint64_t id, std::uint64_t seq) {
  MdMsg m;
  m.type = static_cast<std::uint8_t>(t);
  m.side = side;
  m.instrument = 0;
  m.seq = seq;
  m.price = price;
  m.qty = qty;
  m.order_id = static_cast<std::uint32_t>(id);
  out.push_back(m);
}

}  // namespace

int main(int argc, char** argv) {
  const char* in_path = nullptr;
  const char* out_path = nullptr;
  std::string symbol = "AAPL";
  std::uint64_t day = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
    else if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) symbol = argv[++i];
    else if (std::strcmp(argv[i], "--day") == 0 && i + 1 < argc) day = std::strtoull(argv[++i], nullptr, 10);
    else if (argv[i][0] != '-') in_path = argv[i];
  }
  if (!in_path || !out_path) {
    std::fprintf(stderr, "usage: gavel_to_feed --out feed.bin --symbol AAPL --day 20200130 stream.gvl\n");
    return 2;
  }

  gavel::StreamReader in(in_path);
  if (!in.ok()) {
    std::fprintf(stderr, "cannot open gavel stream %s\n", in_path);
    return 1;
  }

  gavel::Config cfg;
  cfg.num_symbols = 1;
  cfg.checkpoint_interval = 0;  // suppress state_hash events, they are not book updates
  gavel::Engine eng(cfg);

  std::unordered_map<std::uint64_t, Resting> live;
  live.reserve(1 << 20);
  std::vector<MdMsg> out;
  out.reserve(1 << 21);
  Stats st;
  std::uint64_t seq = 0;
  std::int64_t tlo = INT64_MAX, thi = INT64_MIN;  // traded price range, the active band

  gavel::InputMsg msg;
  while (in.read(msg)) {
    eng.on_msg(msg);

    // Parse every event the engine just published, then drain.
    const auto& buf = eng.emitter().buffer();
    std::size_t off = 0;
    while (off + 2 <= buf.size()) {
      const auto type = static_cast<gavel::EventType>(buf[off]);
      const std::uint8_t len = buf[off + 1];
      const std::uint8_t* body = buf.data() + off + 2;
      off += 2 + len;

      switch (type) {
        case gavel::EventType::accepted: {
          gavel::EvAccepted ev;
          std::memcpy(&ev, body, sizeof(ev));
          if (ev.price <= 0) { st.market_or_peg++; break; }  // market or unpriced peg, does not rest displayed
          const std::uint8_t side = static_cast<std::uint8_t>(to_caliper_side(ev.side));
          live[ev.id] = Resting{ev.price, side, ev.qty};
          push(out, MdType::Add, side, ev.price, static_cast<std::uint32_t>(ev.qty), ev.id, ++seq);
          st.add++;
          break;
        }
        case gavel::EventType::executed: {
          gavel::EvExecuted ev;
          std::memcpy(&ev, body, sizeof(ev));
          auto it = live.find(ev.resting);
          if (it == live.end()) { st.unresolved++; break; }
          push(out, MdType::Trade, it->second.side, ev.price,
               static_cast<std::uint32_t>(ev.qty), ev.resting, ++seq);
          if (ev.price < tlo) tlo = ev.price;
          if (ev.price > thi) thi = ev.price;
          it->second.rem = ev.resting_remaining;
          if (ev.resting_remaining <= 0) live.erase(it);
          st.trade++;
          break;
        }
        case gavel::EventType::canceled: {
          gavel::EvCanceled ev;
          std::memcpy(&ev, body, sizeof(ev));
          auto it = live.find(ev.id);
          if (it == live.end()) { st.unresolved++; break; }
          push(out, MdType::Delete, it->second.side, it->second.price, 0, ev.id, ++seq);
          live.erase(it);
          st.del++;
          break;
        }
        case gavel::EventType::reduced: {
          gavel::EvReduced ev;
          std::memcpy(&ev, body, sizeof(ev));
          auto it = live.find(ev.id);
          if (it == live.end()) { st.unresolved++; break; }
          it->second.rem = ev.remaining;
          push(out, MdType::Modify, it->second.side, it->second.price,
               static_cast<std::uint32_t>(ev.remaining), ev.id, ++seq);
          st.modify++;
          break;
        }
        case gavel::EventType::replaced: {
          gavel::EvReplaced ev;
          std::memcpy(&ev, body, sizeof(ev));
          auto it = live.find(ev.old_id);
          if (it == live.end()) { st.unresolved++; break; }
          push(out, MdType::Delete, it->second.side, it->second.price, 0, ev.old_id, ++seq);
          live.erase(it);  // the fresh order rests via its own accepted event
          st.del++;
          break;
        }
        case gavel::EventType::repriced: {
          gavel::EvRepriced ev;
          std::memcpy(&ev, body, sizeof(ev));
          auto it = live.find(ev.id);
          if (it == live.end()) { st.unresolved++; break; }
          const std::uint8_t side = it->second.side;
          const std::int32_t rem = it->second.rem;
          push(out, MdType::Delete, side, ev.old_price, 0, ev.id, ++seq);
          push(out, MdType::Add, side, ev.new_price, static_cast<std::uint32_t>(rem), ev.id, ++seq);
          it->second.price = ev.new_price;
          st.del++;
          st.add++;
          break;
        }
        default:
          st.skipped_nonbook++;
          break;
      }
    }
    eng.emitter().drain();
  }

  if (out.empty()) {
    std::fprintf(stderr, "no book updates produced from %s\n", in_path);
    return 1;
  }

  // Price band: center on the traded range, the prices that actually quote and
  // cross, padded by a margin. Deep resting orders and far stop caps clamp to
  // the band edges, the way a bounded book holds them, so the ladder stays
  // dense around the market instead of spanning every stray price.
  if (tlo > thi) { tlo = out[0].price; thi = out[0].price; }  // no trades, fall back
  std::int64_t margin = (thi - tlo) / 4;
  if (margin < 256) margin = 256;
  std::int64_t lo = tlo - margin;
  if (lo < 0) lo = 0;
  std::uint64_t span = static_cast<std::uint64_t>(thi - lo) + margin + 1;
  std::uint32_t num_levels = span > 8192 ? 8192 : static_cast<std::uint32_t>(span);

  std::uint64_t clamped = 0;
  for (const MdMsg& m : out) {
    if (m.price < lo || m.price >= lo + static_cast<std::int64_t>(num_levels)) clamped++;
  }

  FeedHeader h;
  std::memset(&h, 0, sizeof(h));
  std::memcpy(h.magic, caliper::kFeedMagic, 8);
  std::strncpy(h.symbol, symbol.c_str(), sizeof(h.symbol) - 1);
  h.day = day;
  h.msg_count = out.size();
  h.num_levels = num_levels;
  h.price_base = lo;
  h.tick_units = gavel::itch::kHalfTickUnits;

  std::FILE* f = std::fopen(out_path, "wb");
  if (!f) { std::fprintf(stderr, "cannot write %s\n", out_path); return 1; }
  std::fwrite(&h, sizeof(h), 1, f);
  std::fwrite(out.data(), sizeof(MdMsg), out.size(), f);
  std::fclose(f);

  std::printf("symbol %s day %llu\n", symbol.c_str(), static_cast<unsigned long long>(day));
  std::printf("book updates %zu  (add %llu modify %llu delete %llu trade %llu)\n",
              out.size(), (unsigned long long)st.add, (unsigned long long)st.modify,
              (unsigned long long)st.del, (unsigned long long)st.trade);
  std::printf("traded range %.2f to %.2f dollars\n", tlo * 0.005, thi * 0.005);
  std::printf("price band   base %lld half ticks, %u levels (%.2f to %.2f dollars), %.3f%% clamped\n",
              (long long)lo, num_levels, lo * 0.005, (lo + num_levels) * 0.005,
              100.0 * static_cast<double>(clamped) / static_cast<double>(out.size()));
  std::printf("not booked   %llu market/peg, %llu unresolved id, %llu non book events\n",
              (unsigned long long)st.market_or_peg, (unsigned long long)st.unresolved,
              (unsigned long long)st.skipped_nonbook);
  std::printf("wrote %s\n", out_path);
  return 0;
}
