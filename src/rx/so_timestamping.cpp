#include "common/platform.h"

#if CALIPER_NATIVE && CALIPER_HW_TIMESTAMP

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/wire.h"
#include "rx/feed_source.h"
#include "rx/transport.h"
#include "timing/tsc.h"

// Hardware timestamp transport. A normal udp socket on enp35s0.4000 with
// SO_TIMESTAMPING, so rx_ts and tx_ts are real igb PHC stamps and tick to trade
// is a true NIC to NIC wire to wire. Unlike the af_xdp veth path this is not
// kernel bypass: the number includes the kernel rx and tx path, the NIC, and
// the wire. box2 sends the feed as udp to this host, caliper reacts and sends
// the order back to box2. The per stage rdtscp brackets are unchanged, they are
// transport independent and stay the bypass compute measurement.

namespace caliper {

namespace {

const char* kPhys = "enp35s0";
const char* kVlan = "enp35s0.4000";
const char* kSelf = "10.10.10.1";
const char* kPeer = "10.10.10.2";
constexpr uint16_t kPort = 31337;
constexpr uint64_t kArrivalIntervalNs = 4000;  // box2 cadence, for the CO correction

#ifndef FD_TO_CLOCKID
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)
#endif

uint64_t to_ns(const timespec& t) {
  return static_cast<uint64_t>(t.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(t.tv_nsec);
}

uint64_t mono_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return to_ns(t);
}

// Raw hardware slot ts[2] out of a control message set. ts[2] is zero on a
// software fallback, which the caller treats as no hardware stamp.
bool raw_hw_stamp(msghdr* msg, uint64_t& out) {
  for (cmsghdr* c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
      scm_timestamping ts;
      std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
      if (ts.ts[2].tv_sec == 0 && ts.ts[2].tv_nsec == 0) return false;
      out = to_ns(ts.ts[2]);
      return true;
    }
  }
  return false;
}

// Pulls both the raw hardware tx stamp and the OPT_ID send id from a tx error
// queue message, so a stamp is matched to the exact order that produced it. A
// stamp from an earlier send carries an earlier id and is discarded, which is
// what stops a stale stamp pairing with this order and underflowing the wire.
bool tx_hw_stamp(msghdr* msg, uint64_t& ts_out, uint32_t& id_out) {
  bool has_ts = false, has_id = false;
  for (cmsghdr* c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
      scm_timestamping ts;
      std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
      if (ts.ts[2].tv_sec || ts.ts[2].tv_nsec) { ts_out = to_ns(ts.ts[2]); has_ts = true; }
    } else if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) {
      sock_extended_err se;
      std::memcpy(&se, CMSG_DATA(c), sizeof(se));
      if (se.ee_origin == SO_EE_ORIGIN_TIMESTAMPING) { id_out = se.ee_data; has_id = true; }
    }
  }
  return has_ts && has_id;
}

}  // namespace

struct Transport::Impl {
  int fd = -1;
  bool ok = false;
  sockaddr_in peer{};
  sockaddr_in reply_to{};  // the source of the last frame, so the order goes back
                           // to box2's open sender port and draws no ICMP reject

  FeedSource band_file;  // local copy of the feed, read for the price band only
  uint64_t arrival_ns = kArrivalIntervalNs;

  uint8_t rx_buf[2048];
  uint64_t received = 0;
  uint64_t orders = 0;
  uint64_t tx_stamped = 0;
  uint64_t tx_missing = 0;
  uint32_t send_id = 0;  // mirrors the kernel OPT_ID counter, one per sendto

