#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <linux/ktime.h>

#define MAX_SUSPICIOUS_IPS 2048
#define THRESHOLD_TCP 100
#define THRESHOLD_UDP 100
#define BLOCK_TIME 60000
#define PATTERN_COUNT 10
#define BOT_DETECTION_THRESHOLD 5

struct packet_info {
    __u32 tcp_count;
    __u32 udp_count;
    ktime_t last_seen;
    __u64 patterns[PATTERN_COUNT];
    __u32 bot_count;
    ktime_t last_packet_time;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_SUSPICIOUS_IPS);
    __type(key, __u32);
    __type(value, struct packet_info);
} ip_stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_SUSPICIOUS_IPS);
    __type(key, __u32);
    __type(value, __u32);
} blocked_ips SEC(".maps");

SEC("filter/firewall")
int xdp_firewall(struct xdp_md *ctx) {
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    if (data + sizeof(struct ethhdr) > data_end) {
        return XDP_PASS;
    }

    struct ethhdr *eth = data;
    if (eth->h_proto != htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    struct iphdr *ip = data + sizeof(struct ethhdr);
    if ((void *)(ip + 1) > data_end) {
        return XDP_PASS;
    }

    __u32 key = ip->saddr;
    struct packet_info *info = bpf_map_lookup_elem(&ip_stats, &key);
    ktime_t now = bpf_ktime_get_ns();

    __u32 *blocked = bpf_map_lookup_elem(&blocked_ips, &key);
    if (blocked) {
        if (now < *blocked + BLOCK_TIME * 1000000) {
            return XDP_DROP;
        } else {
            bpf_map_delete_elem(&blocked_ips, &key);
        }
    }

    if (ip->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (struct tcphdr *)(ip + 1);
        if ((void *)(tcp + 1) > data_end) {
            return XDP_PASS;
        }

        if (ntohs(tcp->dest) == 25565) {
            if (info) {
                info->tcp_count++;
                info->last_seen = now;
                info->patterns[(info->tcp_count % PATTERN_COUNT)] = now;

                if (info->tcp_count > THRESHOLD_TCP) {
                    bpf_map_update_elem(&blocked_ips, &key, &now, BPF_ANY);
                    return XDP_DROP;
                }

                if (now - info->last_packet_time < 1000) {
                    bpf_map_update_elem(&blocked_ips, &key, &now, BPF_ANY);
                    return XDP_DROP;
                }

                info->last_packet_time = now;
            } else {
                struct packet_info new_info = {1, 0, now, {0}, 0, now};
                new_info.patterns[0] = now;
                bpf_map_update_elem(&ip_stats, &key, &new_info, BPF_ANY);
            }
        }
    } else if (ip->protocol == IPPROTO_UDP) {
        struct udphdr *udp = (struct udphdr *)(ip + 1);
        if ((void *)(udp + 1) > data_end) {
            return XDP_PASS;
        }

        if (ntohs(udp->dest) == 25565) {
            if (info) {
                info->udp_count++;
                info->last_seen = now;
                info->patterns[(info->udp_count % PATTERN_COUNT)] = now;

                if (info->udp_count > THRESHOLD_UDP) {
                    bpf_map_update_elem(&blocked_ips, &key, &now, BPF_ANY);
                    return XDP_DROP;
                }

                if (now - info->last_packet_time < 1000) {
                    bpf_map_update_elem(&blocked_ips, &key, &now, BPF_ANY);
                    return XDP_DROP;
                }

                info->last_packet_time = now;
            } else {
                struct packet_info new_info = {0, 1, now, {0}, 0, now};
                new_info.patterns[0] = now;
                bpf_map_update_elem(&ip_stats, &key, &new_info, BPF_ANY);
            }
        }
    }

    if (info) {
        __u64 last_pattern_time = info->patterns[info->tcp_count % PATTERN_COUNT];
        if (now - last_pattern_time < 1000000) {
            bpf_map_update_elem(&blocked_ips, &key, &now, BPF_ANY);
            return XDP_DROP;
        }

        info->bot_count++;
        if (info->bot_count > BOT_DETECTION_THRESHOLD) {
            bpf_map_update_elem(&blocked_ips, &key, &now, BPF_ANY);
            return XDP_DROP;
        }
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
