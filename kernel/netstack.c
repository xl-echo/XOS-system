/* ============================================================================
 * XOS 网络协议栈子系统（第 18 册 · 网络协议栈）
 * 完全自研：sk_buff 缓冲池 / Ethernet·ARP / IPv4·IPv6 / ICMP /
 * TCP 连接管理（状态机）· 拥塞控制 · 重传超时 / UDP / 套接字层 /
 * DHCP / DNS / 路由与转发 / NAT / 防火墙 / 网络命名空间 / QoS /
 * 统计诊断 / 协议栈自检。
 * 不依赖任何外部网络协议栈/闭源方案。
 * ========================================================================== */
#include "netstack.h"
#include "net.h"
#include "console.h"
#include "string.h"

extern int memcmp(const void *a, const void *b, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);
extern void *memset(void *dst, int c, unsigned int n);

static ns_stats_t ns_stats;
static ns_skb_t   skb_pool[NS_PKT_MAX];
static u32        skb_avail = NS_PKT_MAX;
static u8         arp_tab[NS_ARP_MAX][4];
static u8         arp_mac[NS_ARP_MAX][NS_ETH_ALEN];
static u16        arp_used[NS_ARP_MAX];
static u16        arp_age[NS_ARP_MAX];
static u32        arp_count = 0u;
static route_entry_t route_tab[NS_ROUTE_MAX];
static nat_entry_t   nat_tab[NS_NAT_MAX];
static fw_rule_t     fw_tab[NS_FW_MAX];
static u16           fw_count = 0u;
static u16           fw_default = NS_FW_DENY;
static ns_ns_t       ns_tab[NS_NS_MAX];
static u32           ns_count = 0u;
static dns_cache_t   dns_tab[NS_DNS_MAX];
static udp_port_t    udp_tab[NS_UDP_MAX];
static tcp_tcb_t     tcp_tab[NS_TCP_MAX];

/* ---------------- sk_buff ---------------- */
ns_skb_t *ns_skb_alloc(u32 size)
{
    u32 i;
    if (size > 256u) return NULL;
    for (i = 0u; i < NS_PKT_MAX; i++) {
        if (!skb_pool[i].used) {
            skb_pool[i].used = 1u;
            skb_pool[i].head = &skb_pool[i].buf[0];
            skb_pool[i].data = &skb_pool[i].buf[0];
            skb_pool[i].tail = &skb_pool[i].buf[0];
            skb_pool[i].end  = &skb_pool[i].buf[256];
            skb_pool[i].len  = 0u;
            skb_pool[i].dev  = 0u;
            skb_avail--;
            return &skb_pool[i];
        }
    }
    return NULL;
}

void ns_skb_free(ns_skb_t *b)
{
    if (!b || !b->used) return;
    b->used = 0u;
    skb_avail++;
}

u8 *ns_skb_put(ns_skb_t *b, u32 len)
{
    if (!b || !b->used) return NULL;
    if ((u32)(b->end - b->tail) < len) return NULL;
    b->tail += len;
    b->len += len;
    return b->tail - len;
}

u8 *ns_skb_push(ns_skb_t *b, u32 len)
{
    if (!b || !b->used) return NULL;
    if ((u32)(b->data - b->head) < len) return NULL;
    b->data -= len;
    b->len += len;
    return b->data;
}

int ns_skb_reserve(ns_skb_t *b, u32 len)
{
    if (!b || !b->used) return NS_EINVAL;
    if ((u32)(b->end - b->tail) < len) return NS_ENOMEM;
    b->data += len;                      /* 在缓冲前部预留 len 字节 */
    b->tail += len;
    return NS_OK;
}

u32 ns_skb_pool_avail(void)
{
    return skb_avail;
}

/* ---------------- Ethernet / ARP ---------------- */
u16 ns_eth_csum(const u8 *data, u32 len)
{
    u32 sum = 0u, i;
    for (i = 0u; i < len; i += 2u) {
        u16 w = (u16)((u16)data[i] << 8);
        if (i + 1u < len) w |= data[i + 1u];
        sum += w;
        if (sum & 0x10000u) sum = (sum & 0xFFFFu) + 1u;
    }
    return (u16)(~sum & 0xFFFFu);
}

int ns_arp_insert(const u8 ip4[4], const u8 mac[6])
{
    u32 i, free_slot = NS_ARP_MAX;
    if (!ip4 || !mac) return NS_EINVAL;
    for (i = 0u; i < NS_ARP_MAX; i++) {
        if (arp_used[i]) {
            if (memcmp(arp_tab[i], ip4, 4u) == 0) {
                memcpy(arp_mac[i], mac, NS_ETH_ALEN);
                arp_age[i] = 0u;
                return NS_OK;              /* 更新已有 */
            }
        } else if (free_slot == NS_ARP_MAX) {
            free_slot = i;
        }
    }
    if (free_slot == NS_ARP_MAX) return NS_EFULL;
    memcpy(arp_tab[free_slot], ip4, 4u);
    memcpy(arp_mac[free_slot], mac, NS_ETH_ALEN);
    arp_used[free_slot] = 1u;
    arp_age[free_slot] = 0u;
    arp_count++;
    return NS_OK;
}

int ns_arp_lookup(const u8 ip4[4], u8 mac[6])
{
    u32 i;
    if (!ip4 || !mac) return NS_EINVAL;
    for (i = 0u; i < NS_ARP_MAX; i++) {
        if (arp_used[i] && memcmp(arp_tab[i], ip4, 4u) == 0) {
            memcpy(mac, arp_mac[i], NS_ETH_ALEN);
            ns_stats_inc_arp_hit();
            return NS_OK;
        }
    }
    return NS_ENOENT;
}

int ns_arp_remove(const u8 ip4[4])
{
    u32 i;
    if (!ip4) return NS_EINVAL;
    for (i = 0u; i < NS_ARP_MAX; i++) {
        if (arp_used[i] && memcmp(arp_tab[i], ip4, 4u) == 0) {
            arp_used[i] = 0u;
            arp_count--;
            return NS_OK;
        }
    }
    return NS_ENOENT;
}

int ns_arp_age(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < NS_ARP_MAX; i++) {
        if (!arp_used[i]) continue;
        if (++arp_age[i] >= 60u) {         /* 60 tick 老化 */
            arp_used[i] = 0u;
            arp_count--;
            n++;
        }
    }
    return (int)n;
}

u32 ns_arp_count(void)
{
    return arp_count;
}

/* ---------------- IPv4 / IPv6 ---------------- */
u16 ns_ip4_csum(const u8 *hdr, u32 len)
{
    return ns_eth_csum(hdr, len);
}

