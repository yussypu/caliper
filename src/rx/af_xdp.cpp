#include "common/platform.h"

#if CALIPER_NATIVE

#include <xdp/xsk.h>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "common/wire.h"
#include "rx/feed.h"
#include "rx/feed_source.h"
#include "rx/transport.h"
#include "timing/tsc.h"

// Native transport on x86_64 Linux. The receive path is a real AF_XDP socket:
// a registered UMEM with fill, rx, tx, and completion rings bound at startup,
// the kernel redirecting ingress frames straight to the ring. On this single
// box the link is a veth pair, because the one physical port (igb, the host
// lifeline) cannot be cabled back to itself and igb does not expose the XDP rx
// metadata that would carry a NIC hardware timestamp. So rx_ts and the tx
// return value are rdtscp reads taken at the ring boundary, not NIC PTP
// stamps. They bracket the userspace tick to trade across the real AF_XDP
// datapath. The harness banner states this. The feed is replayed by an off
// core sender that pushes the feed.h pattern as raw UDP frames into the peer
// veth, standing in for gavel.

namespace caliper {

namespace {

constexpr char kRecvIf[] = "calib1";   // xsk binds here, frames arrive here
constexpr char kSendIf[] = "calib0";   // sender injects frames here
constexpr uint32_t kQueueId = 0;
constexpr uint16_t kDport = 31337;     // udp port the feed lands on, others are dropped

constexpr uint32_t kFrameSize = 2048;
constexpr uint32_t kNumRxFrames = 2048;
constexpr uint32_t kNumTxFrames = 2048;
constexpr uint32_t kNumFrames = kNumRxFrames + kNumTxFrames;
constexpr uint64_t kTxBase = static_cast<uint64_t>(kNumRxFrames) * kFrameSize;

constexpr uint32_t kEthLen = 14;
constexpr uint32_t kIpLen = 20;
constexpr uint32_t kUdpLen = 8;
constexpr uint32_t kHdrLen = kEthLen + kIpLen + kUdpLen;  // 42

constexpr uint64_t kFeedCount = 2'000'000;

// Enforced arrival spacing in ns. The sender paces sends onto this grid and the
// CO correction back fills against the same number, so the corrected tail is
// measured against a load the harness actually offers. It sits above the closed
// loop service time so the consumer keeps up and the rx ring does not overflow.
constexpr uint64_t kArrivalIntervalNs = 4000;

constexpr unsigned kRxBatch = 64;

uint64_t mono_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

uint16_t ip_checksum(const void* data, int len) {
  const uint16_t* p = static_cast<const uint16_t*>(data);
  uint32_t sum = 0;
  while (len > 1) {
    sum += *p++;
    len -= 2;
  }
  if (len) sum += *reinterpret_cast<const uint8_t*>(p);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

bool get_mac(const char* ifn, uint8_t mac[6]) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return false;
  ifreq r;
  std::memset(&r, 0, sizeof(r));
  std::strncpy(r.ifr_name, ifn, IFNAMSIZ - 1);
  bool ok = ioctl(s, SIOCGIFHWADDR, &r) == 0;
  if (ok) std::memcpy(mac, r.ifr_hwaddr.sa_data, 6);
  close(s);
  return ok;
}

void pin_thread(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

}  // namespace

struct Transport::Impl {
  void* umem_area = nullptr;
  uint64_t umem_bytes = 0;
  bool umem_hugepages = false;
  xsk_umem* umem = nullptr;
  xsk_socket* xsk = nullptr;
  xsk_ring_prod fq{};
  xsk_ring_cons cq{};
  xsk_ring_cons rx{};
  xsk_ring_prod tx{};
  int xsk_fd = -1;
  bool ok = false;

  // rx batch cache: peek a batch, release the ring slots, hand out one frame
  // per rx() call and recycle each umem frame to the fill ring on the next.
  uint64_t cache_addr[kRxBatch];
  uint32_t cache_len[kRxBatch];
  unsigned cache_n = 0;
  unsigned cache_pos = 0;
  uint64_t pending_recycle = 0;
  bool have_pending = false;

  uint64_t tx_cursor = 0;  // cycles over the tx frame partition

  std::thread sender;
  std::atomic<bool> sender_done{false};
  std::atomic<bool> shutdown{false};
  uint64_t received = 0;
  uint64_t arrival_ns = kArrivalIntervalNs;  // CALIPER_RATE_NS overrides, for the load sweep

  FeedSource feed_file;  // real venue day if present, else synthetic fallback

