# caliper

A closed loop tick to trade latency lab on x86_64. It takes market data in over the wire, decodes it, updates a book, runs a strategy that fires an order back, and measures the whole path with hardware timestamps and per stage attribution. The point is to know where every nanosecond goes and to report the tail honestly.

The market data is a real venue day. A full NASDAQ TotalView-ITCH session is replayed through the [gavel](https://github.com/yussypu/gavel) matching engine, and caliper reacts to what the engine publishes. So the flow has the structure of real order flow, not a generator's statistics. See `docs/feed.md` for the pipeline.

## what it measures

Tick to trade is the time from a market data packet arriving at the NIC to an order leaving the NIC in response. caliper measures the full closed loop and splits it into stages, each with its own histogram, so a tail spike can be traced to one stage rather than guessed at.

Stages on the hot path:

1. rx: NIC receive to userspace, measured with NIC hardware RX timestamp where the card supports it.
2. decode: wire format to internal message.
3. book: order book update.
4. decide: strategy reads the book and produces an order or no order.
5. encode: internal order to wire format.
6. tx: userspace to NIC send, measured with NIC hardware TX timestamp where the card supports it.

`rx` and `tx` come from the NIC PTP clock via `SO_TIMESTAMPING`. The userspace stages (`decode`, `book`, `decide`, `encode`) come from `rdtscp` reads at stage boundaries. The two clocks are reconciled once at startup so stage sums line up with the wire to wire number.

The exchange side is gavel, a deterministic matching engine. It ingests a real NASDAQ trading day, matches under price time priority, and publishes an event stream. A bridge normalizes that stream into caliper book updates, the off core sender replays them on the wire, and caliper reacts. gavel is the venue, caliper is the colocated reactor. See `docs/feed.md`.

## results

All numbers come from `make bench` on the host described in [system setup](#system-setup) and recorded in full in `results/host.md`. Report the full spectrum, not the mean. Means hide the tail and the tail is the product.

**What these numbers are.** The feed is the real AAPL trading day of 2020-01-30, 2,082,454 book updates produced by replaying the NASDAQ ITCH session through gavel and normalizing the published events (`docs/feed.md`). The run is on a single box with one physical port that is also the host lifeline, so there is no true NIC hardware wire to wire (that needs two cabled ports or two hosts). The closed loop runs over a real AF_XDP datapath on a veth pair, and `rx_ts` and `tx_ts` are `rdtscp` reads at the ring boundary, software timestamps, not NIC PTP. The wire to wire number below is the userspace loop across that datapath, and its tx leg includes the `sendto` wakeup the veth copy path needs. The representative, hardware independent figures are the per stage userspace costs. The run is on the kernel isolated core 2 (isolcpus, nohz_full, rcu_nocbs), the sender paces the feed to one packet per 4000 ns, and the umem is backed by 2 MB huge pages. See `results/host.md` for the full provenance.

Wire to wire over the AF_XDP veth loop, software timestamps (rdtscp endpoints). The corrected and uncorrected columns agree because the sender offers a real paced load the loop keeps up with, so there is almost nothing for the CO correction to back fill:

| percentile | uncorrected | CO corrected |
| --- | --- | --- |
| p50 | 2391 ns | 2391 ns |
| p99 | 2891 ns | 2891 ns |
| p99.9 | 3102 ns | 3102 ns |
| p99.99 | 3351 ns | 3362 ns |
| p99.999 | 3611 ns | 3651 ns |
| max | 12380 ns | 12380 ns |

Per stage, userspace, `rdtscp` with clock overhead subtracted (the representative numbers):

| stage | p50 | p99 | p99.9 | p99.99 | max |
| --- | --- | --- | --- | --- | --- |
| decode | 10 | 100 | 100 | 110 | 560 |
| book | 20 | 40 | 150 | 350 | 9680 |
| decide | 10 | 10 | 20 | 20 | 490 |
| encode | 20 | 30 | 90 | 350 | 650 |

`rx` and `tx` are not separately timestamped on this box; they are software reads bracketing the AF_XDP rings, so they are reported only as part of the wire to wire number above, not as hardware stages. With the core kernel isolated the per stage rows stay flat from p50 to p99.99; the residual max is the rare host event the writeup tracks (`results/host.md`).

The book stage is 20 ns at the median because the best bid and ask are maintained incrementally rather than by rescanning the ladder (hunt writeup finding F). With the userspace compute that small, tick to trade is bound by the tx wakeup, not by caliper's work, which is also why userspace IPC reads low: the loop spends its cycles waiting for the next paced packet, not retiring instructions.

PMU over the whole run, userspace: branch misses 12.6M (about 6 per update, down from 15.8M before the incremental best removed the scan exit mispredicts), L1d misses 51.2M, dTLB misses 304 with the umem on huge pages, down from 1.41M on base pages. LLC misses are not countable through the generic perf interface on this Zen part, so that counter reports unavailable rather than a fabricated value.

Full CDFs for each stage are in `results/`, in nanoseconds. Every chart is a CDF. There are no bar charts of averages in this repo.

## measurement methodology

The measurement is part of the result. If the harness lies, the number is worth nothing.

**Coordinated omission.** A naive loop that times one request after another stalls during a slow request and never records the requests that would have arrived during the stall, so the worst samples go missing and p99 comes out optimistic. caliper records against an expected arrival interval and back fills the omitted samples, the correction Gil Tene describes. The correction is only honest if that interval is the cadence the load actually arrives at, so the sender paces the feed onto a fixed grid and the correction is told the same grid. When the loop keeps up, the corrected and uncorrected columns agree; when a real stall omits samples, they diverge. Both histograms are written out so the gap, when there is one, is visible.

**TSC.** Timing uses `rdtscp`. At startup caliper checks for invariant TSC via CPUID, calibrates ns per cycle against `CLOCK_MONOTONIC` over a fixed window, and measures the cost of an `rdtscp` pair in a tight loop. That cost is subtracted from every stage sample, so a stage reading is the stage and not the clock read.

**Histograms.** Samples land in a self contained HdrHistogram style structure at nanosecond resolution. Recording is a fixed cost array index with no allocation, so the act of recording does not perturb the path it measures. The bucketing, the percentile reads, and the CO back fill have golden tests under `ctest`, because a bug there would corrupt every number silently.

**Counters.** `perf_event_open` reads PMU counters around the hot path: cycles, instructions, L1d misses, LLC misses, branch misses, dTLB misses. When a tail shows up the counters say which one moved, which turns a guess into a cause.

## system setup

A latency number without the host configuration is not reproducible. `scripts/host_setup.sh` applies the runtime settings and prints the boot cmdline it expects. The host:

- Boot cmdline: `isolcpus`, `nohz_full`, and `rcu_nocbs` over the hot cores so the scheduler tick and RCU callbacks stay off them.
- IRQ affinity moved off the hot cores. The hot core takes no interrupts.
- Threads pinned with `pthread_setaffinity_np`. One thread per hot core.
- Hyperthreading sibling of each hot core left idle or offlined, so the core is not shared.
- C states pinned so the core does not drop into a deep sleep and pay wakeup latency. Governor set to performance.
- Huge pages reserved, and the AF_XDP umem is allocated on 2 MB huge pages so the frame array spans a handful of dTLB entries instead of thousands. `mlockall` on the process, hot data touched at startup so there are no first touch page faults on the path.
- Offered load is paced. The off core sender places each packet on a fixed time grid, so the corrected histogram is measured against a cadence the harness actually enforces rather than an assumed one.
- Turbo decision documented per run. Turbo lowers the median and raises jitter. caliper targets predictable, so the default run disables turbo and a second run records the turbo on numbers for comparison.

## the hunt

The headline is the drop, for example "drove p99.99 tick to trade from X ns to Y ns". The body is how each tail cause was found. Each entry follows the same shape so the chase is legible.

Template per finding:

> **symptom.** Where the tail showed up, which percentile, how big.
> **measurement.** The histogram or counter that isolated it.
> **cause.** The named mechanism.
> **fix.** The change.
> **result.** Before and after percentiles.

The full writeup is in `docs/hunt.md`, with a per knob ablation and the load sweep. Six findings carry a before and after: the decode tail was an unpaced burst artifact and went from p50 110 ns to 10 ns once the load was paced; the tails past p99.9 were host timer noise and fell to meet p99.9 once the core was kernel isolated; dTLB misses went from 1.4M to a few hundred once the umem moved to huge pages; the coordinated omission correction stopped overcorrecting once the offered cadence was enforced; the branch mispredict suspected in the book type switch was refuted on the real day; and an incremental best bid and ask replaced the O(n) ladder scan, dropping the book stage from 200 ns to 20 ns and confirming the scan exit was where the mispredicts lived. Two of the six are measured negatives, which real data is for. Tick to trade holds flat from 100 kpps to about 5 Mpps with no saturation knee in range. The two template findings below still need a before and after each, so `docs/hunt.md` keeps their result lines open rather than carrying invented numbers.

### 1. first touch page fault on the order buffer

- symptom: a spike at p99.99 that appears once near the start of every run.
- measurement: dTLB and page fault counters fire on the first write to a buffer.
- cause: the buffer pages are mapped lazily and faulted in on first write.
- fix: `mlockall` plus a startup pass that writes one byte per page.
- result: mitigated, not measured as a before and after. The harness already does this and now backs the umem with huge pages. See `docs/hunt.md`.

### 2. false sharing on the book hot line

- symptom: p99.9 wider than the per stage sum suggests for `book`.
- measurement: LLC and coherence counters rise when two fields on the same cache line are written from different cores.
- cause: a counter and the book sequence number share a 64 byte line.
- fix: pad to separate lines.
- result: open. The book runs on one core today, so a second writer is needed to reproduce it. See `docs/hunt.md`.

### 3. branch mispredict, hypothesis refuted on real flow

- symptom: branch misses run about 8 per update on the real AAPL day, more than the straight line stages explain. The first suspect was the `switch (m.type)` in book.apply.
- measurement: built a branchless cmov dispatch (`-DCALIPER_BOOK_BRANCHLESS=ON`) and ran it against the switch on the real day. Branch misses did not move: 16.9M with the switch, 16.4M branchless, inside the run variance.
- cause: the switch was not the mispredict source. The data dependent exit of the best bid and ask scan in refresh() is.
- fix: none shipped. The branchless dispatch is reverted, kept behind the flag.
- result: closed as a measured negative, the kind real data is for. See `docs/hunt.md`.

## build and run

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # histogram and CO unit tests
sudo scripts/host_setup.sh        # applies runtime knobs, prints expected boot cmdline
sudo scripts/setup_veth.sh        # native AF_XDP binds to a veth pair, bring it up
sudo scripts/run_bench.sh         # runs the loop, writes results/ and the CDFs
```

On x86_64 Linux the native backend opens an AF_XDP socket, calls `mlockall`, pins cores, and opens the PMU group, so the run needs root. `scripts/run_bench.sh` brings up the `calib0`/`calib1` veth pair if it is missing and runs under `sudo`. The native backend needs `libxdp` and `libbpf` (`libxdp-dev`, `libbpf-dev`); the portable backend on a dev host needs neither.

The histogram, the coordinated omission correction, and the incremental book best, the code every published number runs through, have unit tests under `ctest`, including a shadow model check of the book best against a brute force scan over 500000 operations. Three experiments from the hunt writeup are behind build flags: `-DCALIPER_BOOK_FULLSCAN=ON` reverts the book to the O(n) rescan for the incremental best before and after, and `-DCALIPER_DECODE_PREFETCH=ON` and `-DCALIPER_BOOK_BRANCHLESS=ON` are the two measured negatives, off by default. `CALIPER_RATE_NS` sets the offered packet interval for the latency versus load sweep.

**Real feed.** caliper reacts to a real NASDAQ day, replayed through gavel. To build the feed (needs a gavel checkout next to caliper):

```
cd ../gavel && cmake -G "Unix Makefiles" -B build && cmake --build build -j && scripts/get_data.sh
cd ../caliper && scripts/build_feed.sh        # writes data/AAPL.feed
```

Without a feed file caliper falls back to the synthetic generator, so it still builds and runs anywhere. `docs/feed.md` has the full pipeline.

The run replays the feed pattern as raw UDP frames from an off core sender, reacts to each, records corrected and uncorrected histograms, and writes the percentile tables and CDFs to `results/`. On a host with two ports or a second box, point the sender at the real wire and the rx and tx endpoints become NIC hardware timestamps.

## layout

```
src/
  rx/          receive and send transport: af_xdp.cpp (native, real AF_XDP
               rings, plus the off core feed sender) and sim_transport.cpp
               (portable). tx is the transport send leg, not a separate dir.
  decode/      wire to internal
  book/        order book
  strategy/    the reacting logic
  encode/      internal to wire
  timing/      rdtscp, calibration, overhead subtraction
  hist/        HdrHistogram glue, coordinated omission correction
  pmu/         perf_event_open counter reads (pmu_perf.cpp native)
bridge/         gavel_to_feed: runs the gavel engine over a real venue day and
                normalizes the published events into a caliper feed file
scripts/
  host_setup.sh   governor, boost, SMT sibling, IRQ affinity, hugepages, C states
  setup_veth.sh   the calib0/calib1 veth pair the native backend binds to
  build_feed.sh   builds the bridge and writes data/SYMBOL.feed
  run_bench.sh
data/           the feed file the run replays (SYMBOL.feed, not committed)
results/        percentile tables, per stage CDFs, latency_vs_load.csv, host.md
docs/           hunt.md the tail writeup with ablation, feed.md the data pipeline
```

## hardware notes

This targets x86_64 with kernel bypass. The hot path uses AF_XDP, with DPDK as an option for the cards that warrant it. Hardware timestamps need a NIC with a PTP clock and TX timestamping. The headline numbers are taken on bare metal Linux x86_64, not a VM and not ARM, because `isolcpus`, kernel bypass, and PMU access need the real machine.

## references

- Gil Tene on coordinated omission.
- HdrHistogram.
- AF_XDP and DPDK documentation for the receive path.
- `perf_event_open` for the PMU counters.