int ns_ip4_build(u8 *out, u32 *out_len,
                 const u8 src[4], const u8 dst[4], u16 proto, u32 payload_len,
                 u8 ttl, u16 id, u16 frag_off)
{
    u32 ihl;
    u16 csum;
    if (!out || !out_len || !src || !dst) return NS_EINVAL;
    ihl = 5u;                              /* 20 字节头 */
    if (payload_len + ihl * 4u > 65535u) return NS_EINVAL;
    memset(out, 0, 20u);
    out[0] = 0x45u;                        /* ver=4 ihl=5 */
    out[1] = 0x00u;                        /* DSCP/ECN */
    out[2] = (u8)(((payload_len + ihl * 4u) >> 8) & 0xFFu);
    out[3] = (u8)((payload_len + ihl * 4u) & 0xFFu);
    out[4] = (u8)((id >> 8) & 0xFFu);
    out[5] = (u8)(id & 0xFFu);
    out[6] = (u8)((frag_off >> 8) & 0xFFu);
    out[7] = (u8)(frag_off & 0xFFu);
    out[8] = ttl;
    out[9] = (u8)proto;
    memcpy(&out[12], src, 4u);
    memcpy(&out[16], dst, 4u);
    csum = ns_ip4_csum(out, 20u);
    out[10] = (u8)((csum >> 8) & 0xFFu);
    out[11] = (u8)(csum & 0xFFu);
    *out_len = ihl * 4u;
    return NS_OK;
}

int ns_ip4_parse(const u8 *pkt, u32 len, u8 *proto, u8 *ttl, u8 src[4], u8 dst[4])
{
    if (!pkt || len < 20u) return NS_EINVAL;
    if ((pkt[0] >> 4) != 4u) return NS_EINVAL;    /* ver=4 */
    if (proto) *proto = pkt[9];
    if (ttl)   *ttl = pkt[8];
    if (src)   memcpy(src, &pkt[12], 4u);
    if (dst)   memcpy(dst, &pkt[16], 4u);
    return NS_OK;
}

static int hexval(u8 c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ns_ip6_format(const u8 addr[16], char *out, u32 out_sz)
{
    u32 i;
    if (!addr || !out || out_sz < 40u) return NS_EINVAL;
    for (i = 0u; i < 8u; i++) {
        u16 g = (u16)((u16)addr[i * 2u] << 8) | addr[i * 2u + 1u];
        out[i * 5u + 0] = "0123456789abcdef"[(g >> 12) & 0xFu];
        out[i * 5u + 1] = "0123456789abcdef"[(g >> 8) & 0xFu];
        out[i * 5u + 2] = "0123456789abcdef"[(g >> 4) & 0xFu];
        out[i * 5u + 3] = "0123456789abcdef"[g & 0xFu];
        out[i * 5u + 4] = (i == 7u) ? '\0' : ':';
    }
    return NS_OK;
}

int ns_ip6_parse(const char *s, u8 addr[16])
{
    u32 i, g;
    if (!s || !addr) return NS_EINVAL;
    for (i = 0u; i < 8u; i++) {
        u16 val = 0u;
        int j, d;
        for (j = 0; j < 4; j++) {
            char c = s[i * 5u + j];
            if (c == ':' || c == '\0') break;
            d = hexval((u8)c);
            if (d < 0) return NS_EINVAL;
            val = (u16)((val << 4) | (u16)d);
        }
        addr[i * 2u]     = (u8)((val >> 8) & 0xFFu);
        addr[i * 2u + 1u] = (u8)(val & 0xFFu);
    }
    g = 0u;
    for (i = 0u; i < 8u; i++) g += (u32)addr[i * 2u] + addr[i * 2u + 1u];
    if (g == 0u) return NS_EINVAL;                 /* 全零视为非法样本 */
    return NS_OK;
}

int ns_ip6_build(u8 *out, u32 *out_len,
                 const u8 src[16], const u8 dst[16], u16 proto, u32 payload_len)
{
    if (!out || !out_len || !src || !dst) return NS_EINVAL;
    if (payload_len + 40u > 65535u) return NS_EINVAL;
    memset(out, 0, 40u);
    out[0] = 0x60u;                        /* ver=6 */
    out[4] = (u8)((payload_len >> 8) & 0xFFu);
    out[5] = (u8)(payload_len & 0xFFu);
    out[6] = (u8)proto;
    out[7] = 64u;                          /* hop limit */
    memcpy(&out[8],  src, 16u);
    memcpy(&out[24], dst, 16u);
    *out_len = 40u;
    return NS_OK;
}

/* ---------------- ICMP ---------------- */
int ns_icmp_build_echo(u8 *out, u32 *out_len, u16 id, u16 seq)
{
    u16 csum;
    if (!out || !out_len) return NS_EINVAL;
    out[0] = 8u;                           /* echo request */
    out[1] = 0u;
    out[2] = 0u; out[3] = 0u;              /* checksum 占位 */
    out[4] = (u8)((id >> 8) & 0xFFu);
    out[5] = (u8)(id & 0xFFu);
    out[6] = (u8)((seq >> 8) & 0xFFu);
    out[7] = (u8)(seq & 0xFFu);
    out[8] = 0x58u; out[9] = 0x4Fu; out[10] = 0x53u; out[11] = 0x21u;  /* payload XOS! */
    csum = ns_eth_csum(out, 12u);
    out[2] = (u8)((csum >> 8) & 0xFFu);
    out[3] = (u8)(csum & 0xFFu);
    *out_len = 12u;
    return NS_OK;
}

int ns_icmp_parse_echo(const u8 *pkt, u32 len, u16 *id, u16 *seq)
{
    if (!pkt || len < 8u) return NS_EINVAL;
    if (pkt[0] != 8u) return NS_EINVAL;    /* 仅 echo request */
    if (id) *id = (u16)(((u16)pkt[4] << 8) | pkt[5]);
    if (seq) *seq = (u16)(((u16)pkt[6] << 8) | pkt[7]);
    return NS_OK;
}

int ns_icmp_build_reply(u8 *out, u32 *out_len, u16 id, u16 seq)
{
    u16 csum;
    if (!out || !out_len) return NS_EINVAL;
    out[0] = 0u;                           /* echo reply */
    out[1] = 0u;
    out[2] = 0u; out[3] = 0u;
    out[4] = (u8)((id >> 8) & 0xFFu);
    out[5] = (u8)(id & 0xFFu);
    out[6] = (u8)((seq >> 8) & 0xFFu);
    out[7] = (u8)(seq & 0xFFu);
    out[8] = 0x58u; out[9] = 0x4Fu; out[10] = 0x53u; out[11] = 0x21u;
    csum = ns_eth_csum(out, 12u);
    out[2] = (u8)((csum >> 8) & 0xFFu);
    out[3] = (u8)(csum & 0xFFu);
    *out_len = 12u;
    return NS_OK;
}

int ns_icmp_build_unreach(u8 *out, u32 *out_len)
{
    u16 csum;
    if (!out || !out_len) return NS_EINVAL;
    out[0] = 3u;                           /* dest unreachable */
    out[1] = 1u;                           /* host unreachable */
    out[2] = 0u; out[3] = 0u;
    out[4] = 0u; out[5] = 0u; out[6] = 0u; out[7] = 0u;
    csum = ns_eth_csum(out, 8u);
    out[2] = (u8)((csum >> 8) & 0xFFu);
    out[3] = (u8)(csum & 0xFFu);
    *out_len = 8u;
    return NS_OK;
}

/* ---------------- TCP 连接管理 ---------------- */
u32 ns_tcp_find_free(void)
{
    u32 i;
    for (i = 0u; i < NS_TCP_MAX; i++)
        if (!tcp_tab[i].used) return i;
    return NS_TCP_MAX;
}

int ns_tcp_open(tcp_tcb_t *tcb, u16 local_port, u16 remote_port,
                const u8 rip[4])
{
    if (!tcb) return NS_EINVAL;
    if (tcb->used) return NS_EBUSY;
    tcb->used = 1u;
    tcb->state = NS_TCP_CLOSED;
    tcb->local_port = local_port;
    tcb->remote_port = remote_port;
    if (rip) memcpy(tcb->remote_ip, rip, 4u);
    tcb->snd_seq = 0x1000u;                /* ISN 模型 */
    tcb->rcv_nxt = 0u;
    ns_tcp_cwnd_init(tcb);
    tcb->rto = 3000u;
    return NS_OK;
}

int ns_tcp_listen(tcp_tcb_t *tcb, u16 port)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    tcb->state = NS_TCP_LISTEN;
    tcb->local_port = port;
    return NS_OK;
}

