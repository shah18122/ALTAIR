#!/usr/bin/env bash
# ops/linux/nic_swap.sh -- move SSH/DCV to the spare card and give the MAIN card to DPDK for the feed.
#
# Two steps, each on a 10-minute dead-man: unless the next step (or `confirm`)
# runs in time, `revert` puts every file back and REBOOTS into the old layout.
# A reboot in the middle reverts too (altair-nic-swap-guard.service).
#
#   BEFORE (EC2 console, Smit): snapshot the root volume; associate an Elastic
#   IP with the SPARE ENI (172.31.34.83); enable the EC2 serial console.
#
#   sudo ops/linux/nic_swap.sh status
#   sudo ops/linux/nic_swap.sh step1 --go   spare back to the kernel, DHCP, preferred
#                                           default route. Then open a NEW SSH to the
#                                           spare's Elastic IP and, within 10 minutes:
#   sudo ops/linux/nic_swap.sh step2 --go   (refused unless this SSH arrives on the spare)
#                                           main card to vfio-pci; the recorder's feed set
#                                           to DPDK on it. Check, then within 10 minutes:
#   sudo ops/linux/nic_swap.sh confirm      keep it
#   sudo ops/linux/nic_swap.sh revert       put it all back and reboot (also the dead-man)
#
# Never during market hours (09:00-15:47 IST) and never while the recorder runs.
# Orders keep the kernel line (kept alive) on the spare: one DPDK card serves one
# process, and the main card's goes to the feed.

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
self="$here/$(basename "$0")"
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
state=/etc/altair/nic_swap
pending="$state/pending"
main_if=enp39s0
spare_mac="${ALTAIR_SPARE_MAC:-02:ff:f4:e3:1f:67}"
spare_pci="${ALTAIR_SPARE_PCI:-0000:28:00.0}"
spare_ip="${ALTAIR_SPARE_IP:-172.31.34.83}"   # the spare ENI's private address (EC2 console, Network interfaces)
deadman=10min
mkdir -p "$state"

