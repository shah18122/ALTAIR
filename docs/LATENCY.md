# Latency: where the time goes, and what is done about it

Updated 2026-11-05. The target asked for was 100 µs for both data and order
punching. This page sets out what can and cannot get there, what has been done,
and what is next. Every number is measured, and says where it was measured.

## The honest floor

| Hop | Today | Can we cut it? |
|---|---|---|
| Wire: EC2 Mumbai to FYERS (Cloudflare edge), one round trip | **1.4 to 1.55 ms** (TCP connect, `altair_latency_probe`, 2026-11-04) | Not from EC2. It is the physical path to FYERS. |
| FYERS' own order handling (their OMS and RMS, then the exchange) | unknown, likely several ms | No. It belongs to FYERS. |
| FYERS exchange timestamps | **1 second** resolution (`last_traded_time`, `exch_feed_time`) | They cannot measure microseconds. The per-hop clocks below are ours. |
| Inside our server, tick in to decision | was up to ~1 ms, now **tens of µs** | Yes. This is where the work below went. |

**100 µs end to end is not possible over a retail broker's internet API.** It
needs the server in NSE's colocation (or a broker's co-lo rack with DMA), the
exchange's own feed (TBT multicast) and a direct order gateway. There,
tick-to-trade is 5 to 50 µs. FYERS offers none of this to retail. That is a
change of broker, membership and budget (lakhs a month), not code.

What code can do is make **our** part of the path near zero, so that the wire
is all that is left.

## What was found and fixed (2026-11-05)

| # | Delay found | Fix | Measured |
|---|---|---|---|
| 1 | Every order opened a **new TCP + TLS connection** (trust store load, DNS, connect, handshake), about 8 ms before the order's first byte left. | `broker/https_keepalive.hpp`: one kept, pre-warmed connection for orders and cancels, and another for reads. The order line never queues behind the 2-second order-book poll. It is kept warm with a profile read when idle for 20 s. A dead connection is replaced **before** a byte is written. Once a request is written, a failure is never retried and stays "uncertain". | ~8 ms off every order (connect + TLS from the probe) |
| 2 | The price bus's owner thread **slept 1 ms** whenever its rings were empty, so a tick in a quiet moment waited up to 1 ms. | `SharedBus(..., spin)`: with `spin = true` (config/latency.toml) it busy-polls on its isolated core. | lone tick, ring to subscriber socket: **836 µs to 4.6 µs** (p50, `altair_shared_bus_test`, cloud VM) |
| 3 | The order router looked for new orders **every 5 ms** (2.5 ms on average). | inotify on the request files: it wakes as an order is written. The 5 ms pass stays as the floor. | ~2.5 ms off the average order |
| 4 | **Delayed ACKs.** A server that writes a reply in pieces waits for our ACK, and Linux delays it by up to 40 ms. | `TCP_QUICKACK` on the order line during a request and on the FYERS feed socket after every frame. `TCP_NODELAY` on both. | test reply **44 ms to 0.13 ms** (`altair_https_keepalive_test`) |
| 5 | No per-hop numbers. | `feed_status.json` gets `bus_hop` (p50/p99/max). The engine's metrics get `bus_to_engine_us`. `data/live_orders/latency.json` gets the order line's round trip and connect times. | from the next session |

## The DPDK order line (built 2026-11-05)

`net/bypass_https.hpp`: kept-alive HTTPS/1.1 on ALTAIR's own TCP
(`net/user_tcp.hpp`) and TLS (`net/tls_stream.hpp`) over a DPDK port
(`net/links.hpp`). It answers ARP and resolves the gateway itself, so the
kernel is nowhere on the order's path. The router uses it in front of the
kernel line. A request that never left (no ARP, no SYN-ACK, no TLS) goes over
the kernel line instead. A request that was written and got no reply is
uncertain and is never resent.

- Tested end to end: `sudo ops/linux/bypass_https_netns_test.sh build/linux`
  (two namespaces, a veth, a Python HTTPS server, 20 requests on one
  connection, Content-Length and chunked replies, a server close, a reconnect).
- Expected gain over the tuned kernel line: **10 to 30 µs** a request (no
  syscalls, no kernel TCP, no interrupt and wake-up).
- Turned on by `/etc/altair/order_line.conf` (the Terminal starts the router
  without flags):

  ```
  DPDK_PORT=0000:28:00.0
  DPDK_IP=172.31.34.83
  DPDK_GW=172.31.32.1
  ```

  or `altair_order_router --dpdk-port ... --dpdk-ip ... --dpdk-gw ...`.