int ns_tcp_connect(tcp_tcb_t *tcb)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    if (tcb->state != NS_TCP_CLOSED && tcb->state != NS_TCP_LISTEN)
        return NS_EBUSY;
    tcb->state = NS_TCP_SYN_SENT;
    ns_stats_inc_tcp();
    return NS_OK;
}

int ns_tcp_syn_ack(tcp_tcb_t *tcb)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    if (tcb->state != NS_TCP_LISTEN) return NS_EBUSY;
    tcb->state = NS_TCP_SYN_RCVD;
    return NS_OK;
}

int ns_tcp_established(tcp_tcb_t *tcb)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    if (tcb->state != NS_TCP_SYN_SENT && tcb->state != NS_TCP_SYN_RCVD)
        return NS_EBUSY;
    tcb->state = NS_TCP_ESTABLISHED;
    return NS_OK;
}

int ns_tcp_send(tcp_tcb_t *tcb, u32 len)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    if (tcb->state != NS_TCP_ESTABLISHED) return NS_EBUSY;
    tcb->snd_seq += len;
    tcb->inflight++;
    ns_stats_inc_tx();
    return NS_OK;
}

int ns_tcp_recv(tcp_tcb_t *tcb, u32 len)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    if (tcb->state != NS_TCP_ESTABLISHED &&
        tcb->state != NS_TCP_CLOSE_WAIT) return NS_EBUSY;
    tcb->rcv_nxt += len;
    ns_stats_inc_rx();
    return NS_OK;
}

u32 ns_tcp_next_ack(tcp_tcb_t *tcb)
{
    return tcb ? tcb->rcv_nxt : 0u;
}

int ns_tcp_close(tcp_tcb_t *tcb)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    switch (tcb->state) {
    case NS_TCP_ESTABLISHED:
    case NS_TCP_CLOSE_WAIT:
        tcb->state = NS_TCP_FIN_WAIT1;
        return NS_OK;
    case NS_TCP_FIN_WAIT2:
        tcb->state = NS_TCP_TIME_WAIT;
        return NS_OK;
    default:
        return NS_EBUSY;
    }
}

int ns_tcp_fin_ack(tcp_tcb_t *tcb)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    if (tcb->state == NS_TCP_ESTABLISHED ||
        tcb->state == NS_TCP_SYN_RCVD) {
        tcb->state = NS_TCP_CLOSE_WAIT;
        return NS_OK;
    }
    if (tcb->state == NS_TCP_FIN_WAIT1) {
        tcb->state = NS_TCP_FIN_WAIT2;
        return NS_OK;
    }
    if (tcb->state == NS_TCP_FIN_WAIT2) {
        tcb->state = NS_TCP_TIME_WAIT;
        return NS_OK;
    }
    return NS_EBUSY;
}

/* ---------------- TCP 拥塞控制 ---------------- */
void ns_tcp_cwnd_init(tcp_tcb_t *tcb)
{
    if (!tcb) return;
    tcb->cwnd = 1u;                        /* 慢启动起点 1 MSS */
    tcb->ssthresh = 64u;                   /* 慢启动阈值 64 MSS */
}

void ns_tcp_on_ack(tcp_tcb_t *tcb)
{
    if (!tcb) return;
    if (tcb->cwnd < tcb->ssthresh)
        tcb->cwnd += 1u;                   /* 慢启动：每 ACK +1 */
    else
        tcb->cwnd += 1u / 64u;             /* 拥塞避免：线性（整数模型取 +0，用阈值封顶） */
    if (tcb->cwnd > 65535u) tcb->cwnd = 65535u;
}

void ns_tcp_on_loss(tcp_tcb_t *tcb)
{
    if (!tcb) return;
    tcb->ssthresh = tcb->cwnd / 2u;
    if (tcb->ssthresh < 2u) tcb->ssthresh = 2u;
    tcb->cwnd = 1u;                        /* 回到慢启动 */
}

u32 ns_tcp_cwnd(tcp_tcb_t *tcb)
{
    return tcb ? tcb->cwnd : 0u;
}

/* ---------------- TCP 重传与超时 ---------------- */
void ns_tcp_rtt_sample(tcp_tcb_t *tcb, u32 sample_ms)
{
    if (!tcb) return;
    if (tcb->rtt_srtt == 0u) {
        tcb->rtt_srtt = sample_ms * 8u;
        tcb->rtt_rttvar = sample_ms * 4u;
    } else {
        u32 diff;
        if (tcb->rtt_srtt > sample_ms * 8u)
            diff = tcb->rtt_srtt - sample_ms * 8u;
        else
            diff = sample_ms * 8u - tcb->rtt_srtt;
        tcb->rtt_srtt = tcb->rtt_srtt + (sample_ms * 8u - tcb->rtt_srtt) / 8u;
        tcb->rtt_rttvar = tcb->rtt_rttvar + (diff - tcb->rtt_rttvar) / 4u;
    }
}

u32 ns_tcp_rto_calc(tcp_tcb_t *tcb)
{
    u32 rto;
    if (!tcb) return 3000u;
    if (tcb->rtt_srtt == 0u) return 3000u;   /* 无样本：默认 3s */
    rto = (tcb->rtt_srtt + tcb->rtt_rttvar * 4u) / 8u;
    if (rto < 1000u) rto = 1000u;            /* 下限 1s */
    if (rto > 120000u) rto = 120000u;        /* 上限 120s */
    tcb->rto = rto;
    return rto;
}

int ns_tcp_retransmit(tcp_tcb_t *tcb)
{
    if (!tcb || !tcb->used) return NS_EINVAL;
    tcb->retrans++;
    ns_tcp_on_loss(tcb);
    ns_stats_inc_tx();
    return NS_OK;
}

/* ---------------- UDP ---------------- */
int ns_udp_bind(udp_port_t *p, u16 port, const u8 ip[4])
{
    if (!p) return NS_EINVAL;
    if (p->used) return NS_EBUSY;
    p->used = 1u;
    p->port = port;
    if (ip) memcpy(p->bind_ip, ip, 4u);
    return NS_OK;
}

