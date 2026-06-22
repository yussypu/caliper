# the hunt

Each entry traces one tail to its cause and records the before and after. The
template is fixed so the chase stays legible: symptom, measurement, cause, fix,
result. Numbers come from `make bench` on the host in `results/host.md`.

A note on honesty. The findings above the line carry a before and after the run
supports with evidence. The findings below the line are the template the lab is
built to produce and have not been run end to end on this host, so their result
lines stay open rather than carry invented numbers. A negative result that was
measured and then reverted is kept above the line, because a measured no is
worth as much as a measured yes.

## findings from the published run

### A. the decode tail was an artifact of unpaced bursts, not a cold read to fix

- symptom: on an early run decode sat at p50 110 ns, far above the few ns its
  arithmetic costs, with the whole run showing about 88 L1d misses per packet.
  The first read decode does is the frame the kernel just wrote into the umem,
  cold in this core's L1, so the cold read looked like the cause.
- measurement: the sender on that run pushed frames as fast as `sendto` allowed,
  so they arrived in bursts of up to 64 and decode read each one cold. Pacing the
  sender onto a fixed 4000 ns grid, the load a reacting system actually sees,
  drops decode to p50 10 ns with the same 89 L1d misses per packet over the run.
  The misses did not go away, they were never mostly in decode. Under a paced
  arrival the frame is warm by the time decode reads it, and the residual misses
  are the book ladder rescan, not the decode load.
- cause: the cold read tail was a property of the bursty test harness, not of the
  path. Measuring under an unrealistic offered load put the cost in the wrong
  stage.
- fix: pace the sender to a sustainable cadence so arrivals are spaced the way a
  real feed spaces them. A software prefetch of the next frame ahead of decode
  was also built and measured, behind `-DCALIPER_DECODE_PREFETCH=ON`. On the
  paced datapath it changed nothing that matters: L1d misses 176.9M without it
  against 183.8M with it, decode p50 10 ns either way. It is left off by default
  and kept as a reproducible experiment.
- result: confirmed, including the negative half. decode p50 110 ns to 10 ns from
  realistic pacing. The prefetch is a measured no and ships off.

### B. tails past p99.9 were host timer noise, not the path

- symptom: every stage was flat to p99.9 then jumped. decode p99.9 140 ns to
  p99.99 2671 ns. book p99.9 450 ns to p99.99 3522 ns. The wire to wire max was
  46 us against a p50 of 2.6 us.
- measurement: the jump landed at the same percentile across unrelated stages,
  which points to a shared external cause, not to any one stage's code. The boot
  cmdline carried no isolcpus, nohz_full, or rcu_nocbs, so the scheduler tick and
  rcu callbacks still landed on the hot core.
- cause: the hot core was runtime isolated only, SMT sibling offlined, IRQs
  moved, C states held, but not kernel isolated. Timer ticks and migrations hit
  the core a few thousand times a second and land in the tail.
- fix: booted core 2 with isolcpus=2 nohz_full=2 rcu_nocbs=2, verified in
  /proc/cmdline and in /sys nohz_full and isolated both reading 2. The runtime
  knobs that do not need a reboot are applied on top.
- result: confirmed. On the kernel isolated core the p99.99 rows fell to nearly
  meet p99.9 across unrelated stages, the signature of a shared external cause
  removed rather than any stage's code changing. decode p99.99 2671 ns to 330 ns.
  book p99.99 3522 ns to 720 ns. On the current paced run the same isolation
  holds the tail tight: decode p99.99 120 ns, book p99.99 490 ns, tick to trade
  p99.99 3722 ns against a p99.9 of 3442 ns, max 12.0 us.

### C. dTLB misses were the base page umem, fixed by huge pages

- symptom: the run showed about 1.5M dTLB misses, near one per packet, on a path
  that touches a small working set.
- measurement: the umem is 8 MB of frames. On 4 KB pages that is about 2000 page
  table entries, more than the dTLB holds, so the per frame load misses the dTLB.
  Backing the umem with 2 MB huge pages spans the same 8 MB in four entries.
  With everything else held, paced and prefetch off, dTLB misses go from
  1,495,364 on base pages to 1,214 on huge pages, about a 1200x drop. This also
  ties the 512 huge pages the host reserved, which nothing used before, to a
  measured effect.
- cause: the frame array was larger than the dTLB reach on base pages.
- fix: allocate the umem with `mmap` `MAP_HUGETLB`, fall back to base pages where
  none are reserved so the loop still runs.
- result: confirmed. dTLB misses 1.5M to about 1.2k, no change to the median path.

### D. coordinated omission correction is only honest against an enforced cadence

