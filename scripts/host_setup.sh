#!/usr/bin/env bash
# Applies the runtime host knobs for a caliper run and prints the boot cmdline
# it expects. Run as root on the x86_64 Linux server. Has no effect on the M1
# development host, where it just prints what the server needs.
#
# Tuned for the AMD Zen host caliper runs on: hot core 2, its SMT sibling 8.
# isolcpus, nohz_full, and rcu_nocbs are boot cmdline only and need a reboot,
# so this script prints them but does not apply them. Everything below is
# applied live without a reboot.
set -euo pipefail

HOT_CORE=${HOT_CORE:-2}
SIBLING=${SIBLING:-8}  # SMT sibling of core 2 on this 6c/12t part

uname_m=$(uname -m)
if [[ "$uname_m" != "x86_64" ]]; then
  echo "host is $uname_m, not x86_64. caliper headline numbers need bare metal x86_64 Linux."
  echo "this script only applies runtime knobs on the server. nothing to do here."
fi

echo "expected boot cmdline (set in the bootloader, reboot to apply):"
echo "  isolcpus=${HOT_CORE} nohz_full=${HOT_CORE} rcu_nocbs=${HOT_CORE}"
echo "not applied here: these need a reboot. the run documents whether they were in effect."
echo

if [[ "$uname_m" != "x86_64" || "$(uname -s)" != "Linux" ]]; then
  exit 0
fi

echo "setting governor to performance"
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [[ -w "$g" ]] && echo performance > "$g" || true
done

echo "disabling boost (default run targets predictable, not fastest)"
if [[ -w /sys/devices/system/cpu/cpufreq/boost ]]; then
  echo 0 > /sys/devices/system/cpu/cpufreq/boost            # AMD/acpi-cpufreq global
elif [[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo    # intel_pstate
fi

echo "offlining SMT sibling ${SIBLING} so core ${HOT_CORE} is not shared"
if [[ -w /sys/devices/system/cpu/cpu${SIBLING}/online ]]; then
  echo 0 > /sys/devices/system/cpu/cpu${SIBLING}/online || true
fi

echo "moving IRQ affinity off core ${HOT_CORE}"
mask=$(printf '%x' $(( ~(1 << HOT_CORE) & 0xfff )))
for irq in /proc/irq/*/smp_affinity; do
  echo "$mask" > "$irq" 2>/dev/null || true
done

echo "reserving huge pages"
echo 512 > /proc/sys/vm/nr_hugepages 2>/dev/null || true

echo "lowering perf_event_paranoid for the PMU group"
echo 0 > /proc/sys/kernel/perf_event_paranoid 2>/dev/null || true

echo "C states: the harness holds /dev/cpu_dma_latency at 0 us for its lifetime,"
echo "which keeps the hot core out of deep sleep without a reboot."

echo "done. record the host configuration alongside the run."