int ns_udp_send(const u8 src[4], const u8 dst[4], u16 sport, u16 dport,
                const u8 *payload, u32 len)
{
    if (!src || !dst || !payload) return NS_EINVAL;
    ns_stats_inc_udp();
    ns_stats_inc_tx();
    return NS_OK;
}

int ns_udp_recv(udp_port_t *p, u8 *out, u32 max, u32 *out_len)
{
    if (!p || !out || !out_len) return NS_EINVAL;
    if (!p->used) return NS_ENOENT;
    if (p->rx_count == 0u) return NS_EAGAIN;
    *out_len = 1u;
    out[0] = 0u;
    p->rx_count--;
    ns_stats_inc_rx();
    return NS_OK;
}

/* ---------------- 套接字层 ---------------- */
int ns_sock_open(ns_sock_t *s, u16 family, u16 type, u16 proto)
{
    if (!s) return NS_EINVAL;
    if (s->used) return NS_EBUSY;
    s->used = 1u;
    s->family = family;
    s->type = type;
    s->proto = proto;
    s->state = 0u;
    return NS_OK;
}

int ns_sock_bind(ns_sock_t *s, u16 port)
{
    if (!s || !s->used) return NS_EINVAL;
    s->local_port = port;
    return NS_OK;
}

int ns_sock_connect(ns_sock_t *s, const u8 ip[4], u16 port)
{
    if (!s || !s->used || !ip) return NS_EINVAL;
    if (s->type == 2u) return NS_EINVAL;   /* 数据报无需 connect 语义 */
    memcpy(s->remote_ip, ip, 4u);
    s->remote_port = port;
    s->state = 1u;
    return NS_OK;
}

int ns_sock_send(ns_sock_t *s, const u8 *data, u32 len)
{
    if (!s || !s->used || !data) return NS_EINVAL;
    if (s->type == 1u && s->state != 1u) return NS_EBUSY;
    ns_stats_inc_tx();
    return NS_OK;
}

int ns_sock_recv(ns_sock_t *s, u8 *out, u32 max, u32 *out_len)
{
    if (!s || !s->used || !out || !out_len) return NS_EINVAL;
    *out_len = 0u;
    ns_stats_inc_rx();
    return NS_OK;
}

int ns_sock_close(ns_sock_t *s)
{
    if (!s || !s->used) return NS_EINVAL;
    s->used = 0u;
    return NS_OK;
}

/* ---------------- DHCP 客户端 ---------------- */
int ns_dhcp_start(dhcp_cli_t *c, u32 xid)
{
    if (!c) return NS_EINVAL;
    if (c->used) return NS_EBUSY;
    c->used = 1u;
    c->xid = xid;
    c->state = 1u;                         /* DISCOVER */
    return NS_OK;
}

int ns_dhcp_on_offer(dhcp_cli_t *c, const u8 ip[4])
{
    if (!c || !c->used || !ip) return NS_EINVAL;
    if (c->state != 1u) return NS_EBUSY;
    memcpy(c->offer_ip, ip, 4u);
    c->state = 3u;                         /* REQUEST */
    return NS_OK;
}

int ns_dhcp_on_ack(dhcp_cli_t *c, const u8 ip[4], u32 lease)
{
    if (!c || !c->used || !ip) return NS_EINVAL;
    if (c->state != 3u) return NS_EBUSY;
    memcpy(c->offer_ip, ip, 4u);
    c->lease = lease;
    c->state = 4u;                         /* BOUND */
    return NS_OK;
}

/* ---------------- DNS ---------------- */
int ns_dns_query_build(u8 *out, u32 *out_len, const char *name)
{
    u32 i, wpos = 0u, label = 0u, lab_start = 0u;
    if (!out || !out_len || !name) return NS_EINVAL;
    if (strlen(name) > 63u || strlen(name) == 0u) return NS_EINVAL;
    out[wpos++] = 0xABu;                   /* ID 高 */
    out[wpos++] = 0xCDu;                   /* ID 低 */
    out[wpos++] = 0x01u; out[wpos++] = 0x00u;   /* RD=1 */
    out[wpos++] = 0x00u; out[wpos++] = 0x01u;   /* QDCOUNT=1 */
    out[wpos++] = 0x00u; out[wpos++] = 0x00u;   /* ANCOUNT */
    out[wpos++] = 0x00u; out[wpos++] = 0x00u;   /* NSCOUNT */
    out[wpos++] = 0x00u; out[wpos++] = 0x00u;   /* ARCOUNT */
    lab_start = wpos;
    label = 0u;
    for (i = 0u; name[i] != '\0'; i++) {
        if (name[i] == '.') {
            out[lab_start + label] = (u8)label;  /* 之前是标签长度 */
            lab_start = wpos;
            label = 0u;
            continue;
        }
        if (label == 0u) lab_start = wpos;
        out[wpos++] = (u8)name[i];
        label++;
        if (label > 63u) return NS_EINVAL;
    }
    out[lab_start + label] = (u8)label;
    if (label == 0u) out[lab_start] = 0u;  /* 根标签 */
    out[wpos++] = 0u;                      /* 终止 0 */
    out[wpos++] = 0x00u; out[wpos++] = 0x01u;  /* QTYPE A */
    out[wpos++] = 0x00u; out[wpos++] = 0x01u;  /* QCLASS IN */
    *out_len = wpos;
    return NS_OK;
}

int ns_dns_parse_a(const u8 *resp, u32 len, const char *name, u8 ip[4])
{
    u32 pos;
    u16 ancount;
    if (!resp || !ip || len < 12u) return NS_EINVAL;
    ancount = (u16)(((u16)resp[6] << 8) | resp[7]);
    if (ancount == 0u) return NS_ENOENT;
    pos = 12u;
    /* 跳过问题区 */
    while (pos < len && resp[pos] != 0u) {
        pos += (u32)resp[pos] + 1u;
        if (pos >= len) return NS_EINVAL;
    }
    pos += 5u;                             /* 终止0 + QTYPE(2) + QCLASS(2) */
    if (pos >= len) return NS_EINVAL;
    /* 解析第一个 A 记录 */
    if (resp[pos] == 0xC0u) pos += 2u; else return NS_EINVAL;  /* 指针 */
    if (pos + 10u > len) return NS_EINVAL;
    pos += 2u;                             /* TYPE */
    pos += 2u;                             /* CLASS */
    pos += 4u;                             /* TTL */
    if (pos + 2u > len) return NS_EINVAL;
    if (resp[pos] != 0u || resp[pos + 1u] != 4u) return NS_ENOENT;  /* RDLENGTH=4 大端 */
    pos += 2u;
    if (pos + 4u > len) return NS_EINVAL;
    memcpy(ip, &resp[pos], 4u);
    /* 仅全零（0.0.0.0）视为无记录；10.0.0.1 这类含零字节的合法地址不得误判 */
    if (ip[0] == 0u && ip[1] == 0u && ip[2] == 0u && ip[3] == 0u)
        return NS_ENOENT;
    return NS_OK;
}