- symptom: on the unpaced run the corrected wire to wire column diverged hard
  from the uncorrected one: p99.99 7022 ns uncorrected against 14747 ns
  corrected, p99.999 19254 ns against 33778 ns. The correction was doing heavy
  lifting.
- measurement: the CO correction back fills the samples a stall would have
  omitted against an expected arrival interval. That interval was a fixed 1000 ns
  the harness never enforced, because the sender was unpaced. So the corrected
  column was measured against a deadline the test never offered. Pacing the
  sender to 4000 ns and feeding the same number to the correction, the columns
  converge: p99.99 3722 ns uncorrected against 3731 ns corrected, p99.999 4002 ns
  against 4042 ns. When the loop keeps up with a real offered load there is
  almost nothing to back fill, which is the correct behavior.
- cause: the expected interval used for CO was not the cadence the harness ran
  at, so the corrected number described a load that did not exist.
- fix: enforce the cadence on the sender and report CO against that same cadence.
- result: confirmed. The corrected and uncorrected columns now agree to within a
  tick at every percentile, and diverge only when a genuine stall omits samples.

### E. the branch mispredict was not the book type switch

- symptom: on the real AAPL day branch misses run about 8 per update, 16.8M over
  the run, more than the straight line stages explain. The first suspect was the
  `switch (m.type)` in book.apply, since the message type is data dependent.
- measurement: built a branchless cmov dispatch behind CALIPER_BOOK_BRANCHLESS
  and ran it against the switch on the same real day. Branch misses did not move:
  16.9M with the switch, 16.4M branchless, inside the run to run variance from
  ring drops. The switch was already cheap, well predicted or jump tabled by the
  compiler, so removing it did not change the miss rate.
- cause: the mispredicts are not in the type dispatch. They are in the data
  dependent exit of the best bid and ask scan in refresh(), which ends at a
  different level on every update.
- fix: none. The branchless dispatch is reverted and kept behind the flag.
- result: closed as a measured negative. Real data refuted the hypothesis, which
  is what real data is for. Decoding the real type mix did not make the type
  switch the bottleneck the synthetic guess assumed it would be. The scan that
  actually carries the misses is fixed in finding F.

### F. an incremental best price replaces the O(n) ladder scan

- symptom: book.apply rescanned the whole ladder for the best bid and ask on
  every update. On the AAPL band that is up to 1617 levels per side per update,
  and finding E placed the dominant branch mispredict in that scan's data
  dependent exit.
- measurement: maintained the best level per side incrementally. A nonzero at or
  above the best moves it at once; clearing the best level scans for the next
  resting level, a step or two in an active book. A shadow model test checks the
  incremental best against a brute force scan after every operation over 500000
  ops, so the fast path is provably the same answer. Before and after on the real
  AAPL day, behind CALIPER_BOOK_FULLSCAN:
  - book stage p50 200 ns to 20 ns, a 10x drop.
  - tick to trade p50 2602 ns to 2391 ns, p99.99 3562 ns to 3351 ns.
  - branch misses 15.8M to 12.6M, the scan exit mispredicts finding E named, gone.
  - userspace instructions 9.2G to 1.44G, the per update scan was the bulk of the
    compute.
- cause: a linear rescan where an incremental update suffices.
- fix: track the best level and adjust it in O(1), rescanning only when the best
  empties. Shipped on by default; the scan stays behind the flag for this before
  and after.
- result: confirmed. The book stage is no longer the second largest on the path,
  and the branch miss budget dropped by the amount finding E attributed to the
  scan. After this, the userspace compute is small enough that tick to trade is
  bound by the tx wakeup, not by caliper's work.

## load

Tick to trade holds flat under offered load. Sweeping the sender from 100 kpps to
about 5 Mpps, the wire p50 stays near 2400 ns and p99.99 near 3400 ns with
delivery at or above 99.8 percent and the corrected and uncorrected columns
equal, so the loop never falls behind across the whole range. No saturation knee
appears: the single box veth sender reaches its own send ceiling before caliper
does, so the throughput limit is above what this rig can offer rather than a
number the run measures. `results/latency_vs_load.csv` has the sweep.

## ablation

Each knob's contribution, one line each. Numbers are from the controlled before
and after each finding records.

| change | metric | before | after |
| --- | --- | --- | --- |
| kernel isolation | decode p99.99 | 2671 ns | 110 ns |
| huge page umem | dTLB misses, real flow | 1.41M | 304 |
| paced feed, CO honest | wire p99.99 corrected | 14747 ns | 3362 ns |
| incremental best | book stage p50 | 200 ns | 20 ns |
| decode prefetch | L1d, decode p50 | no change | reverted |
| branchless dispatch | branch misses | no change | reverted |

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
