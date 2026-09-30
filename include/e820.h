/* ============================================================================
 * XOS E820 物理内存探测与内存映射管理
 * 对应功能点 1-30（物理内存探测、解析、排序、合并、统计）
 * ============================================================================ */
#ifndef __XOS_E820_H__
#define __XOS_E820_H__

#include "types.h"

/* Stage2 写入的固定地址（见 boot/stage2.S） */
#define E820_BUF_ADDR    0x0500u
#define E820_CNT_ADDR    0x04F0u
#define E820_MAGIC_ADDR  0x04F4u
#define E820_SIGVAL      0x584F5345u    /* 'XOSE'，Stage2 写入 */
#define E820_MAX_ENTRIES 64

/* E820 内存类型 */
#define E820_USABLE        1u   /* 可用内存 */
#define E820_RESERVED      2u   /* 保留 */
#define E820_ACPI_RECLAIM  3u   /* ACPI 可回收 */
#define E820_ACPI_NVS      4u   /* ACPI NVS */
#define E820_BAD           5u   /* 坏内存 */

/* E820 扩展属性位 */
#define E820_ATTR_ENABLED      0x1u  /* 该区间可被 OS 使用 */
#define E820_ATTR_NONVOLATILE  0x2u

/* e820_validate 返回码 */
#define E820_VALID_OK        0u
#define E820_VALID_NOMAGIC   1u   /* Stage2 未写入签名，数据不可信 */
#define E820_VALID_COUNT     2u   /* 条目数非法 */
#define E820_VALID_EMPTY     3u   /* 无有效条目 */
#define E820_VALID_LENGTH    4u   /* 存在零长度条目 */
#define E820_VALID_TYPE      5u   /* 存在非法类型 */
#define E820_VALID_ORDER     6u   /* 排序被破坏 */
#define E820_VALID_OVERLAP   7u   /* 不同类区间重叠 */
#define E820_VALID_NORAM     8u   /* 无任何可用内存 */

/* 单条内存映射（24 字节，与 BIOS 返回布局一致） */
typedef struct {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi;
} PACKED e820_entry_t;

/* 内存统计结果 */
typedef struct {
    u64 total_bytes;        /* 全部地址空间（各条目长度之和，可能含重叠） */
    u64 union_bytes;        /* 去重后的地址空间覆盖长度 */
    u64 usable_bytes;       /* 可用内存 */
    u64 reserved_bytes;     /* 保留内存 */
    u64 acpi_reclaim_bytes;
    u64 acpi_nvs_bytes;
    u64 bad_bytes;
    u32 entries;            /* 条目总数 */
    u32 usable_entries;
    u32 usable_pages;       /* 可用页数（4KB） */
    u64 highest_addr;       /* 最高可寻址地址 */
    u32 below_1m_usable;    /* 1MB 以下可用字节 */
} mem_stats_t;

void                 e820_init(void);
bool                 e820_ready(void);
u32                  e820_count(void);

/* 只读访问：防止调用方破坏「已排序、已合并」的不变式 */
const e820_entry_t  *e820_get(u32 index);
const e820_entry_t  *e820_table(void);

void                 e820_sort(void);
u32                  e820_merge(void);
void                 e820_stats(mem_stats_t *out);
const char          *e820_type_name(u32 type);
void                 e820_dump(void);

/* 新增接口 */
u32                  e820_validate(void);          /* 校验映射表有效性 */
const e820_entry_t  *e820_find(u64 addr);          /* 按物理地址查找所属条目 */
u32                  e820_selftest(void);          /* 自检，0 = 通过 */

#endif /* __XOS_E820_H__ */
