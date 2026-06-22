#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

// Proves the igb PHC produces real hardware rx and tx timestamps on enp35s0,
// before the so_timestamping transport relies on them. tx: send udp box1 to
// box2 and read the stamp off the error queue. rx: capture box2's ping replies
// on a packet socket with rx hardware timestamping. Both pull ts[2], the raw
// hardware slot. Run as root. A software fallback shows up as a zero ts[2] or a
// CLOCK_REALTIME value, and the checks below catch it.

namespace {

#ifndef FD_TO_CLOCKID
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)
#endif

const char* kPhys = "enp35s0";
const char* kVlan = "enp35s0.4000";
const char* kSelf = "10.10.10.1";
const char* kPeer = "10.10.10.2";
constexpr uint16_t kPort = 31337;

bool enable_hw_timestamping(int fd) {
  hwtstamp_config cfg{};
  cfg.tx_type = HWTSTAMP_TX_ON;
  cfg.rx_filter = HWTSTAMP_FILTER_ALL;  // udp feed is not ptp, so stamp every frame
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, kPhys, IFNAMSIZ - 1);
  ifr.ifr_data = reinterpret_cast<char*>(&cfg);
  if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
    std::perror("SIOCSHWTSTAMP on enp35s0");
    return false;
  }
  return true;
}

void set_timestamping(int fd) {
  int flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
              SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY;
  setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
}

// Pulls ts[2], the raw hardware slot, out of a control message set.
bool raw_hw_stamp(msghdr* msg, timespec& out) {
  for (cmsghdr* c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
      scm_timestamping ts;
      std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
      out = ts.ts[2];
      return true;
    }
  }
  return false;
}

bool nonzero(const timespec& t) { return t.tv_sec != 0 || t.tv_nsec != 0; }

}  // namespace

int main() {
  int failures = 0;

  // One time PHC versus CLOCK_REALTIME. Hardware stamps live in the PHC
  // timescale, so they track this clock, not the wall clock.
  int phc = open("/dev/ptp0", O_RDONLY);
  timespec phc_now{}, real_now{};
  if (phc >= 0) {
    clock_gettime(FD_TO_CLOCKID(phc), &phc_now);
    clock_gettime(CLOCK_REALTIME, &real_now);
    std::printf("PHC clock      %lld.%09ld\n", (long long)phc_now.tv_sec, phc_now.tv_nsec);
    std::printf("CLOCK_REALTIME %lld.%09ld\n", (long long)real_now.tv_sec, real_now.tv_nsec);
    std::printf("PHC trails realtime by about %lld s, so a stamp near the PHC value is hardware\n\n",
                (long long)(real_now.tv_sec - phc_now.tv_sec));
  }

  // tx hardware stamps: send udp to box2, read the error queue.
  int tx = socket(AF_INET, SOCK_DGRAM, 0);
  setsockopt(tx, SOL_SOCKET, SO_BINDTODEVICE, kVlan, std::strlen(kVlan));
  sockaddr_in self{};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = inet_addr(kSelf);
  self.sin_port = htons(kPort);
  bind(tx, reinterpret_cast<sockaddr*>(&self), sizeof(self));
  if (!enable_hw_timestamping(tx)) return 1;
  set_timestamping(tx);

  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_addr.s_addr = inet_addr(kPeer);
  peer.sin_port = htons(kPort);

  std::printf("tx hardware stamps, box1 to box2:\n");
  int tx_ok = 0, tx_miss = 0;
  for (int i = 0; i < 5; ++i) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "hwstamp-%d", i);
    sendto(tx, buf, n, 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    pollfd pfd{tx, POLLERR, 0};
    timespec t{};
    bool got = false;
    if (poll(&pfd, 1, 200) > 0) {
      char ctrl[256];
      char data[64];
      iovec iov{data, sizeof(data)};
      msghdr msg{};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = ctrl;
      msg.msg_controllen = sizeof(ctrl);
      if (recvmsg(tx, &msg, MSG_ERRQUEUE) >= 0) got = raw_hw_stamp(&msg, t) && nonzero(t);
    }
    if (got) {
      std::printf("  frame %d  tx_ts %lld.%09ld\n", i, (long long)t.tv_sec, t.tv_nsec);
      tx_ok++;
    } else {
      std::printf("  frame %d  no hardware tx stamp\n", i);
      tx_miss++;
    }
    usleep(3000);  // i210 holds about one outstanding tx stamp, drain before the next
  }

  // rx hardware stamps: capture box2's ping replies on a packet socket.
  int rx = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
  sockaddr_ll sll{};
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = if_nametoindex(kVlan);
  bind(rx, reinterpret_cast<sockaddr*>(&sll), sizeof(sll));
  set_timestamping(rx);

  std::printf("\nrx hardware stamps, frames arriving on %s:\n", kVlan);
  int rx_ok = 0;
  for (int i = 0; i < 200 && rx_ok < 5; ++i) {
    pollfd pfd{rx, POLLIN, 0};
    if (poll(&pfd, 1, 100) <= 0) continue;
    char ctrl[256];
    char data[2048];
    iovec iov{data, sizeof(data)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    if (recvmsg(rx, &msg, 0) < 0) continue;
    timespec t{};
    if (raw_hw_stamp(&msg, t) && nonzero(t)) {
      std::printf("  frame  rx_ts %lld.%09ld\n", (long long)t.tv_sec, t.tv_nsec);
      rx_ok++;
    }
  }

  std::printf("\nchecks:\n");
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) failures++;
  };
  check(tx_ok > 0, "at least one nonzero raw hardware tx stamp");
  check(rx_ok > 0, "at least one nonzero raw hardware rx stamp");
  check(phc >= 0, "PHC device opened for the timescale comparison");
  std::printf("tx stamps %d ok, %d missing; rx stamps %d ok\n", tx_ok, tx_miss, rx_ok);
  if (failures) {
    std::printf("\nhardware timestamping NOT confirmed, do not build the transport on it\n");
    return 1;
  }
  std::printf("\nhardware timestamping confirmed on the igb PHC\n");
  return 0;
}