  bool configure() {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, kVlan, std::strlen(kVlan));
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = inet_addr(kSelf);
    self.sin_port = htons(kPort);
    if (bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) < 0) {
      std::perror("bind 10.10.10.1");
      return false;
    }

    hwtstamp_config cfg{};
    cfg.tx_type = HWTSTAMP_TX_ON;
    cfg.rx_filter = HWTSTAMP_FILTER_ALL;  // udp feed is not ptp, stamp every frame
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, kPhys, IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&cfg);
    if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
      std::perror("SIOCSHWTSTAMP on enp35s0");
      return false;
    }

    int flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
                SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY |
                SOF_TIMESTAMPING_OPT_ID;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
      std::perror("SO_TIMESTAMPING");
      return false;
    }

    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = inet_addr(kPeer);
    peer.sin_port = htons(kPort);
    reply_to = peer;  // until the first frame names box2's actual sender port
    return true;
  }

  // Discards pending error queue entries that are not a tx timestamp, mainly the
  // ICMP port unreachable box2 returns when nothing listens on the order port.
  // Clearing them keeps POLLERR from wedging the rx poll into a busy spin.
  void drain_errq() {
    for (;;) {
      char ctrl[256], junk[64];
      iovec iov{junk, sizeof(junk)};
      msghdr m{};
      m.msg_iov = &iov;
      m.msg_iovlen = 1;
      m.msg_control = ctrl;
      m.msg_controllen = sizeof(ctrl);
      if (recvmsg(fd, &m, MSG_ERRQUEUE) < 0) break;
    }
  }

  // Reads the next feed frame and its hardware rx stamp. Returns false on a long
  // idle once frames have flowed, which is the end of box2's replay. A poll that
  // wakes on a socket error rather than data drains the error queue and re-polls,
  // so a rejected order response never spins the loop.
  bool rx(RxPacket& out) {
    for (;;) {
      pollfd pfd{fd, POLLIN, 0};
      int timeout = received == 0 ? 1800000 : 2000;
      int pr = poll(&pfd, 1, timeout);
      if (pr <= 0) return false;
      if (!(pfd.revents & POLLIN)) { drain_errq(); continue; }

      char ctrl[256];
      sockaddr_in src{};
      iovec iov{rx_buf, sizeof(rx_buf)};
      msghdr msg{};
      msg.msg_name = &src;
      msg.msg_namelen = sizeof(src);
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = ctrl;
      msg.msg_controllen = sizeof(ctrl);
      ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
      if (n < 0) { drain_errq(); continue; }
      if (n < static_cast<ssize_t>(sizeof(MdMsg))) continue;
      if (src.sin_family == AF_INET) reply_to = src;

      uint64_t rx_ts = 0;
      if (!raw_hw_stamp(&msg, rx_ts)) {
        std::fprintf(stderr,
                     "so_timestamping: rx frame had no hardware stamp, kernel fell "
                     "back to software. Stopping.\n");
        std::exit(2);
      }
      out.data = rx_buf;
      out.len = static_cast<size_t>(n);
      out.rx_ts = rx_ts;
      out.expected_interval = arrival_ns;
      received++;
      return true;
    }
  }

  // Sends the order to box2 and drains its hardware tx stamp. The serial loop
  // sends one order per received frame, so the i210's single tx stamp slot is
  // drained before the next send. The drain reads past any ICMP error to find
  // the timestamp; a stamp not ready within the budget is counted as missing and
  // the frame is reported unstamped, never fabricated.
  uint64_t tx(const uint8_t* data, size_t len) {
    const uint32_t this_id = send_id++;
    sendto(fd, data, len, 0, reinterpret_cast<sockaddr*>(&reply_to), sizeof(reply_to));
    orders++;
    // Wait for this order's own stamp, discarding earlier ones. The stamp lands
    // a couple of dozen us after send, so the wait is short and the loop still
    // keeps up with the feed; the deadline bounds a genuinely lost stamp. The
    // hardware stamps the tx the instant the NIC sends, so draining late does
    // not change the measured tx_ts.
    const uint64_t deadline = mono_ns() + 200000;  // 200 us
    for (;;) {
      char ctrl[256], junk[64];
      iovec iov{junk, sizeof(junk)};
      msghdr msg{};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = ctrl;
      msg.msg_controllen = sizeof(ctrl);
      if (recvmsg(fd, &msg, MSG_ERRQUEUE) >= 0) {
        uint64_t tx_ts = 0;
        uint32_t id = 0;
        if (tx_hw_stamp(&msg, tx_ts, id) && id == this_id) {
          tx_stamped++;
          return tx_ts;  // this order's own stamp, paired exactly
        }
        continue;  // an earlier stamp or unrelated error, discard and keep draining
      }
      if (mono_ns() >= deadline) break;
      pollfd pfd{fd, POLLERR, 0};
      poll(&pfd, 1, 1);  // let more stamps arrive
    }
    tx_missing++;
    return 0;  // this order's stamp not seen in time, the harness skips the sample
  }
};

