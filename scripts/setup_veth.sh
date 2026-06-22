#!/usr/bin/env bash
# Brings up the calib0/calib1 veth pair the native AF_XDP backend binds to.
# A single box has one physical port and it is the host lifeline, so the
# closed loop runs over a veth pair instead. The sender injects frames into
# calib0, the xsk binds calib1. Run as root before make bench. Does not touch
# the physical NIC.
set -euo pipefail

SEND_IF=${SEND_IF:-calib0}
RECV_IF=${RECV_IF:-calib1}

if ip link show "$SEND_IF" >/dev/null 2>&1; then
  echo "$SEND_IF already exists, deleting"
  ip link del "$SEND_IF"
fi

echo "creating veth pair $SEND_IF <-> $RECV_IF"
ip link add "$SEND_IF" type veth peer name "$RECV_IF"
ip link set "$SEND_IF" address 02:00:00:00:55:01
ip link set "$RECV_IF" address 02:00:00:00:55:02

# Keep the kernel stack from touching these. The xsk owns calib1 ingress.
sysctl -q -w "net.ipv6.conf.${SEND_IF}.disable_ipv6=1" || true
sysctl -q -w "net.ipv6.conf.${RECV_IF}.disable_ipv6=1" || true

ip link set "$SEND_IF" up
ip link set "$RECV_IF" up

echo "up:"
ip -br link show "$SEND_IF"
ip -br link show "$RECV_IF"
