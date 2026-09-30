/* ============================================================================
 * XOS 网络设备驱动核心实现（第 17 册 · 设备驱动 · 网络）
 * 完全自研：
 *   1) PCI 总线枚举（0xCF8/0xCFC 遍历 bus0 dev0..31 fn0..7，命中 Intel
 *      82540EM=8086:100E / 82545EM=8086:100F / AMD PCnet=1022:2000 /
 *      RTL8139=10EC:8139 并注册以太网控制器设备描述符）；
 *   2) 网卡设备描述符模型（8 槽：类型/状态/MAC/链路/速度/双工/混杂/多队列/
 *      中断线/WOL/卸载能力/固件/蓝牙 HCI/veth 对端/统计）；
 *   3) DMA 描述符环（收发各 32 深 × 2 队列：head/tail/count/desc 状态位）；
 *   4) 收发缓冲区池（模型数据槽 256B×32×2×8 槽，xmit 入环 / rx 出环往返）；
 *   5) 收发中断入口（rx/tx 中断计数）+ 轮询模式 NAPI（预算轮询）；
 *   6) 网卡多队列（Q0/Q1 独立环与计数）、固件镜像加载（magic+len+CRC16 校验）、
 *      链路状态机（down/nego/up + 速率双工）、MAC 地址管理（valid/set/get）、
 *      混杂模式与过滤规则表（8 条 + 类型匹配）、卸载能力标志（IP/TCP/UDP
 *      checksum、TSO、VLAN）、Wake-on-LAN（魔术包 102B 检测 + 省电挂起/恢复）、
 *      热插拔移除、软复位序列、错误恢复清零、性能调优（NAPI 预算/环深）；
 *   7) 蓝牙 HCI 控制器模型（reset/init/ready 状态机 + 命令/事件队列）；
 *   8) 虚拟网卡 veth/tap 对（A→B 环回投递，自研）；
 *   9) 统计诊断 dump、真机自检 12 组。
 * 不依赖任何外部网络核心/闭源方案；全部为自研实现。
 * ========================================================================== */
#include "net.h"
#include "console.h"
#include "string.h"

