/* ============================================================================
 * XOS 网络协议栈子系统头文件（第 18 册 · 网络协议栈）
 * 完全自研：sk_buff 缓冲池、Ethernet/ARP、IPv4/IPv6、ICMP、TCP 连接管理
 * （状态机/拥塞控制/重传超时）、UDP、套接字层、DHCP、DNS、路由与转发、
 * NAT、防火墙包过滤、网络命名空间、QoS、统计诊断、协议栈自检。
 * 不依赖任何外部网络协议栈/闭源方案。
 * ========================================================================== */
#ifndef XOS_NETSTACK_H
#define XOS_NETSTACK_H

#include "types.h"

#define NS_ETH_ALEN   6
#define NS_IP4_ALEN   4
#define NS_IP6_ALEN   16
#define NS_ARP_MAX    16             /* ARP 缓存条目数 */
#define NS_TCP_MAX    8              /* TCB 表项数 */
#define NS_UDP_MAX    8              /* UDP 端口表项数 */
#define NS_SOCK_MAX   16             /* 套接字表项数 */
#define NS_ROUTE_MAX  16             /* 路由表项数 */
#define NS_NAT_MAX    8              /* NAT 映射项数 */
#define NS_FW_MAX     16             /* 防火墙规则数 */
#define NS_NS_MAX     4              /* 网络命名空间数 */
#define NS_DNS_MAX    8              /* DNS 缓存条目数 */
#define NS_PKT_MAX    8              /* sk_buff 池容量 */

/* 返回码 */
#define NS_OK          0
#define NS_ENOMEM      (-1)
#define NS_EINVAL      (-2)
#define NS_ENOENT      (-3)
#define NS_EBUSY       (-4)
#define NS_EEXIST      (-5)
#define NS_EAGAIN      (-6)
#define NS_EFULL       (-7)
#define NS_ENETUNREACH (-8)
#define NS_EPERM       (-11)
#define NS_ECONNREFUSED (-9)
#define NS_ETIMEDOUT   (-10)

/* 协议号 */
#define NS_PROTO_ICMP   1u
#define NS_PROTO_TCP    6u
#define NS_PROTO_UDP    17u
#define NS_PROTO_IPV6   41u

/* ===== sk_buff（数据包缓冲） ===== */
typedef struct {
    u8  buf[256];                   /* 内联缓冲（模型池） */
    u8 *head;                       /* 起始 */
    u8 *data;                       /* 当前数据起点 */
    u8 *tail;                       /* 当前数据终点 */
    u8 *end;                        /* 缓冲终点 */
    u32 len;                        /* 有效数据长度 */
    u16 dev;                        /* 关联设备索引 */
    u16 used;
} ns_skb_t;

ns_skb_t *ns_skb_alloc(u32 size);
void      ns_skb_free(ns_skb_t *b);
u8       *ns_skb_put(ns_skb_t *b, u32 len);    /* 尾部追加数据 */
u8       *ns_skb_push(ns_skb_t *b, u32 len);   /* 头部前推数据 */
int       ns_skb_reserve(ns_skb_t *b, u32 len);
u32       ns_skb_pool_avail(void);

/* ===== 链路层 Ethernet / ARP ===== */
u16 ns_eth_csum(const u8 *data, u32 len);      /* 16 位补码校验和 */
int ns_arp_insert(const u8 ip4[4], const u8 mac[6]);
int ns_arp_lookup(const u8 ip4[4], u8 mac[6]);
int ns_arp_remove(const u8 ip4[4]);
int ns_arp_age(void);                            /* 老化扫描，返回失效数 */
u32 ns_arp_count(void);

/* ===== IPv4 / IPv6 ===== */
u16 ns_ip4_csum(const u8 *hdr, u32 len);        /* IP 头校验和 */
int ns_ip4_build(u8 *out, u32 *out_len,
                 const u8 src[4], const u8 dst[4], u16 proto, u32 payload_len,
                 u8 ttl, u16 id, u16 frag_off);
int ns_ip4_parse(const u8 *pkt, u32 len, u8 *proto, u8 *ttl, u8 src[4], u8 dst[4]);
int ns_ip6_format(const u8 addr[16], char *out, u32 out_sz);  /* 文本表示 */
int ns_ip6_parse(const char *s, u8 addr[16]);                 /* 文本 → 16B */
int ns_ip6_build(u8 *out, u32 *out_len,
                 const u8 src[16], const u8 dst[16], u16 proto, u32 payload_len);

