# the hunt

Each entry traces one tail to its cause and records the before and after. The
template is fixed so the chase stays legible: symptom, measurement, cause, fix,
result.

Provenance. Findings A through F come from one canonical AF_XDP run, the one
written to `results/`: the real AAPL day, the book maintaining its best
incrementally, core 2 kernel isolated, the feed paced to 4000 ns, the umem on
huge pages. Each toggles a single knob from that run and reports the same
baseline as its after, so the after of one knob is the canonical number. Two
knobs cannot be toggled in place: kernel isolation needs a reboot and pacing
changes the offered load. Their before states are earlier runs on the synthetic
feed and are labeled as such. Finding G is a second configuration, the
so_timestamping run across two hosts with real NIC hardware stamps, written to
`results/hardware/` with its own provenance; it is not the bypass run and is
labeled wherever its numbers appear.

A note on honesty. A negative result that was measured and reverted is kept
above the line, because a measured no is worth as much as a measured yes. The
template findings below the line have not been run end to end on this host, so
their result lines stay open rather than carry invented numbers.

## findings

### A. the decode tail was an unpaced burst, and a prefetch does not help

- symptom: an early run on the synthetic feed showed decode at p50 110 ns, far
  above the few ns its arithmetic costs. The frame is read cold from the umem
  each packet.
- measurement: that feed sent in bursts of up to 64, so decode read each frame
  cold. Pacing the feed spaces arrivals, and on the canonical real run decode
  sits at p50 10 ns: the frame is warm by the time decode reads it. A software
  prefetch of the next frame, re-run on the canonical run behind
  CALIPER_DECODE_PREFETCH, does not help: decode p50 10 ns with and without it,
  L1d misses 53.8M against 50.0M, inside the run to run variance.
- cause: the cold read tail was a property of the bursty harness, not the path.
- fix: pace the feed. The prefetch is built, measured, and reverted.
- result: confirmed, including the negative half. decode p50 110 ns (synthetic,
  unpaced burst) to 10 ns (canonical, paced). Prefetch: no change, ships off.

### B. tails past p99.9 were host timer noise

- symptom: before isolation, on the synthetic feed with the core not kernel
  isolated, every stage jumped after p99.9: decode p99.9 140 ns to p99.99
  2671 ns, and the wire to wire max was 46 us.
- measurement: the jump landed at the same percentile across unrelated stages, a
  shared external cause, not any one stage's code. The boot cmdline carried no
  isolcpus, nohz_full, or rcu_nocbs.
- cause: the core was runtime isolated only. Timer ticks and rcu callbacks hit
  it a few thousand times a second and land in the tail.
- fix: booted core 2 with isolcpus=2 nohz_full=2 rcu_nocbs=2.
- result: confirmed. decode p99.99 2671 ns (un-isolated) to 110 ns (isolated).
  On the canonical run the whole per stage spectrum is flat to p99.99 and tick
  to trade p99.99 is 3362 ns. The before is an earlier run, the only un-isolated
  one, since isolation cannot be toggled without a reboot.

### C. dTLB misses were the base page umem, fixed by huge pages

- symptom: on base pages the run shows about 1.4M dTLB misses, near one per
  packet, on a path with a small working set.
- measurement: the umem is 8 MB of frames. On 4 KB pages that is about 2000 page
  table entries, more than the dTLB holds. Backing it with 2 MB huge pages spans
  the same 8 MB in four entries. Toggling nr_hugepages on the canonical run,
  with everything else held, dTLB misses go from 1,427,720 on base pages to 5234
  on huge pages.
- cause: the frame array was larger than the dTLB reach on base pages.
- fix: allocate the umem with mmap MAP_HUGETLB, base page fallback if none are
  reserved.
- result: confirmed, about a 270x drop. One honesty note: the huge page dTLB
  count is the single figure that moves run to run, from a few hundred to a few
  thousand, because the small base page allocations around the umem fault
  differently each run. The base page count is steady near 1.4M and the effect
  is robust at the thousand fold scale; the exact huge number is this run's 5234.

### D. coordinated omission correction is only honest against an enforced cadence

- symptom: on the unpaced synthetic run the corrected wire column diverged hard
  from the uncorrected one: p99.99 7022 ns uncorrected against 14747 ns
  corrected.
- measurement: the CO correction back fills against an expected interval. That
  interval was a fixed 1000 ns the harness never enforced, because the sender
  was unpaced, so the corrected column described a load the test never offered.
  Pacing the sender and feeding the correction the same cadence, the columns
  converge: on the canonical run p99.99 is 3362 ns in both, equal to a tick at
  every percentile.
- cause: the expected interval was not the cadence the harness ran at.
- fix: enforce the cadence and report CO against it. The before is an earlier
  unpaced run, since pacing changes the offered load.
- result: confirmed. The columns now agree, and would diverge only when a real
  stall omits samples.

### E. the branch mispredict was not the book type switch

- symptom: on the canonical run branch misses run about 6 per update, 12.96M
  over the run, more than the straight line stages explain. The first suspect
  was the data dependent `switch (m.type)` in book.apply.
- measurement: built a branchless cmov dispatch behind CALIPER_BOOK_BRANCHLESS
  and toggled it on the canonical run. Branch misses did not move: 12.96M with
  the switch, 13.08M branchless. The switch was already cheap, well predicted or
  jump tabled by the compiler.
- cause: the mispredicts are not in the type dispatch. They are in the data
  dependent exit of the best bid and ask scan, which finding F removes.
