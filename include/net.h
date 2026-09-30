/* ============================================================================
 * XOS 网络设备驱动核心头文件（第 17 册 · 设备驱动 · 网络）
 * 完全自研：PCI 总线枚举（0xCF8/0xCFC 扫描 82540EM/82545EM/PCnet/RTL8139）+
 * 以太网控制器驱动（设备描述符模型）+ DMA 描述符环（收发）+ 收发缓冲区池 +
 * 中断处理入口 + 轮询模式 NAPI + 网卡多队列 + 固件镜像加载（校验和验证）+
 * 链路状态检测 + MAC 地址管理 + 混杂模式与过滤规则 + 网卡卸载能力 +
 * Wake-on-LAN 省电 + 热插拔 + 统计诊断 + 错误恢复与复位 + 性能调优 +
 * 蓝牙 HCI 控制器模型 + 虚拟网卡 veth/tap 对（自研环回）。
 * 不依赖任何外部网络核心；全部为自研实现。
 * ========================================================================== */
#ifndef XOS_NET_H
#define XOS_NET_H

#include "types.h"

#define NET_MAGIC       0x584E4554u    /* "XNET" */
#define NET_DEV_MAX     8              /* 网卡设备描述符表项数 */
#define NET_NAME_MAX    16
#define NET_RING_DEPTH  32             /* 默认 DMA 描述符环深度 */
#define NET_BUF_SIZE    128             /* 收发缓冲大小（模型数据槽） */
#define NET_Q_MAX       2              /* 多队列数 */
#define NET_FILTER_MAX  8              /* 过滤规则上限 */
#define NET_MAC_LEN     6

/* 设备类型 */
#define NET_DEV_NONE    0u
#define NET_DEV_ETH     1u             /* PCI 以太网控制器 */
#define NET_DEV_BT      2u             /* 蓝牙 HCI 控制器 */
#define NET_DEV_VETH    3u             /* 虚拟网卡（veth/tap） */

/* 链路状态 */
#define NET_LINK_DOWN   0u
#define NET_LINK_NEGO   1u             /* 协商中 */
#define NET_LINK_UP     2u

/* 卸载能力标志 */
#define NET_OFF_IP_CSUM  0x01u
#define NET_OFF_TCP_CSUM 0x02u
#define NET_OFF_UDP_CSUM 0x04u
#define NET_OFF_TSO      0x08u
#define NET_OFF_VLAN     0x10u

/* 错误码 */
#define NET_OK           0
#define NET_ENODEV       (-1)
#define NET_EINVAL       (-2)
#define NET_EBUSY        (-3)
#define NET_EFULL        (-4)
#define NET_EMPTY        (-5)
#define NET_EPERM        (-6)

/* ===== DMA 描述符 ===== */
typedef struct net_desc {
    u32  addr;               /* 缓冲地址（模型） */
    u16  len;                /* 数据长度 */
    u16  flags;              /* bit0=使用中 bit1=完成 bit2=错误 */
} net_desc_t;

/* ===== DMA 描述符环 ===== */
typedef struct net_ring {
    net_desc_t desc[NET_RING_DEPTH];
    u16  head;               /* 生产者 */
    u16  tail;               /* 消费者 */
    u16  count;
    u16  depth;
} net_ring_t;

/* ===== 过滤规则 ===== */
typedef struct net_filter {
    u8   mac[NET_MAC_LEN];
    u8   type;               /* 0=任意 1=单播 2=多播 3=广播 */
} net_filter_t;

/* ===== 网卡设备描述符 ===== */
typedef struct net_dev {
    u32  magic;
    u16  type;               /* NET_DEV_* */
    u16  state;              /* 0=关闭 1=打开 2=挂起 */
    char name[NET_NAME_MAX];
    u16  vendor, device;     /* PCI vendor/device（可探测时） */
    u16  pci_bus, pci_dev, pci_fn;
    u8   mac[NET_MAC_LEN];
    u16  mac_set;
    u16  link;               /* NET_LINK_* */
    u16  speed;              /* Mbps */
    u16  duplex;             /* 0=半 1=全 */
    u16  promisc;            /* 混杂模式 */
    u16  q_count;            /* 多队列数 */
    u16  irq_line;           /* 中断线 */
    u16  wol_enabled;        /* Wake-on-LAN */
    u16  wol_pending;        /* 魔术包待唤醒 */
    u16  present;
    u16  refs;
    u16  napi_budget;        /* NAPI 每次轮询预算 */
    u16  fw_loaded;          /* 固件已加载 */
    u32  err_count;
    u32  reset_count;
    u16  peer_idx;           /* veth 对端（NET_DEV_VETH） */
    u16  bt_state;           /* 蓝牙 HCI 状态 0=reset 1=init 2=ready */
    u16  bt_cmd_q;           /* HCI 命令队列深度 */
    net_ring_t rx_ring[NET_Q_MAX];
    net_ring_t tx_ring[NET_Q_MAX];
    u16  buf_pool_free;
    u16  buf_pool_size;
    net_filter_t filter[NET_FILTER_MAX];
    u16  filter_count;
    u16  offload;            /* 卸载能力标志 */
    /* 统计 */
    u32  rx_packets, tx_packets;
    u32  rx_bytes, tx_bytes;
    u32  rx_dropped, tx_dropped;
    u32  rx_errors, tx_errors;
    u32  rx_irq, tx_irq;
    u32  napi_polls;
} net_dev_t;

