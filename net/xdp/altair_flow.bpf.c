// net/xdp/altair_flow.bpf.c -- the XDP filter in front of ALTAIR's AF_XDP sockets.
//
// Runs in the kernel on every received frame, before the network stack. A
// frame of a flow listed in `flows` (remote address + remote port + local
// port, the bypass connection's own 4-tuple minus the local address) goes to
// the AF_XDP socket bound to the queue it arrived on. EVERYTHING ELSE passes
// to the kernel untouched -- SSH, DCV, and the recorder's own FYERS socket
// keep working, because only the bypass connection's local port is listed.
//
// If no socket is bound on that queue the redirect falls back to XDP_PASS, so
// a misconfiguration degrades to "the kernel gets it", never "dropped".
//
// Built with: clang -O2 -g -target bpf -c altair_flow.bpf.c

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct flow_key {
    __u32 remote_ip;    // network order
    __u16 remote_port;  // network order
    __u16 local_port;   // network order
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, struct flow_key);
    __type(value, __u32);   // unused (1)
} flows SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks SEC(".maps");

// [0] frames redirected, [1] frames passed
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u64);
} counters SEC(".maps");

static __always_inline void count(__u32 i) {
    __u64* v = bpf_map_lookup_elem(&counters, &i);
    if (v) *v += 1;
}

SEC("xdp")
int altair_flow(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* end = (void*)(long)ctx->data_end;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > end) goto pass;
    __u16 proto = eth->h_proto;
    void* l3 = eth + 1;
    if (proto == bpf_htons(ETH_P_8021Q)) {
        if (l3 + 4 > end) goto pass;
        proto = *(__u16*)(l3 + 2);
        l3 += 4;
    }
    if (proto != bpf_htons(ETH_P_IP)) goto pass;

    struct iphdr* ip = l3;
    if ((void*)(ip + 1) > end) goto pass;
    if (ip->protocol != IPPROTO_TCP || ip->ihl < 5) goto pass;
    if (ip->frag_off & bpf_htons(0x3FFF)) goto pass;   // fragments: the kernel reassembles

    struct tcphdr* tcp = (void*)ip + ip->ihl * 4;
    if ((void*)(tcp + 1) > end) goto pass;

    struct flow_key key = {
        .remote_ip = ip->saddr,
        .remote_port = tcp->source,
        .local_port = tcp->dest,
    };
    if (!bpf_map_lookup_elem(&flows, &key)) goto pass;

    count(0);
    return bpf_redirect_map(&xsks, ctx->rx_queue_index, XDP_PASS);

pass:
    count(1);
    return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";