- fix: none. The branchless dispatch is reverted, kept behind the flag.
- result: closed as a measured negative. Real data refuted the hypothesis.

### F. an incremental best price replaces the O(n) ladder scan

- symptom: book.apply rescanned the whole ladder for the best bid and ask on
  every update, up to 1617 levels per side, and finding E placed the dominant
  branch mispredict in that scan's data dependent exit.
- measurement: maintained the best level per side incrementally. A nonzero at or
  above the best moves it at once; clearing the best level scans for the next
  resting level, a step or two in an active book. A shadow model test checks the
  incremental best against a brute force scan after every operation over 500000
  ops. Toggling CALIPER_BOOK_FULLSCAN on the canonical run:
  - book stage p50 210 ns to 20 ns, a 10x drop.
  - tick to trade p50 2622 ns to 2402 ns, p99.99 3542 ns to 3362 ns.
  - branch misses 15.87M to 12.96M, the scan exit mispredicts finding E named.
  - L1d misses 83.9M to 53.8M, the per update scan was the bulk of the touches.
- cause: a linear rescan where an incremental update suffices.
- fix: track the best level and adjust it in O(1), rescanning only when the best
  empties. Shipped on by default; the scan stays behind the flag for the before.
- result: confirmed. After this the userspace compute is small enough that tick
  to trade is bound by the tx wakeup, not by caliper's work.

### G. the kernel and NIC tax, measured against real hardware timestamps

- symptom: the af_xdp numbers are bypass, with rdtscp software endpoints over a
  veth pair, not a real wire to wire. The open question was what the kernel rx
  and tx path plus the NIC actually cost on a socket datapath.
- measurement: a second configuration, the so_timestamping backend, binds a real
  udp socket on enp35s0.4000 with SO_TIMESTAMPING, and a second host replays the
  AAPL day to it at 10000 pps. rx_ts and tx_ts are real igb PHC hardware stamps,
  matched to the order by OPT_ID, so tick to trade is a true NIC to NIC wire to
  wire. Over 2,082,382 orders with a 100 percent hardware tx stamp yield: median
  28655 ns, p99 32239 ns, p99.9 35295 ns, p99.99 81791 ns, max 272162 ns. The
  per stage rdtscp compute is unchanged from the bypass run, decode 10, book 30,
  decide 10, encode 20 ns, summing to 70 ns.
- cause: the median minus the 70 ns of userspace compute leaves about 28585 ns.
  That residual is the kernel rx path from the NIC to recvmsg, the kernel tx path
  from sendto to the NIC, the NIC, and the on host wire.
- fix: none, this is the cost of a socket datapath. It is the reason colocated
  systems bypass the kernel with af_xdp or dpdk.
- result: confirmed. The two configurations bracket the path: the bypass run
  isolates the 70 ns of compute, the hardware run shows the roughly 28.6 us the
  kernel and NIC add on top. The hardware path's tail past p99.99 is kernel
  scheduling and softirq jitter off the isolated core, which the bypass path,
  polling on the hot core, does not pay. Provenance in results/hardware/host.md.

## load

Tick to trade holds flat under offered load. Sweeping the sender from 100 kpps
to about 5 Mpps, the wire p50 stays near 2400 ns and p99.99 near 3400 ns with
delivery at or above 99.8 percent and the corrected and uncorrected columns
equal, so the loop never falls behind across the whole range. No saturation knee
appears: the single box veth sender reaches its own send ceiling before caliper
does, so the throughput limit is above what this rig can offer rather than a
number the run measures. `results/latency_vs_load.csv` has the sweep.

## ablation

Each knob's contribution, one line each, from the canonical run. The before for
isolation and for pacing is an earlier run on the synthetic feed, since neither
can be toggled in place; every other before is a single toggle on the canonical
run.

| change | metric | before | after | before run |
| --- | --- | --- | --- | --- |
| kernel isolation | decode p99.99 | 2671 ns | 110 ns | synthetic, un-isolated |
| paced feed, CO honest | wire p99.99 corrected | 14747 ns | 3362 ns | synthetic, unpaced |
| incremental best | book stage p50 | 210 ns | 20 ns | canonical toggle |
| huge page umem | dTLB misses | 1.43M | 5234 | canonical toggle |
| decode prefetch | decode p50 | 10 ns | 10 ns | canonical toggle, no change |
| branchless dispatch | branch misses | 12.96M | 13.08M | canonical toggle, no change |

## template findings, not yet run on this host

### 1. first touch page fault on the order buffer

- symptom: a spike at p99.99 that appears once near the start of every run.
- measurement: dTLB and page fault counters fire on the first write to a buffer.
- cause: buffer pages are mapped lazily and faulted in on first write.
- fix: `mlockall` plus a startup pass that writes one byte per page.
- result: mitigated, not measured as a before and after. The harness does
  mlockall, warms the umem, and now backs it with huge pages, so the before case
  needs that protection removed on purpose to measure the spike it prevents.

### 2. false sharing on the book hot line

- symptom: p99.9 wider than the per stage sum suggests for `book`.
- measurement: LLC and coherence counters rise when two fields on one cache line
  are written from different cores.
- cause: a counter and the book sequence number share a 64 byte line.
- fix: pad to separate lines.
- result: open. The book runs on the hot core alone today, so there is no second
  writer to share the line. This needs a writer added on another core and the
  AMD L3 or coherence counters, which are not on the core PMU used here.