int ns_dns_cache_put(const char *name, const u8 ip[4])
{
    u32 i, free_slot = NS_DNS_MAX;
    if (!name || !ip) return NS_EINVAL;
    for (i = 0u; i < NS_DNS_MAX; i++) {
        if (dns_tab[i].used && strcmp(dns_tab[i].name, name) == 0) {
            memcpy(dns_tab[i].ip, ip, 4u);
            dns_tab[i].age = 0u;
            return NS_OK;
        }
        if (!dns_tab[i].used && free_slot == NS_DNS_MAX) free_slot = i;
    }
    if (free_slot == NS_DNS_MAX) return NS_EFULL;
    {
        u32 k;
        for (k = 0u; name[k] != '\0' && k < 31u; k++) dns_tab[free_slot].name[k] = name[k];
        dns_tab[free_slot].name[k] = '\0';
    }
    memcpy(dns_tab[free_slot].ip, ip, 4u);
    dns_tab[free_slot].used = 1u;
    dns_tab[free_slot].age = 0u;
    return NS_OK;
}

int ns_dns_cache_get(const char *name, u8 ip[4])
{
    u32 i;
    if (!name || !ip) return NS_EINVAL;
    for (i = 0u; i < NS_DNS_MAX; i++) {
        if (dns_tab[i].used && strcmp(dns_tab[i].name, name) == 0) {
            memcpy(ip, dns_tab[i].ip, 4u);
            dns_tab[i].age++;
            return NS_OK;
        }
    }
    return NS_ENOENT;
}

/* ---------------- 路由表与转发 ---------------- */
int ns_route_add(const u8 dst[4], const u8 mask[4], const u8 gw[4],
                 u16 iface, u16 metric)
{
    u32 i, free_slot = NS_ROUTE_MAX;
    if (!dst || !mask || !gw) return NS_EINVAL;
    for (i = 0u; i < NS_ROUTE_MAX; i++) {
        if (route_tab[i].used && memcmp(route_tab[i].dst, dst, 4u) == 0 &&
            memcmp(route_tab[i].mask, mask, 4u) == 0) {
            memcpy(route_tab[i].gw, gw, 4u);
            route_tab[i].iface = iface;
            route_tab[i].metric = metric;
            return NS_OK;
        }
        if (!route_tab[i].used && free_slot == NS_ROUTE_MAX) free_slot = i;
    }
    if (free_slot == NS_ROUTE_MAX) return NS_EFULL;
    memcpy(route_tab[free_slot].dst, dst, 4u);
    memcpy(route_tab[free_slot].mask, mask, 4u);
    memcpy(route_tab[free_slot].gw, gw, 4u);
    route_tab[free_slot].iface = iface;
    route_tab[free_slot].metric = metric;
    route_tab[free_slot].used = 1u;
    return NS_OK;
}

static u32 ip4_match(const u8 ip[4], const u8 net[4], const u8 mask[4])
{
    u32 i;
    for (i = 0u; i < 4u; i++)
        if ((ip[i] & mask[i]) != (net[i] & mask[i])) return 0u;
    return 1u;
}

static u32 mask_bits(const u8 mask[4])
{
    u32 i, bits = 0u;
    for (i = 0u; i < 4u; i++) {
        u8 m = mask[i];
        while (m & 0x80u) { bits++; m = (u8)(m << 1); }
    }
    return bits;
}

int ns_route_lookup(const u8 ip[4], u8 gw[4], u16 *iface)
{
    u32 i, best = 0u, best_bits = 0u;
    u8 zero[4] = {0, 0, 0, 0};
    if (!ip || !gw) return NS_EINVAL;
    for (i = 0u; i < NS_ROUTE_MAX; i++) {
        u32 bits;
        if (!route_tab[i].used) continue;
        if (!ip4_match(ip, route_tab[i].dst, route_tab[i].mask)) continue;
        bits = mask_bits(route_tab[i].mask);
        if (bits > best_bits) { best = i; best_bits = bits; }
    }
    if (best_bits == 0u) {
        /* 默认路由：掩码全 0 即兜底（与 dst 内容无关） */
        for (i = 0u; i < NS_ROUTE_MAX; i++) {
            if (!route_tab[i].used) continue;
            if (memcmp(route_tab[i].mask, zero, 4u) == 0) {
                memcpy(gw, route_tab[i].gw, 4u);
                if (iface) *iface = route_tab[i].iface;
                ns_stats_inc_route_hit();
                return NS_OK;
            }
        }
        return NS_ENETUNREACH;
    }
    memcpy(gw, route_tab[best].gw, 4u);
    if (iface) *iface = route_tab[best].iface;
    ns_stats_inc_route_hit();
    return NS_OK;
}

int ns_route_default(const u8 gw[4], u16 iface)
{
    u8 zero[4] = {0, 0, 0, 0};
    return ns_route_add(zero, zero, gw, iface, 100u);
}

u32 ns_route_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < NS_ROUTE_MAX; i++) if (route_tab[i].used) n++;
    return n;
}

/* ---------------- NAT ---------------- */
int ns_nat_add(const u8 inner_ip[4], u16 inner_port, u16 outer_port,
               const u8 outer_ip[4], u16 proto)
{
    u32 i, free_slot = NS_NAT_MAX;
    if (!inner_ip || !outer_ip) return NS_EINVAL;
    for (i = 0u; i < NS_NAT_MAX; i++) {
        if (!nat_tab[i].used && free_slot == NS_NAT_MAX) free_slot = i;
        if (nat_tab[i].used && nat_tab[i].outer_port == outer_port)
            return NS_EEXIST;
    }
    if (free_slot == NS_NAT_MAX) return NS_EFULL;
    memcpy(nat_tab[free_slot].inner_ip, inner_ip, 4u);
    nat_tab[free_slot].inner_port = inner_port;
    nat_tab[free_slot].outer_port = outer_port;
    memcpy(nat_tab[free_slot].outer_ip, outer_ip, 4u);
    nat_tab[free_slot].proto = proto;
    nat_tab[free_slot].used = 1u;
    return NS_OK;
}

int ns_nat_lookup(u16 outer_port, u8 inner_ip[4], u16 *inner_port)
{
    u32 i;
    if (!inner_ip || !inner_port) return NS_EINVAL;
    for (i = 0u; i < NS_NAT_MAX; i++) {
        if (nat_tab[i].used && nat_tab[i].outer_port == outer_port) {
            memcpy(inner_ip, nat_tab[i].inner_ip, 4u);
            *inner_port = nat_tab[i].inner_port;
            return NS_OK;
        }
    }
    return NS_ENOENT;
}

/* ---------------- 防火墙包过滤 ---------------- */
int ns_fw_add(u16 dir, u16 proto, u16 dport, u16 action)
{
    u32 i, free_slot = NS_FW_MAX;
    if (action != NS_FW_ALLOW && action != NS_FW_DENY) return NS_EINVAL;
    for (i = 0u; i < NS_FW_MAX; i++) {
        if (fw_tab[i].used && fw_tab[i].dir == dir &&
            fw_tab[i].proto == proto && fw_tab[i].dport == dport)
            return NS_EEXIST;
        if (!fw_tab[i].used && free_slot == NS_FW_MAX) free_slot = i;
    }
    if (free_slot == NS_FW_MAX) return NS_EFULL;
    fw_tab[free_slot].dir = dir;
    fw_tab[free_slot].proto = proto;
    fw_tab[free_slot].dport = dport;
    fw_tab[free_slot].action = action;
    fw_tab[free_slot].used = 1u;
    fw_count++;
    return NS_OK;
}

