/* ============================================================================
 * XOS 物理内存管理器（PMM）—— 页帧位图分配器
 * 对应功能点 31-120
 *   31-40  页常量、对齐、页号换算
 *   41-60  位图结构、置位/清位/查找、统计
 *   61-90  单页/连续页分配释放、保留区管理
 *   91-120 自检、错误码、边界处理
 * ============================================================================ */
#ifndef __XOS_PMM_H__
#define __XOS_PMM_H__

#include "types.h"

/* ---- 页常量 ---- */
#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12u
#define PAGE_MASK       (PAGE_SIZE - 1u)

/* 受管物理内存上限：128 MB */
#define PMM_MAX_PHYS    (128u * 1024u * 1024u)
#define PMM_MAX_PAGES   (PMM_MAX_PHYS / PAGE_SIZE)

/* ---- 错误码 ---- */
#define PMM_OK          0
#define PMM_ENOMEM      1
#define PMM_EINVAL      2
#define PMM_EBUSY       3   /* 重复释放 */
#define PMM_ERESERVED   4   /* 目标是保留区，禁止释放 */
#define PMM_ENOTALLOC   5   /* 目标页从未被分配 */
#define PMM_EREFCNT     6   /* 引用计数非法 */

/* ---- 页状态 ---- */
#define PMM_STATE_FREE      0
#define PMM_STATE_USED      1
#define PMM_STATE_RESERVED  2
#define PMM_STATE_OOR       3   /* 超出受管范围 */

/* ---- 调试毒化模式 ---- */
#define PMM_DBG_OFF         0
#define PMM_DBG_POISON      1   /* 释放时填充毒化值 */
#define PMM_DBG_VERIFY      2   /* 额外在分配时校验毒化值 */
#define PMM_POISON_FREE     0xDEADBEEFu
#define PMM_POISON_ALLOC    0xCDCDCDCDu

/* ---- 释放页内容策略 ---- */
#define PMM_SCRUB_OFF       0u      /* 释放时写毒化特征值（便于查 use-after-free） */
#define PMM_SCRUB_ON        1u      /* 释放时清零（杜绝跨持有者数据泄漏） */

/* ---- 分配策略标志 ---- */
#define PMM_ALLOC_ZERO      0x01u   /* 返回前清零 */
#define PMM_ALLOC_DMA       0x02u   /* 必须落在 16MB 以下 */
#define PMM_ALLOC_HIGH      0x04u   /* 优先高地址 */
#define PMM_ALLOC_CONTIG    0x08u   /* 必须物理连续 */
#define PMM_ALLOC_ATOMIC    0x10u   /* 不触发回收与 OOM 通报 */

/* ---- 页号 / 物理地址换算 ---- */
#define PAGE_ALIGN_DOWN(x)  ((x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)    (((x) + PAGE_MASK) & ~PAGE_MASK)
#define PAGE_IS_ALIGNED(x)  (((x) & PAGE_MASK) == 0u)
#define PHYS_TO_PAGE(x)     ((u32)(x) >> PAGE_SHIFT)
#define PAGE_TO_PHYS(p)     ((u32)(p) << PAGE_SHIFT)

/* ---- 位图操作：set_bit / clear_bit / test_bit 定义于 types.h ---- */