/* ---------------- 端口 I/O（本文件独立内联，与 irq.c/sound.c 互不干扰） ---------------- */
static inline void outl(u16 port, u32 val)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline u32 inl(u16 port)
{
    u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---------------- 全局状态 ---------------- */
static net_sys_t g_net;

/* 模型数据槽：设备 × 队列 × 环深 × 256B（bss 约 64KB） */
static u8 g_pktbuf[NET_DEV_MAX][NET_Q_MAX][6][NET_BUF_SIZE];   /* 槽深 8->6 压 bss */

/* ---------------- 工具函数 ---------------- */
static void net_name_set(char *dst, const char *src)
{
    u32 i;
    for (i = 0u; i < NET_NAME_MAX - 1u && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static u16 net_find_free(void)
{
    u32 i;
    for (i = 0u; i < NET_DEV_MAX; i++) {
        if (g_net.dev[i].magic != NET_MAGIC)
            return (u16)i;
    }
    return NET_DEV_MAX;
}

static u16 net_find_type(u16 type)
{
    u32 i;
    for (i = 0u; i < NET_DEV_MAX; i++) {
        if (g_net.dev[i].magic == NET_MAGIC && g_net.dev[i].type == type)
            return (u16)i;
    }
    return NET_DEV_MAX;
}

static void net_ring_reset(net_ring_t *r, u16 depth)
{
    r->head = 0u; r->tail = 0u; r->count = 0u; r->depth = depth;
}

/* ---------------- PCI 总线枚举 ---------------- */
static u32 net_pci_cfg_read(u8 bus, u8 dev, u8 fn, u8 reg)
{
    outl(0xCF8u, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
         ((u32)fn << 8) | (u32)(reg & 0xFCu));
    return inl(0xCFCu);
}

int net_probe_pci(void)
{
    u32 b, d, f, id, found = 0u;
    for (b = 0u; b < 1u && found == 0u; b++) {
        for (d = 0u; d < 32u; d++) {
            for (f = 0u; f < 8u; f++) {
                id = net_pci_cfg_read((u8)b, (u8)d, (u8)f, 0u);
                if (id == 0xFFFFFFFFu || id == 0u) continue;
                {
                    u16 vend = (u16)(id & 0xFFFFu);
                    u16 devi = (u16)(id >> 16);
                    if ((vend == 0x8086u && (devi == 0x100Eu || devi == 0x100Fu)) ||
                        (vend == 0x10ECu && devi == 0x8139u) ||
                        (vend == 0x1022u && devi == 0x2000u)) {
                        u16 idx = net_find_free();
                        if (idx < NET_DEV_MAX) {
                            net_dev_t *nd = &g_net.dev[idx];
                            nd->magic = NET_MAGIC;
                            nd->type = NET_DEV_ETH;
                            nd->state = 0u;
                            net_name_set(nd->name, "pci-eth");
                            nd->vendor = vend;
                            nd->device = devi;
                            nd->pci_bus = b; nd->pci_dev = d; nd->pci_fn = f;
                            nd->irq_line = (u16)d;
                            nd->link = NET_LINK_DOWN;
                            nd->speed = 0u;
                            nd->duplex = 1u;
                            nd->promisc = 0u;
                            nd->q_count = NET_Q_MAX;
                            nd->wol_enabled = 0u;
                            nd->wol_pending = 0u;
                            nd->offload = 0u;
                            nd->fw_loaded = 0u;
                            nd->mac_set = 0u;
                            nd->refs = 0u;
                            nd->napi_budget = 16u;
                            nd->present = 1u;
                            nd->buf_pool_size = (u16)(NET_RING_DEPTH * 2u);
                            nd->buf_pool_free = nd->buf_pool_size;
                            net_ring_reset(&nd->rx_ring[0], NET_RING_DEPTH);
                            net_ring_reset(&nd->tx_ring[0], NET_RING_DEPTH);
                            net_ring_reset(&nd->rx_ring[1], NET_RING_DEPTH);
                            net_ring_reset(&nd->tx_ring[1], NET_RING_DEPTH);
                            g_net.dev_count++;
                            g_net.probe_hits++;
                            found = 1u;
                        }
                    }
                }
            }
        }
    }
    return (g_net.probe_hits > 0u) ? NET_OK : NET_ENODEV;
}

/* ---------------- 注册 / 打开 / 关闭 ---------------- */
int net_register(u16 type, const char *name)
{
    u16 idx = net_find_free();
    net_dev_t *nd;
    u16 i;
    if (idx >= NET_DEV_MAX) return NET_EFULL;
    if (type != NET_DEV_ETH && type != NET_DEV_BT && type != NET_DEV_VETH)
        return NET_EINVAL;
    nd = &g_net.dev[idx];
    nd->magic = NET_MAGIC;
    nd->type = type;
    nd->state = 0u;
    if (name) net_name_set(nd->name, name);
    else net_name_set(nd->name, "netdev");
    nd->vendor = 0u; nd->device = 0u;
    nd->link = NET_LINK_DOWN;
    nd->speed = 0u;
    nd->duplex = 1u;
    nd->promisc = 0u;
    nd->q_count = 1u;
    nd->irq_line = 0u;
    nd->wol_enabled = 0u;
    nd->wol_pending = 0u;
    nd->offload = 0u;
    nd->fw_loaded = 0u;
    nd->mac_set = 0u;
    for (i = 0u; i < NET_MAC_LEN; i++) nd->mac[i] = 0u;
    nd->refs = 0u;
    nd->napi_budget = 16u;
    nd->present = 1u;
    nd->peer_idx = NET_DEV_MAX;
    nd->bt_state = 0u;
    nd->bt_cmd_q = 0u;
    nd->err_count = 0u;
    nd->reset_count = 0u;
    nd->filter_count = 0u;
    nd->buf_pool_size = (u16)(NET_RING_DEPTH * 2u);
    nd->buf_pool_free = nd->buf_pool_size;
    net_ring_reset(&nd->rx_ring[0], NET_RING_DEPTH);
    net_ring_reset(&nd->tx_ring[0], NET_RING_DEPTH);
    net_ring_reset(&nd->rx_ring[1], NET_RING_DEPTH);
    net_ring_reset(&nd->tx_ring[1], NET_RING_DEPTH);
    g_net.dev_count++;
    return (int)idx;                                /* 成功返回槽位 */
}

int net_open(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->state == 2u) return NET_EBUSY;
    d->state = 1u;
    d->refs++;
    return NET_OK;
}

int net_close(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->refs == 0u) return NET_EBUSY;
    d->refs--;
    if (d->refs == 0u) d->state = 0u;
    return NET_OK;
}

/* ---------------- MAC 地址管理 ---------------- */
int net_mac_valid(const u8 *mac)
{
    u16 i;
    if (!mac) return NET_EINVAL;
    for (i = 0u; i < NET_MAC_LEN; i++) {
        if ((mac[i] & 0x01u) != 0u && i != 0u) {} /* 单播/多播位在第 0 字节 */
    }
    /* 禁止全零与全 FF（多播广播地址需在过滤规则中使用） */
    {
        u16 z = 1u, f = 1u;
        for (i = 0u; i < NET_MAC_LEN; i++) {
            if (mac[i] != 0u) z = 0u;
            if (mac[i] != 0xFFu) f = 0u;
        }
        if (z || f) return NET_EINVAL;
    }
    return NET_OK;
}

int net_set_mac(u16 idx, const u8 *mac)
{
    net_dev_t *d;
    u16 i;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (net_mac_valid(mac) != NET_OK) return NET_EINVAL;
    for (i = 0u; i < NET_MAC_LEN; i++) d->mac[i] = mac[i];
    d->mac_set = 1u;
    return NET_OK;
}

int net_get_mac(u16 idx, u8 *mac)
{
    net_dev_t *d;
    u16 i;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (!mac) return NET_EINVAL;
    if (!d->mac_set) return NET_EBUSY;
    for (i = 0u; i < NET_MAC_LEN; i++) mac[i] = d->mac[i];
    return NET_OK;
}

/* ---------------- 链路状态检测 ---------------- */
int net_link_set(u16 idx, u16 link, u16 speed, u16 duplex)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (link > NET_LINK_UP) return NET_EINVAL;
    if (duplex > 1u) return NET_EINVAL;
    if (link != NET_LINK_DOWN && (speed != 10u && speed != 100u && speed != 1000u))
        return NET_EINVAL;
    d->link = link;
    d->speed = (link == NET_LINK_DOWN) ? 0u : speed;
    d->duplex = duplex;
    return NET_OK;
}

u16 net_link_get(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    return d->link;
}

