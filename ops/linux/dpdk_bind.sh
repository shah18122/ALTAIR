#!/usr/bin/env bash
# ops/linux/dpdk_bind.sh -- hand a SECOND ENI to DPDK (vfio-pci), or give it back.
#
# Attach a second network interface to the instance first (EC2 console:
# Network Interfaces -> Create, same subnet -> Attach). It appears as a new
# ENA device, e.g. ens6. This script refuses any interface that carries the
# default route, an SSH session or the first ENI, so it can never cut the
# server off.
#
#   sudo ops/linux/dpdk_bind.sh               list the ENA devices and what owns them
#   sudo ops/linux/dpdk_bind.sh ens6          bind ens6 to vfio-pci for DPDK
#   sudo ops/linux/dpdk_bind.sh --undo PCI    give the device back to the kernel (ena)
#   sudo ops/linux/dpdk_bind.sh --at-boot ens6   bind it now and after every boot
#                                                (altair-dpdk-bind.service)
#
# Then: altair_dpdk_tls_client --eal "-a <PCI> --in-memory" --port <PCI> --local-ip <the ENI's address> ...
# EC2 guests have no IOMMU, so vfio runs in no-IOMMU mode (the AWS-documented
# way to run DPDK on ENA without igb_uio).

set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
command -v dpdk-devbind.py >/dev/null || { echo "install dpdk first (setup.sh installs it)" >&2; exit 2; }

if [ $# -eq 0 ]; then
    dpdk-devbind.py --status-dev net
    exit 0
fi

if [ "$1" = --at-boot ]; then
    [ $# -eq 2 ] || { echo "usage: dpdk_bind.sh --at-boot IFACE" >&2; exit 2; }
    mkdir -p /etc/altair
    printf 'DPDK_IFACE=%s\n' "$2" > /etc/altair/dpdk.conf
    systemctl enable altair-dpdk-bind.service
    # Keep the spare off the kernel's network config at boot: on 2026-11-04 its
    # DHCP took the default route before the bind ran and cut SSH off. Matched by
    # MAC (the name can change); written once, never over a hand-made file.
    if [ -r "/sys/class/net/$2/address" ]; then
        mac="$(cat "/sys/class/net/$2/address")"
        umask 077
        if [ -d /etc/netplan ] && [ ! -e /etc/netplan/90-altair-spare.yaml ]; then
            printf 'network:\n  version: 2\n  ethernets:\n    altair-spare:\n      match:\n        macaddress: "%s"\n      dhcp4: false\n      dhcp6: false\n      optional: true\n' "$mac" \
                > /etc/netplan/90-altair-spare.yaml
            echo "wrote /etc/netplan/90-altair-spare.yaml (no DHCP on $mac)"
        fi
        if [ -d /etc/NetworkManager/conf.d ] && [ ! -e /etc/NetworkManager/conf.d/99-altair-spare.conf ]; then
            printf '[keyfile]\nunmanaged-devices=mac:%s\n' "$mac" > /etc/NetworkManager/conf.d/99-altair-spare.conf
            echo "wrote /etc/NetworkManager/conf.d/99-altair-spare.conf (NetworkManager leaves $mac alone)"
        fi
    fi
    if [ -e "/sys/class/net/$2" ]; then exec "$0" "$2"; fi
    echo "$2 is already bound (or absent); it is bound again at every boot"
    exit 0
fi

if [ "$1" = --undo ]; then
    [ $# -eq 2 ] || { echo "usage: dpdk_bind.sh --undo PCI" >&2; exit 2; }
    dpdk-devbind.py --bind=ena "$2"
    echo "$2 is back with the kernel's ena driver"
    exit 0
fi

ifc="$1"
if [ ! -e "/sys/class/net/$ifc/device" ]; then
    echo "no network device $ifc (already bound to DPDK, or not attached)"
    exit 0
fi
pci="$(basename "$(readlink -f "/sys/class/net/$ifc/device")")"
default_if="$(ip route show default | awk '{for (i=1;i<NF;i++) if ($i=="dev") print $(i+1)}' | head -1)"
first_if="$(ls -1 /sys/class/net | grep -v '^lo$' | while read -r n; do [ -e "/sys/class/net/$n/device" ] && echo "$n"; done | sort | head -1)"
# DPDK_ALLOW_PRIMARY=1 (/etc/altair/dpdk.conf, written by ops/linux/nic_swap.sh
# after SSH and DCV have moved to the spare): the first ENI may go to DPDK, but
# never while it carries the default route or this SSH session.
if [ "${DPDK_ALLOW_PRIMARY:-0}" = 1 ] && [ "$ifc" = "$first_if" ] && [ "$ifc" != "$default_if" ]; then
    first_if=""
fi
if [ "$ifc" = "$default_if" ] || [ "$ifc" = "$first_if" ]; then
    echo "refused: $ifc carries the default route or is the first ENI (SSH, DCV and the recorder use it)" >&2
    exit 3
fi
addr="$(ip -4 -br addr show dev "$ifc" | awk '{print $3}')"
if [ -n "${SSH_CONNECTION:-}" ] && [ -n "$addr" ] && [ "${addr%/*}" = "$(echo "$SSH_CONNECTION" | awk '{print $3}')" ]; then
    echo "refused: this SSH session arrives on $ifc" >&2
    exit 3
fi
echo "$ifc ($pci) address ${addr:-none}: keep it -- ALTAIR uses it as --local-ip, with ARP its own"
modprobe vfio-pci
if [ -z "$(ls /sys/kernel/iommu_groups 2>/dev/null)" ]; then
    echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
fi
ip link set "$ifc" down
dpdk-devbind.py --bind=vfio-pci "$pci"
# The recorder runs DPDK as its own user: give it the vfio device nodes.
svc_user="${DPDK_USER:-$(stat -c %U "$(dirname "$0")")}"
for d in /dev/vfio/*; do [ -e "$d" ] && chown "$svc_user" "$d" 2>/dev/null || true; done
dpdk-devbind.py --status-dev net | grep -F "$pci"
echo "bound: use --eal \"-a $pci --in-memory\" --port $pci"