  void recycle(uint64_t addr) {
    uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&fq, 1, &idx) == 1) {
      *xsk_ring_prod__fill_addr(&fq, idx) = addr;
      xsk_ring_prod__submit(&fq, 1);
    }
  }

  void drain_completions() {
    uint32_t idx = 0;
    unsigned n = xsk_ring_cons__peek(&cq, kRxBatch, &idx);
    if (n) xsk_ring_cons__release(&cq, n);
  }

  void run_sender();
};

void Transport::Impl::run_sender() {
  pin_thread(4);  // off the hot core, stands in for the exchange

  int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (s < 0) {
    sender_done.store(true);
    return;
  }
  int ifindex = if_nametoindex(kSendIf);
  uint8_t smac[6], dmac[6];
  if (!get_mac(kSendIf, smac) || !get_mac(kRecvIf, dmac)) {
    close(s);
    sender_done.store(true);
    return;
  }

  uint8_t frame[kHdrLen + sizeof(MdMsg)];
  std::memset(frame, 0, sizeof(frame));
  ethhdr* eth = reinterpret_cast<ethhdr*>(frame);
  std::memcpy(eth->h_dest, dmac, 6);
  std::memcpy(eth->h_source, smac, 6);
  eth->h_proto = htons(ETH_P_IP);

  uint8_t* ip = frame + kEthLen;
  ip[0] = 0x45;
  uint16_t tot = htons(kIpLen + kUdpLen + sizeof(MdMsg));
  std::memcpy(ip + 2, &tot, 2);
  ip[8] = 64;   // ttl
  ip[9] = 17;   // udp
  uint32_t saddr = inet_addr("10.55.0.1");
  uint32_t daddr = inet_addr("10.55.0.2");
  std::memcpy(ip + 12, &saddr, 4);
  std::memcpy(ip + 16, &daddr, 4);
  uint16_t csum = ip_checksum(ip, kIpLen);
  std::memcpy(ip + 10, &csum, 2);

  uint8_t* udp = frame + kEthLen + kIpLen;
  uint16_t sport = htons(40000), dport = htons(31337);
  uint16_t ulen = htons(kUdpLen + sizeof(MdMsg));
  std::memcpy(udp + 0, &sport, 2);
  std::memcpy(udp + 2, &dport, 2);
  std::memcpy(udp + 4, &ulen, 2);

  uint8_t* payload = frame + kHdrLen;

  sockaddr_ll sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sll_family = AF_PACKET;
  sa.sll_ifindex = ifindex;
  sa.sll_halen = 6;
  std::memcpy(sa.sll_addr, dmac, 6);

  // The feed is the real venue day if a feed file loaded, else the synthetic
  // generator. Either way every record is one MdMsg on the wire.
  const bool real = feed_file.loaded();
  const uint64_t count = real ? feed_file.count() : kFeedCount;
  const MdMsg* records = real ? feed_file.data() : nullptr;
  Feed synth(kFeedCount);

  // Pace sends onto a fixed grid so the offered load is the rate the CO
  // correction assumes. Absolute targets off one base, so spacing does not
  // drift with per send cost. The wait is a busy spin because this thread owns
  // core 4 and a sleep would add scheduler wakeup jitter to the cadence.
  MdMsg m;
  uint64_t base = mono_ns();
  for (uint64_t i = 0; i < count && !shutdown.load(); ++i) {
    uint64_t target = base + i * arrival_ns;
    while (mono_ns() < target && !shutdown.load()) {
    }
    if (real) {
      std::memcpy(payload, &records[i], sizeof(MdMsg));
    } else {
      synth.at(i, m);
      std::memcpy(payload, &m, sizeof(m));
    }
    sendto(s, frame, sizeof(frame), 0, reinterpret_cast<sockaddr*>(&sa),
           sizeof(sa));
  }
  close(s);
  sender_done.store(true);
}