/* ===== ICMP ===== */
int ns_icmp_build_echo(u8 *out, u32 *out_len, u16 id, u16 seq);   /* echo 请求 */
int ns_icmp_parse_echo(const u8 *pkt, u32 len, u16 *id, u16 *seq);/* echo 请求解析 */
int ns_icmp_build_reply(u8 *out, u32 *out_len, u16 id, u16 seq);  /* echo 应答 */
int ns_icmp_build_unreach(u8 *out, u32 *out_len);                 /* 不可达 */

/* ===== TCP 连接管理 ===== */
#define NS_TCP_CLOSED       0u
#define NS_TCP_LISTEN       1u
#define NS_TCP_SYN_SENT     2u
#define NS_TCP_SYN_RCVD     3u
#define NS_TCP_ESTABLISHED  4u
#define NS_TCP_FIN_WAIT1    5u
#define NS_TCP_FIN_WAIT2    6u
#define NS_TCP_CLOSE_WAIT   7u
#define NS_TCP_TIME_WAIT    8u

typedef struct {
    u16 state;
    u16 local_port;
    u16 remote_port;
    u8  local_ip[4];
    u8  remote_ip[4];
    u32 snd_seq;
    u32 rcv_nxt;
    u32 cwnd;                /* 拥塞窗口 */
    u32 ssthresh;            /* 慢启动阈值 */
    u32 rtt_srtt;            /* 平滑 RTT（毫秒×8） */
    u32 rtt_rttvar;
    u32 rto;                 /* 重传超时（毫秒） */
    u32 retrans;             /* 重传计数 */
    u16 inflight;
    u16 used;
} tcp_tcb_t;

int ns_tcp_open(tcp_tcb_t *tcb, u16 local_port, u16 remote_port,
                const u8 rip[4]);
int ns_tcp_listen(tcp_tcb_t *tcb, u16 port);
int ns_tcp_connect(tcp_tcb_t *tcb);            /* 发起 SYN */
int ns_tcp_syn_ack(tcp_tcb_t *tcb);            /* 收到 SYN 回 SYN+ACK */
int ns_tcp_established(tcp_tcb_t *tcb);        /* 收到 ACK 建立 */
int ns_tcp_send(tcp_tcb_t *tcb, u32 len);      /* 更新序列号 */
int ns_tcp_recv(tcp_tcb_t *tcb, u32 len);      /* 更新 rcv_nxt */
int ns_tcp_close(tcp_tcb_t *tcb);              /* 发起 FIN */
int ns_tcp_fin_ack(tcp_tcb_t *tcb);            /* 收到 FIN 回 ACK */
u32 ns_tcp_next_ack(tcp_tcb_t *tcb);
u32 ns_tcp_find_free(void);

/* ===== TCP 拥塞控制 ===== */
void ns_tcp_cwnd_init(tcp_tcb_t *tcb);         /* cwnd=1 ssthresh=64 */
void ns_tcp_on_ack(tcp_tcb_t *tcb);            /* 慢启动/拥塞避免 */
void ns_tcp_on_loss(tcp_tcb_t *tcb);           /* 拥塞时 ssthresh=cwnd/2 */
u32  ns_tcp_cwnd(tcp_tcb_t *tcb);

/* ===== TCP 重传与超时 ===== */
void ns_tcp_rtt_sample(tcp_tcb_t *tcb, u32 sample_ms);  /* SRTT/RTTVAR */
u32  ns_tcp_rto_calc(tcp_tcb_t *tcb);                   /* RTO = SRTT + 4*RTTVAR */
int  ns_tcp_retransmit(tcp_tcb_t *tcb);                 /* 重传计数+1 */

/* ===== UDP ===== */
typedef struct {
    u16 port;
    u8  bind_ip[4];
    u16 rx_count;
    u16 used;
} udp_port_t;

int ns_udp_bind(udp_port_t *p, u16 port, const u8 ip[4]);
int ns_udp_send(const u8 src[4], const u8 dst[4], u16 sport, u16 dport,
                const u8 *payload, u32 len);
int ns_udp_recv(udp_port_t *p, u8 *out, u32 max, u32 *out_len);

/* ===== 套接字层 ===== */
typedef struct {
    u16 family;              /* 2=IPv4 10=IPv6 */
    u16 type;                /* 1=流 2=数据报 */
    u16 proto;
    u16 state;
    u16 local_port;
    u16 remote_port;
    u8  remote_ip[4];
    u16 used;
} ns_sock_t;

int ns_sock_open(ns_sock_t *s, u16 family, u16 type, u16 proto);
int ns_sock_bind(ns_sock_t *s, u16 port);
int ns_sock_connect(ns_sock_t *s, const u8 ip[4], u16 port);
int ns_sock_send(ns_sock_t *s, const u8 *data, u32 len);
int ns_sock_recv(ns_sock_t *s, u8 *out, u32 max, u32 *out_len);
int ns_sock_close(ns_sock_t *s);