/* ===== 全局网络子系统状态 ===== */
typedef struct net_sys {
    u32  magic;
    u32  dev_count;
    u32  probe_hits;         /* PCI 探测命中数 */
    u32  err_total;
    u32  veth_pairs;
    u32  total_rx, total_tx;
    net_dev_t dev[NET_DEV_MAX];
} net_sys_t;

/* ===== API ===== */
void     net_init(void);
int      net_probe_pci(void);             /* PCI 扫描网卡 */
int      net_register(u16 type, const char *name);
int      net_open(u16 idx);
int      net_close(u16 idx);

/* MAC 地址管理 */
int      net_set_mac(u16 idx, const u8 *mac);
int      net_get_mac(u16 idx, u8 *mac);
int      net_mac_valid(const u8 *mac);

/* 链路状态检测 */
int      net_link_set(u16 idx, u16 link, u16 speed, u16 duplex);
u16      net_link_get(u16 idx);

/* 收发路径 */
int      net_xmit(u16 idx, const u8 *data, u16 len, u16 q);
int      net_rx(u16 idx, u8 *data, u16 *len, u16 q);
int      net_rx_inject(u16 idx, const u8 *data, u16 len, u16 q);  /* 模拟收包 */

/* DMA 描述符环 */
int      net_ring_setup(u16 idx, u16 rx_depth, u16 tx_depth);
u16      net_ring_avail_rx(u16 idx, u16 q);
u16      net_ring_avail_tx(u16 idx, u16 q);

/* 中断与 NAPI */
int      net_irq_handle(u16 idx);         /* 中断入口 */
int      net_napi_poll(u16 idx, u16 budget);

/* 混杂模式与过滤 */
int      net_set_promisc(u16 idx, u16 on);
int      net_filter_add(u16 idx, const u8 *mac, u8 type);
int      net_filter_clear(u16 idx);
int      net_filter_match(u16 idx, const u8 *mac, u8 type);

/* 卸载能力 */
int      net_set_offload(u16 idx, u16 bits, u16 on);
u16      net_get_offload(u16 idx);

/* 固件加载 */
int      net_fw_load(u16 idx, const u8 *img, u16 len);
int      net_fw_crc16(const u8 *img, u16 len);

/* Wake-on-LAN 省电 */
int      net_wol_set(u16 idx, u16 on);
int      net_suspend(u16 idx);
int      net_resume(u16 idx);
int      net_wol_magic(u16 idx, const u8 *frame, u16 len); /* 魔术包检测 */

/* 错误恢复与复位 */
int      net_reset(u16 idx);
int      net_err_recover(u16 idx);

/* 热插拔 */
int      net_remove(u16 idx);

/* 虚拟网卡 veth */
int      net_veth_pair(u16 *a, u16 *b);
int      net_veth_send(u16 idx, const u8 *data, u16 len);

/* 蓝牙 HCI 控制器 */
int      net_bt_hci_cmd(u16 idx, u8 opcode, u16 len);
int      net_bt_hci_poll(u16 idx, u8 *evt);
int      net_bt_init(u16 idx);

/* 性能调优 */
int      net_tune(u16 idx, u16 budget);

/* 统计诊断 */
void     net_stats(u16 idx);
void     net_dump(void);

/* 自检（真机可执行） */
int      net_selftest_core(void);         /* PCI 探测/注册/打开/MAC/链路 */
int      net_selftest_ring(void);         /* DMA 环/缓冲池/收发路径 */
int      net_selftest_filt(void);         /* 混杂/过滤/匹配 */
int      net_selftest_offload(void);      /* 卸载能力 */
int      net_selftest_fw(void);           /* 固件加载/CRC */
int      net_selftest_link(void);         /* 链路状态机 */
int      net_selftest_napi(void);         /* 中断/NAPI */
int      net_selftest_wol(void);          /* WOL 省电 */
int      net_selftest_hotplug(void);      /* 热插拔/复位 */
int      net_selftest_veth(void);         /* veth 对 */
int      net_selftest_bt(void);           /* 蓝牙 HCI */
int      net_selftest_tune(void);         /* 性能调优 */

#endif /* XOS_NET_H */