/* ---------------- 收发路径 ---------------- */
int net_xmit(u16 idx, const u8 *data, u16 len, u16 q)
{
    net_dev_t *d;
    net_ring_t *r;
    net_desc_t *desc;
    u32 slot;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->state != 1u) return NET_EBUSY;
    if (q >= d->q_count) return NET_EINVAL;
    if (!data || len < 14u || len > NET_BUF_SIZE) return NET_EINVAL;
    r = &d->tx_ring[q];
    if (r->count >= r->depth) return NET_EFULL;
    desc = &r->desc[r->head];
    slot = (u32)(r->head & 7u);           /* 模型缓冲 8 槽 */
    memcpy(g_pktbuf[idx][q][slot], data, len);
    desc->addr = (u32)slot;
    desc->len = len;
    desc->flags = 3u;                     /* 使用中 | 完成 */
    r->head = (r->head + 1u) % r->depth;
    r->count++;
    d->tx_packets++;
    d->tx_bytes += len;
    g_net.total_tx++;
    if (d->buf_pool_free > 0u) d->buf_pool_free--;
    /* veth 对端投递 */
    if (d->type == NET_DEV_VETH && d->peer_idx < NET_DEV_MAX) {
        (void)net_rx_inject(d->peer_idx, data, len, 0u);
    }
    return NET_OK;
}

int net_rx_inject(u16 idx, const u8 *data, u16 len, u16 q)
{
    net_dev_t *d;
    net_ring_t *r;
    net_desc_t *desc;
    u32 slot;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (q >= d->q_count) return NET_EINVAL;
    if (!data || len < 14u || len > NET_BUF_SIZE) return NET_EINVAL;
    r = &d->rx_ring[q];
    if (r->count >= r->depth) {
        d->rx_dropped++;
        return NET_EFULL;
    }
    desc = &r->desc[r->head];
    slot = (u32)(r->head & 7u);           /* 模型缓冲 8 槽 */
    memcpy(g_pktbuf[idx][q][slot], data, len);
    desc->addr = (u32)slot;
    desc->len = len;
    desc->flags = 1u;                     /* 使用中 */
    r->head = (r->head + 1u) % r->depth;
    r->count++;
    d->rx_packets++;
    d->rx_bytes += len;
    g_net.total_rx++;
    return NET_OK;
}

int net_rx(u16 idx, u8 *data, u16 *len, u16 q)
{
    net_dev_t *d;
    net_ring_t *r;
    net_desc_t *desc;
    u32 slot;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (q >= d->q_count) return NET_EINVAL;
    if (!data || !len) return NET_EINVAL;
    r = &d->rx_ring[q];
    if (r->count == 0u) return NET_EMPTY;
    desc = &r->desc[r->tail];
    slot = desc->addr;
    memcpy(data, g_pktbuf[idx][q][slot], desc->len);
    *len = desc->len;
    desc->flags = 0u;                     /* 空闲 */
    r->tail = (r->tail + 1u) % r->depth;
    r->count--;
    return NET_OK;
}

/* ---------------- DMA 描述符环 ---------------- */
int net_ring_setup(u16 idx, u16 rx_depth, u16 tx_depth)
{
    net_dev_t *d;
    u16 q;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (rx_depth < 8u || rx_depth > NET_RING_DEPTH) return NET_EINVAL;
    if (tx_depth < 8u || tx_depth > NET_RING_DEPTH) return NET_EINVAL;
    for (q = 0u; q < d->q_count; q++) {
        net_ring_reset(&d->rx_ring[q], rx_depth);
        net_ring_reset(&d->tx_ring[q], tx_depth);
    }
    return NET_OK;
}

u16 net_ring_avail_rx(u16 idx, u16 q)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return 0u;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || q >= d->q_count) return 0u;
    return d->rx_ring[q].count;
}

u16 net_ring_avail_tx(u16 idx, u16 q)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return 0u;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || q >= d->q_count) return 0u;
    return d->tx_ring[q].count;
}

/* ---------------- 中断与 NAPI ---------------- */
int net_napi_poll(u16 idx, u16 budget)
{
    net_dev_t *d;
    u16 q, n = 0u;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (budget == 0u) budget = d->napi_budget;
    for (q = 0u; q < d->q_count; q++) {
        while (n < budget && d->rx_ring[q].count > 0u) {
            d->rx_ring[q].desc[d->rx_ring[q].tail].flags = 0u;
            d->rx_ring[q].tail = (d->rx_ring[q].tail + 1u) % d->rx_ring[q].depth;
            d->rx_ring[q].count--;
            n++;
        }
    }
    d->napi_polls++;
    return (int)n;
}

int net_irq_handle(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    d->rx_irq++;
    d->tx_irq++;
    (void)net_napi_poll(idx, d->napi_budget);
    return NET_OK;
}

/* ---------------- 混杂模式与过滤 ---------------- */
int net_set_promisc(u16 idx, u16 on)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (on > 1u) return NET_EINVAL;
    d->promisc = on;
    return NET_OK;
}

int net_filter_add(u16 idx, const u8 *mac, u8 type)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (type > 3u) return NET_EINVAL;
    if (d->filter_count >= NET_FILTER_MAX) return NET_EFULL;
    if (mac && net_mac_valid(mac) != NET_OK) return NET_EINVAL;
    {
        u16 i;
        net_filter_t *f = &d->filter[d->filter_count];
        if (mac) {
            for (i = 0u; i < NET_MAC_LEN; i++) f->mac[i] = mac[i];
        } else {
            for (i = 0u; i < NET_MAC_LEN; i++) f->mac[i] = 0xFFu;
        }
        f->type = type;
        d->filter_count++;
    }
    return NET_OK;
}

int net_filter_clear(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    d->filter_count = 0u;
    return NET_OK;
}

int net_filter_match(u16 idx, const u8 *mac, u8 type)
{
    net_dev_t *d;
    u16 i;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->promisc) return NET_OK;                    /* 混杂放行 */
    if (type > 3u) return NET_EINVAL;
    if (type == 3u) return NET_OK;                    /* 广播放行（无需 mac） */
    if (!mac) return NET_EINVAL;
    for (i = 0u; i < d->filter_count; i++) {
        net_filter_t *f = &d->filter[i];
        if (f->type == type && memcmp(f->mac, mac, NET_MAC_LEN) == 0)
            return NET_OK;
    }
    return NET_ENODEV;                                /* 不匹配丢弃 */
}