/* ===== DHCP 客户端 ===== */
typedef struct {
    u16 state;               /* 0=init 1=discover 2=offer 3=request 4=bound */
    u32 xid;
    u8  offer_ip[4];
    u8  server_ip[4];
    u32 lease;               /* 租约秒数 */
    u16 used;
} dhcp_cli_t;

int ns_dhcp_start(dhcp_cli_t *c, u32 xid);            /* → DISCOVER */
int ns_dhcp_on_offer(dhcp_cli_t *c, const u8 ip[4]);  /* → REQUEST */
int ns_dhcp_on_ack(dhcp_cli_t *c, const u8 ip[4], u32 lease);  /* → BOUND */

/* ===== DNS 解析 ===== */
typedef struct {
    char  name[32];
    u8    ip[4];
    u16   used;
    u16   age;
} dns_cache_t;

int ns_dns_query_build(u8 *out, u32 *out_len, const char *name);  /* A 查询 */
int ns_dns_parse_a(const u8 *resp, u32 len, const char *name, u8 ip[4]);
int ns_dns_cache_put(const char *name, const u8 ip[4]);
int ns_dns_cache_get(const char *name, u8 ip[4]);

/* ===== 路由表与转发 ===== */
typedef struct {
    u8  dst[4];
    u8  mask[4];
    u8  gw[4];
    u16 iface;
    u16 metric;
    u16 used;
} route_entry_t;

int ns_route_add(const u8 dst[4], const u8 mask[4], const u8 gw[4],
                 u16 iface, u16 metric);
int ns_route_lookup(const u8 ip[4], u8 gw[4], u16 *iface);
int ns_route_default(const u8 gw[4], u16 iface);
u32 ns_route_count(void);

/* ===== NAT ===== */
typedef struct {
    u8  inner_ip[4];
    u16 inner_port;
    u16 outer_port;
    u8  outer_ip[4];
    u16 proto;
    u16 used;
} nat_entry_t;

int ns_nat_add(const u8 inner_ip[4], u16 inner_port, u16 outer_port,
               const u8 outer_ip[4], u16 proto);
int ns_nat_lookup(u16 outer_port, u8 inner_ip[4], u16 *inner_port);

/* ===== 防火墙包过滤 ===== */
#define NS_FW_ALLOW  1u
#define NS_FW_DENY   0u
typedef struct {
    u16 dir;                 /* 0=入 1=出 */
    u16 proto;
    u16 dport;               /* 0=任意 */
    u16 action;              /* allow/deny */
    u16 used;
} fw_rule_t;

int ns_fw_add(u16 dir, u16 proto, u16 dport, u16 action);
int ns_fw_check(u16 dir, u16 proto, u16 dport);     /* 默认拒绝 */
void ns_fw_set_default(u16 action);

/* ===== 网络命名空间 ===== */
typedef struct {
    u32 ns_id;
    u32 route_gen;           /* 本 ns 路由版本 */
    u32 fw_gen;              /* 本 ns 防火墙版本 */
    u16 used;
} ns_ns_t;

int ns_ns_create(ns_ns_t *ns, u32 id);
int ns_ns_enter(ns_ns_t *ns);

/* ===== 流量控制与 QoS ===== */
typedef struct {
    u32 rate;                /* 字节/秒 */
    u32 burst;               /* 桶容量 */
    u32 tokens;              /* 当前令牌 */
    u32 last_refill;         /* 上次填充 tick */
    u16 used;
} qos_tbf_t;

int ns_qos_init(qos_tbf_t *q, u32 rate, u32 burst);
int ns_qos_consume(qos_tbf_t *q, u32 bytes, u32 now);  /* 1=放行 0=限速 */

/* ===== 统计与诊断 ===== */
typedef struct {
    u32 rx_pkts, tx_pkts;
    u32 rx_drop, tx_drop;
    u32 err_count;
    u32 arp_hits, route_hits;
    u32 tcp_conns, udp_pkts;
} ns_stats_t;

u32 ns_stats_rx(void);
u32 ns_stats_tx(void);
void ns_stats_inc_rx(void);
void ns_stats_inc_tx(void);
void ns_stats_inc_drop(void);
void ns_stats_inc_err(void);
void ns_stats_inc_arp_hit(void);
void ns_stats_inc_route_hit(void);
void ns_stats_inc_tcp(void);
void ns_stats_inc_udp(void);
void ns_dump(void);

/* ===== 初始化和自检 ===== */
void ns_init(void);
int  ns_selftest(void);      /* 覆盖 20 子域的内嵌自检 */

#endif /* XOS_NETSTACK_H */