/* ---- 统计 ---- */
typedef struct {
    u32 total_pages;        /* 受管页总数 */
    u32 used_pages;         /* 已占用页数（含保留页） */
    u32 free_pages;         /* 空闲页数 */
    u32 reserved_pages;     /* 保留页数（含于 used_pages） */
    u32 managed_bytes;      /* 受管物理内存字节数 */
    u32 alloc_calls;        /* 累计分配调用次数 */
    u32 free_calls;         /* 累计释放调用次数 */
    u32 alloc_fail;         /* 累计分配失败次数 */
    u32 high_water;         /* 已用页数峰值 */
    u32 largest_free_run;   /* 最大连续空闲页数 */
    u32 free_regions;       /* 空闲区段数量（碎片度指标） */

    /* ---- 扩展子系统 ---- */
    u32 buddy_blocks;       /* 伙伴系统各阶空闲块总数 */
    u32 zone_free_dma;      /* ZONE_DMA 空闲页 */
    u32 zone_free_normal;   /* ZONE_NORMAL 空闲页 */
    u32 zone_used_dma;      /* ZONE_DMA 已用页 */
    u32 zone_used_normal;   /* ZONE_NORMAL 已用页 */
    u32 numa_nodes;         /* 在线 NUMA 节点数 */
    u32 wm_min;             /* 最低水位 */
    u32 wm_low;             /* 低水位 */
    u32 wm_high;            /* 高水位 */
    u32 pressure;           /* 内存压力等级 0..3 */
    u32 shrinker_count;     /* 已注册可回收对象数 */
    u32 reclaim_calls;      /* 回收调度次数 */
    u32 reclaim_freed;      /* 累计回收页数 */
    u32 oom_count;          /* 累计 OOM 通报次数 */
    u32 scrub_count;        /* 累计清零页数 */
    u32 badpage_count;      /* 已下线坏页数 */
    u32 hotplug_count;      /* 已登记热插拔区数 */
    u32 huge_used;          /* 已用 2M 大页数 */
    u32 huge_total;         /* 2M 大页容量 */
    u32 ioremap_count;      /* 当前 IO 映射数 */
    u32 antifrag_pages;     /* 反碎片预留页数 */
    u32 frag_index;         /* 外部碎片指数 0..1000 */
} pmm_stats_t;

/* ---- 生命周期 ---- */
void pmm_init(void);

/* ---- 标记与查询 ---- */
void pmm_mark_used(u32 phys);
void pmm_mark_free(u32 phys);
int  pmm_is_used(u32 phys);
int  pmm_is_reserved(u32 phys);
int  pmm_state(u32 phys);

/* ---- 区间管理 ---- */
void pmm_reserve_range(u32 start, u32 end);
void pmm_release_range(u32 start, u32 end);

/* ---- 查找 ---- */
u32  pmm_find_first_free(void);
u32  pmm_find_contiguous(u32 n);
u32  pmm_count_free_in_range(u32 start, u32 end);
u32  pmm_largest_free_run(void);
u32  pmm_free_region_count(void);

/* ---- 分配与释放 ---- */
u32  pmm_alloc_page(void);
u32  pmm_alloc_pages(u32 n);
u32  pmm_alloc_high_pages(u32 n);
u32  pmm_alloc_page_zeroed(void);
int  pmm_free_page(u32 phys);
int  pmm_free_pages(u32 phys, u32 n);

/* ---- 引用计数（为共享页 / COW 预留） ---- */
u8   pmm_refcount(u32 phys);
int  pmm_ref_inc(u32 phys);
int  pmm_ref_dec(u32 phys);

/* ---- 统计与调试 ---- */
void pmm_stats(pmm_stats_t *out);
u32  pmm_total_pages(void);
u32  pmm_free_page_count(void);
u32  pmm_used_pages(void);
u32  pmm_reserved_pages(void);
u32  pmm_managed_bytes(void);
void pmm_set_debug(u32 level);
u32  pmm_get_debug(void);
void pmm_dump(void);
u32  pmm_poison_check(u32 phys);
u32  pmm_selftest(void);

/* ---- 锁与并发保护（03 册第二轮重做） ---- */
u32  pmm_lock_enter_count(void);      /* 临界区进入总次数 */
u32  pmm_selftest_lock(void);         /* 并发分配/释放自检（调度器就绪后调用） */

/* ---- S05 伙伴系统分配与合并 ---- */
u32  pmm_buddy_selftest(void);

/* ---- S06 内存区域 zone 管理 ---- */
u32  pmm_zone_count(void);
const char *pmm_zone_name(u32 z);
u32  pmm_zone_free(u32 z);
u32  pmm_zone_used(u32 z);
u32  pmm_zone_reserved(u32 z);
u32  pmm_zone_end_page(u32 z);
u32  pmm_zone_limit(void);
u32  pmm_alloc_zone(u32 z);

