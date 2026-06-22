#!/usr/bin/env bash
# Builds and runs the closed loop, writing results/ and the CDFs.
set -euo pipefail

cd "$(dirname "$0")/.."

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

echo
echo "host: $(uname -srm)"
echo

# The native AF_XDP backend binds to a veth pair. Bring it up if it is missing.
# Needs root, same as the run. The portable backend ignores this.
if [[ "$(uname -srm)" == Linux*x86_64 ]] && ! ip link show calib1 >/dev/null 2>&1; then
  echo "setting up the calib0/calib1 veth pair for AF_XDP"
  sudo "$(dirname "$0")/setup_veth.sh"
  echo
fi

# AF_XDP socket creation, mlockall, core pinning, and the PMU group need root.
if [[ "$(uname -srm)" == Linux*x86_64 ]]; then
  sudo ./build/caliper
else
  ./build/caliper
fi