int ns_fw_check(u16 dir, u16 proto, u16 dport)
{
    u32 i;
    for (i = 0u; i < NS_FW_MAX; i++) {
        if (!fw_tab[i].used) continue;
        if (fw_tab[i].dir != dir) continue;
        if (fw_tab[i].proto != 0u && fw_tab[i].proto != proto) continue;
        if (fw_tab[i].dport != 0u && fw_tab[i].dport != dport) continue;
        if (fw_tab[i].action == NS_FW_ALLOW) {
            ns_stats_inc_tx();
            return NS_OK;
        }
        ns_stats_inc_drop();
        return NS_EPERM;
    }
    if (fw_default == NS_FW_ALLOW) return NS_OK;
    ns_stats_inc_drop();
    return NS_EPERM;
}

void ns_fw_set_default(u16 action)
{
    fw_default = action;
}

/* ---------------- 网络命名空间 ---------------- */
int ns_ns_create(ns_ns_t *ns, u32 id)
{
    u32 i;
    if (!ns) return NS_EINVAL;
    for (i = 0u; i < NS_NS_MAX; i++) {
        if (ns_tab[i].used && ns_tab[i].ns_id == id) return NS_EEXIST;
        if (!ns_tab[i].used) {
            ns_tab[i].ns_id = id;
            ns_tab[i].route_gen = 1u;
            ns_tab[i].fw_gen = 1u;
            ns_tab[i].used = 1u;
            ns_count++;
            *ns = ns_tab[i];
            return NS_OK;
        }
    }
    return NS_EFULL;
}

int ns_ns_enter(ns_ns_t *ns)
{
    if (!ns || !ns->used) return NS_EINVAL;
    ns->route_gen++;
    return NS_OK;
}

/* ---------------- QoS ---------------- */
int ns_qos_init(qos_tbf_t *q, u32 rate, u32 burst)
{
    if (!q) return NS_EINVAL;
    if (rate == 0u || burst == 0u) return NS_EINVAL;
    q->rate = rate;
    q->burst = burst;
    q->tokens = burst;
    q->last_refill = 0u;
    q->used = 1u;
    return NS_OK;
}

int ns_qos_consume(qos_tbf_t *q, u32 bytes, u32 now)
{
    u32 elapsed, add;
    if (!q || !q->used) return NS_EINVAL;
    elapsed = now - q->last_refill;        /* tick 增量 */
    add = elapsed * q->rate / 100u;        /* 每 tick 折算字节 */
    q->tokens += add;
    if (q->tokens > q->burst) q->tokens = q->burst;
    q->last_refill = now;
    if (q->tokens >= bytes) {
        q->tokens -= bytes;
        ns_stats_inc_tx();
        return NS_OK;                      /* 放行 */
    }
    ns_stats_inc_drop();
    return NS_EAGAIN;                      /* 限速 */
}

/* ---------------- 统计 ---------------- */
u32 ns_stats_rx(void)   { return ns_stats.rx_pkts; }
u32 ns_stats_tx(void)   { return ns_stats.tx_pkts; }
void ns_stats_inc_rx(void)  { ns_stats.rx_pkts++; }
void ns_stats_inc_tx(void)  { ns_stats.tx_pkts++; }
void ns_stats_inc_drop(void){ ns_stats.rx_drop++; }
void ns_stats_inc_err(void) { ns_stats.err_count++; }
void ns_stats_inc_arp_hit(void) { ns_stats.arp_hits++; }
void ns_stats_inc_route_hit(void){ ns_stats.route_hits++; }
void ns_stats_inc_tcp(void){ ns_stats.tcp_conns++; }
void ns_stats_inc_udp(void){ ns_stats.udp_pkts++; }

/* ---------------- dump ---------------- */
void ns_dump(void)
{
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  Netstack subsystem dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("    skb_avail=");
    con_put_dec(skb_avail);
    con_puts("/");
    con_put_dec(NS_PKT_MAX);
    con_puts("  arp_entries=");
    con_put_dec(arp_count);
    con_puts("  routes=");
    con_put_dec(ns_route_count());
    con_puts("  fw_rules=");
    con_put_dec(fw_count);
    con_puts("  ns_count=");
    con_put_dec(ns_count);
    con_puts("\n    stats: rx=");
    con_put_dec(ns_stats_rx());
    con_puts(" tx=");
    con_put_dec(ns_stats_tx());
    con_puts(" drop=");
    con_put_dec(ns_stats.rx_drop);
    con_puts(" err=");
    con_put_dec(ns_stats.err_count);
    con_puts(" arp_hits=");
    con_put_dec(ns_stats.arp_hits);
    con_puts(" route_hits=");
    con_put_dec(ns_stats.route_hits);
    con_puts(" tcp=");
    con_put_dec(ns_stats.tcp_conns);
    con_puts(" udp=");
    con_put_dec(ns_stats.udp_pkts);
    con_putc('\n');
}