Transport::Transport() : impl_(new Impl()) {
  Impl* d = impl_;

  // Back the umem with 2 MB huge pages so the frame array spans a handful of
  // TLB entries instead of thousands of 4 KB ones, which keeps the per frame
  // load off the dTLB miss path. Fall back to base pages where huge pages are
  // not reserved so the loop still runs.
  d->umem_bytes = static_cast<uint64_t>(kNumFrames) * kFrameSize;
  d->umem_area = mmap(nullptr, d->umem_bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (d->umem_area != MAP_FAILED) {
    d->umem_hugepages = true;
  } else {
    d->umem_area = mmap(nullptr, d->umem_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (d->umem_area == MAP_FAILED) {
    d->umem_area = nullptr;
    std::fprintf(stderr, "af_xdp: umem alloc failed\n");
    return;
  }

  xsk_umem_config ucfg;
  std::memset(&ucfg, 0, sizeof(ucfg));
  ucfg.fill_size = kNumRxFrames;
  ucfg.comp_size = kNumTxFrames;
  ucfg.frame_size = kFrameSize;
  ucfg.frame_headroom = 0;
  ucfg.flags = 0;
  int err = xsk_umem__create(&d->umem, d->umem_area,
                             static_cast<uint64_t>(kNumFrames) * kFrameSize,
                             &d->fq, &d->cq, &ucfg);
  if (err) {
    std::fprintf(stderr, "af_xdp: umem create failed: %s\n", strerror(-err));
    return;
  }

  xsk_socket_config scfg;
  std::memset(&scfg, 0, sizeof(scfg));
  scfg.rx_size = kNumRxFrames;
  scfg.tx_size = kNumTxFrames;
  scfg.libxdp_flags = 0;
  scfg.xdp_flags = 0;
  scfg.bind_flags = XDP_USE_NEED_WAKEUP;
  err = xsk_socket__create(&d->xsk, kRecvIf, kQueueId, d->umem, &d->rx, &d->tx,
                           &scfg);
  if (err) {
    std::fprintf(stderr,
                 "af_xdp: socket create on %s failed: %s\n"
                 "  run scripts/setup_veth.sh first (needs the %s/%s veth pair)\n",
                 kRecvIf, strerror(-err), kSendIf, kRecvIf);
    return;
  }
  d->xsk_fd = xsk_socket__fd(d->xsk);
  d->ok = true;

  if (const char* r = std::getenv("CALIPER_RATE_NS")) {
    uint64_t v = std::strtoull(r, nullptr, 10);
    if (v > 0) d->arrival_ns = v;
  }

  if (d->feed_file.load(feed_path())) {
    const FeedHeader& h = d->feed_file.header();
    std::printf("af_xdp: feed %.8s day %llu, %llu real book updates, %u level band\n",
                h.symbol, static_cast<unsigned long long>(h.day),
                static_cast<unsigned long long>(h.msg_count), h.num_levels);
  } else {
    std::printf("af_xdp: no feed file at %s, using the synthetic generator\n", feed_path());
  }

  std::printf(
      "af_xdp: bound xsk to %s queue %u, umem %u frames of %u bytes on %s pages\n"
      "af_xdp: sender paces the feed to one packet per %llu ns\n"
      "af_xdp: endpoints are rdtscp (software). rx_ts and tx_ts are NOT NIC\n"
      "        hardware timestamps. true wire to wire needs two hosts or two\n"
      "        cabled ports. per stage decode/book/decide/encode are the\n"
      "        representative numbers on this box.\n",
      kRecvIf, kQueueId, kNumFrames, kFrameSize,
      d->umem_hugepages ? "huge" : "base",
      static_cast<unsigned long long>(d->arrival_ns));
}

Transport::~Transport() {
  if (!impl_) return;
  impl_->shutdown.store(true);
  if (impl_->sender.joinable()) impl_->sender.join();
  if (impl_->xsk) xsk_socket__delete(impl_->xsk);
  if (impl_->umem) xsk_umem__delete(impl_->umem);
  if (impl_->umem_area) munmap(impl_->umem_area, impl_->umem_bytes);
  delete impl_;
}

void Transport::warm() {
  Impl* d = impl_;
  if (!d->ok) return;

  // Touch every umem frame so the path takes no first touch fault.
  std::memset(d->umem_area, 0, static_cast<uint64_t>(kNumFrames) * kFrameSize);

  // Prime the fill ring with every rx frame.
  uint32_t idx = 0;
  unsigned reserved = xsk_ring_prod__reserve(&d->fq, kNumRxFrames, &idx);
  for (unsigned i = 0; i < reserved; ++i)
    *xsk_ring_prod__fill_addr(&d->fq, idx++) =
        static_cast<uint64_t>(i) * kFrameSize;
  xsk_ring_prod__submit(&d->fq, reserved);

  d->sender = std::thread([d] { d->run_sender(); });
}

bool Transport::rx(RxPacket& out) {
  Impl* d = impl_;
  if (CALIPER_UNLIKELY(!d->ok)) return false;

  if (d->have_pending) {
    d->recycle(d->pending_recycle);
    d->have_pending = false;
  }

  while (d->cache_pos == d->cache_n) {
    uint32_t idx = 0;
    unsigned n = xsk_ring_cons__peek(&d->rx, kRxBatch, &idx);
    if (n == 0) {
      if (xsk_ring_prod__needs_wakeup(&d->fq)) {
        pollfd pfd{d->xsk_fd, POLLIN, 0};
        poll(&pfd, 1, 0);
      }
      if (d->sender_done.load()) {
        // Sender finished. Drain any last in flight frames, then end.
        pollfd pfd{d->xsk_fd, POLLIN, 0};
        if (poll(&pfd, 1, 5) <= 0) return false;
        continue;
      }
      pollfd pfd{d->xsk_fd, POLLIN, 0};
      poll(&pfd, 1, 100);
      continue;
    }
    for (unsigned k = 0; k < n; ++k) {
      const xdp_desc* desc = xsk_ring_cons__rx_desc(&d->rx, idx++);
      d->cache_addr[k] = desc->addr;
      d->cache_len[k] = desc->len;
    }
    xsk_ring_cons__release(&d->rx, n);
    d->cache_n = n;
    d->cache_pos = 0;
  }

  uint64_t addr = d->cache_addr[d->cache_pos];
  uint32_t len = d->cache_len[d->cache_pos];
  d->cache_pos++;

  uint8_t* frame = static_cast<uint8_t*>(xsk_umem__get_data(d->umem_area, addr));

  // Drop anything that is not our udp feed: short frames, non ipv4, non udp,
  // or the wrong port. On a live multicast feed this is the group and port
  // filter. Rejected frames recycle and the next is tried.
  bool ours = len >= kHdrLen + sizeof(MdMsg);
  if (ours) {
    uint16_t etype;
    uint16_t dport;
    std::memcpy(&etype, frame + 12, 2);
    std::memcpy(&dport, frame + kEthLen + kIpLen + 2, 2);
    uint8_t proto = frame[kEthLen + 9];
    ours = etype == htons(ETH_P_IP) && proto == 17 && dport == htons(kDport);
  }
  if (CALIPER_UNLIKELY(!ours)) {
    d->pending_recycle = addr;
    d->have_pending = true;
    return rx(out);
  }

#if CALIPER_DECODE_PREFETCH
  // Prefetch the next frame's payload while this one is decoded, booked, and
  // answered. One closed loop iteration of distance hides the cold umem load
  // so the demand read in decode hits L1 instead of pacing the decode stage.
  if (d->cache_pos < d->cache_n) {
    uint8_t* nxt = static_cast<uint8_t*>(
        xsk_umem__get_data(d->umem_area, d->cache_addr[d->cache_pos]));
    __builtin_prefetch(nxt + kHdrLen, 0, 3);
    __builtin_prefetch(nxt + kHdrLen + 64, 0, 3);
  }
#endif

  out.data = frame + kHdrLen;
  out.len = len - kHdrLen;
  out.rx_ts = timing::now();
  out.expected_interval = d->arrival_ns;

  d->pending_recycle = addr;
  d->have_pending = true;
  d->received++;
  return true;
}

uint64_t Transport::tx(const uint8_t* data, size_t len) {
  Impl* d = impl_;
  if (CALIPER_UNLIKELY(!d->ok)) return timing::now();

  d->drain_completions();

  uint32_t idx = 0;
  if (xsk_ring_prod__reserve(&d->tx, 1, &idx) != 1) {
    // tx ring full, return the timestamp without blocking the loop.
    return timing::now();
  }
  uint64_t addr = kTxBase + (d->tx_cursor % kNumTxFrames) * kFrameSize;
  d->tx_cursor++;

  uint8_t* frame = static_cast<uint8_t*>(xsk_umem__get_data(d->umem_area, addr));
  size_t n = len <= kFrameSize ? len : kFrameSize;
  std::memcpy(frame, data, n);

  xdp_desc* desc = xsk_ring_prod__tx_desc(&d->tx, idx);
  desc->addr = addr;
  desc->len = static_cast<uint32_t>(n);
  xsk_ring_prod__submit(&d->tx, 1);

  if (xsk_ring_prod__needs_wakeup(&d->tx)) {
    sendto(d->xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
  }
  return timing::now();
}

BookBand Transport::book_band() const {
  BookBand b{};
  if (impl_->feed_file.loaded()) {
    const FeedHeader& h = impl_->feed_file.header();
    b.instruments = 1;
    b.levels = static_cast<int>(h.num_levels);
    b.price_base = h.price_base;
    std::memcpy(b.symbol, h.symbol, 8);
    return b;
  }
  b.instruments = 8;
  b.levels = 1024;
  b.price_base = 100000;
  std::memcpy(b.symbol, "SYNTH\0\0", 8);
  return b;
}

const char* Transport::backend() const { return "af_xdp-veth"; }

}  // namespace caliper

#endif  // CALIPER_NATIVE
