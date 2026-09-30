/* ============================================================================
 * XOS 内存探测与统计（Memory Detection & Statistics）
 *
 * 对应第 04 册「内存探测与统计」的 20 个子域：
 *   S01 E820 条目类型识别      S02 可用内存容量统计     S03 保留内存统计
 *   S04 ACPI 区域识别          S05 低于 1MB 内存统计    S06 内存映射排序
 *   S07 相邻条目合并           S08 内存空洞识别         S09 物理地址空间上限探测
 *   S10 内存热插拔区域探测     S11 SMBIOS 内存条信息读取 S12 内存速率与时序读取
 *   S13 ECC 能力探测           S14 内存映射持久化       S15 内存映射可视化输出
 *   S16 探测异常与容错         S17 探测结果校验         S18 内存统计接口
 *   S19 内存使用率监控         S20 内存映射与内核参数对齐
 *
 * 安全前提：本模块只读取固件留下的只读结构，不写任何固件区域；
 * 所有物理地址访问都必须先通过 mdet_in_window() 的窗口校验，
 * 窗口之外一律拒绝访问并计入 out_of_window，绝不越界解引用。
 * ============================================================================ */
#ifndef __XOS_MEMDETECT_H__
#define __XOS_MEMDETECT_H__

#include "types.h"
#include "e820.h"

/* --------------------------------------------------------------------------
 * 恒等映射窗口
 * 内核启动期只对 0x00000000 - 0x07FFFFFF（128MB）建立恒等映射，
 * 所有固件结构的读取都必须落在这个窗口内。
 * -------------------------------------------------------------------------- */
#define MDET_WINDOW_BASE   0x00000000u
#define MDET_WINDOW_TOP    0x08000000u

/* --------------------------------------------------------------------------
 * 容量上限
 * -------------------------------------------------------------------------- */
#define MDET_MAX_REGIONS     64u   /* 规范化后的内存映射条目上限 */
#define MDET_MAX_HOLES       32u   /* 记录的空洞上限 */
#define MDET_MAX_ACPI_TABLES 24u   /* 枚举的 ACPI 表上限 */
#define MDET_MAX_MEMMAP      16u   /* 内核参数 memmap= 条目上限 */
#define MDET_MON_RING        32u   /* 使用率采样环形缓冲长度 */

/* 持久化块落点：内核保留区（0x00000000-0x0003FFFF）的最后一页。
 * 选择依据：既在内核保留区内（PMM 不会把它当空闲页分配出去），
 * 又在内核映像 + .bss 之上（不覆盖代码与数据）。 */
#define MDET_BOOTINFO_ADDR   0x00008000u  /* 低区保留窗口内、内核镜像(0x10000 起)之下，
                                             高于 IVT/BIOS 数据区，写入不会踩坏内核静态数据 */
#define MDET_BOOTINFO_MAGIC  0x4D444254u   /* 'MDBT' */
#define MDET_BOOTINFO_VER    0x00010001u

/* --------------------------------------------------------------------------
 * 返回码
 * -------------------------------------------------------------------------- */
#define MDET_OK         0
#define MDET_ENOENT   (-1)   /* 目标结构不存在 */
#define MDET_EINVAL   (-2)   /* 入参非法 */
#define MDET_EBADSUM  (-3)   /* 校验和不符 */
#define MDET_EWINDOW  (-4)   /* 目标落在恒等映射窗口之外 */
#define MDET_ENOSPC   (-5)   /* 容量上限已满 */
#define MDET_EFORMAT  (-6)   /* 结构格式不符合规范 */
#define MDET_EBUSY    (-7)   /* 前置状态未就绪 */

/* --------------------------------------------------------------------------
 * S01：E820 条目类型识别
 * -------------------------------------------------------------------------- */
#define MDET_TYPE_MAX   8u

typedef struct {
    u8  type_seen[MDET_TYPE_MAX];       /* 各类型是否出现过 */
    u64 type_bytes[MDET_TYPE_MAX];      /* 各类型字节数 */
    u32 unknown_type_count;             /* 未知类型条目数（容错，不中断探测） */
    u32 zero_len_count;                 /* 零长度条目数（容错） */
    u32 high_bit_count;                 /* 地址含 64 位高位、无法用 32 位寻址的条目数 */
} mdet_types_t;