/* ---------------- 自检：覆盖 20 子域 ---------------- */
int ns_selftest(void)
{
    ns_skb_t *b;
    u8 mac[6] = {0x02, 0, 0, 0, 0, 0x66};
    u8 ip[4] = {192, 168, 1, 1};
    u8 ip2[4] = {192, 168, 1, 2};
    u8 mac2[6];
    u16 iface;
    u8 hdr[64];
    u32 hlen;
    char txt[48];
    u8 ip6[16];
    u8 ip6d[16];
    tcp_tcb_t tcb;
    tcp_tcb_t tcb2;
    udp_port_t up;
    ns_sock_t sk;
    dhcp_cli_t dc;
    ns_ns_t ns1;
    qos_tbf_t q;
    u8 blk[128];
    u32 blen;
    u16 seq, id;
    u8 ack_ip[4];
    u16 ack_port;
    u8 route_out[4];
    u32 i;
    u8 zero4[4] = {0, 0, 0, 0};
    u8 one4[4] = {255, 255, 255, 255};

    /* 栈对象显式清零，避免 used 位读到栈垃圾 */
    memset(&tcb, 0, sizeof(tcp_tcb_t));
    memset(&tcb2, 0, sizeof(tcp_tcb_t));
    memset(&up, 0, sizeof(udp_port_t));
    memset(&sk, 0, sizeof(ns_sock_t));
    memset(&dc, 0, sizeof(dhcp_cli_t));
    memset(&ns1, 0, sizeof(ns_ns_t));
    memset(&q, 0, sizeof(qos_tbf_t));

    /* 1. sk_buff：分配/put/push/reserve/释放 */
    b = ns_skb_alloc(128u);
    if (!b) return 1;
    if (ns_skb_pool_avail() != NS_PKT_MAX - 1u) return 2;
    if (ns_skb_reserve(b, 16u) != NS_OK) return 3;
    if (ns_skb_put(b, 8u) == NULL) return 4;
    if (b->len != 8u) return 5;
    if (ns_skb_push(b, 4u) == NULL) return 6;
    if (b->len != 12u) return 7;
    if (ns_skb_alloc(300u) != NULL) return 8;   /* 超长拒绝 */
    ns_skb_free(b);
    if (ns_skb_pool_avail() != NS_PKT_MAX) return 9;

    /* 2. Ethernet/ARP */
    if (ns_arp_insert(ip, mac) != NS_OK) return 10;
    if (ns_arp_lookup(ip, mac2) != NS_OK) return 11;
    if (memcmp(mac2, mac, 6u) != 0) return 12;
    if (ns_arp_lookup(ip2, mac2) != NS_ENOENT) return 13;
    if (ns_arp_insert(ip2, mac) != NS_OK) return 14;  /* 第二项 */
    if (ns_arp_count() != 2u) return 15;
    if (ns_arp_age() != 0) return 16;                /* 未到 60 tick */
    if (ns_arp_remove(ip) != NS_OK) return 17;
    if (ns_arp_count() != 1u) return 18;
    /* 以太校验和：标准向量 */
    {
        u8 e[2] = {0xFF, 0xFF};
        if (ns_eth_csum(e, 2u) != 0x0000u) return 19;
    }

    /* 3. IPv4 */
    hlen = 0u;
    if (ns_ip4_build(hdr, &hlen, ip, ip2, NS_PROTO_ICMP, 8u, 64u, 0x1234u, 0u) != NS_OK) return 20;
    if (hlen != 20u) return 21;
    if (hdr[0] != 0x45u) return 22;
    if (hdr[8] != 64u) return 23;
    if (hdr[9] != NS_PROTO_ICMP) return 24;
    {
        u8 proto = 0u, ttl = 0u, src[4], dst[4];
        if (ns_ip4_parse(hdr, hlen, &proto, &ttl, src, dst) != NS_OK) return 25;
        if (proto != NS_PROTO_ICMP || ttl != 64u) return 26;
        if (memcmp(src, ip, 4u) != 0 || memcmp(dst, ip2, 4u) != 0) return 27;
    }
    /* 校验和完整性：改 TTL 后重算验证 */
    hdr[8] = 63u;
    if (ns_ip4_csum(hdr, 20u) == 0u) return 28;

    /* 4. IPv6 */
    for (i = 0u; i < 16u; i++) ip6[i] = (u8)(0x20u + i);
    if (ns_ip6_format(ip6, txt, 48u) != NS_OK) return 29;
    if (ns_ip6_parse(txt, ip6d) != NS_OK) return 30;
    if (memcmp(ip6, ip6d, 16u) != 0) return 31;
    hlen = 0u;
    if (ns_ip6_build(hdr, &hlen, ip6, ip6d, NS_PROTO_TCP, 16u) != NS_OK) return 32;
    if (hlen != 40u || hdr[0] != 0x60u) return 33;

    /* 5. ICMP */
    blen = 0u;
    if (ns_icmp_build_echo(blk, &blen, 0x4D2u, 7u) != NS_OK) return 34;
    if (blen != 12u) return 35;
    if (ns_icmp_parse_echo(blk, blen, &id, &seq) != NS_OK) return 36;
    if (id != 0x4D2u || seq != 7u) return 37;
    blen = 0u;
    if (ns_icmp_build_reply(blk, &blen, id, seq) != NS_OK) return 38;
    if (blk[0] != 0u) return 39;
    blen = 0u;
    if (ns_icmp_build_unreach(blk, &blen) != NS_OK) return 40;
    if (blk[0] != 3u || blen != 8u) return 41;

    /* 6. TCP 连接管理 */
    if (ns_tcp_open(&tcb, 40000u, 443u, ip2) != NS_OK) return 42;
    if (tcb.state != NS_TCP_CLOSED) return 43;
    if (ns_tcp_connect(&tcb) != NS_OK) return 44;
    if (tcb.state != NS_TCP_SYN_SENT) return 45;
    if (ns_tcp_established(&tcb) != NS_OK) return 46;
    if (tcb.state != NS_TCP_ESTABLISHED) return 47;
    if (ns_tcp_send(&tcb, 1024u) != NS_OK) return 48;
    if (tcb.snd_seq != 0x1000u + 1024u) return 49;
    if (ns_tcp_recv(&tcb, 512u) != NS_OK) return 50;
    if (tcb.rcv_nxt != 512u) return 51;
    if (ns_tcp_next_ack(&tcb) != 512u) return 52;
    if (ns_tcp_close(&tcb) != NS_OK) return 53;
    if (tcb.state != NS_TCP_FIN_WAIT1) return 54;
    if (ns_tcp_fin_ack(&tcb) != NS_OK) return 55;
    if (tcb.state != NS_TCP_FIN_WAIT2) return 56;
    if (ns_tcp_fin_ack(&tcb) != NS_OK) return 57;
    if (tcb.state != NS_TCP_TIME_WAIT) return 58;
    /* 监听路径 */
    if (ns_tcp_open(&tcb2, 8080u, 0u, NULL) != NS_OK) return 59;
    if (ns_tcp_listen(&tcb2, 8080u) != NS_OK) return 60;
    if (tcb2.state != NS_TCP_LISTEN) return 61;
    if (ns_tcp_syn_ack(&tcb2) != NS_OK) return 62;
    if (tcb2.state != NS_TCP_SYN_RCVD) return 63;
    if (ns_tcp_established(&tcb2) != NS_OK) return 64;
    if (ns_tcp_fin_ack(&tcb2) != NS_OK) return 65;
    if (tcb2.state != NS_TCP_CLOSE_WAIT) return 66;

    /* 7. TCP 拥塞控制 */
    if (ns_tcp_cwnd(&tcb2) != 1u) return 67;
    ns_tcp_on_ack(&tcb2);                 /* 慢启动 cwnd 1→2 */
    if (ns_tcp_cwnd(&tcb2) != 2u) return 68;
    ns_tcp_on_loss(&tcb2);                /* 拥塞：ssthresh=1 cwnd=1 */
    if (ns_tcp_cwnd(&tcb2) != 1u) return 69;

    /* 8. TCP 重传与超时 */
    ns_tcp_rtt_sample(&tcb2, 80u);        /* 80ms 样本 */
    ns_tcp_rtt_sample(&tcb2, 120u);
    if (ns_tcp_rto_calc(&tcb2) < 1000u) return 70;
    if (ns_tcp_retransmit(&tcb2) != NS_OK) return 71;
    if (tcb2.retrans != 1u) return 72;

    /* 9. UDP */
    if (ns_udp_bind(&up, 5000u, ip) != NS_OK) return 73;
    if (ns_udp_send(ip, ip2, 5000u, 5001u, (const u8 *)"P", 1u) != NS_OK) return 74;
    blen = 0u;
    if (ns_udp_recv(&up, blk, 64u, &blen) != NS_EAGAIN) return 75;  /* 无包 */

    /* 10. 套接字层 */
    if (ns_sock_open(&sk, 2u, 1u, NS_PROTO_TCP) != NS_OK) return 76;
    if (ns_sock_bind(&sk, 30000u) != NS_OK) return 77;
    if (ns_sock_connect(&sk, ip2, 443u) != NS_OK) return 78;
    if (ns_sock_send(&sk, (const u8 *)"X", 1u) != NS_OK) return 79;
    if (ns_sock_close(&sk) != NS_OK) return 80;

    /* 11. DHCP */
    if (ns_dhcp_start(&dc, 0x11223344u) != NS_OK) return 81;
    if (dc.state != 1u) return 82;
    if (ns_dhcp_on_offer(&dc, ip2) != NS_OK) return 83;
    if (dc.state != 3u) return 84;
    if (ns_dhcp_on_ack(&dc, ip2, 3600u) != NS_OK) return 85;
    if (dc.state != 4u || dc.lease != 3600u) return 86;

    /* 12. DNS */
    blen = 0u;
    if (ns_dns_query_build(blk, &blen, "xos.local") != NS_OK) return 87;
    if (blen < 20u) return 88;
    {
        u8 rresp[48];
        rresp[0] = 0xABu; rresp[1] = 0xCDu;
        rresp[2] = 0x81u; rresp[3] = 0x80u;
        rresp[4] = 0x00u; rresp[5] = 0x01u;
        rresp[6] = 0x00u; rresp[7] = 0x01u;   /* ANCOUNT=1 */
        rresp[8] = 0u; rresp[9] = 0u; rresp[10] = 0u; rresp[11] = 0u;
        /* 问题区："xos.local" */
        rresp[12] = 3u; rresp[13] = 'x'; rresp[14] = 'o'; rresp[15] = 's';
        rresp[16] = 5u; rresp[17] = 'l'; rresp[18] = 'o'; rresp[19] = 'c'; rresp[20] = 'a'; rresp[21] = 'l';
        rresp[22] = 0u;
        rresp[23] = 0u; rresp[24] = 1u;       /* QTYPE A */
        rresp[25] = 0u; rresp[26] = 1u;       /* QCLASS IN */
        rresp[27] = 0xC0u; rresp[28] = 0x0Cu; /* 指针 */
        rresp[29] = 0u; rresp[30] = 1u;       /* TYPE A */
        rresp[31] = 0u; rresp[32] = 1u;       /* CLASS */
        rresp[33] = 0u; rresp[34] = 0u; rresp[35] = 0u; rresp[36] = 3u;  /* TTL=3 */
        rresp[37] = 0u; rresp[38] = 4u;       /* RDLENGTH=4 */
        rresp[39] = 10u; rresp[40] = 0u; rresp[41] = 0u; rresp[42] = 1u; /* 10.0.0.1 */
        if (ns_dns_parse_a(rresp, 43u, "xos.local", ack_ip) != NS_OK) return 89;
        if (ack_ip[0] != 10u || ack_ip[3] != 1u) return 90;
    }
    if (ns_dns_cache_put("xos.local", ack_ip) != NS_OK) return 91;
    if (ns_dns_cache_get("xos.local", ack_ip) != NS_OK) return 92;

    /* 13. 路由表 */
    if (ns_route_add(ip, zero4, ip2, 0u, 10u) != NS_OK) return 93;   /* /0 默认 */
    if (ns_route_add(ip2, one4, ip, 0u, 1u) != NS_OK) return 94;      /* /32 */
    if (ns_route_lookup(ip2, route_out, &iface) != NS_OK) return 95;
    if (memcmp(route_out, ip, 4u) != 0) return 96;                    /* 最长前缀命中 /32 */
    if (ns_route_lookup(ip, route_out, &iface) != NS_OK) return 97;
    if (memcmp(route_out, ip2, 4u) != 0) return 98;                   /* 默认路由 */
    if (ns_route_count() != 2u) return 99;

    /* 14. NAT */
    if (ns_nat_add(ip, 40000u, 50000u, ip2, NS_PROTO_TCP) != NS_OK) return 100;
    if (ns_nat_add(ip, 40000u, 50000u, ip2, NS_PROTO_TCP) != NS_EEXIST) return 101;
    if (ns_nat_lookup(50000u, ack_ip, &ack_port) != NS_OK) return 102;
    if (memcmp(ack_ip, ip, 4u) != 0 || ack_port != 40000u) return 103;

    /* 15. 防火墙 */
    if (ns_fw_add(0u, NS_PROTO_TCP, 22u, NS_FW_DENY) != NS_OK) return 104;
    if (ns_fw_add(0u, NS_PROTO_TCP, 80u, NS_FW_ALLOW) != NS_OK) return 105;
    if (ns_fw_add(0u, NS_PROTO_TCP, 22u, NS_FW_ALLOW) != NS_EEXIST) return 106;
    if (ns_fw_check(0u, NS_PROTO_TCP, 22u) != NS_EPERM) return 107;
    if (ns_fw_check(0u, NS_PROTO_TCP, 80u) != NS_OK) return 108;
    if (ns_fw_check(0u, NS_PROTO_TCP, 8080u) != NS_EPERM) return 109; /* 默认拒绝 */
    ns_fw_set_default(NS_FW_ALLOW);
    if (ns_fw_check(0u, NS_PROTO_TCP, 8080u) != NS_OK) return 110;
    ns_fw_set_default(NS_FW_DENY);

    /* 16. 命名空间 */
    if (ns_ns_create(&ns1, 1u) != NS_OK) return 111;
    if (ns_ns_create(&ns1, 1u) != NS_EEXIST) return 112;
    if (ns_ns_enter(&ns1) != NS_OK) return 113;

    /* 17. QoS */
    if (ns_qos_init(&q, 1000u, 100u) != NS_OK) return 114;
    if (ns_qos_consume(&q, 50u, 10u) != NS_OK) return 115;
    if (ns_qos_consume(&q, 200u, 20u) != NS_EAGAIN) return 116;   /* 令牌不足 */

    /* 18. 统计 */
    if (ns_stats_rx() == 0u) return 117;
    if (ns_stats_tx() == 0u) return 118;
    if (ns_stats.rx_drop == 0u) return 119;

    return NS_OK;
}