**It needs a public IP on the DPDK card.** A secondary ENI gets no public IP
by itself, and without one the SYN goes nowhere (the router then says "no
SYN-ACK: is there a public IP on this card?" and uses the kernel line).

### DPDK on the main card (Smit's choice, 2026-11-05): `ops/linux/nic_swap.sh`

Every ENI goes through the same Nitro card, so the main card is not faster
than the spare. Smit still chose the swap: the kernel (SSH, DCV, the order
line) moves to the spare (enp40s0, 172.31.34.83), and the main card (enp39s0)
goes to DPDK for the feed. One DPDK port serves one process, so the feed takes
it and orders stay on the kept-alive kernel line on the spare.

Before (EC2 console): snapshot the root volume, enable the serial console,
and associate an Elastic IP with the spare ENI. Then, on the server after
15:47 IST with the recorder stopped:

1. `sudo ops/linux/nic_swap.sh step1 --go`: puts the spare back on the kernel
   with its own routing table, so SSH to its Elastic IP works. A 10-minute
   dead-man revert is armed.
2. Open a **new** SSH to the spare's Elastic IP and run
   `sudo ops/linux/nic_swap.sh step2 --go`: binds enp39s0 to DPDK, writes
   `ALTAIR_FEED_DPDK_*` into altair.env.
3. Check SSH and DCV, then `sudo ops/linux/nic_swap.sh confirm`. Without it,
   everything is reverted and the box reboots after 10 minutes, and at any
   boot while the swap is unconfirmed.

`sudo ops/linux/nic_swap.sh revert` undoes it at any time. It refuses to run
09:00-15:47 IST on weekdays and while the recorder is active.

### EC2 steps (Smit, after 15:47 IST; nothing here has been done)

1. **Snapshot first.** EC2, Volumes, the root volume, Create snapshot.
2. **Elastic IP for the order card.** EC2, Elastic IPs, Allocate. Then
   Actions, Associate, Network interface = the spare ENI (172.31.34.83). This
   is also the static IP SEBI requires for API orders from 2026-04-01:
   register it with FYERS.
3. **Elastic IP on the main card too.** An instance with more than one ENI
   may not get its auto-assigned public IP back after a stop and start. An
   Elastic IP on enp39s0 keeps the SSH address fixed. Update the SSH command
   if it changes.
4. (For the feed on DPDK, when chosen) **a third ENI**: EC2, Network
   interfaces, Create (same subnet, same security group), Attach to the
   instance, then an Elastic IP on it. On the server,
   `sudo ops/linux/dpdk_bind.sh <its PCI address> <iface>`.
5. On the server: write `/etc/altair/order_line.conf` (above), rebuild
   (`ops/build.sh`), restart the router, and check
   `data/live_orders/latency.json` for an `orders_dpdk` block with
   `connects: 1`.

Rollback: delete `/etc/altair/order_line.conf` and restart the router. The
kernel line takes over. Elastic IPs cost about $3.60 a month each while
attached.

## OS level: what is reachable on EC2

On EC2 the BIOS, the motherboard and the hypervisor belong to AWS (Nitro).
Nothing below the guest kernel can be touched. Everything the guest can do is
in `ops/tune_host.sh`:

- **Done before** (`--go`): isolated cores 2-7 (`isolcpus`, `nohz_full`,
  `rcu_nocbs`), hugepages, busy polling, irqbalance kept off the isolated
  cores, memlock and rtprio limits.
- **`--deep`** (new, opt-in, needs a reboot): `idle=poll` (a vCPU never
  halts, so there is no VM exit and wake-up on an event: tens of µs off every
  wake), `irqaffinity` and `kthread_cpus` on the housekeeping cores, no
  watchdogs, `tsc=reliable`, `skew_tick=1`, `rcu_nocb_poll`, `preempt=full`,
  `audit=0`, plus timer migration off and smaller dirty-page bursts.
- **`--mitigations-off`**: faster syscalls, but drops the CPU's protection
  between processes on this VM. Only on a box that runs nothing but ALTAIR.
- **`--trim`**: stops snapd, unattended-upgrades, apport, ModemManager and
  similar. **This also stops automatic security updates.** Run `apt upgrade`
  by hand on weekends.
- **`--headless`**: boots to multi-user (no display manager). It refuses if a
  DCV console session exists. DCV virtual sessions are unaffected.
- Everything is undone with `sudo ops/tune_host.sh --undo --go` and a reboot.
  Before a `--deep` reboot, take the snapshot and enable the EC2 serial
  console, as the script's header says.

Not worth it here:

- A PREEMPT_RT kernel (Ubuntu Pro `realtime-kernel`) helps threads that sleep
  and wake. ALTAIR's hot threads spin on isolated cores, so it buys little.
  Revisit if a wake-up shows in the per-hop numbers.
- A unikernel, or "ALTAIR as the OS". It saves microseconds inside the box,
  costs the desktop, the tools and safety, and does nothing about the 1.5 ms
  wire.
- ENA Express. It works only between instances in the same AZ, not to the
  internet.
- Real hardware control (BIOS C-states, turbo, SMT off, the NIC's own
  interrupt moderation). It needs **c7i.metal-24xl** (about 6x the cost).
  Measured gain over a tuned VM: a few µs.

## Ranked: what is left, by value

| Rank | Idea | Expected saving | Cost / risk |
|---|---|---|---|
| 1 | Turn on the kept order line (merged; needs a router restart) | ~8 ms an order | none |
| 2 | `spin = true` already on: restart the price service to pick up the bus spin | ~0.5 ms average a tick | none |
| 3 | Elastic IP on the spare, `/etc/altair/order_line.conf`: orders on DPDK | 10 to 30 µs an order | $3.60 a month |
| 4 | `tune_host.sh --deep` after hours | 10 to 50 µs on any wake-up | a reboot |
| 5 | Feed on DPDK (third ENI) instead of generic-mode AF_XDP | 5 to 20 µs a tick | a third ENI and Elastic IP |
| 6 | Shared-memory ring from the bus to the engine instead of loopback TCP | ~5 to 10 µs a tick | moderate code |
| 7 | Pick the Mumbai AZ nearest FYERS' edge (probe from a small instance in each AZ) | 0 to 300 µs of wire | a few hours of a t3 |
| 7b | c7i.4xlarge (16 vCPUs): each spinning thread on its own PHYSICAL core, its hyperthread twin idle. On the 2xlarge, cores 2-7 are 3 physical cores shared by 5-6 spinners | 1 to 10 µs, less jitter | about 2x the instance cost, a stop/start |
| 8 | NSE colocation through a broker with co-lo and DMA | the 1.5 ms itself | a different broker and a large budget |

The per-hop numbers from the next live session (`feed_status.json`
`bus_hop`, engine `bus_to_engine_us`, `data/live_orders/latency.json`) say
which of 4 to 6 is worth doing next.