/* --------------------------------------------------------------------------
 * S02 / S03 / S05 / S18：容量统计
 * -------------------------------------------------------------------------- */
typedef struct {
    u64 total_space;        /* 全部条目长度之和 */
    u64 union_space;        /* 去重后的地址空间覆盖 */
    u64 usable;             /* 可用内存 */
    u64 reserved;           /* 保留内存（含坏内存） */
    u64 acpi_reclaim;       /* ACPI 可回收 */
    u64 acpi_nvs;           /* ACPI NVS */
    u64 bad;                /* 坏内存 */
    u64 below_1m_total;     /* 1MB 以下的全部空间 */
    u64 below_1m_usable;    /* 1MB 以下的可用空间 */
    u64 below_1m_reserved;  /* 1MB 以下的保留空间 */
    u32 region_count;
    u32 usable_count;
    u32 reserved_count;
    u32 acpi_count;
    u32 bad_count;
    u64 usable_pct_x100;    /* 可用占比 × 100（整数，避免浮点） */
    u64 below_1m_usable_pct_x100;
} mdet_stats_t;

/* --------------------------------------------------------------------------
 * S04：ACPI 区域识别
 * -------------------------------------------------------------------------- */
typedef struct {
    u32  rsdp_addr;         /* 0 = 未找到 */
    u8   revision;          /* 0 = ACPI 1.0，2 = ACPI 2.0 及以上 */
    u8   rsdt_ok;           /* RSDT 签名与校验和均通过 */
    u8   xsdt_present;      /* RSDT 头中 XSDT 地址非零 */
    u8   rsdp_checksum_ok;
    u8   oem_id[6];
    u8   oem_rev;
    u32  rsdt_addr;
    u32  xsdt_addr;
    u32  table_count;       /* 成功读取头的表数 */
    u32  table_addr[MDET_MAX_ACPI_TABLES];
    char table_sig[MDET_MAX_ACPI_TABLES][5];
    u32  facp_addr;         /* FADT */
    u32  apic_addr;         /* MADT */
    u32  mcfg_addr;         /* MCFG（PCIe 配置空间） */
    u32  hpet_addr;
    u32  srat_addr;         /* SRAT（NUMA / 内存亲和性） */
    u32  dsdt_addr;
    u32  checksum_fail;     /* 校验和不符的表数 */
    u32  out_of_window;     /* 落在恒等映射窗口之外、无法读取的表数 */
    u32  bad_signature;     /* 签名不含可打印字符的表数 */
} mdet_acpi_t;

/* --------------------------------------------------------------------------
 * S10：内存热插拔区域探测
 * -------------------------------------------------------------------------- */
#define MDET_MAX_HOTPLUG 16u

typedef struct {
    u32 count;
    u32 srat_present;
    u32 srat_memory_entries;
    u64 total_bytes;
    struct { u64 base, len; } region[MDET_MAX_HOTPLUG];
    u32 nonvolatile_count;      /* E820 属性位标记为非易失的条目数 */
} mdet_hotplug_t;

/* --------------------------------------------------------------------------
 * S11 / S12 / S13：SMBIOS 内存条信息、速率与时序、ECC 能力
 * -------------------------------------------------------------------------- */
#define MDET_SMBIOS_SLOTS 8u