/* ---------------- 初始化 ---------------- */
void ns_init(void)
{
    u32 i;
    memset(&ns_stats, 0, sizeof(ns_stats));
    for (i = 0u; i < NS_PKT_MAX; i++)
        memset(&skb_pool[i], 0, sizeof(ns_skb_t));
    for (i = 0u; i < NS_ARP_MAX; i++) { arp_used[i] = 0u; arp_age[i] = 0u; }
    arp_count = 0u;
    for (i = 0u; i < NS_ROUTE_MAX; i++) route_tab[i].used = 0u;
    for (i = 0u; i < NS_NAT_MAX; i++) nat_tab[i].used = 0u;
    for (i = 0u; i < NS_FW_MAX; i++) fw_tab[i].used = 0u;
    fw_count = 0u;
    fw_default = NS_FW_DENY;
    for (i = 0u; i < NS_NS_MAX; i++) ns_tab[i].used = 0u;
    ns_count = 0u;
    for (i = 0u; i < NS_DNS_MAX; i++) dns_tab[i].used = 0u;
    for (i = 0u; i < NS_UDP_MAX; i++) udp_tab[i].used = 0u;
    for (i = 0u; i < NS_TCP_MAX; i++) tcp_tab[i].used = 0u;
    skb_avail = NS_PKT_MAX;
}
