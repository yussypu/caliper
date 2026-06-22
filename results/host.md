# host configuration for the published run

Every number in README.md and docs/hunt.md comes from the run described here.
A latency without its host config is not reproducible, so this file ships next
to the numbers.

## machine

- CPU: AMD Ryzen 5 3600, 6 cores, 12 threads, single socket, Zen 2
- two CCX of 3 cores each, each with its own L3:
  CCX0 cores 0,1,2 (CPUs 0-2,6-8), CCX1 cores 3,4,5 (CPUs 3-5,9-11)
- microcode: 0x8701034
- kernel: Linux 6.8.0-124-generic x86_64
- OS: Ubuntu 24.04.4 LTS
- bare metal (systemd-detect-virt: none), Hetzner dedicated

## clock

- invariant TSC: yes (CPUID 80000007 EDX bit 8)
- kernel refined TSC: 3599.999 MHz, clocksource tsc
- caliper calibration: ns_per_tick 0.2778 (3.6000 GHz), rdtscp pair overhead
  72 ticks (20.0 ns), subtracted from every stage sample

## runtime knobs applied (scripts/host_setup.sh, no reboot)

- governor: performance
- boost: disabled (default run targets predictable, not fastest)
- SMT sibling of the hot core offlined: CPU 8 off, online CPUs 0-7,9-11
- IRQ affinity moved off the hot core (core 2)
- hugepages: 512 x 2 MB reserved
- perf_event_paranoid: 0
- C states: harness holds /dev/cpu_dma_latency at 0 us for its lifetime
- process: mlockall, hot thread pinned to CPU 2, feed sender pinned to CPU 4

## kernel isolation (boot cmdline, applied this run)

- isolcpus=2 nohz_full=2 rcu_nocbs=2 in /proc/cmdline. /sys nohz_full and
  isolated both read 2. Core 2 is kernel isolated: off the scheduler, tickless,
  rcu callbacks offloaded. Set in /etc/default/grub.d/zz-caliper.cfg, original
  /etc/default/grub backed up at /etc/default/grub.bak.caliper.
- effect: the tails past p99.9 fall to nearly meet p99.9. See docs/hunt.md
  finding B for the before and after. Earlier runs on this box carried none of
  these on the cmdline and showed the scheduler and timer noise in the tail.

## datapath and what the timestamps mean

- backend: af_xdp-veth. A real AF_XDP socket (registered UMEM, fill, rx, tx,
  completion rings bound at startup) on a veth pair, fed by an off core sender
  that replays the feed as raw UDP frames into the peer.
- feed: the real AAPL trading day of 2020-01-30, 2,082,454 book updates produced
  by replaying the NASDAQ ITCH session (1,937,937 order events) through the gavel
  matching engine and normalizing the published events. Traded range 318.79 to
  324.18 dollars, a 1617 half tick book band. See docs/feed.md.
- the physical NIC (enp35s0, igb / Intel I210) is NOT in the datapath. It is
  the single host port and the host lifeline, and igb does not expose the XDP
  rx metadata that carries a hardware rx timestamp, so it cannot produce a
  hardware wire to wire on one box.
- rx_ts and tx_ts are rdtscp reads taken at the ring boundary, software
  timestamps, NOT NIC PTP. The wire to wire number is the userspace loop across
  the real AF_XDP datapath, and its tx leg includes a sendto wakeup the veth
  copy path requires. It is not a hardware tick to trade.
- the representative, hardware independent numbers are the per stage userspace
  costs: decode, book, decide, encode, each measured by rdtscp with the clock
  overhead subtracted.
- umem: 8 MB of frames backed by 2 MB huge pages (mmap MAP_HUGETLB, base page
  fallback if none reserved). This holds dTLB misses to a few thousand over the
  run, 5234 on the canonical run, down from 1.43M on base pages. See docs/hunt.md
  finding C.
- offered load: the sender paces the feed to one packet per 4000 ns on a fixed
  time grid, a cadence the closed loop sustains, so the CO correction is measured
  against a load the harness enforces. See docs/hunt.md finding D.

## run

- 2,082,454 real book updates replayed at one per 4000 ns, the strategy reacting
  on a two sided penny book
- per stage all flat from p50 to p99.99 on the kernel isolated core: decode p50
  10 ns p99.99 110 ns, book p50 20 ns p99.99 380 ns (incremental best, finding F),
  tick to trade p50 2402 ns p99.99 3362 ns, max 13.4 us
- PMU userspace: branch misses 12.96M (down from 15.87M once the incremental best
  removed the scan exit mispredicts), L1d misses 53.8M, dTLB misses 5234 on huge
  pages (down from 1.43M base; the huge page figure varies run to run, finding C),
  LLC unavailable on this Zen part. Userspace IPC is low because the compute is
  now small enough that the loop waits on the next paced packet
- load: tick to trade holds flat from 100 kpps to about 5 Mpps with no saturation
  knee in range, results/latency_vs_load.csv. Per knob ablation in docs/hunt.md
- output: results/tick_to_trade.csv, tick_to_trade_co.csv, latency_vs_load.csv,
  and the per stage CDFs, all in nanoseconds
