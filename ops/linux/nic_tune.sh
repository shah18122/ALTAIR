#!/usr/bin/env bash
# ops/linux/nic_tune.sh -- the feed NIC at boot, before the recorder starts.
#
#   - interrupt moderation off (rx-usecs 0, adaptive off): a packet raises its
#     interrupt at once instead of after the ENA's coalescing window (up to ~20-60 us)
#   - every queue interrupt of IFACE on the housekeeping cores, so softirq work
#     never lands on a core an ALTAIR thread spins on
#   - RPS off on its queues (the feed's softirq stays on the IRQ's core)
#
# Nothing here resets the link (no ring or channel change), so it is safe at
# any time, but altair-nic-tune.service only runs it at boot.
#
#   sudo ops/linux/nic_tune.sh [IFACE] [--housekeeping 0-1]   (IFACE: the default route's)

set -euo pipefail
ifc="${1:-$(ip -o route show default | awk '{print $5; exit}')}"
hk="0-1"; [ "${2:-}" = "--housekeeping" ] && hk="${3:?}"
[ -e "/sys/class/net/$ifc" ] || { echo "no interface $ifc" >&2; exit 2; }

if command -v ethtool >/dev/null; then
    ethtool -C "$ifc" adaptive-rx off 2>/dev/null || true
    ethtool -C "$ifc" rx-usecs 0 2>/dev/null || true
    ethtool -C "$ifc" tx-usecs 0 2>/dev/null || true
    echo "$ifc coalescing: $(ethtool -c "$ifc" 2>/dev/null | awk '/^Adaptive|^rx-usecs:|^tx-usecs:/{printf "%s ", $0}')"
fi
n=0
for irq in $(awk -v i="$ifc" '$0 ~ i {sub(":","",$1); print $1}' /proc/interrupts); do
    echo "$hk" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null && n=$((n + 1))
done
echo "$ifc: $n queue interrupts on cores $hk"
for r in /sys/class/net/"$ifc"/queues/rx-*/rps_cpus; do [ -w "$r" ] && echo 0 > "$r"; done