say() { echo "[nic_swap] $*"; logger -t altair-nic-swap "$*" 2>/dev/null || true; }
ist_minutes() { TZ=Asia/Kolkata date +%H%M | sed 's/^0*//'; }
market_guard() {
    local m; m="$(ist_minutes)"; m="${m:-0}"
    local dow; dow="$(TZ=Asia/Kolkata date +%u)"
    if [ "$dow" -le 5 ] && [ "$m" -ge 900 ] && [ "$m" -lt 1547 ] && [ "${NIC_SWAP_ANY_TIME:-0}" != 1 ]; then
        echo "refused: market hours (09:00-15:47 IST). Run it after 15:47." >&2; exit 3
    fi
    if systemctl is-active --quiet altair-recorder.service; then
        echo "refused: the recorder is running (systemctl stop altair-recorder.service first)" >&2; exit 3
    fi
}
arm() {   # arm <what>: the dead-man and the boot guard
    echo "$1" > "$pending"
    systemctl stop altair-nic-revert.timer altair-nic-revert.service 2>/dev/null || true
    systemd-run --quiet --unit=altair-nic-revert --on-active="$deadman" "$self" revert
    cat > /etc/systemd/system/altair-nic-swap-guard.service <<EOF
[Unit]
Description=ALTAIR: an unconfirmed NIC swap is reverted at boot (ops/linux/nic_swap.sh)
ConditionPathExists=$pending
After=local-fs.target
Before=network-pre.target
Wants=network-pre.target

[Service]
Type=oneshot
ExecStart=$self revert --no-reboot

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --quiet altair-nic-swap-guard.service
    say "armed ($1): reverts in $deadman unless the next step or 'confirm' runs"
}
pci_of() { basename "$(readlink -f "/sys/class/net/$1/device")"; }
iface_of_mac() { for n in /sys/class/net/*; do [ "$(cat "$n/address" 2>/dev/null)" = "$1" ] && basename "$n"; done; }

case "${1:-status}" in
status)
    dpdk-devbind.py --status-dev net 2>/dev/null || true
    ip -br addr; ip route show default
    echo "SSH arrives on: $(echo "${SSH_CONNECTION:-none}" | awk '{print $3}')"
    [ -f "$pending" ] && echo "PENDING: $(cat "$pending") (reverts unless confirmed)" || echo "nothing pending"
    ;;
step1)
    [ "${2:-}" = --go ] || { echo "dry run: would give the spare ($spare_pci) back to the kernel with DHCP and the preferred default route"; exit 0; }
    market_guard
    [ -f "$pending" ] && { echo "refused: a swap is pending ($(cat "$pending")); confirm or revert first" >&2; exit 3; }
    main_mac="$(cat /sys/class/net/$main_if/address)"
    main_ip="$(ip -4 -br addr show dev $main_if | awk '{print $3}' | cut -d/ -f1)"
    gw="$(ip route show default | awk '/default/ {print $3; exit}')"
    main_pci="$(pci_of $main_if)"
    [ -n "$main_ip" ] && [ -n "$gw" ] || { echo "refused: $main_if has no address or there is no default route" >&2; exit 3; }
    backup="$state/backup-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$backup"
    cp -a /etc/netplan "$backup/netplan"
    [ -d /etc/NetworkManager/conf.d ] && cp -a /etc/NetworkManager/conf.d "$backup/nm-conf.d"
    cp -a /etc/altair/dpdk.conf "$backup/" 2>/dev/null || true
    cp -a /etc/altair/altair.env "$backup/" 2>/dev/null || true
    printf 'MAIN_MAC=%s\nMAIN_IP=%s\nMAIN_PCI=%s\nGW=%s\nBACKUP=%s\n' "$main_mac" "$main_ip" "$main_pci" "$gw" "$backup" > "$state/layout"
    arm step1
    # The spare: off DPDK, back to the kernel, DHCP with the lower metric.
    systemctl disable --quiet altair-dpdk-bind.service 2>/dev/null || true
    rm -f /etc/altair/dpdk.conf /etc/netplan/90-altair-spare.yaml /etc/NetworkManager/conf.d/99-altair-spare.conf
    dpdk-devbind.py --bind=ena "$spare_pci"
    sleep 2
    # Static, with its own routing table for traffic FROM its address: SSH that
    # arrives on the spare is answered through the spare, while the main card
    # keeps the default route and this session. The spare's own default route
    # (metric 300) becomes the only one once the main card leaves in step 2.
    # Amazon's DNS (169.254.169.253) answers on any card.
    prefix="$(ip -4 -br addr show dev $main_if | awk '{print $3}' | cut -d/ -f2)"
    umask 077
    cat > /etc/netplan/60-altair-spare-kernel.yaml <<NETPLAN
network:
  version: 2
  ethernets:
    altair-spare:
      match:
        macaddress: "$spare_mac"
      dhcp4: false
      addresses: [$spare_ip/$prefix]
      nameservers:
        addresses: [169.254.169.253]
      routes:
        - to: default
          via: $gw
          metric: 300
        - to: default
          via: $gw
          table: 101
      routing-policy:
        - from: $spare_ip
          table: 101
NETPLAN
    systemctl reload NetworkManager 2>/dev/null || true
    netplan apply
    sleep 5
    sp="$(iface_of_mac "$spare_mac")"
    say "spare is $sp: $(ip -4 -br addr show dev "$sp" 2>/dev/null | awk '{print $3}'); default route: $(ip route show default | head -1)"
    echo
    echo "NOW: open a NEW SSH to the spare's Elastic IP (ssh -i altair.pem -p 443 ubuntu@<EIP>) and run,"
    echo "within $deadman:  sudo $self step2 --go    (this session may drop; that is fine)"
    ;;
step2)
    [ "${2:-}" = --go ] || { echo "dry run: would give $main_if to vfio-pci and point the recorder's feed at DPDK on it"; exit 0; }
    market_guard
    [ "$(cat "$pending" 2>/dev/null)" = step1 ] || { echo "refused: run step1 first" >&2; exit 3; }
    # shellcheck disable=SC1091
    . "$state/layout"
    sp="$(iface_of_mac "$spare_mac")"
    sp_ip="$(ip -4 -br addr show dev "$sp" | awk '{print $3}' | cut -d/ -f1)"
    ssh_on="$(echo "${SSH_CONNECTION:-}" | awk '{print $3}')"
    [ -n "$sp_ip" ] && [ "$ssh_on" = "$sp_ip" ] || { echo "refused: this SSH must arrive on the spare ($sp_ip), not '${ssh_on:-none}'" >&2; exit 3; }
    arm step2
    # The main card: no kernel config, then vfio-pci, and bound again at every boot.
    printf 'network:\n  version: 2\n  ethernets:\n    altair-main-dpdk:\n      match:\n        macaddress: "%s"\n      dhcp4: false\n      dhcp6: false\n      optional: true\n' "$MAIN_MAC" \
        > /etc/netplan/90-altair-main-dpdk.yaml
    chmod 600 /etc/netplan/90-altair-main-dpdk.yaml
    if [ -d /etc/NetworkManager/conf.d ]; then
        printf '[keyfile]\nunmanaged-devices=mac:%s\n' "$MAIN_MAC" > /etc/NetworkManager/conf.d/99-altair-main-dpdk.conf
        systemctl reload NetworkManager 2>/dev/null || true
    fi
    # The kernel lets go of the main card first: its address and default route go,
    # the spare's default route (metric 300) is left, and this SSH stays on the spare.
    netplan apply
    sleep 3
    ip route show default | grep -q "dev $main_if" && ip route del default dev "$main_if" 2>/dev/null || true
    printf 'DPDK_IFACE=%s\nDPDK_ALLOW_PRIMARY=1\n' "$main_if" > /etc/altair/dpdk.conf
    systemctl enable --quiet altair-dpdk-bind.service
    DPDK_ALLOW_PRIMARY=1 "$here/dpdk_bind.sh" "$main_if"
    touch /etc/altair/altair.env
    sed -i '/^ALTAIR_FEED_BYPASS/d;/^ALTAIR_FEED_DPDK/d' /etc/altair/altair.env
    printf 'ALTAIR_FEED_DPDK_PORT=%s\nALTAIR_FEED_DPDK_IP=%s\nALTAIR_FEED_DPDK_GW=%s\n' "$MAIN_PCI" "$MAIN_IP" "$GW" >> /etc/altair/altair.env
    say "main card $MAIN_PCI is on DPDK; the feed uses it ($MAIN_IP via $GW) from the next recorder run"
    echo
    echo "CHECK now (still within $deadman): DCV over the new IP, then the feed on DPDK:"
    echo "  timeout 60 build/linux/app/altair_price_service --live --go --seconds 40 --dpdk-port $MAIN_PCI --dpdk-ip $MAIN_IP --dpdk-gw $GW"
    echo "  (as root; look for 'DPDK feed on $MAIN_PCI: TCP .. ms, TLS .. ms, N messages')"
    echo "Then: sudo $self confirm      (or: sudo $self revert)"
    ;;
confirm)
    [ -f "$pending" ] || { echo "nothing pending"; exit 0; }
    systemctl stop altair-nic-revert.timer altair-nic-revert.service 2>/dev/null || true
    systemctl disable --quiet altair-nic-swap-guard.service 2>/dev/null || true
    rm -f /etc/systemd/system/altair-nic-swap-guard.service "$pending"
    systemctl daemon-reload
    say "confirmed: the new layout stays (the backup is kept under $state)"
    ;;
revert)
    [ -f "$state/layout" ] || { echo "nothing to revert"; rm -f "$pending"; exit 0; }
    # shellcheck disable=SC1091
    . "$state/layout"
    say "reverting to $BACKUP"
    rm -f /etc/netplan/60-altair-spare-kernel.yaml /etc/netplan/90-altair-main-dpdk.yaml /etc/NetworkManager/conf.d/99-altair-main-dpdk.conf
    cp -a "$BACKUP/netplan/." /etc/netplan/
    [ -d "$BACKUP/nm-conf.d" ] && cp -a "$BACKUP/nm-conf.d/." /etc/NetworkManager/conf.d/
    if [ -f "$BACKUP/dpdk.conf" ]; then cp -a "$BACKUP/dpdk.conf" /etc/altair/dpdk.conf; systemctl enable --quiet altair-dpdk-bind.service; else rm -f /etc/altair/dpdk.conf; fi
    [ -f "$BACKUP/altair.env" ] && cp -a "$BACKUP/altair.env" /etc/altair/altair.env
    rm -f "$pending"
    systemctl disable --quiet altair-nic-swap-guard.service 2>/dev/null || true
    rm -f /etc/systemd/system/altair-nic-swap-guard.service
    systemctl daemon-reload
    if [ "${2:-}" = --no-reboot ]; then
        # At boot, before the network: the main card is still with ena (DPDK binds later,
        # and dpdk.conf no longer names it); the restored files take effect now.
        say "reverted at boot; the network comes up in the old layout"
    else
        say "reverted; rebooting into the old layout"
        systemctl reboot
    fi
    ;;
*)
    sed -n '2,24p' "$0"; exit 2 ;;
esac