/* ---------------- 卸载能力 ---------------- */
int net_set_offload(u16 idx, u16 bits, u16 on)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (bits == 0u || (bits & ~(NET_OFF_IP_CSUM | NET_OFF_TCP_CSUM |
                                NET_OFF_UDP_CSUM | NET_OFF_TSO | NET_OFF_VLAN)))
        return NET_EINVAL;
    if (on) d->offload |= bits;
    else d->offload &= (u16)~bits;
    return NET_OK;
}

u16 net_get_offload(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return 0u;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return 0u;
    return d->offload;
}

/* ---------------- 固件加载（CRC16 校验） ---------------- */
int net_fw_crc16(const u8 *img, u16 len)
{
    u16 crc = 0xFFFFu;
    u32 i;
    u16 b;
    if (!img) return 0;
    for (i = 0u; i < len; i++) {
        crc ^= (u16)img[i];
        for (b = 0u; b < 6u; b++)
            crc = (crc & 1u) ? (u16)((crc >> 1) ^ 0xA001u) : (u16)(crc >> 1);
    }
    return (int)crc;
}

int net_fw_load(u16 idx, const u8 *img, u16 len)
{
    net_dev_t *d;
    u16 magic, plen, crc, calc;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (!img || len < 6u) return NET_EINVAL;
    magic = (u16)((u16)img[0] | ((u16)img[1] << 8));
    if (magic != 0x5846u) return NET_EINVAL;          /* "XF" */
    plen = (u16)((u16)img[2] | ((u16)img[3] << 8));
    if ((u32)plen + 6u != (u32)len) return NET_EINVAL;
    crc = (u16)((u16)img[len - 2u] | ((u16)img[len - 1u] << 8));
    calc = (u16)net_fw_crc16(img, (u16)(plen + 4u));
    if (calc != crc) return NET_EINVAL;
    d->fw_loaded = 1u;
    return NET_OK;
}

/* ---------------- Wake-on-LAN / 省电 ---------------- */
int net_wol_set(u16 idx, u16 on)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (on > 1u) return NET_EINVAL;
    d->wol_enabled = on;
    if (!on) d->wol_pending = 0u;
    return NET_OK;
}

int net_suspend(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->state != 1u) return NET_EBUSY;
    d->state = 2u;
    return NET_OK;
}

int net_resume(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->state != 2u) return NET_EBUSY;
    d->state = 1u;
    if (d->wol_pending) d->err_count++;    /* 记录 WOL 唤醒 */
    d->wol_pending = 0u;
    return NET_OK;
}

int net_wol_magic(u16 idx, const u8 *frame, u16 len)
{
    net_dev_t *d;
    u16 i;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (!d->wol_enabled) return NET_EPERM;
    if (!frame || len < 102u) return NET_EINVAL;
    for (i = 0u; i < 6u; i++)
        if (frame[i] != 0xFFu) return NET_EINVAL;
    for (i = 0u; i < 16u; i++)
        if (memcmp(frame + 6u + i * 6u, d->mac, NET_MAC_LEN) != 0)
            return NET_EINVAL;
    d->wol_pending = 1u;
    return NET_OK;
}

/* ---------------- 错误恢复与复位 ---------------- */
int net_reset(u16 idx)
{
    net_dev_t *d;
    u16 q;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    for (q = 0u; q < d->q_count; q++) {
        net_ring_reset(&d->rx_ring[q], d->rx_ring[q].depth);
        net_ring_reset(&d->tx_ring[q], d->tx_ring[q].depth);
    }
    d->buf_pool_free = d->buf_pool_size;
    d->reset_count++;
    d->err_count = 0u;
    d->state = 1u;
    return NET_OK;
}

int net_err_recover(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    d->rx_errors = 0u;
    d->tx_errors = 0u;
    d->rx_dropped = 0u;
    d->tx_dropped = 0u;
    d->err_count = 0u;
    return NET_OK;
}

/* ---------------- 热插拔 ---------------- */
int net_remove(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC) return NET_ENODEV;
    if (d->refs > 0u) return NET_EBUSY;
    d->magic = 0u;
    d->present = 0u;
    if (g_net.dev_count > 0u) g_net.dev_count--;
    return NET_OK;
}

/* ---------------- 虚拟网卡 veth ---------------- */
int net_veth_pair(u16 *a, u16 *b)
{
    u16 ia, ib;
    net_dev_t *da, *db;
    u8 maca[NET_MAC_LEN] = {0x02u, 0, 0, 0, 0, 0x01u};
    u8 macb[NET_MAC_LEN] = {0x02u, 0, 0, 0, 0, 0x02u};
    if (!a || !b) return NET_EINVAL;
    ia = (u16)net_register(NET_DEV_VETH, "veth-a");
    if (ia >= NET_DEV_MAX) return NET_EFULL;
    ib = (u16)net_register(NET_DEV_VETH, "veth-b");
    if (ib >= NET_DEV_MAX) return NET_EFULL;
    da = &g_net.dev[ia];
    db = &g_net.dev[ib];
    da->peer_idx = ib;
    db->peer_idx = ia;
    (void)net_set_mac(ia, maca);
    (void)net_set_mac(ib, macb);
    g_net.veth_pairs++;
    *a = ia;
    *b = ib;
    return NET_OK;
}

int net_veth_send(u16 idx, const u8 *data, u16 len)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->type != NET_DEV_VETH) return NET_ENODEV;
    return net_xmit(idx, data, len, 0u);
}

