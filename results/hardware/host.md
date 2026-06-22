# host configuration for the hardware timestamp run

This run is the second of two configurations. It measures a true NIC to NIC wire
to wire with real hardware timestamps from the igb PHC, across two hosts. The
other configuration, in `results/`, is the af_xdp veth bypass that isolates the
userspace compute with rdtscp. Neither overwrites the other; they measure
different things and are both published with the host config that produced them.

## the two host setup

- box1, this igb reactor: enp35s0 (igb, Intel I210), VLAN 4000 on enp35s0.4000,
  10.10.10.1/24, MTU 1400, over a Hetzner vSwitch. caliper runs here on the
  kernel isolated core 2.
- box2, the sender: 10.10.10.2, replays the AAPL feed as udp to 10.10.10.1:31337
  at an enforced 10000 pps, all 2,082,454 records.
- the link: L2 over the vSwitch, about 0.9 ms RTT, which is transport and never
  enters tick to trade since both stamps are taken on box1's own PHC.

## timestamping

- enp35s0 SIOCSHWTSTAMP: tx_type HWTSTAMP_TX_ON, rx_filter HWTSTAMP_FILTER_ALL,
  because the feed is plain udp, not ptp.
- socket SO_TIMESTAMPING: RX_HARDWARE | TX_HARDWARE | RAW_HARDWARE | OPT_TSONLY |
  OPT_ID. rx_ts is the raw hardware slot ts[2] on each frame. tx_ts is read off
  MSG_ERRQUEUE and matched to the order by OPT_ID send id, so a stale stamp never
  pairs with the wrong order.
- PHC: /dev/ptp0, index 0. The hardware stamps live in the PHC timescale, proven
  nonzero ts[2] with rx_ts < tx_ts on every paired frame; a software fallback
  would zero ts[2] and the run would stop.
- tx stamp yield on this run: 2,082,382 of 2,082,382, 100.00 percent, 0 missing.
  Orders are sent back to box2's open sender port so they draw no ICMP reject,
  which keeps the error queue clean and the yield high.

## what this number includes, and does not

- includes: the kernel rx path from the NIC to recvmsg, the userspace compute,
  the kernel tx path from sendto to the NIC, the NIC, and the on host wire. This
  is the honest cost of a socket datapath, not a bypass.
- this is not af_xdp bypass. The af_xdp config in `results/` measures the same
  userspace compute with rdtscp but over a veth pair, whose wire leg is a sendto
  wakeup artifact, not a NIC. Use that config for the compute, this one for the
  real wire to wire.

## results

- hardware wire to wire, PHC, uncorrected: p50 28655 ns, p99 32239 ns, p99.9
  35295 ns, p99.99 81791 ns, p99.999 174207 ns, max 272162 ns. CO corrected
  agrees to within a few hundred ns at the tail, because box2 holds an
  independent 10000 pps and the loop keeps up, so there is almost nothing to
  back fill.
- per stage rdtscp, transport independent: decode 10 ns, book 30 ns, decide
  10 ns, encode 20 ns at p50, the same as the af_xdp run.
- decomposition: 28655 ns median minus the 70 ns per stage compute sum leaves
  about 28585 ns of kernel rx, kernel tx, NIC, and wire. See docs/hunt.md
  finding G.
- the tail past p99.99 is kernel scheduling and softirq jitter, since the NIC
  IRQ and the kernel rx/tx softirqs land off the isolated core. It varies run to
  run, occasionally into the low milliseconds. The bypass path does not pay this
  because it polls the rings on the hot core.
