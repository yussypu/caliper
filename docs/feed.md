# the feed

caliper reacts to a real venue day. The market data is not synthetic: it is a
full NASDAQ TotalView-ITCH trading session, replayed through a matching engine,
normalized into book updates, and fed to the closed loop. This file describes
the pipeline and the provenance.

## the two projects

- gavel is a deterministic matching engine. It ingests order entry, matches
  under price time priority, and publishes an event stream. Same input, same
  output, bit identical, proven across optimization levels.
- caliper is the reacting side. It decodes the published feed, books it, runs a
  strategy, and measures tick to trade.

In this setup gavel is the exchange and caliper is the colocated system reacting
to what the exchange publishes. The order flow that drives the exchange is a
real historical day, so the flow caliper reacts to has the structure of real
order flow rather than a generator's statistics.

## the pipeline

```
NASDAQ TotalView-ITCH day (01302020, 5.2 GB gz)
  -> gavel-extract            filter to symbols, parse ITCH 5.0, stream only
  -> SYMBOL.gvl               gavel input stream, 48 byte sequenced records
  -> gavel Engine (bridge)    match, publish accepted/executed/canceled/...
  -> gavel_to_feed            normalize events to caliper book updates
  -> SYMBOL.feed              caliper feed: header plus MdMsg records
  -> caliper                  decode, book, decide, encode, measure
```

gavel's data fetch streams the day through `gavel-extract` with the gzip
decompressed in flight, so nothing large touches disk. The per symbol `.gvl`
streams are derived NASDAQ data and are not committed.

## the bridge is a feed handler

`bridge/gavel_to_feed.cpp` runs the gavel engine over a `.gvl` stream, taps the
published event stream, and writes a caliper feed file. The real work is the
same work a feed handler does at a desk: the venue publishes events keyed by
order id, the local book is keyed by price level, so the bridge keeps the order
id to price map and resolves every cancel, execute, reduce, and replace back to
the price level it touches. The event mapping:

| gavel event | caliper update |
| --- | --- |
| accepted (priced) | Add at price, qty |
| executed | Trade at price, qty, on the resting side |
| canceled | Delete at the resting order's price |
| reduced | Modify to the remaining qty at that price |
| replaced | Delete the old order; the fresh one rests via its own accept |
| repriced | Delete at the old price, Add at the new |

Market orders and unpriced pegs do not rest at a displayed price, so they are
not booked. The bridge reports how many events it skipped and, importantly, how
many id lookups missed: on the AAPL day that count is zero, so every cancel and
execute resolved to a live resting order.

## the price band

A symbol's day spans a few dollars of active trading plus a tail of deep resting
orders far from the market. The book ladder is sized to the traded range padded
by a margin, and prices outside that band clamp to the edges, the way a bounded
book holds a deep order. The header carries the band so caliper builds a dense
ladder around the market instead of one that spans every stray price. For AAPL
on 01302020 the traded range is 318.79 to 324.18 dollars, the band is 1617 half
tick levels, and about 15 percent of updates are deep orders that clamp to the
edges and never touch the best bid or ask. The day is 1,937,937 venue order
events, which the bridge normalizes to 2,082,454 caliper book updates.

## reproduce

```
# one time: build gavel and fetch the day (in the gavel checkout)
cd ../gavel
cmake -G "Unix Makefiles" -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
scripts/get_data.sh                       # streams 01302020, filters 5 symbols

# build the feed and run caliper on it
cd ../caliper
scripts/build_feed.sh                     # writes data/AAPL.feed
sudo scripts/run_bench.sh                 # CALIPER_FEED overrides the feed path
```

caliper falls back to the synthetic generator in `src/rx/feed.h` when no feed
file is present, so the loop still builds and runs without the data.

## what is real and what is not

Real: the order flow, the message type mix, the price and size distribution, the
book dynamics. These come from a NASDAQ trading day through a matching engine
that passes a fidelity check against the day's actual executions.

Two timestamp configurations. The AF_XDP bypass run on one box uses rdtscp
software reads over a veth pair, not NIC stamps, which isolates the per stage
userspace compute. The hardware run uses real igb PHC stamps across two hosts
over a vSwitch, so its wire to wire is a true NIC to NIC measurement that
includes the kernel rx and tx path. Both are published, in `results/` and
`results/hardware/`, each with its own provenance. See `docs/hunt.md` finding G.