typedef struct {
    u32  entry_addr;        /* 0 = 未找到 */
    u8   is_v3;             /* 入口点为 _SM3_（64 位结构表地址） */
    u8   major, minor;
    u8   entry_checksum_ok;
    u8   table_checksum_ok; /* SMBIOS 3.0 才有结构表校验和 */
    u16  struct_count;
    u32  table_len;
    u16  array_count;       /* Type 16 物理内存阵列数 */
    u16  device_count;      /* Type 17 内存器件数 */
    u16  device_populated;  /* 已安装（容量非 0）的器件数 */
    u16  slot_populated;    /* 已填入槽位明细表的器件数（上限 MDET_SMBIOS_SLOTS） */
    u32  total_size_mb;     /* 已安装容量合计（MB） */
    u16  max_speed_mt;      /* 最高额定速率 MT/s */
    u16  max_cfg_speed_mt;  /* 最高运行速率 MT/s */
    u16  speed_mismatch;    /* 运行速率低于额定速率的器件数 */
    u8   ecc_type;          /* Type 16 纠错类型 */
    u8   ecc_present;       /* 任一器件报告支持纠错 */
    u8   ecc_capable_slots;
    u8   form_factor;       /* 首个器件的封装形式 */
    u8   memory_tech;       /* 首个器件的内存技术 */
    u8   type_detail;       /* 首个器件的类型位图 */
    u8   unknown_size_slots;/* 容量字段为 0xFFFF（未知）的器件数 */
    u16  slot_size_mb[MDET_SMBIOS_SLOTS];
    u16  slot_speed_mt[MDET_SMBIOS_SLOTS];
    u8   slot_module_width[MDET_SMBIOS_SLOTS];  /* Type17 byte08 总线位宽 */
    u8   slot_memory_tech[MDET_SMBIOS_SLOTS];   /* 每器件内存技术编码 */
    u16  slot_voltage_mv[MDET_SMBIOS_SLOTS];    /* Type17 配置电压(mV) */
    u8   slot_manufacturer[MDET_SMBIOS_SLOTS][16];
    u8   slot_part[MDET_SMBIOS_SLOTS][24];
    u8   slot_serial[MDET_SMBIOS_SLOTS][16];
    u32  type19_count;                          /* Type19 阵列映射地址条目数 */
    struct { u64 base, len; } type19[4];        /* 物理地址窗口（KB->字节） */
    u32  out_of_window;
    u32  format_errors;     /* 结构长度非法等格式问题数 */
} mdet_smbios_t;

/* --------------------------------------------------------------------------
 * S09：物理地址空间上限探测
 * -------------------------------------------------------------------------- */
typedef struct {
    u64 e820_max_end;       /* E820 覆盖的最高物理地址 */
    u64 e820_highest_usable;/* 最高可用物理地址 */
    u8  cpuid_phys_bits;    /* CPUID 0x80000008 EAX[7:0]，0 = 不支持该叶 */
    u8  cpuid_linear_bits;  /* EAX[15:8] */
    u8  cpuid_ok;
    u64 cpuid_max_phys;     /* 由物理位宽推出的可寻址上限 */
    u8  e820_within_cpuid;  /* E820 上限是否落在 CPU 可寻址范围内 */
    u8  needs_64bit_addr;   /* 是否需要 64 位物理寻址 */
    u64 unaddressable;      /* E820 声明但 CPU 无法寻址的空间 */
} mdet_physaddr_t;

/* --------------------------------------------------------------------------
 * S08：内存空洞识别
 * -------------------------------------------------------------------------- */
typedef struct {
    u32 count;
    u64 total_bytes;
    u64 largest_base;
    u64 largest_len;
    u64 below_1m_hole;      /* 1MB 以下的空洞合计 */
    u64 above_4g_hole;      /* 4GB 以上的空洞合计（64 位扩展区之间） */
    struct { u64 base, len; } hole[MDET_MAX_HOLES];
} mdet_holes_t;

/* --------------------------------------------------------------------------
 * S14：内存映射持久化
 * -------------------------------------------------------------------------- */
typedef struct {
    u32 magic;
    u32 version;
    u32 count;
    u32 checksum;           /* 覆盖 magic..region[count-1] 的 32 位和 */
    u64 total_usable;
    u64 total_reserved;
    u64 max_phys;
    u32 acpi_tables;
    u32 smbios_devices;
    u32 flags;
    u32 reserved0;
    e820_entry_t region[MDET_MAX_REGIONS];
} mdet_bootinfo_t;

/* --------------------------------------------------------------------------
 * S19：内存使用率监控
 * -------------------------------------------------------------------------- */
typedef struct {
    u32 samples;            /* 采样次数 */
    u32 peak_used_pages;    /* 已用页峰值 */
    u32 min_free_pages;     /* 空闲页谷值 */
    u32 max_free_pages;     /* 空闲页峰值 */
    u32 high_water_events;  /* 使用率越过高水位的次数 */
    u32 low_water_events;   /* 使用率回落到低水位以下的次数 */
    u32 ring[MDET_MON_RING];/* 每次采样的空闲页数 */
    u32 ring_head;
    u32 last_used_x100;     /* 最近一次使用率 × 100 */
    u32 last_free_pages;
    u32 last_total_pages;
    u8  in_high_water;      /* 迟滞状态：当前是否处于高水位之上 */
} mdet_monitor_t;