/* ---------------- 蓝牙 HCI 控制器 ---------------- */
int net_bt_init(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->type != NET_DEV_BT) return NET_ENODEV;
    d->bt_state = 1u;                    /* init */
    d->bt_state = 2u;                    /* ready */
    return NET_OK;
}

int net_bt_hci_cmd(u16 idx, u8 opcode, u16 len)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->type != NET_DEV_BT) return NET_ENODEV;
    if (d->bt_state == 0u) return NET_EBUSY;
    if (len > 32u) return NET_EINVAL;
    d->bt_cmd_q++;
    return NET_OK;
}

int net_bt_hci_poll(u16 idx, u8 *evt)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (d->type != NET_DEV_BT) return NET_ENODEV;
    if (d->bt_cmd_q == 0u) return NET_EMPTY;
    d->bt_cmd_q--;
    if (evt) *evt = 0x0Eu;               /* HCI 命令完成事件 */
    return NET_OK;
}

/* ---------------- 性能调优 ---------------- */
int net_tune(u16 idx, u16 budget)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return NET_ENODEV;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC || !d->present) return NET_ENODEV;
    if (budget < 1u || budget > 64u) return NET_EINVAL;
    d->napi_budget = budget;
    return NET_OK;
}

/* ---------------- 统计诊断 ---------------- */
void net_stats(u16 idx)
{
    net_dev_t *d;
    if (idx >= NET_DEV_MAX) return;
    d = &g_net.dev[idx];
    if (d->magic != NET_MAGIC) return;
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  net[");
    con_put_dec(idx);
    con_puts("] ");
    con_puts(d->name);
    con_puts(" rx=");
    con_put_dec(d->rx_packets);
    con_puts("/");
    con_put_dec(d->rx_bytes);
    con_puts("B tx=");
    con_put_dec(d->tx_packets);
    con_puts("/");
    con_put_dec(d->tx_bytes);
    con_puts("B drop=");
    con_put_dec(d->rx_dropped + d->tx_dropped);
    con_puts(" err=");
    con_put_dec(d->err_count);
    con_puts(" napi=");
    con_put_dec(d->napi_polls);
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_putc('\n');
}

void net_dump(void)
{
    u32 i;
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  Network subsystem dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    for (i = 0u; i < NET_DEV_MAX; i++) {
        net_dev_t *d = &g_net.dev[i];
        if (d->magic != NET_MAGIC) continue;
        con_puts("    dev[");
        con_put_dec(i);
        con_puts("] ");
        con_puts(d->name);
        con_puts(" type=");
        con_put_dec(d->type);
        con_puts(" state=");
        con_put_dec(d->state);
        con_puts(" link=");
        con_put_dec(d->link);
        con_puts(" speed=");
        con_put_dec(d->speed);
        con_puts(" q=");
        con_put_dec(d->q_count);
        con_puts(" mac=");
        if (d->mac_set) {
            u16 b;
            for (b = 0u; b < NET_MAC_LEN; b++) {
                con_put_hex8(d->mac[b]);
                if (b < 5u) con_putc('-');
            }
        } else {
            con_puts("n/a");
        }
        con_puts(" off=");
        con_put_hex16(d->offload);
        con_puts(" fw=");
        con_put_dec(d->fw_loaded);
        if (d->vendor) {
            con_puts(" pci=");
            con_put_hex32(((u32)d->device << 16) | d->vendor);
        }
        con_putc('\n');
    }
    con_puts("  [net] dev_count=");
    con_put_dec(g_net.dev_count);
    con_puts(" probe_hits=");
    con_put_dec(g_net.probe_hits);
    con_puts(" total_rx=");
    con_put_dec(g_net.total_rx);
    con_puts(" total_tx=");
    con_put_dec(g_net.total_tx);
    con_puts(" veth_pairs=");
    con_put_dec(g_net.veth_pairs);
    con_puts(" errs=");
    con_put_dec(g_net.err_total);
    con_putc('\n');
}

/* ================= 初始化 ================= */
void net_init(void)
{
    g_net.magic = NET_MAGIC;
    g_net.dev_count = 0u;
    g_net.probe_hits = 0u;
    g_net.err_total = 0u;
    g_net.veth_pairs = 0u;
    g_net.total_rx = 0u;
    g_net.total_tx = 0u;
}

/* ================= 自检 ================= */

/* ---------------- 自检组 1：PCI 探测 / 注册 / 打开 / MAC / 链路 ---------------- */
int net_selftest_core(void)
{
    u16 idx_eth, idx_reg;
    u8 mac[6] = {0x08u, 0x00u, 0x27u, 0x11u, 0x22u, 0x33u};
    u8 macout[6];

    if (g_net.magic != NET_MAGIC) return 1;

    /* PCI 枚举必须命中 VirtualBox 虚拟网卡（82540EM 等） */
    if (g_net.probe_hits == 0u) return 2;
    idx_eth = net_find_type(NET_DEV_ETH);
    if (idx_eth >= NET_DEV_MAX) return 3;

    /* 打开/关闭与引用计数 */
    if (net_open(idx_eth) != NET_OK) return 4;
    if (net_open(idx_eth) != NET_OK) return 5;
    if (g_net.dev[idx_eth].refs != 2u) return 6;
    if (net_close(idx_eth) != NET_OK) return 7;
    if (net_close(idx_eth) != NET_OK) return 8;
    if (net_close(idx_eth) != NET_EBUSY) return 9;   /* 已关闭再关拒绝 */

    /* MAC：非法（全零/全FF）拒绝，合法设置与读取 */
    if (net_mac_valid(mac) != NET_OK) return 10;
    if (net_set_mac(idx_eth, mac) != NET_OK) return 11;
    if (net_get_mac(idx_eth, macout) != NET_OK) return 12;
    if (memcmp(mac, macout, 6) != 0) return 13;
    {
        u8 bad[6] = {0, 0, 0, 0, 0, 0};
        if (net_mac_valid(bad) != NET_EINVAL) return 14;
    }

    /* 链路状态机 */
    if (net_link_set(idx_eth, NET_LINK_UP, 1000u, 1u) != NET_OK) return 15;
    if (net_link_get(idx_eth) != NET_LINK_UP) return 16;
    if (g_net.dev[idx_eth].speed != 1000u) return 17;
    if (net_link_set(idx_eth, NET_LINK_UP, 5u, 1u) != NET_EINVAL) return 18;
    if (net_link_set(idx_eth, NET_LINK_DOWN, 0u, 0u) != NET_OK) return 19;

    /* 普通注册设备 */
    idx_reg = (u16)net_register(NET_DEV_ETH, "test-eth");
    if (idx_reg >= NET_DEV_MAX) return 20;
    return 0u;
}

/* ---------------- 自检组 2：DMA 环 / 缓冲池 / 收发往返 ---------------- */
int net_selftest_ring(void)
{
    u16 idx = net_find_type(NET_DEV_ETH), q;
    u8 pkt[64], out[64];
    u16 len;
    u32 i;

    if (idx >= NET_DEV_MAX) return 1;
    if (net_open(idx) != NET_OK) return 2;

    /* 环参数校验 */
    if (net_ring_setup(idx, 4u, 16u) != NET_EINVAL) return 3;
    if (net_ring_setup(idx, 64u, 16u) != NET_EINVAL) return 4;
    if (net_ring_setup(idx, 16u, 16u) != NET_OK) return 5;

    /* 发送非法参数 */
    if (net_xmit(idx, pkt, 0u, 0u) != NET_EINVAL) return 6;
    if (net_xmit(idx, pkt, 13u, 0u) != NET_EINVAL) return 7;
    if (net_xmit(idx, pkt, 300u, 0u) != NET_EINVAL) return 8;

    /* 构造 64B 以太网帧并发送，队列 0/1 往返读取 */
    for (i = 0u; i < 64u; i++) pkt[i] = (u8)i;
    for (q = 0u; q < NET_Q_MAX; q++) {
        if (net_xmit(idx, pkt, 64u, q) != NET_OK) return 9;
        if (net_ring_avail_tx(idx, q) != 1u) return 10;
    }

    /* 接收环注入并取回（模拟收包路径） */
    for (q = 0u; q < NET_Q_MAX; q++) {
        if (net_rx_inject(idx, pkt, 64u, q) != NET_OK) return 11;
        if (net_ring_avail_rx(idx, q) != 1u) return 12;
        len = 0u;
        if (net_rx(idx, out, &len, q) != NET_OK) return 13;
        if (len != 64u) return 14;
        if (memcmp(pkt, out, 64u) != 0) return 15;
    }

    /* 空接收 → 空错误 */
    if (net_rx(idx, out, &len, 0u) != NET_EMPTY) return 16;

    /* 发送端统计 */
    if (g_net.dev[idx].tx_packets != 2u) return 17;
    if (g_net.dev[idx].rx_packets != 2u) return 18;

    net_close(idx);
    return 0u;
}

/* ---------------- 自检组 3：混杂 / 过滤 ---------------- */
int net_selftest_filt(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    u8 a[6] = {0xAAu, 0, 0, 0, 0, 0x01u};
    u8 b[6] = {0xBBu, 0, 0, 0, 0, 0x02u};

    if (idx >= NET_DEV_MAX) return 1;
    if (net_set_promisc(idx, 2u) != NET_EINVAL) return 2;
    if (net_set_promisc(idx, 1u) != NET_OK) return 3;
    if (g_net.dev[idx].promisc != 1u) return 4;
    /* 混杂放行任意 */
    if (net_filter_match(idx, a, 1u) != NET_OK) return 5;
    if (net_set_promisc(idx, 0u) != NET_OK) return 6;

    /* 规则添加：单播放行 / 其他丢弃 */
    if (net_filter_add(idx, a, 1u) != NET_OK) return 7;
    if (net_filter_add(idx, b, 2u) != NET_OK) return 8;
    if (net_filter_add(idx, (const u8 *)0, 3u) != NET_OK) return 9;
    if (g_net.dev[idx].filter_count != 3u) return 10;
    if (net_filter_match(idx, a, 1u) != NET_OK) return 11;
    if (net_filter_match(idx, b, 1u) != NET_ENODEV) return 12;   /* 类型不匹配 */
    if (net_filter_match(idx, b, 2u) != NET_OK) return 13;
    if (net_filter_match(idx, (const u8 *)0, 3u) != NET_OK) return 14; /* 广播 */
    if (net_filter_match(idx, a, 2u) != NET_ENODEV) return 15;
    if (net_filter_clear(idx) != NET_OK) return 16;
    if (g_net.dev[idx].filter_count != 0u) return 17;
    if (net_filter_match(idx, a, 1u) != NET_ENODEV) return 18;   /* 清空后丢弃 */
    return 0u;
}

/* ---------------- 自检组 4：卸载能力 ---------------- */
int net_selftest_offload(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    if (idx >= NET_DEV_MAX) return 1;
    if (net_set_offload(idx, 0u, 1u) != NET_EINVAL) return 2;
    if (net_set_offload(idx, 0x8000u, 1u) != NET_EINVAL) return 3;
    if (net_set_offload(idx, NET_OFF_IP_CSUM, 1u) != NET_OK) return 4;
    if (net_set_offload(idx, NET_OFF_TCP_CSUM | NET_OFF_UDP_CSUM, 1u) != NET_OK)
        return 5;
    if (net_set_offload(idx, NET_OFF_TSO | NET_OFF_VLAN, 1u) != NET_OK) return 6;
    if (net_get_offload(idx) != (NET_OFF_IP_CSUM | NET_OFF_TCP_CSUM |
                                 NET_OFF_UDP_CSUM | NET_OFF_TSO | NET_OFF_VLAN))
        return 7;
    if (net_set_offload(idx, NET_OFF_TSO, 0u) != NET_OK) return 8;
    if ((net_get_offload(idx) & NET_OFF_TSO) != 0u) return 9;
    return 0u;
}

/* ---------------- 自检组 5：固件加载 ---------------- */
int net_selftest_fw(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    u8 img[32];
    u16 plen = 16u;
    u16 crc;
    u32 i;

    if (idx >= NET_DEV_MAX) return 1;
    if (net_fw_load(idx, (const u8 *)0, 0u) != NET_EINVAL) return 2;
    if (net_fw_load(idx, img, 4u) != NET_EINVAL) return 3;   /* 长度不足 */

    /* 合法镜像：magic "XF"(0x5846) + plen + payload + crc16 */
    img[0] = 0x46u; img[1] = 0x58u;
    img[2] = (u8)(plen & 0xFFu); img[3] = (u8)(plen >> 8);
    for (i = 0u; i < plen; i++) img[4u + i] = (u8)(0xA0u + i);
    crc = (u16)net_fw_crc16(img, (u16)(plen + 4u));
    img[4u + plen] = (u8)(crc & 0xFFu);
    img[5u + plen] = (u8)(crc >> 8);
    if (net_fw_load(idx, img, (u16)(plen + 6u)) != NET_OK) return 4;
    if (g_net.dev[idx].fw_loaded != 1u) return 5;

    /* 坏 magic */
    img[0] = 0x00u; img[1] = 0x58u;
    if (net_fw_load(idx, img, (u16)(plen + 6u)) != NET_EINVAL) return 6;

    /* 坏长度 */
    img[0] = 0x46u; img[1] = 0x58u;
    img[2] = 0xFFu; img[3] = 0xFFu;
    if (net_fw_load(idx, img, (u16)(plen + 6u)) != NET_EINVAL) return 7;

    /* 坏 CRC */
    img[2] = (u8)(plen & 0xFFu); img[3] = (u8)(plen >> 8);
    img[4u + plen] ^= 0x55u;
    if (net_fw_load(idx, img, (u16)(plen + 6u)) != NET_EINVAL) return 8;
    return 0u;
}

/* ---------------- 自检组 6：链路状态机 ---------------- */
int net_selftest_link(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    if (idx >= NET_DEV_MAX) return 1;
    if (net_link_set(idx, NET_LINK_NEGO, 100u, 1u) != NET_OK) return 2;
    if (net_link_get(idx) != NET_LINK_NEGO) return 3;
    if (net_link_set(idx, NET_LINK_DOWN, 0u, 0u) != NET_OK) return 4;
    if (net_link_set(idx, NET_LINK_UP, 10u, 0u) != NET_OK) return 5;
    if (g_net.dev[idx].speed != 10u || g_net.dev[idx].duplex != 0u) return 6;
    if (net_link_set(idx, NET_LINK_UP, 1000u, 1u) != NET_OK) return 7;
    if (net_link_set(idx, 3u, 100u, 1u) != NET_EINVAL) return 8;
    return 0u;
}

/* ---------------- 自检组 7：中断 / NAPI ---------------- */
int net_selftest_napi(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    u8 pkt[32];
    u32 i;
    if (idx >= NET_DEV_MAX) return 1;
    if (net_open(idx) != NET_OK) return 2;
    for (i = 0u; i < 32u; i++) pkt[i] = (u8)(0x40u + i);
    /* 注入 4 包后 NAPI 预算 2 → 两轮清空 */
    if (net_rx_inject(idx, pkt, 32u, 0u) != NET_OK) return 3;
    if (net_rx_inject(idx, pkt, 32u, 0u) != NET_OK) return 4;
    if (net_rx_inject(idx, pkt, 32u, 0u) != NET_OK) return 5;
    if (net_rx_inject(idx, pkt, 32u, 0u) != NET_OK) return 6;
    if (net_ring_avail_rx(idx, 0u) != 4u) return 7;
    if (net_napi_poll(idx, 2u) != 2) return 8;
    if (net_ring_avail_rx(idx, 0u) != 2u) return 9;
    if (net_irq_handle(idx) != NET_OK) return 10;
    if (g_net.dev[idx].rx_irq != 1u) return 11;
    if (net_ring_avail_rx(idx, 0u) != 0u) return 12;
    net_close(idx);
    return 0u;
}

/* ---------------- 自检组 8：Wake-on-LAN / 省电 ---------------- */
int net_selftest_wol(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    u8 frame[102];
    u32 i;
    if (idx >= NET_DEV_MAX) return 1;
    if (net_wol_set(idx, 0u) != NET_OK) return 2;
    /* 未启用 WOL 时魔术包拒绝 */
    if (net_wol_magic(idx, frame, 102u) != NET_EPERM) return 3;
    if (net_wol_set(idx, 1u) != NET_OK) return 4;
    if (!g_net.dev[idx].mac_set) return 5;

    /* 构造合法魔术包：FF*6 + MAC×16 */
    for (i = 0u; i < 6u; i++) frame[i] = 0xFFu;
    for (i = 0u; i < 16u; i++)
        memcpy(frame + 6u + i * 6u, g_net.dev[idx].mac, 6);
    if (net_wol_magic(idx, frame, 102u) != NET_OK) return 6;
    if (g_net.dev[idx].wol_pending != 1u) return 7;

    /* 错误 MAC 拒绝 */
    frame[6u] ^= 0x01u;
    if (net_wol_magic(idx, frame, 102u) != NET_EINVAL) return 8;
    frame[6u] ^= 0x01u;

    /* 长度不足拒绝 */
    if (net_wol_magic(idx, frame, 101u) != NET_EINVAL) return 9;

    /* 省电挂起/恢复 */
    if (net_open(idx) != NET_OK) return 10;
    if (net_suspend(idx) != NET_OK) return 11;
    if (net_open(idx) != NET_EBUSY) return 12;   /* 挂起中不可再开 */
    if (net_resume(idx) != NET_OK) return 13;
    if (g_net.dev[idx].state != 1u) return 14;
    net_close(idx);
    return 0u;
}

/* ---------------- 自检组 9：热插拔 / 复位 / 错误恢复 ---------------- */
int net_selftest_hotplug(void)
{
    u16 idx, idx2;
    u8 mac[6] = {0x02u, 0, 0, 0, 0, 0x77u};
    idx2 = (u16)net_register(NET_DEV_ETH, "hotplug");
    if (idx2 >= NET_DEV_MAX) return 1;
    (void)net_set_mac(idx2, mac);
    if (net_open(idx2) != NET_OK) return 2;
    if (net_remove(idx2) != NET_EBUSY) return 3;   /* 占用中不可拔 */
    net_close(idx2);
    if (net_remove(idx2) != NET_OK) return 4;       /* 关闭后可拔 */
    if (net_open(idx2) != NET_ENODEV) return 5;     /* 拔出后不可用 */

    /* 复位与错误恢复 */
    idx = net_find_type(NET_DEV_ETH);
    if (idx >= NET_DEV_MAX) return 6;
    if (net_open(idx) != NET_OK) return 7;
    g_net.dev[idx].err_count = 5u;
    g_net.dev[idx].rx_errors = 2u;
    if (net_err_recover(idx) != NET_OK) return 8;
    if (g_net.dev[idx].err_count != 0u || g_net.dev[idx].rx_errors != 0u)
        return 9;
    if (net_reset(idx) != NET_OK) return 10;
    if (g_net.dev[idx].reset_count == 0u) return 11;
    if (net_ring_avail_rx(idx, 0u) != 0u) return 12;
    net_close(idx);
    return 0u;
}

/* ---------------- 自检组 10：veth 对 ---------------- */
int net_selftest_veth(void)
{
    u16 a, b;
    u8 pkt[48], out[48];
    u16 len;
    u32 i;
    if (net_veth_pair(&a, &b) != NET_OK) return 1;
    if (a >= NET_DEV_MAX || b >= NET_DEV_MAX) return 2;
    if (g_net.dev[a].peer_idx != b || g_net.dev[b].peer_idx != a) return 3;
    if (net_open(a) != NET_OK) return 4;
    if (net_open(b) != NET_OK) return 5;
    for (i = 0u; i < 48u; i++) pkt[i] = (u8)(0x80u + i);
    if (net_veth_send(a, pkt, 48u) != NET_OK) return 6;
    /* 对端 B 收到 */
    if (net_ring_avail_rx(b, 0u) != 1u) return 7;
    len = 0u;
    if (net_rx(b, out, &len, 0u) != NET_OK) return 8;
    if (len != 48u) return 9;
    if (memcmp(pkt, out, 48u) != 0) return 10;
    /* 反向投递 */
    if (net_veth_send(b, pkt, 48u) != NET_OK) return 11;
    if (net_ring_avail_rx(a, 0u) != 1u) return 12;
    net_close(a);
    net_close(b);
    return 0u;
}

/* ---------------- 自检组 11：蓝牙 HCI ---------------- */
int net_selftest_bt(void)
{
    u16 idx;
    u8 evt;
    idx = (u16)net_register(NET_DEV_BT, "bt0");
    if (idx >= NET_DEV_MAX) return 1;
    if (g_net.dev[idx].bt_state != 0u) return 2;
    if (net_bt_hci_cmd(idx, 0x01u, 0u) != NET_EBUSY) return 3;  /* 未初始化 */
    if (net_bt_init(idx) != NET_OK) return 4;
    if (g_net.dev[idx].bt_state != 2u) return 5;
    if (net_bt_hci_cmd(idx, 0x03u, 0u) != NET_OK) return 6;
    if (net_bt_hci_cmd(idx, 0x05u, 4u) != NET_OK) return 7;
    if (net_bt_hci_cmd(idx, 0x05u, 33u) != NET_EINVAL) return 8;
    if (g_net.dev[idx].bt_cmd_q != 2u) return 9;
    evt = 0u;
    if (net_bt_hci_poll(idx, &evt) != NET_OK) return 10;
    if (evt != 0x0Eu) return 11;
    if (net_bt_hci_poll(idx, &evt) != NET_OK) return 12;
    if (net_bt_hci_poll(idx, &evt) != NET_EMPTY) return 13;
    return 0u;
}

/* ---------------- 自检组 12：性能调优 ---------------- */
int net_selftest_tune(void)
{
    u16 idx = net_find_type(NET_DEV_ETH);
    if (idx >= NET_DEV_MAX) return 1;
    if (net_tune(idx, 0u) != NET_EINVAL) return 2;
    if (net_tune(idx, 65u) != NET_EINVAL) return 3;
    if (net_tune(idx, 32u) != NET_OK) return 4;
    if (g_net.dev[idx].napi_budget != 32u) return 5;
    if (net_ring_setup(idx, 32u, 8u) != NET_OK) return 6;
    if (g_net.dev[idx].rx_ring[0].depth != 32u) return 7;
    if (g_net.dev[idx].tx_ring[0].depth != 8u) return 8;
    return 0u;
}