/* ---- S07 NUMA 节点与亲和性 ---- */
u32  pmm_numa_node_count(void);
u32  pmm_numa_node_of(u32 phys);
u32  pmm_numa_distance(u32 a, u32 b);
u32  pmm_numa_start(u32 n);
u32  pmm_numa_end(u32 n);
u32  pmm_numa_free(u32 n);
u32  pmm_alloc_node(u32 n);

/* ---- S08 内存热插拔 ---- */
u32  pmm_hotplug_count(void);
u32  pmm_hotplug_state(u32 idx);
u32  pmm_hotplug_start(u32 idx);
u32  pmm_hotplug_end(u32 idx);
int  pmm_hotplug_add(u32 start, u32 end);
int  pmm_hotplug_remove(u32 idx);

/* ---- S09 大页（2M）管理 ---- */
u32  pmm_huge_count(void);
u32  pmm_huge_size(void);
u32  pmm_huge_used(void);
u32  pmm_alloc_huge(void);
int  pmm_free_huge(u32 phys);

/* ---- S11 内存碎片整理 ---- */
u32  pmm_antifrag_pages(void);
u32  pmm_antifrag_start(void);
u32  pmm_frag_index(void);
u32  pmm_compact(void);

/* ---- S12 水位与压力管理 ---- */
u32  pmm_watermark(u32 level);
int  pmm_watermark_ok(u32 free_pages, u32 level);
u32  pmm_pressure_level(void);
const char *pmm_pressure_name(u32 lv);

/* ---- S13 内存回收（shrinker 框架） ---- */
typedef u32 (*pmm_shrink_fn)(u32 nr_to_free, void *arg);
int  pmm_register_shrinker(const char *name, pmm_shrink_fn fn, void *arg);
int  pmm_unregister_shrinker(int id);
u32  pmm_shrinker_count(void);
const char *pmm_shrinker_name(u32 id);
u32  pmm_shrinker_freed(u32 id);
u32  pmm_shrinker_calls(u32 id);
u32  pmm_reclaim_calls(void);
u32  pmm_reclaim_freed(void);
u32  pmm_reclaim(u32 target);

/* ---- S14 OOM 处理 ---- */
typedef void (*pmm_oom_fn)(u32 free_pages, u32 target, void *arg);
int  pmm_register_oom_handler(pmm_oom_fn fn, void *arg);
u32  pmm_oom_count(void);
u32  pmm_oom_last_free(void);
u32  pmm_oom_last_target(void);
u32  pmm_oom_handler_count(void);

/* ---- S17 IO 内存映射 ioremap ---- */
#define IOREMAP_CACHEABLE   0x01u
#define IOREMAP_ALLOW_RAM   0x02u
void *pmm_ioremap(u32 phys, u32 size, u32 flags);
int  pmm_iounmap(void *addr);
u32  pmm_ioremap_count(void);
u32  pmm_ioremap_phys(u32 idx);
u32  pmm_ioremap_size(u32 idx);

/* ---- S18 内存加密与保护（释放清零） ---- */
void pmm_set_scrub(u32 on);
u32  pmm_get_scrub(void);
u32  pmm_scrub_count(void);
u32  pmm_scrub_verify(u32 phys);

/* ---- S19 内存错误检测与隔离（坏页下线） ---- */
u32  pmm_badpage_count(void);
u32  pmm_badpage_at(u32 idx);
int  pmm_is_offline(u32 phys);
int  pmm_offline_page(u32 phys);
int  pmm_online_page(u32 phys);

/* ---- S20 分配策略与优先级 ---- */
u32  pmm_alloc_flags(u32 n, u32 flags);

/* ---- 扩展子系统自检 ---- */
u32  pmm_ext_selftest(void);

#endif /* __XOS_PMM_H__ */