/* --------------------------------------------------------------------------
 * S20：内存映射与内核参数对齐
 * -------------------------------------------------------------------------- */
typedef struct {
    u64 mem_limit;          /* mem= 指定的可用内存上限，0 = 未指定 */
    u32 memmap_count;
    struct { u64 base, len; u32 type; } memmap[MDET_MAX_MEMMAP];
    u32 applied;            /* 成功应用的参数数 */
    u32 rejected;           /* 被拒绝的参数数 */
    u32 last_reject_code;
    u64 clipped_bytes;      /* 因 mem= 上限被裁掉的可用空间 */
    u32 forced_reserved;    /* memmap= 强制改为保留的条目数 */
} mdet_cmdline_t;

/* --------------------------------------------------------------------------
 * 对外接口
 * -------------------------------------------------------------------------- */

/* 物理地址窗口校验：窗口之外一律不得解引用 */
int  mdet_in_window(u64 addr, u32 len);

/* S01 + S02 + S03 + S05 + S06 + S07：从 E820 表构建规范化映射并统计 */
int  mdet_init(const e820_entry_t *entries, u32 count);
int  mdet_restore(void);          /* 复原到 mdet_init 时的映射快照 */
u32  mdet_region_count(void);
const e820_entry_t *mdet_region(u32 index);
u32  mdet_ready_state(void);      /* 调试探针：mdet_ready 当前值 */

/* S04：ACPI 区域识别 */
int  mdet_acpi_probe(void);
const mdet_acpi_t *mdet_acpi(void);
const char *mdet_acpi_table_name(const char *sig);

/* S10：内存热插拔区域探测（依赖 SRAT，未找到 SRAT 时退化为 E820 属性位判定） */
int  mdet_hotplug_probe(void);
const mdet_hotplug_t *mdet_hotplug(void);

/* S11 + S12 + S13：SMBIOS 内存条信息、速率时序、ECC 能力 */
int  mdet_smbios_probe(void);
const mdet_smbios_t *mdet_smbios(void);

/* S09：物理地址空间上限探测 */
int  mdet_physaddr_probe(void);
const mdet_physaddr_t *mdet_physaddr(void);

/* S08：内存空洞识别 */
int  mdet_holes_scan(void);
const mdet_holes_t *mdet_holes(void);

/* S14：持久化与回读 */
int  mdet_persist(void);
int  mdet_persist_verify(void);
const mdet_bootinfo_t *mdet_bootinfo_read(void);
u32  mdet_bootinfo_addr(void);

/* S15：可视化输出 */
void mdet_visualize(void);

/* S16 / S17：容错与校验 */
u32  mdet_validate(void);
const char *mdet_status_name(int code);

/* S18：统计接口 */
void mdet_stats(mdet_stats_t *out);
const mdet_types_t *mdet_types(void);

/* S19：使用率监控 */
int  mdet_monitor_sample(void);
void mdet_monitor_report(void);
const mdet_monitor_t *mdet_monitor(void);

/* S20：内核参数 */
int  mdet_cmdline_set_mem(u64 limit);
int  mdet_cmdline_add_memmap(u64 base, u64 len, u32 type);
int  mdet_cmdline_apply(void);
void mdet_cmdline_reset(void);    /* 清空全部内核参数并复原映射 */
const mdet_cmdline_t *mdet_cmdline(void);

/* 汇总输出 */
void mdet_dump(void);

/* 自检：返回 0 = 通过，非 0 = 失败用例编号 */
u32  mdet_selftest(void);
u32  mdet_acpi_selftest(void);
u32  mdet_smbios_selftest(void);

/* 04 册第二轮重做：并发保护 / 探测重试与退避（调度器无关，锁内可直接调用） */
u32  mdet_lock_calls(void);                     /* 临界区进入总次数 */
typedef struct {
    u32 retry_total;        /* 触发重试的探测次数 */
    u32 retry_failed;       /* 重试耗尽仍失败的次数 */
    u32 last_backoff_us;    /* 最近一次退避的模拟时长(us) */
    u32 probe_timeouts;     /* 探测超时事件数 */
} mdet_retry_stats_t;
const mdet_retry_stats_t *mdet_retry_stats(void);
int  mdet_probe_with_retry(int (*fn)(void), u32 max_retry);  /* 失败自动退避重试 */

#endif /* __XOS_MEMDETECT_H__ */
