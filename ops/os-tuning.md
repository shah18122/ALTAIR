# EC2 server: trim, boot order, S3 mirror, OS tuning (2026-11-12)

Everything below is dry-run by default, refuses market hours where it matters,
and is undone by its own `--undo`. Run after 15:50 IST.

## Deploy (once, after the close)

    cd ~/ALTAIR && git pull --ff-only
    sudo ops/linux/host_trim.sh --go           # remove/mask unneeded packages + one-time config
    sudo ops/linux/s3_sync.sh --install-cli    # AWS CLI v2 (skip if `aws` exists)
    sudo install -m 600 /dev/null /etc/altair/s3.env && echo ALTAIR_S3_BUCKET=<bucket> | sudo tee /etc/altair/s3.env
    sudo ops/tune_host.sh --go                 # GRUB isolation, sysctl, limits, irqbalance
    sudo ops/install.sh --go                   # every unit, boot order, timers
    sudo reboot                                # GRUB part; then read data/logs/boot-*.txt

## What changes

| Area | Script | What |
|---|---|---|
| Packages | `ops/linux/host_trim.sh` | purge crash reporters, telemetry, modem/SAN/iSCSI, fwupd, packagekit, landscape, lxd snap; mask cups, avahi, bluetooth, ModemManager, tracker. Kept: snapd (SSM agent), cloud-init, DCV + GNOME, chrony, irqbalance. A purge that would drag anything else out is skipped. |
| One-time config | same | apt/unattended-upgrades only Saturday; needrestart never restarts services; journal 2 GB; chrony on Amazon Time Sync 169.254.169.123; chrony-wait so `time-sync.target` means synced; snap refresh Sat 02:00-04:00; motd/apt news off |
| Boot order | `ops/install.sh` + units | network-online, chrony-wait, nic-tune, dpdk-bind, callback, webhook; then login, recorder (boot+90 s), engine (boot+2 min, only with /etc/altair/engine.conf), cpuset, boot check (boot+3 min), S3 (boot+5 min) |
| S3 | `ops/linux/s3_sync.sh`, `altair-s3-sync.timer` | every 5 min. In the session: nice 19, idle I/O, housekeeping cores, 8 MB/s cap, today's tapes held back. After hours: everything at full speed. Upload only, never deletes; secrets never sent. The altair-data git pushes (15:47, 20:00) stay as the small versioned research copy. |
| Cores | `ops/tune_host.sh`, `ops/linux/cpuset.sh` | cores 2-7: no tick (nohz_full), no RCU callbacks, no managed IRQs. 08:45-15:50: every system service held on cores 0-1; `altair.slice` (recorder, engine, webhook) keeps all. 15:50: every core open, so the 20:00 retrain uses all 8. `--static-isolation` gives the old fixed isolation instead. |
| NIC | `ops/linux/nic_tune.sh` | at boot: interrupt moderation off on the feed NIC, its queue IRQs on cores 0-1, RPS off. No link reset. |
| Kernel net | `ops/tune_host.sh` sysctl | busy poll 50 us, no TCP autocorking, no slow start after idle (order line), small notsent queue, 16 MB socket buffers, larger backlog |
| Memory | `ops/tune_host.sh` | mlockall for ALTAIR services, 512 x 2 MB hugepages, THP madvise, swappiness 1, no NUMA balancing, `altair.slice` MemoryLow 4 G. The rest of RAM stays free for the retrain and page cache (reserving more hugepages would take it away). |

Deeper tier (`ops/tune_host.sh --deep`: idle=poll, no watchdogs, timer pinning)
is separate and needs an EBS snapshot first.

## Undo

    sudo ops/linux/host_trim.sh --undo --go
    sudo ops/tune_host.sh --undo --go && sudo reboot
    sudo ops/linux/cpuset.sh open
    sudo ops/install.sh --remove --go && sudo ops/install.sh --go   # old units back from git