Transport::Transport() : impl_(new Impl()) {
  Impl* d = impl_;
  if (!d->configure()) {
    std::fprintf(stderr, "so_timestamping: setup failed, need root and enp35s0.4000\n");
    return;
  }
  if (const char* r = std::getenv("CALIPER_RATE_NS")) {
    uint64_t v = std::strtoull(r, nullptr, 10);
    if (v > 0) d->arrival_ns = v;
  }
  d->band_file.load(feed_path());
  d->ok = true;

  int phc = open("/dev/ptp0", O_RDONLY);
  if (phc >= 0) {
    timespec p{}, rt{};
    clock_gettime(FD_TO_CLOCKID(phc), &p);
    clock_gettime(CLOCK_REALTIME, &rt);
    std::printf("so_timestamping: PHC %lld.%09ld, realtime %lld.%09ld\n",
                (long long)p.tv_sec, p.tv_nsec, (long long)rt.tv_sec, rt.tv_nsec);
    close(phc);
  }
  std::printf(
      "so_timestamping: udp on %s, peer %s:%u, hardware rx and tx stamps from\n"
      "        the igb PHC. rx_ts and tx_ts are real NIC timestamps, so tick to\n"
      "        trade is a true wire to wire that includes the kernel rx and tx\n"
      "        path. This is not bypass, it is the kernel path measured honestly.\n"
      "        box2 must send the feed as udp to %s:%u.\n",
      kVlan, kPeer, kPort, kSelf, kPort);
}

Transport::~Transport() {
  if (!impl_) return;
  if (impl_->fd >= 0) close(impl_->fd);
  std::printf("so_timestamping: %llu orders, %llu hardware tx stamped, %llu missing (%.2f%% sampled)\n",
              static_cast<unsigned long long>(impl_->orders),
              static_cast<unsigned long long>(impl_->tx_stamped),
              static_cast<unsigned long long>(impl_->tx_missing),
              impl_->orders ? 100.0 * impl_->tx_stamped / impl_->orders : 0.0);
  delete impl_;
}

void Transport::warm() {
  // Pin the hot thread to the isolated core and lock memory, same as the run.
  mlockall(MCL_CURRENT | MCL_FUTURE);
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(2, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  std::memset(impl_->rx_buf, 0, sizeof(impl_->rx_buf));
}

bool Transport::rx(RxPacket& out) {
  if (CALIPER_UNLIKELY(!impl_->ok)) return false;
  return impl_->rx(out);
}

uint64_t Transport::tx(const uint8_t* data, size_t len) {
  if (CALIPER_UNLIKELY(!impl_->ok)) return 0;
  return impl_->tx(data, len);
}

// The wire endpoints are nanoseconds from the PHC, not rdtscp ticks. The harness
// records the wire histogram in ns directly and subtracts no rdtscp overhead.
bool Transport::wire_hardware() const { return true; }

BookBand Transport::book_band() const {
  BookBand b{};
  if (impl_->band_file.loaded()) {
    const FeedHeader& h = impl_->band_file.header();
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

const char* Transport::backend() const { return "so-timestamping-nic"; }

}  // namespace caliper

#endif  // CALIPER_NATIVE && CALIPER_HW_TIMESTAMP
