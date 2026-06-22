#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

// Burst validator for the hardware timestamp path. Listens on enp35s0.4000:31337
// for box2's burst, stamps each frame with the igb PHC rx timestamp, sends a
// response to get a PHC tx timestamp, and pairs the two by send id so the tx
// yield is honest when the i210 drops a stamp under load. Stops on any software
// fallback. Run as root.

namespace {

#ifndef FD_TO_CLOCKID
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)
#endif

const char* kPhys = "enp35s0";
const char* kVlan = "enp35s0.4000";
const char* kSelf = "10.10.10.1";
const char* kPeer = "10.10.10.2";
constexpr uint16_t kPort = 31337;
constexpr int kMax = 4000;

uint64_t to_ns(const timespec& t) {
  return static_cast<uint64_t>(t.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(t.tv_nsec);
}

uint64_t mono_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return to_ns(t);
}

}  // namespace

int main() {
  // PHC versus CLOCK_REALTIME, one time, so the clock source is visible.
  int phc = open("/dev/ptp0", O_RDONLY);
  if (phc >= 0) {
    timespec p{}, r{};
    clock_gettime(FD_TO_CLOCKID(phc), &p);
    clock_gettime(CLOCK_REALTIME, &r);
    long long off = static_cast<long long>(to_ns(r)) - static_cast<long long>(to_ns(p));
    std::printf("PHC %lld.%09ld  CLOCK_REALTIME %lld.%09ld  offset %lld ns\n",
                (long long)p.tv_sec, p.tv_nsec, (long long)r.tv_sec, r.tv_nsec, off);
    close(phc);
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, kVlan, std::strlen(kVlan));
  sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = inet_addr(kSelf);
  self.sin_port = htons(kPort);
  if (bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) < 0) {
    std::perror("bind");
    return 1;
  }
  hwtstamp_config cfg{};
  cfg.tx_type = HWTSTAMP_TX_ON;
  cfg.rx_filter = HWTSTAMP_FILTER_ALL;
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, kPhys, IFNAMSIZ - 1);
  ifr.ifr_data = reinterpret_cast<char*>(&cfg);
  if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
    std::perror("SIOCSHWTSTAMP");
    return 1;
  }
  int flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
              SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY |
              SOF_TIMESTAMPING_OPT_ID;
  setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));

  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_addr.s_addr = inet_addr(kPeer);
  peer.sin_port = htons(kPort);

  std::vector<uint64_t> rx_ts(kMax, 0);
  std::vector<char> rx_has(kMax, 0);
  uint64_t received = 0, rx_stamped = 0, rx_sw = 0;
  uint64_t tx_stamped = 0, tx_sw = 0;
  uint64_t min_delta = ~0ull, max_delta = 0, out_of_order = 0;

  auto drain = [&]() {
    for (;;) {
      char ctrl[256], data[64];
      iovec iov{data, sizeof(data)};
      msghdr m{};
      m.msg_iov = &iov;
      m.msg_iovlen = 1;
      m.msg_control = ctrl;
      m.msg_controllen = sizeof(ctrl);
      if (recvmsg(fd, &m, MSG_ERRQUEUE) < 0) break;
      uint64_t tx = 0;
      uint32_t id = 0;
      bool has_ts = false, has_id = false, hw = false;
      for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
          scm_timestamping ts;
          std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
          has_ts = true;
          if (ts.ts[2].tv_sec || ts.ts[2].tv_nsec) { tx = to_ns(ts.ts[2]); hw = true; }
        } else if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) {
          sock_extended_err se;
          std::memcpy(&se, CMSG_DATA(c), sizeof(se));
          if (se.ee_origin == SO_EE_ORIGIN_TIMESTAMPING) { id = se.ee_data; has_id = true; }
        }
      }
      if (!has_ts) continue;
      if (!hw) { tx_sw++; continue; }
      tx_stamped++;
      if (has_id && id < static_cast<uint32_t>(kMax) && rx_has[id]) {
        uint64_t rxt = rx_ts[id];
        if (tx > rxt) {
          uint64_t d = tx - rxt;
          if (d < min_delta) min_delta = d;
          if (d > max_delta) max_delta = d;
        } else {
          out_of_order++;
        }
      }
    }
  };

  std::printf("listening on %s:%u, fire the burst now\n", kVlan, kPort);
  std::fflush(stdout);

  // Time based termination. poll only wakes us; we always drain the error queue
  // so late tx stamps never wedge the loop, and we stop on a wall clock idle.
  bool started = false;
  uint64_t start_ns = mono_ns();
  uint64_t last_rx = start_ns;
  for (;;) {
    pollfd pfd{fd, POLLIN | POLLERR, 0};
    poll(&pfd, 1, 200);
    for (;;) {
      char ctrl[256], data[2048];
      iovec iov{data, sizeof(data)};
      msghdr m{};
      m.msg_iov = &iov;
      m.msg_iovlen = 1;
      m.msg_control = ctrl;
      m.msg_controllen = sizeof(ctrl);
      ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT);
      if (n < 0) break;
      uint64_t r2 = 0;
      bool hw = false;
      for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
          scm_timestamping ts;
          std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
          if (ts.ts[2].tv_sec || ts.ts[2].tv_nsec) { r2 = to_ns(ts.ts[2]); hw = true; }
        }
      }
      uint64_t idx = received++;
      started = true;
      last_rx = mono_ns();
      if (!hw) {
        rx_sw++;
      } else {
        rx_stamped++;
        if (idx < static_cast<uint64_t>(kMax)) { rx_ts[idx] = r2; rx_has[idx] = 1; }
      }
      char resp[20];
      std::memset(resp, 0, sizeof(resp));
      sendto(fd, resp, sizeof(resp), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    }
    drain();
    uint64_t now = mono_ns();
    if (!started && now - start_ns > 20000000000ull) break;   // 20 s, never started
    if (started && now - last_rx > 1500000000ull) break;      // 1.5 s idle, burst done
    if (received >= static_cast<uint64_t>(kMax)) break;
  }
  for (int i = 0; i < 100; ++i) { drain(); usleep(1000); }

  std::printf("\nburst result:\n");
  std::printf("  frames received     %llu of 1000 sent\n", (unsigned long long)received);
  if (received < 1000)
    std::printf("    gap %llu: frames before the listener was up, or socket buffer overflow\n",
                (unsigned long long)(1000 - received));
  else if (received > 1000)
    std::printf("    %llu extra: stray frames on the vlan\n", (unsigned long long)(received - 1000));
  std::printf("  rx hardware stamps  %llu (%.1f%% of received)\n", (unsigned long long)rx_stamped,
              received ? 100.0 * rx_stamped / received : 0.0);
  std::printf("  tx hardware stamps  %llu, yield %.1f%% of responses\n", (unsigned long long)tx_stamped,
              received ? 100.0 * tx_stamped / received : 0.0);
  std::printf("  tx not stamped      %llu (i210 single tx slot under load)\n",
              (unsigned long long)(received - tx_stamped - tx_sw));
  if (tx_stamped)
    std::printf("  rx<tx pairs sane    delta min %llu ns, max %llu ns, out of order %llu\n",
                (unsigned long long)min_delta, (unsigned long long)max_delta,
                (unsigned long long)out_of_order);

  int failures = 0;
  std::printf("\nchecks:\n");
  auto chk = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) failures++;
  };
  chk(received > 0, "frames arrived");
  chk(rx_sw == 0, "no rx software fallback (every rx frame had a raw hardware stamp)");
  chk(tx_sw == 0, "no tx software fallback");
  chk(tx_stamped > 0, "at least one hardware tx stamp");
  chk(out_of_order == 0, "rx_ts < tx_ts on every paired frame");
  chk(max_delta < 100000000ull, "rx to tx deltas sane (under 100 ms)");

  if (rx_sw || tx_sw) {
    std::printf("\nSOFTWARE FALLBACK detected (rx %llu, tx %llu). The kernel did not use the\n"
                "NIC clock. Stop and investigate before trusting any number.\n",
                (unsigned long long)rx_sw, (unsigned long long)tx_sw);
    return 2;
  }
  if (failures) { std::printf("\nburst validation FAILED\n"); return 1; }
  std::printf("\nburst validation passed, stamps are igb PHC hardware\n");
  return 0;
}
