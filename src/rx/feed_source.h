#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/feed_file.h"
#include "common/wire.h"

// Loads a caliper feed file produced by the gavel bridge: a real venue day of
// book updates for one symbol. Reads the header and the MdMsg array into memory
// at startup, off the hot path. loaded() is false on a missing or bad file, and
// the caller falls back to the synthetic generator in feed.h.

namespace caliper {

class FeedSource {
 public:
  bool load(const char* path) {
    if (!path) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fread(&header_, sizeof(header_), 1, f) != 1) { std::fclose(f); return false; }
    if (std::memcmp(header_.magic, kFeedMagic, 8) != 0) { std::fclose(f); return false; }
    msgs_.resize(header_.msg_count);
    const size_t got = std::fread(msgs_.data(), sizeof(MdMsg), header_.msg_count, f);
    std::fclose(f);
    if (got != header_.msg_count) { msgs_.clear(); return false; }
    loaded_ = true;
    return true;
  }

  bool loaded() const { return loaded_; }
  const FeedHeader& header() const { return header_; }
  size_t count() const { return msgs_.size(); }
  const MdMsg* data() const { return msgs_.data(); }

 private:
  FeedHeader header_{};
  std::vector<MdMsg> msgs_;
  bool loaded_ = false;
};

// Feed file path: CALIPER_FEED overrides the default under data/.
inline const char* feed_path() {
  const char* p = std::getenv("CALIPER_FEED");
  return p ? p : "data/AAPL.feed";
}

}  // namespace caliper
