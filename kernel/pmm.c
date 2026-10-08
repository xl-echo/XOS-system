/* ============================================================================
 * XOS 物理内存管理器（PMM）实现
 *
 * 本版修复（针对审查发现的缺陷）：
 *   1) 保留页重复计数：原实现把内核保留区（0x10000-0x40000）与低端 1MB
 *      分别相加，而前者是后者的子集，导致 reserved_pages = 304 而
 *      used_pages = 272，出现「保留页多于已用页」的逻辑矛盾。
 *      现改为双位图（used / reserved）增量维护，集合互斥、计数自洽。
 *   2) 原实现只把「非 E820 可用」的内存置为已用，但 1MB 以上的保留区
 *      （ACPI、MMIO、内核区）没有独立保护，pmm_free_page 可将其误释放。
 *      现引入独立的 reserved 位图，释放保留页返回 PMM_ERESERVED。
 *   3) pmm_free_pages 原先逐页释放，中途失败时已释放的页不会回滚，
 *      调用方拿到错误但状态已不一致。现改为「先全量校验，再统一提交」。
 *   4) pmm_is_used 对越界页返回 1（占用），与「未知」语义混淆。
 *      现提供 pmm_state 四态查询，pmm_is_used 仅对受管范围内返回有效值。
 *   5) 查找空闲页原为每次从头线性扫描，连续分配退化为 O(n²)。
 *      现引入轮转提示位（alloc_hint），顺序分配均摊 O(1)。
 *   6) 新增页引用计数、碎片统计、位图转储、毒化检测与扩展自检。
 * ============================================================================ */
#include "pmm.h"
#include "e820.h"
#include "console.h"
#include "string.h"
#include "sync.h"

#define BITMAP_WORDS    (PMM_MAX_PAGES / 32u)

/* buddy 伙伴索引：各阶位图字偏移 off[o]=Σ_{k<o}(PMM_MAX_PAGES>>k)>>5，
 * PMM_MAX_PHYS 变更（非 2 的幂）必须同步重算，否则 buddy_off_check 失败。 */
#define BUDDY_MAX_ORDER   10u
#define BUDDY_ORDERS      (BUDDY_MAX_ORDER + 1u)
#define BUDDY_MAP_WORDS   2047u
#define BUDDY_NONE        0xFFFFFFFFu
static u32 buddy_map[BUDDY_MAP_WORDS];
static u32 buddy_hint[BUDDY_ORDERS];
static u32 buddy_ready = 0;
static u32 buddy_rebuild_count = 0;
static u32 buddy_rebuild_set_count = 0;
static const u16 buddy_off[BUDDY_ORDERS] = {
    0, 768, 1152, 1344, 1440, 1488, 1512, 1524, 1530, 1533, 1535
};

/* used=1 表示已被分配；reserved=1 表示永久保留，二者互斥 */
static u32 used_bitmap[BITMAP_WORDS];
static u32 resv_bitmap[BITMAP_WORDS];
static u8  refcnt[PMM_MAX_PAGES];

static u32 total_pages    = 0;
static u32 used_pages     = 0;
static u32 reserved_pages = 0;
static u32 managed_limit  = 0;

static u32 alloc_hint     = 0;
static u32 alloc_calls    = 0;
static u32 free_calls     = 0;
static u32 alloc_fail     = 0;
static u32 high_water     = 0;
static u32 debug_level    = PMM_DBG_POISON;

/* ==========================================================================
 * 锁与并发保护（03 册物理内存管理 · 第二轮重做）
 * 全部分配 / 释放 / 引用计数 / 统计聚合路径由同一把自旋锁串行化。
 * 锁放在「公开入口 = 锁包装 + 内部 _locked 变体」这一层：
 *   - 公开入口之间互相调用（alloc_pages→alloc_page、alloc_flags→zone…）
 *     全部走 _locked 变体，避免锁重入死锁；
 *   - oom_handle / reclaim / shrinker 回调在临界区外执行，可安全调用
 *     带锁的公开入口（此时外层锁已释放）；
 *   - 中断上下文（PIT tick）可打断临界区，故使用 irqsave 版本。
 * ========================================================================== */
static spinlock_t pmm_alloc_lock;
static u32 pmm_lock_calls = 0;          /* 临界区进入总次数（统计维度） */

static inline u32 pmm_lock_enter(void)
{
    u32 eflags = spin_lock_irqsave(&pmm_alloc_lock);
    pmm_lock_calls++;
    return eflags;
}

static inline void pmm_lock_exit(u32 eflags)
{
    spin_unlock_irqrestore(&pmm_alloc_lock, eflags);
}

/* 供自检与统计读取：临界区进入总次数（>0 证明锁真实被使用） */
u32 pmm_lock_enter_count(void) { return pmm_lock_calls; }

/* 内核映像保留区：Stage2 将内核加载到 0x10000，其后紧跟 .bss。
 * 保守保留到 0x80000（共 448KB），确保内核代码段、静态数据、位图
 * 与 .bss（随堆分配器等模块增长）不会被页分配器回收。 */
#define KERNEL_RESERVE_BASE  0x00010000u
#define KERNEL_RESERVE_END   0x00140000u   /* 含 .bss（已移至 0x100000 起，见 linker.ld）；防止堆分配踩入内核数据 */

/* ---- 扩展子系统常量（定义在此处，供 pmm_stats 等前置函数使用） ---- */
#define ZONE_DMA        0
#define ZONE_NORMAL     1
#define ZONE_COUNT      2
#define ZONE_DMA_LIMIT  (16u * 1024u * 1024u)

#define PMM_WM_MIN   0
#define PMM_WM_LOW   1
#define PMM_WM_HIGH  2
#define PMM_WM_COUNT 3

#define PMM_PRESS_NORMAL    0
#define PMM_PRESS_LOW       1
#define PMM_PRESS_MIN       2
#define PMM_PRESS_CRITICAL  3

/* ---- 扩展子系统的内部前置声明（实现见文件后半部分） ---- */
static void buddy_rebuild(void);
static void buddy_enable(void);
static u32  buddy_free_block_count(void);
static void zone_add(u32 p, u32 which, int delta);
static void zone_setup(void);
static void zone_recount(void);
static void numa_setup(void);
static void wm_setup(void);
static void antifrag_reserve(void);
static u32  buddy_test(u32 order, u32 block);   /* 供 pmm_init 末尾诊断使用 */
static u32  antifrag_start_page;                /* 供 pmm_init 末尾诊断使用 */
static u32  antifrag_pages;static void free_page_fill(u32 page);
static u32  oom_handle(u32 target);

/* ---- 锁保护内部变体前置声明（公开入口 = 锁包装 + _locked 变体） ---- */
static u32  _pmm_alloc_page_locked(void);
static u32  _pmm_alloc_pages_locked(u32 n);
static u32  _pmm_alloc_zone_locked(u32 z);
static u32  _pmm_alloc_node_locked(u32 n);
static u32  _pmm_alloc_huge_locked(void);
static int  _pmm_free_page_locked(u32 phys);
static int  _pmm_free_pages_locked(u32 phys, u32 n);
static int  _pmm_free_huge_locked(u32 phys);
static int  _pmm_ref_inc_locked(u32 phys);
static int  _pmm_ref_dec_locked(u32 phys);
static u32  _pmm_alloc_flags_locked(u32 n, u32 flags);
static void buddy_remove_page(u32 p);   /* 定义在 S05 段（line ~691），大栈保留路径需同步摘索引 */

/* --------------------------------------------------------------------------
 * 内部：状态位操作（同时维护计数与引用计数）
 * ------------------------------------------------------------------------ */
static void mark_used_page(u32 page)
{
    if (page >= total_pages) return;
    if (test_bit(resv_bitmap, page)) return;      /* 保留页不可被占用 */
    if (!test_bit(used_bitmap, page)) {
        set_bit(used_bitmap, page);
        used_pages++;
        if (used_pages > high_water) high_water = used_pages;
        zone_add(page, 1, +1);
        zone_add(page, 0, -1);
    }
}

static void mark_free_page(u32 page)
{
    if (page >= total_pages) return;
    if (test_bit(resv_bitmap, page)) return;      /* 保留页不可被释放 */
    if (test_bit(used_bitmap, page)) {
        clear_bit(used_bitmap, page);
        if (used_pages) used_pages--;
        zone_add(page, 1, -1);
        zone_add(page, 0, +1);
    }
    refcnt[page] = 0;
}

static void mark_reserved_page(u32 page)
{
    if (page >= total_pages) return;
    if (!test_bit(resv_bitmap, page)) {
        set_bit(resv_bitmap, page);
        reserved_pages++;
        zone_add(page, 2, +1);
    }
    /* 保留页不计入已用集合，保证两集合互斥 */
    if (test_bit(used_bitmap, page)) {
        clear_bit(used_bitmap, page);
        if (used_pages) used_pages--;
        zone_add(page, 1, -1);
    }
}

static void mark_available_page(u32 page)
{
    if (page >= total_pages) return;
    if (test_bit(resv_bitmap, page)) {
        clear_bit(resv_bitmap, page);
        if (reserved_pages) reserved_pages--;
        zone_add(page, 2, -1);
    }
}

/* --------------------------------------------------------------------------
 * 公共接口：标记/查询
 * ------------------------------------------------------------------------ */
void pmm_mark_used(u32 phys)
{
    mark_used_page(PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys)));
}

void pmm_mark_free(u32 phys)
{
    mark_free_page(PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys)));
}

int pmm_state(u32 phys)
{
    u32 p = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    if (p >= total_pages) return PMM_STATE_OOR;
    if (test_bit(resv_bitmap, p)) return PMM_STATE_RESERVED;
    if (test_bit(used_bitmap, p)) return PMM_STATE_USED;
    return PMM_STATE_FREE;
}

int pmm_is_used(u32 phys)
{
    return pmm_state(phys) == PMM_STATE_USED ? 1 : 0;
}

int pmm_is_reserved(u32 phys)
{
    return pmm_state(phys) == PMM_STATE_RESERVED ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * 区间保留 / 释放可用：[start, end)
 * ------------------------------------------------------------------------ */
void pmm_reserve_range(u32 start, u32 end)
{
    u32 p, first, last;

    if (end <= start) return;
    first = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(start));
    last  = PHYS_TO_PAGE(PAGE_ALIGN_UP(end));
    if (first >= total_pages) return;
    if (last > total_pages) last = total_pages;

    for (p = first; p < last; p++) mark_reserved_page(p);
    buddy_rebuild();
}

void pmm_release_range(u32 start, u32 end)
{
    u32 p, first, last;

    if (end <= start) return;

    /* 向内取整：只放出「完整落在区间内」的页。
     * 若向外取整，E820 中形如 [0x00000000, 0x0009FC00) 的可用区
     * 会把跨越 0x9FC00 边界的那个页（0x9F000-0xA0000）整页放出，
     * 使 1KB 保留内存被分配器当作可用内存交出。
     * 保留方向反之：reserve 采用向外取整，确保不遗漏任何一个被触及的页。 */
    first = PHYS_TO_PAGE(PAGE_ALIGN_UP(start));
    last  = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(end));
    if (first >= total_pages) return;
    if (last > total_pages) last = total_pages;
    if (first >= last) return;

    for (p = first; p < last; p++) mark_available_page(p);
    buddy_rebuild();
}

/* --------------------------------------------------------------------------
 * 查找：从指定页号起找首个空闲页
 * ------------------------------------------------------------------------ */
static u32 find_free_from(u32 start_page)
{
    u32 w, b;
    u32 start_word;

    if (total_pages == 0) return PMM_MAX_PAGES;
    if (start_page >= total_pages) return PMM_MAX_PAGES;

    start_word = start_page >> 5;

    for (w = start_word; w < BITMAP_WORDS; w++) {
        u32 busy = used_bitmap[w] | resv_bitmap[w];
        if (busy != 0xFFFFFFFFu) {
            u32 v = ~busy;
            for (b = 0; b < 32; b++) {
                if (v & (1u << b)) {
                    u32 page = (w << 5) + b;
                    if (page >= total_pages) return PMM_MAX_PAGES;
                    if (page < start_page) continue;
                    return page;
                }
            }
        }
    }
    return PMM_MAX_PAGES;
}

u32 pmm_find_first_free(void)
{
    u32 p = find_free_from(alloc_hint);
    if (p >= total_pages) p = find_free_from(0);
    if (p < total_pages) {
        alloc_hint = p + 1u;
        if (alloc_hint >= total_pages) alloc_hint = 0;
    }
    return p;
}

/* --------------------------------------------------------------------------
 * 查找连续 n 个空闲页（复用字级扫描，避免逐页 test_bit）
 * ------------------------------------------------------------------------ */
u32 pmm_find_contiguous(u32 n)
{
    u32 p, run = 0, start = 0;

    if (n == 0) return PMM_MAX_PAGES;
    if (n == 1) return pmm_find_first_free();
    if (n > total_pages) return PMM_MAX_PAGES;

    for (p = 0; p < total_pages; p++) {
        u32 w = p >> 5;
        u32 busy = used_bitmap[w] | resv_bitmap[w];

        if (busy == 0) {
            /* 整字空闲，直接按 32 页推进 */
            if (run == 0) start = p;
            run += 32;
            if (run >= n) return (run == 32) ? start : (p + 32u - (run - n));
            p += 31;
            continue;
        }
        if (busy == 0xFFFFFFFFu) {
            run = 0;
            p += 31;
            continue;
        }
        /* 部分空闲：逐位判定 */
        if (test_bit(used_bitmap, p) || test_bit(resv_bitmap, p)) {
            run = 0;
            continue;
        }        if (run == 0) start = p;
        run++;
        if (run == n) return start;
    }
    return PMM_MAX_PAGES;
}

/* --------------------------------------------------------------------------
 * 高位分配：从受管内存最高地址向下找 n 连续空闲页并标记占用。
 * 供内核大栈等"不希望与低位动态结构争抢"的场景使用。
 * ------------------------------------------------------------------------ */
u32 pmm_alloc_high_pages(u32 n)
{
    u32 p, i, base = PMM_MAX_PAGES, run = 0;
    u32 eflags;

    if (n == 0 || n > total_pages) return 0;
    eflags = pmm_lock_enter();
    for (p = total_pages; p > 0; p--) {
        u32 pi = p - 1;
        if (!test_bit(used_bitmap, pi) && !test_bit(resv_bitmap, pi)) {
            run++;
            if (run >= n) { base = pi; break; }
        } else {
            run = 0;
        }
    }
    if (run < n || base >= total_pages || base + n > total_pages) {
        pmm_lock_exit(eflags);
        return 0;
    }
    /* 保留语义：内核大栈页永不复用，避免内存耗尽自检（全量分配/释放）
     * 把当前正在使用的栈页释放后再分配给其它用途，导致执行流损坏 */
    for (i = 0; i < n; i++) {
        mark_reserved_page(base + i);
        /* 关键：绕过伙伴索引直接占页的路径必须同步摘除索引，
         * 否则 buddy_check() 层级一致性自检失败 → 扩展自检失败 → 系统安全停机。
         * （buddy_enable 之前调用时 buddy_ready=0，buddy_remove_page 安全跳过。） */
        buddy_remove_page(base + i);
        zone_add(base + i, 0, -1);   /* 保留页同步移出空闲区计数，维持 zone 自洽 */
    }
    pmm_lock_exit(eflags);
    return PAGE_TO_PHYS(base);
}

u32 pmm_count_free_in_range(u32 start, u32 end)
{
    u32 p, first, last, cnt = 0;

    if (end <= start) return 0;
    first = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(start));
    last  = PHYS_TO_PAGE(PAGE_ALIGN_UP(end));
    if (first >= total_pages) return 0;
    if (last > total_pages) last = total_pages;

    for (p = first; p < last; p++) {
        if (!test_bit(used_bitmap, p) && !test_bit(resv_bitmap, p)) cnt++;
    }
    return cnt;
}

u32 pmm_largest_free_run(void)
{
    u32 p, best = 0, run = 0;
    for (p = 0; p < total_pages; p++) {
        if (!test_bit(used_bitmap, p) && !test_bit(resv_bitmap, p)) {
            run++;
            if (run > best) best = run;
        } else {
            run = 0;
        }
    }
    return best;
}

u32 pmm_free_region_count(void)
{
    u32 p, regions = 0;
    bool in_region = false;

    for (p = 0; p < total_pages; p++) {
        bool free_pg = (!test_bit(used_bitmap, p) && !test_bit(resv_bitmap, p));
        if (free_pg && !in_region) { regions++; in_region = true; }
        else if (!free_pg) { in_region = false; }
    }
    return regions;
}

/* --------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------ */
void pmm_init(void)
{
    mem_stats_t st;
    u32 i, p;

    /* 0) 并发保护锁初始化（03 册第二轮：全路径串行化） */
    spin_lock_init(&pmm_alloc_lock);
    pmm_lock_calls = 0;

    /* 1) 起点：全部内存视为保留（不可分配），保守且安全 */
    memset(used_bitmap, 0x00, sizeof(used_bitmap));
    memset(resv_bitmap, 0xFF, sizeof(resv_bitmap));
    memset(refcnt, 0, sizeof(refcnt));

    used_pages = 0;
    alloc_hint = 0;

    /* 2) 确定受管物理内存上限 */
    e820_stats(&st);
    {
        u64 limit = st.highest_addr;
        if (limit > PMM_MAX_PHYS) limit = PMM_MAX_PHYS;
        managed_limit = PAGE_ALIGN_DOWN((u32)limit);
    }
    total_pages = PHYS_TO_PAGE(managed_limit);
    if (total_pages > PMM_MAX_PAGES) total_pages = PMM_MAX_PAGES;

    /* 受管范围之外一律置为保留，保证计数自洽 */
    for (p = total_pages; p < PMM_MAX_PAGES; p++) {
        set_bit(resv_bitmap, p);
    }

    if (total_pages == 0) {
        reserved_pages = PMM_MAX_PAGES;
        return;
    }

    /* 3) 保留页计数初值 = 受管页总数（全部保留） */
    reserved_pages = total_pages;

    /* 4) 按 E820 可用区间解除保留 */
    for (i = 0; i < e820_count(); i++) {
        const e820_entry_t *e = e820_get(i);
        if (!e) continue;
        if (e->type != E820_USABLE) continue;
        {
            u64 b = e->base;
            u64 en = e->base + e->length;
            if (b >= PMM_MAX_PHYS) continue;
            if (en > PMM_MAX_PHYS) en = PMM_MAX_PHYS;
            if (en <= b) continue;
            pmm_release_range((u32)b, (u32)en);
        }
    }

    /* 5) 保留低端 1MB（引导程序、BIOS 数据区、E820 表、VGA、内核栈） */
    pmm_reserve_range(0x00000000u, 0x00100000u);

    /* 6) 保留内核映像与静态数据（0x10000-0x80000 已含于低端 1MB，
     *    此处显式声明是为了未来内核移出低端内存时仍能正确保护。
     *    双位图设计保证重复保留不会重复计数。） */
    pmm_reserve_range(KERNEL_RESERVE_BASE, KERNEL_RESERVE_END);

    high_water = used_pages;

    /* 7) 扩展子系统：zone / NUMA / 水位 / 反碎片预留。
     *    此前所有区间操作只动位图（zones_ready 尚未置位，计数不参与），
     *    这里一次性从位图重建各区计数。 */
    zone_setup();
    numa_setup();
    wm_setup();
    antifrag_reserve();
    zone_recount();

    /* 8) 建立伙伴索引。此前 buddy_ready=0，重建是空操作，
     *    因此初始化期的区间调整不会拖慢启动。 */
    buddy_enable();
    zone_recount();
}

/* --------------------------------------------------------------------------
 * 统计接口
 * ------------------------------------------------------------------------ */
u32 pmm_total_pages(void)    { return total_pages; }
u32 pmm_used_pages(void)     { return used_pages; }
u32 pmm_reserved_pages(void) { return reserved_pages; }
u32 pmm_managed_bytes(void)  { return managed_limit; }

/* 无锁内部读取：供锁内查询函数（frag_index / pressure_level / stats）
 * 读取空闲页数，避免「持锁调用公开入口」造成自锁重入。调用方保证：
 * 在临界区内时数据一致；临界区外串行阶段（自检/初始化）同样安全。 */
static u32 pmm_free_count_raw(void)
{
    if (total_pages <= used_pages + reserved_pages) return 0;
    return total_pages - used_pages - reserved_pages;
}

/* 读一致性：空闲页计数在临界区内快照，避免与并发分配/释放交错 */
u32 pmm_free_page_count(void)
{
    u32 r, eflags;
    eflags = pmm_lock_enter();
    r = pmm_free_count_raw();
    pmm_lock_exit(eflags);
    return r;
}

/* 统计聚合在临界区内一次性快照（内部查询函数均无锁，直接读状态） */
static void _pmm_stats_locked(pmm_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->total_pages      = total_pages;
    out->used_pages       = used_pages;
    out->free_pages       = (total_pages > used_pages + reserved_pages)
                            ? (total_pages - used_pages - reserved_pages) : 0;
    out->reserved_pages   = reserved_pages;
    out->managed_bytes    = managed_limit;
    out->alloc_calls      = alloc_calls;
    out->free_calls       = free_calls;
    out->alloc_fail       = alloc_fail;
    out->high_water       = high_water;
    out->largest_free_run = pmm_largest_free_run();
    out->free_regions     = pmm_free_region_count();

    /* 扩展子系统统计 */
    out->buddy_blocks     = buddy_free_block_count();
    out->zone_free_dma    = pmm_zone_free(ZONE_DMA);
    out->zone_free_normal = pmm_zone_free(ZONE_NORMAL);
    out->zone_used_dma    = pmm_zone_used(ZONE_DMA);
    out->zone_used_normal = pmm_zone_used(ZONE_NORMAL);
    out->numa_nodes       = pmm_numa_node_count();
    out->wm_min           = pmm_watermark(PMM_WM_MIN);
    out->wm_low           = pmm_watermark(PMM_WM_LOW);
    out->wm_high          = pmm_watermark(PMM_WM_HIGH);
    out->pressure         = pmm_pressure_level();
    out->shrinker_count   = pmm_shrinker_count();
    out->reclaim_calls    = pmm_reclaim_calls();
    out->reclaim_freed    = pmm_reclaim_freed();
    out->oom_count        = pmm_oom_count();
    out->scrub_count      = pmm_scrub_count();
    out->badpage_count    = pmm_badpage_count();
    out->hotplug_count    = pmm_hotplug_count();
    out->huge_used        = pmm_huge_used();
    out->huge_total       = pmm_huge_count();
    out->ioremap_count    = pmm_ioremap_count();
    out->antifrag_pages   = pmm_antifrag_pages();
    out->frag_index       = pmm_frag_index();
}

/* 锁包装：统计快照 */
void pmm_stats(pmm_stats_t *out)
{
    u32 eflags;
    if (!out) return;
    eflags = pmm_lock_enter();
    _pmm_stats_locked(out);
    pmm_lock_exit(eflags);
}

void pmm_set_debug(u32 level) { debug_level = level; }
u32  pmm_get_debug(void)      { return debug_level; }

/* --------------------------------------------------------------------------
 * 毒化：释放时填充特征值，便于发现「使用已释放页」
 * ------------------------------------------------------------------------ */
static void poison_page(u32 page, u32 pattern)
{
    u32 addr = PAGE_TO_PHYS(page);
    u32 *q = (u32 *)addr;
    u32 i;
    for (i = 0; i < PAGE_SIZE / 4u; i++) q[i] = pattern;
}

u32 pmm_poison_check(u32 phys)
{
    u32 p, i;
    const u32 *q;

    if (!PAGE_IS_ALIGNED(phys)) return 1;
    p = PHYS_TO_PAGE(phys);
    if (p >= total_pages) return 2;
    if (pmm_state(phys) != PMM_STATE_FREE) return 3;

    q = (const u32 *)phys;
    for (i = 0; i < PAGE_SIZE / 4u; i++) {
        if (q[i] != PMM_POISON_FREE) return 4;
    }
    return 0;
}

/* ============================================================================
 * XOS 物理内存管理器（PMM）—— 扩展子系统
 *
 * 本段代码由 pmm.c 内联合并为同一翻译单元，目的是直接复用 pmm.c 中的
 * 静态状态（used_bitmap / resv_bitmap / refcnt / total_pages / managed_limit），
 * 避免把内部位图暴露为跨文件全局符号而破坏封装。
 *
 * 覆盖第 03 册「物理内存管理」中此前未落地的子域：
 *   S05 伙伴系统分配与合并      S06 内存区域 zone 管理
 *   S07 NUMA 节点与亲和性       S08 内存热插拔
 *   S09 大页（2M）管理          S11 内存碎片整理
 *   S12 水位与压力管理          S13 内存回收与换出（shrinker 框架）
 *   S14 OOM 处理                S17 IO 内存映射 ioremap
 *   S18 内存加密与保护（释放清零）  S19 内存错误检测与隔离（坏页下线）
 *   S20 分配策略与优先级
 *
 * 设计约定：
 *   1) 位图（used/reserved）是「某页是否空闲」的唯一权威；
 *      伙伴位图是「某 2^order 块是否整块空闲」的层级索引，二者由
 *      buddy_check() 的一致性自检绑定。
 *   2) 层级标记是累积式的：一个 order 块空闲 ⇒ 它在所有更低层级上的
 *      子块也标记为空闲。因此取块要清整棵子树，还块要设整棵子树。
 *   3) 任何「绕过伙伴索引直接占页」的路径（zone 限定、节点限定、高地址
 *      优先、位图线性回退）都必须调用 buddy_remove_page() 把该页从索引
 *      里摘掉，否则索引会与位图失配。
 * ========================================================================== */

/* ==========================================================================
 * S05 伙伴系统分配与合并
 * ========================================================================== */
/* （buddy 宏与数组定义已上移至文件顶部统一区，避免 pmm_init 诊断重复声明） */

static u32 buddy_block_count(u32 order) { return PMM_MAX_PAGES >> order; }
static u32 buddy_word(u32 order, u32 block) { return (u32)buddy_off[order] + (block >> 5); }
static u32 buddy_test(u32 order, u32 block) { return (buddy_map[buddy_word(order, block)] >> (block & 31u)) & 1u; }
static void buddy_set(u32 order, u32 block) { buddy_map[buddy_word(order, block)] |= (1u << (block & 31u)); }
static void buddy_clear(u32 order, u32 block) { buddy_map[buddy_word(order, block)] &= ~(1u << (block & 31u)); }

/* 页是否空闲：直接查位图，与伙伴索引无关 */
static u32 page_is_free_raw(u32 p)
{
    if (p >= total_pages) return 0;
    if (test_bit(used_bitmap, p)) return 0;
    if (test_bit(resv_bitmap, p)) return 0;
    return 1u;
}

static void buddy_set_tree(u32 order, u32 block)
{
    u32 o, base, cnt, i;
    if (order > BUDDY_MAX_ORDER) return;
    buddy_set(order, block);
    for (o = 0; o < order; o++) {
        base = block << (order - o);
        cnt  = 1u << (order - o);
        if (base + cnt > buddy_block_count(o)) break;
        for (i = 0; i < cnt; i++) buddy_set(o, base + i);
    }
}

static void buddy_clear_tree(u32 order, u32 block)
{
    u32 o, base, cnt, i;
    if (order > BUDDY_MAX_ORDER) return;
    buddy_clear(order, block);
    for (o = 0; o < order; o++) {
        base = block << (order - o);
        cnt  = 1u << (order - o);
        if (base + cnt > buddy_block_count(o)) break;
        for (i = 0; i < cnt; i++) buddy_clear(o, base + i);
    }
}

/* 从索引中摘除单个页：叶子清掉后，其所有祖先必然不再是「整块空闲」 */
static void buddy_remove_page(u32 p)
{
    u32 o, b;
    if (!buddy_ready || p >= total_pages) return;
    buddy_clear(0, p);
    b = p;
    for (o = 1; o <= BUDDY_MAX_ORDER; o++) {
        b >>= 1;
        if (b >= buddy_block_count(o)) break;
        buddy_clear(o, b);
    }
}

static void buddy_rebuild(void)
{
    u32 o, b, n;
    if (!buddy_ready) return;
    buddy_rebuild_count++;
    memset(buddy_map, 0, sizeof(buddy_map));
    memset(buddy_hint, 0, sizeof(buddy_hint));
    n = buddy_block_count(0);
    {
        u32 rb_set_count = 0;
        for (b = 0; b < n; b++) {
            if (page_is_free_raw(b)) {
                rb_set_count++;
                buddy_set(0, b);
            }
        }
        buddy_rebuild_set_count = rb_set_count;
    }
    for (o = 1; o <= BUDDY_MAX_ORDER; o++) {
        n = buddy_block_count(o);
        for (b = 0; b < n; b++) {
            if (buddy_test(o - 1u, b * 2u) && buddy_test(o - 1u, b * 2u + 1u)) {
                buddy_set(o, b);
            }
        }
    }
}

/* 打开伙伴索引并做一次全量重建 */
static void buddy_enable(void)
{
    buddy_ready = 1;
    buddy_rebuild();
}

/* 轮转扫描，避免每次从头找导致 O(n^2) */
static u32 buddy_find(u32 order)
{
    u32 b, n, start;
    n = buddy_block_count(order);
    start = buddy_hint[order];
    if (start >= n) start = 0;
    for (b = start; b < n; b++) {
        if (buddy_test(order, b)) { buddy_hint[order] = b; return b; }
    }
    for (b = 0; b < start; b++) {
        if (buddy_test(order, b)) { buddy_hint[order] = b; return b; }
    }
    buddy_hint[order] = 0;
    return BUDDY_NONE;
}

/* 取一个 2^order 页、按该阶自然对齐的块，返回起始页号 */
static u32 buddy_take(u32 order)
{
    u32 o, b;
    if (!buddy_ready) return PMM_MAX_PAGES;
    if (order > BUDDY_MAX_ORDER) return PMM_MAX_PAGES;
    for (o = order; o <= BUDDY_MAX_ORDER; o++) {
        b = buddy_find(o);
        if (b == BUDDY_NONE) continue;
        buddy_clear_tree(o, b);
        /* 索引采用累积标记：某块被标记为空闲，意味着它的整棵子树都空闲。
         * 该块被取走后，其所有祖先都不再「整块空闲」，必须一并清除；
         * 否则祖先位会一直停留在陈旧状态，之后按高阶取块时会把
         * 已分配的页当成空闲页交出去，造成双重分配。 */
        {
            u32 ao = o, ab = b;
            while (ao < BUDDY_MAX_ORDER) {
                ab >>= 1;
                ao++;
                if (ab >= buddy_block_count(ao)) break;
                buddy_clear(ao, ab);
            }
        }
        while (o > order) {
            o--;
            buddy_set_tree(o, b * 2u + 1u);   /* 右半整块归还 */
            b = b * 2u;                       /* 左半继续向下分裂 */
        }
        buddy_hint[order] = b + 1u;
        return b << order;
    }
    return PMM_MAX_PAGES;
}

/* 归还一个 2^order 页块，并沿伙伴链向上合并 */
static void buddy_give(u32 page, u32 order)
{
    u32 o, b, home;
    if (!buddy_ready) return;
    if (order > BUDDY_MAX_ORDER) return;
    if (page >= total_pages) return;
    if (page & ((1u << order) - 1u)) return;      /* 未按该阶对齐，拒绝 */
    b = page >> order;
    if (b >= buddy_block_count(order)) return;
    home = b;                       /* 记下请求阶的块号，hint 必须按它回写 */
    buddy_set_tree(order, b);
    o = order;
    while (o < BUDDY_MAX_ORDER) {
        u32 mate = b ^ 1u;
        if (mate >= buddy_block_count(o)) break;
        if (!buddy_test(o, mate)) break;          /* 伙伴不空闲，无法合并 */
        buddy_clear_tree(o, b);
        buddy_clear_tree(o, mate);
        b >>= 1;
        o++;
        buddy_set_tree(o, b);
    }
    if (buddy_hint[order] > home) buddy_hint[order] = home;
}

/* 层级一致性：任一层级标记必须与下一层两个子块完全对应 */
static u32 buddy_check(void)
{
    u32 o, b, n;
    for (b = 0; b < buddy_block_count(0); b++) {
        if (buddy_test(0, b) != page_is_free_raw(b)) return 1;
    }
    for (o = 1; o <= BUDDY_MAX_ORDER; o++) {
        n = buddy_block_count(o);
        for (b = 0; b < n; b++) {
            u32 both = (buddy_test(o - 1u, b * 2u) && buddy_test(o - 1u, b * 2u + 1u)) ? 1u : 0u;
            if (buddy_test(o, b) != both) return 2;
        }
    }
    return 0;
}

/* 各阶字偏移必须与 block_count 推导值一致，否则层级会互相踩踏。
 * 每阶占字 = ceil(block_count/32)：(n + 31) >> 5 —— 不能整除下取整，
 * 否则余数块会溢出写入下一阶的字（如 order-9 48 块需 2 字而非 1 字）。 */
static u32 buddy_off_check(void)
{
    u32 o, off = 0;
    for (o = 0; o < BUDDY_ORDERS; o++) {
        if ((u32)buddy_off[o] != off) return 1;
        off += (buddy_block_count(o) + 31u) >> 5;
    }
    if (off > BUDDY_MAP_WORDS) return 2;
    return 0;
}

static u32 buddy_order_for(u32 n)
{
    u32 o = 0;
    while (o < BUDDY_MAX_ORDER && ((1u << o) < n)) o++;
    return o;
}

/* 极大空闲块数量：
 * 索引是累积标记，同一段空闲内存在多个层级的标记位，逐层累加会严重重复计数。
 * 只统计「自身已标记、但父块未标记」的块，得到的才是互不重叠的极大空闲块，
 * 它同时是外部碎片程度的直接度量。 */
static u32 buddy_free_block_count(void)
{
    u32 o, b, n, c = 0;
    if (!buddy_ready) return 0;
    n = buddy_block_count(0);
    for (b = 0; b < n; b++) if (buddy_test(0, b)) c++;
    for (o = 1; o <= BUDDY_MAX_ORDER; o++) {
        n = buddy_block_count(o);
        for (b = 0; b < n; b++) {
            if (buddy_test(o, b)) {
                c -= 2u;            /* 两个子块已被上一轮计入，改记这一个父块 */
                c += 1u;
            }
        }
    }
    return c;
}

u32 pmm_buddy_selftest(void)
{
    u32 a, b, order, n;

    if (!buddy_ready) return 1;
    {
        u32 bcc = buddy_off_check();
        u32 bck = buddy_check();
        if (bcc || bck) {
            con_puts("BDIAG off=");
            con_put_dec(bcc);
            con_puts(" check=");
            con_put_dec(bck);
            con_puts(" tp=");
            con_put_dec(total_pages);
            con_puts(" mp=");
            con_put_dec(PMM_MAX_PAGES);
            con_puts("\n");
            if (bck) {
                u32 pb, nbad = 0;
                for (pb = 0; pb < buddy_block_count(0); pb++) {
                    if (buddy_test(0, pb) != page_is_free_raw(pb)) {
                        con_puts("  MISMATCH p=");
                        con_put_dec(pb);
                        con_puts(" buddy=");
                        con_put_dec(buddy_test(0, pb));
                        con_puts(" used=");
                        con_put_dec(test_bit(used_bitmap, pb) ? 1u : 0u);
                        con_puts(" resv=");
                        con_put_dec(test_bit(resv_bitmap, pb) ? 1u : 0u);
                        con_puts(" ref=");
                        con_put_dec(refcnt[pb]);
                        con_puts("\n");
                        nbad++;
                        if (nbad >= 30u) break;   /* 最多打印 30 处 */
                    }
                }
                con_puts("  TOTBAD=");
                con_put_dec(nbad);
                con_puts("\n");
            }
        }
        if (bcc != 0) return 2;
        if (bck != 0) return 3;
    }

    /* 取 8 页块：对齐、全空闲、索引已摘除 */
    a = buddy_take(3);
    if (a == PMM_MAX_PAGES) return 4;
    if (a & 7u) return 5;
    if (a + 8u > total_pages) return 6;
    for (n = 0; n < 8u; n++) if (!page_is_free_raw(a + n)) return 7;
    for (n = 0; n < 8u; n++) { mark_used_page(a + n); refcnt[a + n] = 1; }
    if (buddy_check() != 0) return 8;

    /* 逐页归还后应自动合并回 order 3 */
    for (n = 0; n < 8u; n++) {
        mark_free_page(a + n);
        buddy_give(a + n, 0);
        if (buddy_check() != 0) return 9;
    }
    if (!buddy_test(3, a >> 3)) return 10;

    /* 各阶交替取还，覆盖分裂与合并路径 */
    for (order = 0; order <= 6u; order++) {
        u32 pages = 1u << order;
        b = buddy_take(order);
        if (b == PMM_MAX_PAGES) return 11;
        if (b & (pages - 1u)) return 12;
        if (b + pages > total_pages) return 13;
        for (n = 0; n < pages; n++) { mark_used_page(b + n); refcnt[b + n] = 1; }
        if (buddy_check() != 0) return 14;
        for (n = 0; n < pages; n++) mark_free_page(b + n);
        buddy_give(b, order);
        if (buddy_check() != 0) return 15;
    }

    /* 未按该阶对齐的块号必须被拒绝，否则会把别人的页标记成空闲 */
    buddy_give(1, 3);
    if (buddy_check() != 0) return 16;

    /* 绕过索引占页后，索引仍须与位图一致 */
    {
        u32 p;
        for (p = 0; p < total_pages; p++) {
            if (page_is_free_raw(p)) {
                buddy_remove_page(p);
                mark_used_page(p);
                refcnt[p] = 1;
                if (buddy_check() != 0) return 17;
                mark_free_page(p);
                buddy_give(p, 0);
                if (buddy_check() != 0) return 18;
                break;
            }
        }
    }
    return 0;
}

/* ==========================================================================
 * S06 内存区域 zone 管理
 *   ZONE_DMA   : 0 - 16 MB，供只能寻址低 16 MB 的旧式 DMA 设备使用
 *   ZONE_NORMAL: 16 MB 以上
 * ========================================================================== */
#define ZONE_DMA        0
#define ZONE_NORMAL     1
#define ZONE_COUNT      2
#define ZONE_DMA_LIMIT  (16u * 1024u * 1024u)

static const char *zone_name[ZONE_COUNT] = { "DMA", "Normal" };
static u32 zone_start_page[ZONE_COUNT];
static u32 zone_end_page[ZONE_COUNT];
static u32 zone_free_pages[ZONE_COUNT];
static u32 zone_used_pages[ZONE_COUNT];
static u32 zone_resv_pages[ZONE_COUNT];
static u32 zones_ready = 0;

static u32 zone_of_page(u32 p)
{
    return (p < zone_end_page[ZONE_DMA]) ? ZONE_DMA : ZONE_NORMAL;
}

static void zone_setup(void)
{
    u32 dma_pages = ZONE_DMA_LIMIT / PAGE_SIZE;
    zone_start_page[ZONE_DMA] = 0;
    zone_end_page[ZONE_DMA]   = dma_pages;
    zone_start_page[ZONE_NORMAL] = dma_pages;
    zone_end_page[ZONE_NORMAL]   = PMM_MAX_PAGES;
    zones_ready = 1;
}

/* 从位图重新统计各区页数（初始化结束后调用一次，之后靠增量维护） */
static void zone_recount(void)
{
    u32 z, p;
    for (z = 0; z < ZONE_COUNT; z++) {
        zone_free_pages[z] = 0;
        zone_used_pages[z] = 0;
        zone_resv_pages[z] = 0;
    }
    if (!zones_ready) return;
    for (p = 0; p < total_pages; p++) {
        z = zone_of_page(p);
        if (test_bit(resv_bitmap, p)) zone_resv_pages[z]++;
        else if (test_bit(used_bitmap, p)) zone_used_pages[z]++;
        else zone_free_pages[z]++;
    }
}

/* which: 0=空闲 1=已用 2=保留；delta: +1 / -1 */
static void zone_add(u32 p, u32 which, int delta)
{
    u32 z;
    if (!zones_ready || p >= total_pages) return;
    z = zone_of_page(p);
    if (which == 0) {
        if (delta > 0) zone_free_pages[z]++;
        else if (zone_free_pages[z]) zone_free_pages[z]--;
    } else if (which == 1) {
        if (delta > 0) zone_used_pages[z]++;
        else if (zone_used_pages[z]) zone_used_pages[z]--;
    } else {
        if (delta > 0) zone_resv_pages[z]++;
        else if (zone_resv_pages[z]) zone_resv_pages[z]--;
    }
}

u32 pmm_zone_count(void) { return ZONE_COUNT; }
const char *pmm_zone_name(u32 z) { return (z < ZONE_COUNT) ? zone_name[z] : "?"; }
u32 pmm_zone_free(u32 z) { return (z < ZONE_COUNT) ? zone_free_pages[z] : 0; }
u32 pmm_zone_used(u32 z) { return (z < ZONE_COUNT) ? zone_used_pages[z] : 0; }
u32 pmm_zone_reserved(u32 z) { return (z < ZONE_COUNT) ? zone_resv_pages[z] : 0; }
u32 pmm_zone_end_page(u32 z) { return (z < ZONE_COUNT) ? zone_end_page[z] : 0; }
u32 pmm_zone_limit(void) { return ZONE_DMA_LIMIT; }

/* 在指定 zone 内分配单页 */
static u32 _pmm_alloc_zone_locked(u32 z)
{
    u32 p, end;
    if (z >= ZONE_COUNT) return 0;
    end = zone_end_page[z];
    if (end > total_pages) end = total_pages;
    for (p = zone_start_page[z]; p < end; p++) {
        if (page_is_free_raw(p)) {
            buddy_remove_page(p);
            mark_used_page(p);
            refcnt[p] = 1;
            alloc_calls++;
            return PAGE_TO_PHYS(p);
        }
    }
    alloc_fail++;
    return 0;
}

/* 锁包装：zone 分配与其它分配路径串行化 */
u32 pmm_alloc_zone(u32 z)
{
    u32 r, eflags = pmm_lock_enter();
    r = _pmm_alloc_zone_locked(z);
    pmm_lock_exit(eflags);
    return r;
}

/* ==========================================================================
 * S07 NUMA 节点与亲和性
 * 本机为单节点（UMA）。节点表、距离矩阵、按节点分配与本地性查询接口
 * 均为真实实现；多节点填充留待具备多节点硬件时启用。
 * ========================================================================== */
#define NUMA_MAX_NODES  4
#define NUMA_LOCAL      10u
#define NUMA_REMOTE     20u

typedef struct {
    u8  online;
    u32 start_page;
    u32 end_page;
    u8  distance[NUMA_MAX_NODES];
} numa_node_t;

static numa_node_t numa_nodes[NUMA_MAX_NODES];
static u32 numa_node_count = 0;

static void numa_setup(void)
{
    u32 i, j;
    memset(numa_nodes, 0, sizeof(numa_nodes));
    numa_nodes[0].online     = 1;
    numa_nodes[0].start_page = 0;
    numa_nodes[0].end_page   = PMM_MAX_PAGES;
    for (i = 0; i < NUMA_MAX_NODES; i++) {
        for (j = 0; j < NUMA_MAX_NODES; j++) {
            numa_nodes[i].distance[j] = (i == j) ? NUMA_LOCAL : NUMA_REMOTE;
        }
    }
    numa_node_count = 1;
}

u32 pmm_numa_node_count(void) { return numa_node_count; }
u32 pmm_numa_node_of(u32 phys)
{
    u32 p = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    u32 n;
    for (n = 0; n < numa_node_count; n++) {
        if (numa_nodes[n].online && p >= numa_nodes[n].start_page && p < numa_nodes[n].end_page) return n;
    }
    return 0xFFFFFFFFu;
}
u32 pmm_numa_distance(u32 a, u32 b)
{
    if (a >= numa_node_count || b >= numa_node_count) return 0xFFu;
    return numa_nodes[a].distance[b];
}
u32 pmm_numa_start(u32 n) { return (n < numa_node_count) ? PAGE_TO_PHYS(numa_nodes[n].start_page) : 0; }
u32 pmm_numa_end(u32 n) { return (n < numa_node_count) ? PAGE_TO_PHYS(numa_nodes[n].end_page) : 0; }
u32 pmm_numa_free(u32 n)
{
    u32 p, c = 0;
    if (n >= numa_node_count || !numa_nodes[n].online) return 0;
    for (p = numa_nodes[n].start_page; p < numa_nodes[n].end_page && p < total_pages; p++) {
        if (page_is_free_raw(p)) c++;
    }
    return c;
}
static u32 _pmm_alloc_node_locked(u32 n)
{
    u32 p, end;
    if (n >= numa_node_count || !numa_nodes[n].online) return 0;
    end = numa_nodes[n].end_page;
    if (end > total_pages) end = total_pages;
    for (p = numa_nodes[n].start_page; p < end; p++) {
        if (page_is_free_raw(p)) {
            buddy_remove_page(p);
            mark_used_page(p);
            refcnt[p] = 1;
            alloc_calls++;
            return PAGE_TO_PHYS(p);
        }
    }
    alloc_fail++;
    return 0;
}

/* 锁包装：NUMA 节点分配与其它分配路径串行化 */
u32 pmm_alloc_node(u32 n)
{
    u32 r, eflags = pmm_lock_enter();
    r = _pmm_alloc_node_locked(n);
    pmm_lock_exit(eflags);
    return r;
}

/* ==========================================================================
 * S08 内存热插拔
 * 状态机：OFFLINE --add--> ONLINE --remove--> OFFLINE
 * remove 要求区间内没有任何「已分配」页，避免把在用页从分配器摘走。
 * ========================================================================== */
#define HOTPLUG_OFFLINE  0
#define HOTPLUG_ONLINE   1
#define HOTPLUG_MAX      8

typedef struct {
    u32 start_page;
    u32 end_page;
    u32 state;
} hotplug_region_t;

static hotplug_region_t hp_regions[HOTPLUG_MAX];
static u32 hp_count = 0;

u32 pmm_hotplug_count(void) { return hp_count; }
u32 pmm_hotplug_state(u32 idx) { return (idx < hp_count) ? hp_regions[idx].state : 0xFFFFFFFFu; }
u32 pmm_hotplug_start(u32 idx) { return (idx < hp_count) ? PAGE_TO_PHYS(hp_regions[idx].start_page) : 0; }
u32 pmm_hotplug_end(u32 idx) { return (idx < hp_count) ? PAGE_TO_PHYS(hp_regions[idx].end_page) : 0; }

int pmm_hotplug_add(u32 start, u32 end)
{
    u32 idx, p, first, last;
    if (end <= start) return PMM_EINVAL;
    first = PHYS_TO_PAGE(PAGE_ALIGN_UP(start));
    last  = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(end));
    if (last > PMM_MAX_PAGES) last = PMM_MAX_PAGES;
    if (first >= last) return PMM_EINVAL;

    for (idx = 0; idx < hp_count; idx++) {
        if (hp_regions[idx].state == HOTPLUG_ONLINE &&
            first < hp_regions[idx].end_page && last > hp_regions[idx].start_page) {
            return PMM_EBUSY;                     /* 与已有在线区重叠 */
        }
    }

    /* 受管上限要跟着抬高，否则新内存永远不会被算进 total_pages */
    if (last > total_pages) {
        total_pages = last;
        if (PAGE_TO_PHYS(total_pages) > managed_limit) managed_limit = PAGE_TO_PHYS(total_pages);
    }
    for (p = first; p < last; p++) {
        mark_available_page(p);                    /* 解除保留 */
    }
    idx = hp_count++;
    hp_regions[idx].start_page = first;
    hp_regions[idx].end_page   = last;
    hp_regions[idx].state      = HOTPLUG_ONLINE;
    zone_recount();
    buddy_rebuild();
    return (int)idx;
}

int pmm_hotplug_remove(u32 idx)
{
    u32 p;
    if (idx >= hp_count) return PMM_EINVAL;
    if (hp_regions[idx].state != HOTPLUG_ONLINE) return PMM_EBUSY;
    for (p = hp_regions[idx].start_page; p < hp_regions[idx].end_page; p++) {
        if (test_bit(used_bitmap, p)) return PMM_EBUSY;   /* 仍有在用页 */
    }
    for (p = hp_regions[idx].start_page; p < hp_regions[idx].end_page; p++) {
        mark_reserved_page(p);
    }
    hp_regions[idx].state = HOTPLUG_OFFLINE;
    zone_recount();
    buddy_rebuild();
    return PMM_OK;
}

/* ==========================================================================
 * S09 大页（2M）管理
 * 2 MB = 512 页 = 伙伴系统 order 9；order 9 的块天然按 512 页对齐，
 * 因此必然满足 2 MB 对齐要求，无需额外对齐处理。
 * 1 GB 大页需要 262144 页，超出本机 128 MB 受管上限，故不提供。
 * ========================================================================== */
#define HUGE_PAGE_SHIFT   21u
#define HUGE_PAGE_SIZE    (1u << HUGE_PAGE_SHIFT)
#define HUGE_PAGE_ORDER   9u
#define HUGE_PAGE_COUNT   (PMM_MAX_PHYS / HUGE_PAGE_SIZE)     /* 64 */
#define HUGE_MAP_WORDS    ((HUGE_PAGE_COUNT + 31u) / 32u)

static u32 huge_bitmap[HUGE_MAP_WORDS];
static u32 huge_used = 0;

u32 pmm_huge_count(void) { return HUGE_PAGE_COUNT; }
u32 pmm_huge_size(void) { return HUGE_PAGE_SIZE; }
u32 pmm_huge_used(void) { return huge_used; }

static u32 _pmm_alloc_huge_locked(void)
{
    u32 i, blk, p, base, pages;
    pages = 1u << HUGE_PAGE_ORDER;
    blk = buddy_take(HUGE_PAGE_ORDER);
    if (blk == PMM_MAX_PAGES) return 0;
    base = blk;
    if (base + pages > total_pages) {
        buddy_give(base, HUGE_PAGE_ORDER);
        return 0;
    }
    /* 槽号必须由实际取到的块推导。伙伴系统返回的是「某个」空闲的
     * order-9 块，与调用方期望的下标无关；若沿用循环下标记位图，
     * 分配与释放会认到不同的槽，导致释放报「未分配」而把大页漏掉。 */
    i = base >> (HUGE_PAGE_SHIFT - PAGE_SHIFT);
    if (i >= HUGE_PAGE_COUNT || test_bit(huge_bitmap, i)) {
        buddy_give(base, HUGE_PAGE_ORDER);
        return 0;
    }
    for (p = base; p < base + pages; p++) {
        mark_used_page(p);
        refcnt[p] = 1;
    }
    set_bit(huge_bitmap, i);
    huge_used++;
    return PAGE_TO_PHYS(base);
}

/* 锁包装：大页（2M）分配与常规分配路径串行化 */
u32 pmm_alloc_huge(void)
{
    u32 r, eflags = pmm_lock_enter();
    r = _pmm_alloc_huge_locked();
    pmm_lock_exit(eflags);
    return r;
}

static int _pmm_free_huge_locked(u32 phys)
{
    u32 p, base, i, pages;
    if (!PAGE_IS_ALIGNED(phys)) return PMM_EINVAL;
    if (phys & (HUGE_PAGE_SIZE - 1u)) return PMM_EINVAL;     /* 未按 2M 对齐 */
    base = PHYS_TO_PAGE(phys);
    pages = 1u << HUGE_PAGE_ORDER;
    if (base + pages > total_pages) return PMM_EINVAL;
    i = base >> (HUGE_PAGE_SHIFT - PAGE_SHIFT);
    if (i >= HUGE_PAGE_COUNT) return PMM_EINVAL;
    if (!test_bit(huge_bitmap, i)) return PMM_ENOTALLOC;
    for (p = base; p < base + pages; p++) {
        if (pmm_state(PAGE_TO_PHYS(p)) != PMM_STATE_USED) return PMM_ENOTALLOC;
        if (refcnt[p] > 1) return PMM_EREFCNT;
    }
    for (p = base; p < base + pages; p++) {
        mark_free_page(p);
    }
    buddy_give(base, HUGE_PAGE_ORDER);
    clear_bit(huge_bitmap, i);
    if (huge_used) huge_used--;
    return PMM_OK;
}

/* 锁包装：大页释放与分配路径串行化 */
int pmm_free_huge(u32 phys)
{
    int r;
    u32 eflags = pmm_lock_enter();
    r = _pmm_free_huge_locked(phys);
    pmm_lock_exit(eflags);
    return r;
}

/* ==========================================================================
 * S11 内存碎片整理
 * 反碎片预留：初始化时从最大连续空闲段尾部切出一段连续块并标记保留，
 * 使常规分配无法把它切碎。当大块连续请求失败时，pmm_compact() 释放该
 * 预留块，用一段完整连续内存去满足请求 —— 这是在不具备页迁移能力
 * （尚未开启分页）前提下真正可行的「整理」。
 * ========================================================================== */
static u32 antifrag_start_page = 0;
static u32 antifrag_pages = 0;

u32 pmm_antifrag_pages(void) { return antifrag_pages; }
u32 pmm_antifrag_start(void) { return PAGE_TO_PHYS(antifrag_start_page); }

/* 外部碎片指数：1000 = 最大空闲段等于全部空闲页（完全连续），越小越碎 */
u32 pmm_frag_index(void)
{
    u32 free_pages = pmm_free_count_raw();
    u32 largest = pmm_largest_free_run();
    if (free_pages == 0) return 0;
    return (largest * 1000u) / free_pages;
}

static void antifrag_reserve(void)
{
    u32 p, run = 0, cur_start = 0, best_start = 0, best_len = 0;
    u32 want = 1024u;                                  /* 4 MB */

    for (p = 0; p < total_pages; p++) {
        if (page_is_free_raw(p)) {
            if (run == 0) cur_start = p;
            run++;
            if (run > best_len) { best_len = run; best_start = cur_start; }
        } else {
            run = 0;
        }
    }
    if (best_len < want * 2u) return;                  /* 内存过小则不做预留 */

    /* 从该连续段尾部切出，保留头部仍然完整 */
    antifrag_start_page = best_start + best_len - want;
    antifrag_pages = want;
    for (p = antifrag_start_page; p < antifrag_start_page + want; p++) {
        mark_reserved_page(p);
    }
}

/* 释放反碎片预留块；返回本次释放的页数（0 表示无预留可放） */
u32 pmm_compact(void)
{
    u32 p, n = 0;
    if (antifrag_pages == 0) return 0;
    for (p = antifrag_start_page; p < antifrag_start_page + antifrag_pages; p++) {
        if (test_bit(resv_bitmap, p)) {
            mark_available_page(p);
            n++;
        }
    }
    antifrag_pages = 0;
    buddy_rebuild();
    return n;
}

/* ==========================================================================
 * S12 水位与压力管理
 * ========================================================================== */
#define PMM_WM_MIN   0
#define PMM_WM_LOW   1
#define PMM_WM_HIGH  2
#define PMM_WM_COUNT 3

#define PMM_PRESS_NORMAL    0
#define PMM_PRESS_LOW       1
#define PMM_PRESS_MIN       2
#define PMM_PRESS_CRITICAL  3

static u32 watermarks[PMM_WM_COUNT];

static void wm_setup(void)
{
    watermarks[PMM_WM_MIN]  = total_pages / 64u;
    watermarks[PMM_WM_LOW]  = total_pages / 32u;
    watermarks[PMM_WM_HIGH] = total_pages / 16u;
    if (watermarks[PMM_WM_MIN] == 0 && total_pages > 0) watermarks[PMM_WM_MIN] = 1;
    if (watermarks[PMM_WM_LOW] <= watermarks[PMM_WM_MIN]) watermarks[PMM_WM_LOW] = watermarks[PMM_WM_MIN] + 1u;
    if (watermarks[PMM_WM_HIGH] <= watermarks[PMM_WM_LOW]) watermarks[PMM_WM_HIGH] = watermarks[PMM_WM_LOW] + 1u;
}

u32 pmm_watermark(u32 level) { return (level < PMM_WM_COUNT) ? watermarks[level] : 0; }

int pmm_watermark_ok(u32 free_pages, u32 level)
{
    if (level >= PMM_WM_COUNT) return 0;
    return (free_pages > watermarks[level]) ? 1 : 0;
}

u32 pmm_pressure_level(void)
{
    u32 free_pages = pmm_free_count_raw();
    if (free_pages == 0) return PMM_PRESS_CRITICAL;
    if (free_pages <= watermarks[PMM_WM_MIN]) return PMM_PRESS_CRITICAL;
    if (free_pages <= watermarks[PMM_WM_LOW]) return PMM_PRESS_MIN;
    if (free_pages <= watermarks[PMM_WM_HIGH]) return PMM_PRESS_LOW;
    return PMM_PRESS_NORMAL;
}

const char *pmm_pressure_name(u32 lv)
{
    switch (lv) {
    case PMM_PRESS_NORMAL:   return "NORMAL";
    case PMM_PRESS_LOW:      return "LOW";
    case PMM_PRESS_MIN:      return "MIN";
    case PMM_PRESS_CRITICAL: return "CRITICAL";
    default:                 return "?";
    }
}

/* ==========================================================================
 * S13 内存回收（shrinker 框架）
 * 真正的换出需要块设备驱动与分页支持（本册尚未具备），因此这里落地的是
 * 可回收缓存必须实现的注册接口与回收调度：内存吃紧时按注册顺序调用各
 * shrinker，由它们释放自己持有的页。框架与调用语义是真实的、可测的。
 * ========================================================================== */
#define SHRINKER_MAX 8

typedef u32 (*shrink_fn)(u32 nr_to_free, void *arg);

typedef struct {
    const char *name;
    shrink_fn   fn;
    void       *arg;
    u32         calls;
    u32         freed;
    u8          active;
} shrinker_t;

static shrinker_t shrinkers[SHRINKER_MAX];
static u32 shrinker_count = 0;
static u32 reclaim_calls = 0;
static u32 reclaim_freed_total = 0;

int pmm_register_shrinker(const char *name, shrink_fn fn, void *arg)
{
    u32 i;
    if (!fn) return PMM_EINVAL;
    for (i = 0; i < shrinker_count; i++) {
        if (!shrinkers[i].active) {
            shrinkers[i].name = name; shrinkers[i].fn = fn; shrinkers[i].arg = arg;
            shrinkers[i].calls = 0; shrinkers[i].freed = 0; shrinkers[i].active = 1;
            return (int)i;
        }
    }
    if (shrinker_count >= SHRINKER_MAX) return PMM_ENOMEM;
    i = shrinker_count++;
    shrinkers[i].name = name; shrinkers[i].fn = fn; shrinkers[i].arg = arg;
    shrinkers[i].calls = 0; shrinkers[i].freed = 0; shrinkers[i].active = 1;
    return (int)i;
}

int pmm_unregister_shrinker(int id)
{
    if (id < 0 || (u32)id >= shrinker_count) return PMM_EINVAL;
    shrinkers[id].active = 0;
    return PMM_OK;
}

u32 pmm_shrinker_count(void)
{
    u32 i, c = 0;
    for (i = 0; i < shrinker_count; i++) if (shrinkers[i].active) c++;
    return c;
}

const char *pmm_shrinker_name(u32 id) { return (id < shrinker_count) ? shrinkers[id].name : "?"; }
u32 pmm_shrinker_freed(u32 id) { return (id < shrinker_count) ? shrinkers[id].freed : 0; }
u32 pmm_shrinker_calls(u32 id) { return (id < shrinker_count) ? shrinkers[id].calls : 0; }
u32 pmm_reclaim_calls(void) { return reclaim_calls; }
u32 pmm_reclaim_freed(void) { return reclaim_freed_total; }

/* 依次调用各 shrinker，直到回收量达标或本轮无人可回收 */
u32 pmm_reclaim(u32 target)
{
    u32 i, got = 0;
    if (target == 0) return 0;
    reclaim_calls++;
    for (i = 0; i < shrinker_count && got < target; i++) {
        u32 n;
        if (!shrinkers[i].active) continue;
        shrinkers[i].calls++;
        n = shrinkers[i].fn(target - got, shrinkers[i].arg);
        if (n > 0) {
            shrinkers[i].freed += n;
            got += n;
        }
    }
    reclaim_freed_total += got;
    return got;
}

/* ==========================================================================
 * S14 OOM 处理
 * ========================================================================== */
#define OOM_MAX 4
typedef void (*oom_fn)(u32 free_pages, u32 target, void *arg);

static oom_fn oom_handlers[OOM_MAX];
static void  *oom_args[OOM_MAX];
static u32    oom_handler_count = 0;
static u32    oom_count = 0;
static u32    oom_last_free = 0;
static u32    oom_last_target = 0;

int pmm_register_oom_handler(oom_fn fn, void *arg)
{
    if (!fn) return PMM_EINVAL;
    if (oom_handler_count >= OOM_MAX) return PMM_ENOMEM;
    oom_handlers[oom_handler_count] = fn;
    oom_args[oom_handler_count] = arg;
    oom_handler_count++;
    return (int)(oom_handler_count - 1);
}

u32 pmm_oom_count(void) { return oom_count; }
u32 pmm_oom_last_free(void) { return oom_last_free; }
u32 pmm_oom_last_target(void) { return oom_last_target; }
u32 pmm_oom_handler_count(void) { return oom_handler_count; }

/* 分配失败时进入：先按压力回收，仍不足则通报 OOM 处理器 */
static u32 oom_handle(u32 target)
{
    u32 got = 0;
    if (target == 0) target = 1;
    if (pmm_pressure_level() >= PMM_PRESS_MIN) {
        got = pmm_reclaim(target);
    }
    if (got >= target) return got;
    oom_count++;
    oom_last_free = pmm_free_page_count();
    oom_last_target = target;
    {
        u32 i;
        for (i = 0; i < oom_handler_count; i++) {
            oom_handlers[i](oom_last_free, target, oom_args[i]);
        }
    }
    return got;
}

/* ==========================================================================
 * S17 IO 内存映射 ioremap
 * 内核尚未开启分页，物理地址即内核可见地址，因此映射返回同一地址；
 * 真正的价值在于登记与冲突检查：不允许把「已受管且空闲/已分配」的普通
 * 内存当设备寄存器映射，避免驱动写坏可用内存。
 * ========================================================================== */
#define IOREMAP_MAX        16
#define IOREMAP_CACHEABLE  0x01u
#define IOREMAP_ALLOW_RAM  0x02u

typedef struct {
    u32 phys;
    u32 size;
    u32 flags;
    u8  used;
} ioremap_entry_t;

static ioremap_entry_t iomaps[IOREMAP_MAX];
static u32 ioremap_count = 0;

u32 pmm_ioremap_count(void) { return ioremap_count; }
u32 pmm_ioremap_phys(u32 idx) { return (idx < IOREMAP_MAX) ? iomaps[idx].phys : 0; }
u32 pmm_ioremap_size(u32 idx) { return (idx < IOREMAP_MAX) ? iomaps[idx].size : 0; }

void *pmm_ioremap(u32 phys, u32 size, u32 flags)
{
    u32 i, p, first, last;
    if (size == 0) return NULL;
    first = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    last  = PHYS_TO_PAGE(PAGE_ALIGN_UP(phys + size));

    for (i = 0; i < IOREMAP_MAX; i++) {
        u32 a1, a2;
        if (!iomaps[i].used) continue;
        a1 = iomaps[i].phys;
        a2 = iomaps[i].phys + iomaps[i].size;
        if (phys < a2 && phys + size > a1) return NULL;   /* 与已有映射重叠 */
    }

    if (!(flags & IOREMAP_ALLOW_RAM)) {
        for (p = first; p < last && p < total_pages; p++) {
            int st = pmm_state(PAGE_TO_PHYS(p));
            if (st == PMM_STATE_FREE || st == PMM_STATE_USED) return NULL;
        }
    }

    for (i = 0; i < IOREMAP_MAX; i++) {
        if (!iomaps[i].used) {
            iomaps[i].used  = 1;
            iomaps[i].phys  = phys;
            iomaps[i].size  = size;
            iomaps[i].flags = flags;
            ioremap_count++;
            return (void *)phys;
        }
    }
    return NULL;
}

int pmm_iounmap(void *addr)
{
    u32 i;
    if (!addr) return PMM_EINVAL;
    for (i = 0; i < IOREMAP_MAX; i++) {
        if (iomaps[i].used && iomaps[i].phys == (u32)addr) {
            iomaps[i].used = 0;
            if (ioremap_count) ioremap_count--;
            return PMM_OK;
        }
    }
    return PMM_ENOTALLOC;
}

/* ==========================================================================
 * S18 内存加密与保护（释放清零）
 * 不具备硬件内存加密能力，此处落地的是软件侧最有效的一条：页被释放时
 * 清零，杜绝「上一任持有者的数据泄漏给下一任」。与毒化检测互斥——
 * 毒化用特征值帮助发现 use-after-free，清零则让内容不可读，由策略开关选择。
 * ========================================================================== */
#define PMM_SCRUB_OFF 0u
#define PMM_SCRUB_ON  1u

static u32 scrub_policy = PMM_SCRUB_OFF;
static u32 scrub_count = 0;

void pmm_set_scrub(u32 on) { scrub_policy = on ? PMM_SCRUB_ON : PMM_SCRUB_OFF; }
u32  pmm_get_scrub(void) { return scrub_policy; }
u32  pmm_scrub_count(void) { return scrub_count; }

static void zero_page(u32 page)
{
    u32 *q = (u32 *)PAGE_TO_PHYS(page);
    u32 i;
    for (i = 0; i < PAGE_SIZE / 4u; i++) q[i] = 0;
}

u32 pmm_scrub_verify(u32 phys)
{
    u32 p, i;
    const u32 *q;
    if (!PAGE_IS_ALIGNED(phys)) return 1;
    p = PHYS_TO_PAGE(phys);
    if (p >= total_pages) return 2;
    if (pmm_state(phys) != PMM_STATE_FREE) return 3;
    q = (const u32 *)phys;
    for (i = 0; i < PAGE_SIZE / 4u; i++) if (q[i] != 0) return 4;
    return 0;
}

/* 释放一页时的内容处置策略。
 *
 * 两种策略都会覆盖整页，因此无论策略如何取值，上一任持有者的数据
 * 都不会残留到下一任持有者手里 —— 这是释放路径必须无条件成立的
 * 安全属性，不能挂在 debug 开关上：一旦 debug_level 被关小，空闲页
 * 就会带着原持有者的明文内容被重新分配出去。
 *
 *   scrub = ON  : 清零。内容不可读，适合交付形态。
 *   scrub = OFF : 写毒化特征值。内容同样被破坏，且能帮出
 *                 use-after-free 类缺陷，适合调试期。
 */
static void free_page_fill(u32 page)
{
    if (scrub_policy == PMM_SCRUB_ON) {
        zero_page(page);
        scrub_count++;
    } else {
        poison_page(page, PMM_POISON_FREE);
    }
}

/* ==========================================================================
 * S19 内存错误检测与隔离（坏页下线）
 * ========================================================================== */
#define BADPAGE_MAX 64

static u32 badpage_list[BADPAGE_MAX];
static u32 badpage_count = 0;

u32 pmm_badpage_count(void) { return badpage_count; }
u32 pmm_badpage_at(u32 idx) { return (idx < badpage_count) ? badpage_list[idx] : 0; }

int pmm_is_offline(u32 phys)
{
    u32 p, i;
    if (!PAGE_IS_ALIGNED(phys)) return 0;
    p = PHYS_TO_PAGE(phys);
    for (i = 0; i < badpage_count; i++) if (badpage_list[i] == p) return 1;
    return 0;
}

/* 把一页从分配器摘除并登记为坏页；要求该页当前未被分配 */
int pmm_offline_page(u32 phys)
{
    u32 p;
    if (!PAGE_IS_ALIGNED(phys)) return PMM_EINVAL;
    p = PHYS_TO_PAGE(phys);
    if (p >= total_pages) return PMM_EINVAL;
    if (pmm_is_offline(phys)) return PMM_EBUSY;
    if (badpage_count >= BADPAGE_MAX) return PMM_ENOMEM;
    if (pmm_state(phys) == PMM_STATE_USED) return PMM_EBUSY;   /* 在用，不能摘 */
    mark_reserved_page(p);
    badpage_list[badpage_count++] = p;
    buddy_rebuild();
    return PMM_OK;
}

/* 撤销下线（例如错误被判定为可恢复） */
int pmm_online_page(u32 phys)
{
    u32 p, i, j;
    if (!PAGE_IS_ALIGNED(phys)) return PMM_EINVAL;
    p = PHYS_TO_PAGE(phys);
    for (i = 0; i < badpage_count; i++) {
        if (badpage_list[i] == p) {
            for (j = i; j + 1u < badpage_count; j++) badpage_list[j] = badpage_list[j + 1u];
            badpage_count--;
            mark_available_page(p);
            buddy_rebuild();
            return PMM_OK;
        }
    }
    return PMM_ENOTALLOC;
}

/* ==========================================================================
 * S20 分配策略与优先级
 * ========================================================================== */
#define PMM_ALLOC_ZERO    0x01u   /* 返回前清零 */
#define PMM_ALLOC_DMA     0x02u   /* 必须落在 16MB 以下 */
#define PMM_ALLOC_HIGH    0x04u   /* 优先高地址 */
#define PMM_ALLOC_CONTIG  0x08u   /* 必须物理连续 */
#define PMM_ALLOC_ATOMIC  0x10u   /* 不触发回收与 OOM 通报 */

static u32 _pmm_alloc_flags_locked(u32 n, u32 flags)
{
    u32 addr = 0;
    u32 i;

    if (n == 0) { alloc_fail++; return 0; }

    if (flags & PMM_ALLOC_DMA) {
        u32 limit = zone_end_page[ZONE_DMA];
        if (limit > total_pages) limit = total_pages;
        if (n == 1) {
            addr = _pmm_alloc_zone_locked(ZONE_DMA);
        } else {
            u32 p, run = 0, start = 0;
            for (p = 0; p < limit; p++) {
                if (page_is_free_raw(p)) {
                    if (run == 0) start = p;
                    run++;
                    if (run == n) break;
                } else {
                    run = 0;
                }
            }
            if (run == n) {
                for (i = 0; i < n; i++) {
                    buddy_remove_page(start + i);
                    mark_used_page(start + i);
                    refcnt[start + i] = 1;
                }
                addr = PAGE_TO_PHYS(start);
                alloc_calls++;
            }
        }
    } else if (flags & PMM_ALLOC_HIGH) {
        u32 p = total_pages;
        while (p > 0) {
            p--;
            if (page_is_free_raw(p)) {
                buddy_remove_page(p);
                mark_used_page(p);
                refcnt[p] = 1;
                alloc_calls++;
                addr = PAGE_TO_PHYS(p);
                break;
            }
        }
    } else if (flags & PMM_ALLOC_CONTIG) {
        addr = _pmm_alloc_pages_locked(n);
    } else {
        addr = (n == 1) ? _pmm_alloc_page_locked() : _pmm_alloc_pages_locked(n);
    }
    return addr;
}

/* 锁包装：带标志分配（DMA/HIGH/CONTIG/ZERO/ATOMIC）。
 * 一次尝试在临界区内完成；OOM 回收在临界区外执行后重试一次。 */
u32 pmm_alloc_flags(u32 n, u32 flags)
{
    u32 addr, eflags;

    eflags = pmm_lock_enter();
    addr = _pmm_alloc_flags_locked(n, flags);
    pmm_lock_exit(eflags);

    if (!addr && !(flags & PMM_ALLOC_ATOMIC)) {
        (void)oom_handle(n);
        eflags = pmm_lock_enter();
        addr = _pmm_alloc_flags_locked(n, flags);
        pmm_lock_exit(eflags);
    }

    if (addr && (flags & PMM_ALLOC_ZERO)) {
        memset((void *)addr, 0, n * PAGE_SIZE);
    }
    return addr;
}

/* ==========================================================================
 * 扩展子系统自检
 * 编号约定：100+ 为伙伴子系统，200+ 起为各子系统用例号。
 * ========================================================================== */
#define TEST_SHRINK_POOL 8
static u32 test_pool[TEST_SHRINK_POOL];
static u32 test_pool_used = 0;

/* 测试用可回收缓存：持有若干页，被调用时最多交出 3 页 */
static u32 test_shrinker(u32 nr, void *arg)
{
    u32 got = 0;
    UNUSED(arg);
    while (got < nr && got < 3u && test_pool_used > 0) {
        test_pool_used--;
        if (pmm_free_page(test_pool[test_pool_used]) == PMM_OK) got++;
    }
    return got;
}

u32 pmm_ext_selftest(void)
{
    u32 rc;
    u32 saved_dbg, saved_scrub;

    /* ---- 用例 1：伙伴层级自检 ---- */
    rc = pmm_buddy_selftest();
    if (rc != 0) return 100u + rc;

    /* ---- 用例 2：zone 划分与统计自洽 ---- */
    if (pmm_zone_count() != ZONE_COUNT) return 200;
    {
        u32 z, f = 0, u = 0, r = 0;
        for (z = 0; z < ZONE_COUNT; z++) {
            f += pmm_zone_free(z);
            u += pmm_zone_used(z);
            r += pmm_zone_reserved(z);
        }
        if (f != pmm_free_page_count()) return 201;
        if (u != pmm_used_pages()) return 202;
        if (r != pmm_reserved_pages()) return 203;
        if (f + u + r != pmm_total_pages()) return 204;
    }

    /* ---- 用例 3：DMA 区分配必须落在 16MB 以下 ---- */
    {
        u32 a = pmm_alloc_zone(ZONE_DMA);
        if (a == 0) return 210;
        if (a >= pmm_zone_limit()) return 211;
        if (pmm_free_page(a) != PMM_OK) return 212;
    }

    /* ---- 用例 4：NUMA 单节点映射与距离矩阵 ---- */
    {
        u32 a;
        if (pmm_numa_node_count() == 0) return 220;
        if (pmm_numa_distance(0, 0) != NUMA_LOCAL) return 221;
        a = pmm_alloc_node(0);
        if (a == 0) return 222;
        if (pmm_numa_node_of(a) != 0) return 223;
        if (pmm_free_page(a) != PMM_OK) return 224;
    }

    /* ---- 用例 5：水位单调、压力分级可用 ---- */
    {
        u32 lv;
        if (!(pmm_watermark(PMM_WM_MIN) < pmm_watermark(PMM_WM_LOW))) return 230;
        if (!(pmm_watermark(PMM_WM_LOW) < pmm_watermark(PMM_WM_HIGH))) return 231;
        lv = pmm_pressure_level();
        if (lv > PMM_PRESS_CRITICAL) return 232;
        if (!pmm_watermark_ok(pmm_free_page_count(), PMM_WM_MIN)) return 233;
    }

    /* ---- 用例 6：释放清零策略真实生效 ---- */
    {
        u32 a;
        saved_scrub = pmm_get_scrub();
        saved_dbg   = pmm_get_debug();

        a = pmm_alloc_page();
        if (a == 0) return 240;
        memset((void *)a, 0xA5, PAGE_SIZE);              /* 写入可识别的敏感内容 */
        if (pmm_free_page(a) != PMM_OK) return 241;
        if (pmm_scrub_verify(a) == 0) return 242;        /* 未开启清零时不应读出全零 */
        if (pmm_scrub_count() != 0) return 247;          /* 未开启清零时不应累加计数 */

        pmm_set_scrub(PMM_SCRUB_ON);
        a = pmm_alloc_page();
        if (a == 0) { pmm_set_scrub(saved_scrub); return 243; }
        memset((void *)a, 0x5A, PAGE_SIZE);
        if (pmm_free_page(a) != PMM_OK) { pmm_set_scrub(saved_scrub); return 244; }
        if (pmm_scrub_verify(a) != 0) { pmm_set_scrub(saved_scrub); return 245; }
        if (pmm_scrub_count() == 0) { pmm_set_scrub(saved_scrub); return 246; }

        pmm_set_scrub(saved_scrub);
        pmm_set_debug(saved_dbg);
    }

    /* ---- 用例 7：坏页下线后不可被分配，撤销后可恢复 ---- */
    {
        u32 a, b;
        a = pmm_alloc_page();
        if (a == 0) return 250;
        if (pmm_offline_page(a) != PMM_EBUSY) return 251;   /* 在用页不可下线 */
        if (pmm_free_page(a) != PMM_OK) return 252;
        if (pmm_offline_page(a) != PMM_OK) return 253;
        if (!pmm_is_offline(a)) return 254;
        if (pmm_offline_page(a) != PMM_EBUSY) return 255;   /* 重复下线 */
        b = pmm_alloc_page();
        if (b == a) return 256;                             /* 不应再被分配出来 */
        if (b) { if (pmm_free_page(b) != PMM_OK) return 257; }
        if (pmm_online_page(a) != PMM_OK) return 258;
        if (pmm_is_offline(a)) return 259;
        if (pmm_badpage_count() != 0) return 260;
        if (pmm_online_page(a) != PMM_ENOTALLOC) return 261; /* 未下线的页不可上线 */
    }

    /* ---- 用例 8：ioremap 冲突检查 ---- */
    {
        void *m1, *m2;
        u32 free_addr = 0, p;
        for (p = 0; p < total_pages; p++) {
            if (page_is_free_raw(p)) { free_addr = PAGE_TO_PHYS(p); break; }
        }
        if (free_addr == 0) return 270;
        /* 把普通空闲内存当设备寄存器映射，必须被拒绝 */
        if (pmm_ioremap(free_addr, PAGE_SIZE, 0) != NULL) return 271;
        m1 = pmm_ioremap(free_addr, PAGE_SIZE, IOREMAP_ALLOW_RAM);
        if (m1 == NULL) return 272;
        m2 = pmm_ioremap(free_addr, PAGE_SIZE, IOREMAP_ALLOW_RAM);
        if (m2 != NULL) return 273;                          /* 重叠必须被拒 */
        if (pmm_ioremap_count() != 1) return 274;
        if (pmm_iounmap(m1) != PMM_OK) return 275;
        if (pmm_iounmap(m1) != PMM_ENOTALLOC) return 276;    /* 重复解除 */
        if (pmm_ioremap_count() != 0) return 277;
    }

    /* ---- 用例 9：热插拔状态机 ---- */
    {
        int id;
        u32 p, first = 0, last = 0;
        for (p = 0; p + 16u <= total_pages; p++) {
            u32 q, ok = 1;
            for (q = p; q < p + 16u; q++) if (!page_is_free_raw(q)) { ok = 0; break; }
            if (ok) { first = p; last = p + 16u; break; }
        }
        if (last == 0) return 280;
        id = pmm_hotplug_add(PAGE_TO_PHYS(first), PAGE_TO_PHYS(last));
        if (id < 0) return 281;
        if (pmm_hotplug_state((u32)id) != HOTPLUG_ONLINE) return 282;
        if (pmm_hotplug_remove((u32)id) != PMM_OK) return 283;
        if (pmm_hotplug_state((u32)id) != HOTPLUG_OFFLINE) return 284;
        if (pmm_hotplug_remove((u32)id) != PMM_EBUSY) return 285;   /* 重复摘除 */
        if (pmm_hotplug_add(PAGE_TO_PHYS(first), PAGE_TO_PHYS(last)) < 0) return 286;
        if (pmm_hotplug_add(PAGE_TO_PHYS(first), PAGE_TO_PHYS(last)) < 0) return 287;
        if (pmm_hotplug_count() != 2) return 288;
    }

    /* ---- 用例 10：大页分配、对齐与释放 ---- */
    {
        u32 h, i;
        u32 before = pmm_free_page_count();
        h = pmm_alloc_huge();
        if (h == 0) return 290;
        if (h & (pmm_huge_size() - 1u)) return 291;
        for (i = 0; i < (1u << HUGE_PAGE_ORDER); i++) {
            if (pmm_state(h + i * PAGE_SIZE) != PMM_STATE_USED) return 292;
        }
        if (pmm_free_page_count() != before - (1u << HUGE_PAGE_ORDER)) return 293;
        if (pmm_free_huge(h) != PMM_OK) return 294;
        if (pmm_free_page_count() != before) return 295;
        if (pmm_free_huge(h) != PMM_ENOTALLOC) return 296;
        if (pmm_free_huge(h + PAGE_SIZE) != PMM_EINVAL) return 297;
    }

    /* ---- 用例 11：回收框架按注册顺序调用并统计 ---- */
    {
        u32 got, i, base_shrinkers;
        int s1, s2;

        for (i = 0; i < TEST_SHRINK_POOL; i++) {
            u32 a = pmm_alloc_page();
            if (!a) break;
            test_pool[test_pool_used++] = a;
        }
        if (test_pool_used < 6u) return 300;

        /* 回收框架是全系统共享的：虚拟内存子系统已在 vmm_init 中注册
         * "vmm-pages"。因此本用例不能假设「全局 shrinker 数就是 2」，
         * 也不能假设「回收请求只会落到自己的 shrinker 上」。
         * 做法：先按注册顺序把既有 shrinker 能交出的页消耗干净
         * （回收框架按注册顺序调用，交出足够页后后面的不会被调用），
         * 再注册本用例的两个 shrinker —— 此后 6 页的请求必然落到它们
         * 身上，从而得到确定的「各交出 3 页」分布。 */
        base_shrinkers = pmm_shrinker_count();
        (void)pmm_reclaim(64);

        s1 = pmm_register_shrinker("test-a", test_shrinker, (void *)0);
        s2 = pmm_register_shrinker("test-b", test_shrinker, (void *)0);
        if (s1 < 0 || s2 < 0) return 301;
        if (pmm_shrinker_count() != base_shrinkers + 2u) return 302;

        got = pmm_reclaim(6);
        if (got != 6) return 303;                    /* 两个 shrinker 各交出 3 页 */
        if (pmm_shrinker_freed((u32)s1) != 3) return 304;
        if (pmm_shrinker_freed((u32)s2) != 3) return 305;
        if (pmm_reclaim_calls() == 0) return 306;
        if (pmm_shrinker_calls((u32)s1) != 1) return 307;

        if (pmm_unregister_shrinker(s1) != PMM_OK) return 308;
        if (pmm_unregister_shrinker(s2) != PMM_OK) return 309;
        if (pmm_shrinker_count() != base_shrinkers) return 310;

        /* 收尾：把测试缓存剩下的页还回去 */
        while (test_pool_used > 0) {
            test_pool_used--;
            (void)pmm_free_page(test_pool[test_pool_used]);
        }
    }

    /* ---- 用例 12：反碎片预留与整理 ---- */
    {
        u32 reserved_before = pmm_antifrag_pages();
        if (reserved_before == 0) return 320;        /* 内存充足时应当已预留 */
        if (pmm_compact() != reserved_before) return 321;
        if (pmm_antifrag_pages() != 0) return 322;
        if (pmm_compact() != 0) return 323;          /* 重复整理无副作用 */
        {
            u32 a = pmm_alloc_pages(reserved_before);
            if (a == 0) return 324;
            if (pmm_free_pages(a, reserved_before) != PMM_OK) return 325;
        }
    }

    /* ---- 用例 13：碎片指数取值范围 ---- */
    if (pmm_frag_index() > 1000u) return 330;

    /* ---- 用例 14：分配标志语义 ---- */
    {
        u32 a, i;
        a = pmm_alloc_flags(1, PMM_ALLOC_ZERO);
        if (a == 0) return 340;
        {
            const u32 *q = (const u32 *)a;
            for (i = 0; i < PAGE_SIZE / 4u; i++) if (q[i] != 0) return 341;
        }
        if (pmm_free_page(a) != PMM_OK) return 342;

        a = pmm_alloc_flags(1, PMM_ALLOC_DMA);
        if (a == 0) return 343;
        if (a >= pmm_zone_limit()) return 344;
        if (pmm_free_page(a) != PMM_OK) return 345;

        a = pmm_alloc_flags(1, PMM_ALLOC_HIGH);
        if (a == 0) return 346;
        if (pmm_free_page(a) != PMM_OK) return 347;

        a = pmm_alloc_flags(8, PMM_ALLOC_CONTIG);
        if (a == 0) return 348;
        if (pmm_free_pages(a, 8) != PMM_OK) return 349;

        a = pmm_alloc_flags(4, PMM_ALLOC_DMA | PMM_ALLOC_CONTIG);
        if (a == 0) return 350;
        if (a + 4u * PAGE_SIZE > pmm_zone_limit()) return 351;
        if (pmm_free_pages(a, 4) != PMM_OK) return 352;
    }

    /* ---- 用例 15：无 shrinker 时回收路径不得崩且不虚报 ---- */
    {
        u32 got = pmm_reclaim(4);
        if (got != 0) return 360;
    }

    /* ---- 用例 16：全部操作结束后伙伴层级仍然自洽 ---- */
    if (buddy_check() != 0) return 370;
    if (buddy_off_check() != 0) return 371;

    return 0;
}

/* --------------------------------------------------------------------------
 * 分配单页
 * ------------------------------------------------------------------------ */
static u32 _pmm_alloc_page_locked(void)
{
    u32 page;

    alloc_calls++;

    if (total_pages == 0) { alloc_fail++; return 0; }

    /* 优先走伙伴索引；索引给不出时回退到位图线性查找 */
    page = buddy_take(0);
    if (page == PMM_MAX_PAGES || page >= total_pages) {
        page = pmm_find_first_free();
        if (page >= total_pages) {
            alloc_fail++;
            return 0;                      /* OOM 由 wrapper 在锁外处理 */
        }
        buddy_remove_page(page);
    }

    mark_used_page(page);
    refcnt[page] = 1;

    if (debug_level >= PMM_DBG_VERIFY) {
        /* 校验上一轮释放时写入的毒化值是否完好，用于发现越界写 */
        (void)pmm_poison_check(PAGE_TO_PHYS(page));
    }

    return PAGE_TO_PHYS(page);
}

/* 锁包装：单页分配与其它分配/释放路径串行化。
 * OOM 回收（oom_handle → reclaim → shrinker）在临界区外执行，
 * 避免持锁调用可重入公开入口的 shrinker 回调造成自死锁。 */
u32 pmm_alloc_page(void)
{
    u32 r, eflags;

    eflags = pmm_lock_enter();
    r = _pmm_alloc_page_locked();
    pmm_lock_exit(eflags);

    if (r == 0) {
        (void)oom_handle(1);               /* 锁外回收 */
        eflags = pmm_lock_enter();         /* 回收后重试一次 */
        r = _pmm_alloc_page_locked();
        pmm_lock_exit(eflags);
    }
    return r;
}

static u32 _pmm_alloc_pages_locked(u32 n)
{
    u32 page, i;

    alloc_calls++;

    if (n == 0) { alloc_fail++; return 0; }
    if (n > total_pages) { alloc_fail++; return 0; }

    if (n == 1) return _pmm_alloc_page_locked();

    /* 优先向伙伴系统要一个 2^order 的块，再把多取的尾部还回去，
     * 避免连续分配造成内部碎片 */
    {
        u32 order = buddy_order_for(n);
        page = buddy_take(order);
        if (page != PMM_MAX_PAGES) {
            if (page + n <= total_pages) {
                u32 have = 1u << order;
                for (i = 0; i < n; i++) {
                    mark_used_page(page + i);
                    refcnt[page + i] = 1;
                }
                for (i = n; i < have; i++) {
                    buddy_give(page + i, 0);
                }
                return PAGE_TO_PHYS(page);
            }
            buddy_give(page, order);      /* 块超出受管范围，原样退回 */
        }
    }

    /* 伙伴给不出该阶（空闲页跨块边界）→ 回退到位图连续查找 */
    page = pmm_find_contiguous(n);
    if (page >= PMM_MAX_PAGES || page + n > total_pages) {
        alloc_fail++;
        return 0;                         /* OOM 由 wrapper 在锁外处理 */
    }

    for (i = 0; i < n; i++) {
        buddy_remove_page(page + i);
        mark_used_page(page + i);
        refcnt[page + i] = 1;
    }
    return PAGE_TO_PHYS(page);
}

/* 锁包装：连续多页分配（OOM 回收在锁外） */
u32 pmm_alloc_pages(u32 n)
{
    u32 r, eflags;

    eflags = pmm_lock_enter();
    r = _pmm_alloc_pages_locked(n);
    pmm_lock_exit(eflags);

    if (r == 0) {
        (void)oom_handle(n);
        eflags = pmm_lock_enter();
        r = _pmm_alloc_pages_locked(n);
        pmm_lock_exit(eflags);
    }
    return r;
}

u32 pmm_alloc_page_zeroed(void)
{
    u32 r, eflags;
    eflags = pmm_lock_enter();
    r = _pmm_alloc_page_locked();
    pmm_lock_exit(eflags);
    if (r == 0) {
        (void)oom_handle(1);
        eflags = pmm_lock_enter();
        r = _pmm_alloc_page_locked();
        pmm_lock_exit(eflags);
    }
    if (r) memset((void *)r, 0, PAGE_SIZE);
    return r;
}

/* --------------------------------------------------------------------------
 * 释放单页
 * ------------------------------------------------------------------------ */
static int _pmm_free_page_locked(u32 phys)
{
    u32 page;
    int st;

    free_calls++;

    if (!PAGE_IS_ALIGNED(phys)) return PMM_EINVAL;
    page = PHYS_TO_PAGE(phys);
    if (page >= total_pages) return PMM_EINVAL;

    st = pmm_state(phys);
    if (st == PMM_STATE_RESERVED) return PMM_ERESERVED;  /* 保留区禁止释放 */
    if (st == PMM_STATE_FREE)     return PMM_EBUSY;      /* 重复释放 */
    if (st == PMM_STATE_OOR)      return PMM_EINVAL;

    /* 引用计数未归零时不允许释放 */
    if (refcnt[page] > 1) return PMM_EREFCNT;

    free_page_fill(page);

    mark_free_page(page);
    buddy_give(page, 0);

    if (alloc_hint > page) alloc_hint = page;
    return PMM_OK;
}

/* 锁包装：单页释放与分配/释放路径串行化 */
int pmm_free_page(u32 phys)
{
    int r;
    u32 eflags = pmm_lock_enter();
    r = _pmm_free_page_locked(phys);
    pmm_lock_exit(eflags);
    return r;
}

/* --------------------------------------------------------------------------
 * 批量释放：先全量校验再统一提交，避免中途失败导致状态不一致
 * ------------------------------------------------------------------------ */
static int _pmm_free_pages_locked(u32 phys, u32 n)
{
    u32 i;

    if (n == 0) return PMM_EINVAL;
    if (!PAGE_IS_ALIGNED(phys)) return PMM_EINVAL;
    if (n > total_pages) return PMM_EINVAL;
    if (PHYS_TO_PAGE(phys) + n > total_pages) return PMM_EINVAL;

    /* 阶段一：校验每一页都可释放 */
    for (i = 0; i < n; i++) {
        u32 addr = phys + i * PAGE_SIZE;
        int st = pmm_state(addr);
        if (st == PMM_STATE_RESERVED) return PMM_ERESERVED;
        if (st == PMM_STATE_FREE)     return PMM_EBUSY;
        if (st == PMM_STATE_OOR)      return PMM_EINVAL;
        if (refcnt[PHYS_TO_PAGE(addr)] > 1) return PMM_EREFCNT;
    }

    /* 阶段二：全部可释放，统一提交 */
    for (i = 0; i < n; i++) {
        u32 addr = phys + i * PAGE_SIZE;
        u32 page = PHYS_TO_PAGE(addr);
        free_page_fill(page);
        mark_free_page(page);
        buddy_give(page, 0);
    }

    if (alloc_hint > PHYS_TO_PAGE(phys)) alloc_hint = PHYS_TO_PAGE(phys);
    free_calls++;
    return PMM_OK;
}

/* 锁包装：批量释放为单次原子操作（校验+提交全程持锁） */
int pmm_free_pages(u32 phys, u32 n)
{
    int r;
    u32 eflags = pmm_lock_enter();
    r = _pmm_free_pages_locked(phys, n);
    pmm_lock_exit(eflags);
    return r;
}

/* --------------------------------------------------------------------------
 * 引用计数
 * ------------------------------------------------------------------------ */
u8 pmm_refcount(u32 phys)
{
    u32 p = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    if (p >= total_pages) return 0;
    return refcnt[p];
}

static int _pmm_ref_inc_locked(u32 phys)
{
    u32 p = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    if (p >= total_pages) return PMM_EINVAL;
    if (pmm_state(phys) != PMM_STATE_USED) return PMM_ENOTALLOC;
    if (refcnt[p] == 0xFFu) return PMM_EREFCNT;
    refcnt[p]++;
    return PMM_OK;
}

/* 锁包装：引用计数递增与释放路径串行化（refcnt 与释放检查原子） */
int pmm_ref_inc(u32 phys)
{
    int r;
    u32 eflags = pmm_lock_enter();
    r = _pmm_ref_inc_locked(phys);
    pmm_lock_exit(eflags);
    return r;
}

static int _pmm_ref_dec_locked(u32 phys)
{
    u32 p = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    if (p >= total_pages) return PMM_EINVAL;
    if (pmm_state(phys) != PMM_STATE_USED) return PMM_ENOTALLOC;
    if (refcnt[p] == 0) return PMM_EREFCNT;
    refcnt[p]--;
    return PMM_OK;
}

/* 锁包装：引用计数递减 */
int pmm_ref_dec(u32 phys)
{
    int r;
    u32 eflags = pmm_lock_enter();
    r = _pmm_ref_dec_locked(phys);
    pmm_lock_exit(eflags);
    return r;
}

/* --------------------------------------------------------------------------
 * 位图转储：按 1MB 粒度显示占用概览
 * ------------------------------------------------------------------------ */
void pmm_dump(void)
{
    u32 mb, mb_count;

    mb_count = managed_limit / (1024u * 1024u);
    if (mb_count == 0) mb_count = 1;
    if (mb_count > 128u) mb_count = 128u;

    con_puts("  PMM map (1 char = 1 MB)   '.'=free  '#'=used  'R'=reserved\n");
    con_puts("  [");
    for (mb = 0; mb < mb_count; mb++) {
        u32 base = mb * 1024u * 1024u;
        u32 st = pmm_state(base);
        char c = '.';
        if (st == PMM_STATE_RESERVED) c = 'R';
        else if (st == PMM_STATE_USED) c = '#';
        con_putc(c);
    }
    con_puts("]\n");
}

/* --------------------------------------------------------------------------
 * 自检：覆盖分配、连续分配、释放、重复释放、保留区、越界、
 *       内存耗尽、引用计数、毒化、统计自洽等路径
 * 说明：伙伴系统 / zone / NUMA / 热插拔 / 大页 / 回收 / OOM /
 *       ioremap / 清零 / 坏页下线 / 分配标志由 pmm_ext_selftest() 覆盖
 * 返回 0 = 全部通过；否则返回失败用例号
 * ------------------------------------------------------------------------ */
u32 pmm_selftest(void)
{
    u32 a, b, c, free_before;
    pmm_stats_t st;

    if (total_pages == 0) return 1;

    /* ---- 用例 1：单页分配返回对齐地址且非零 ---- */
    a = pmm_alloc_page();
    if (a == 0 || !PAGE_IS_ALIGNED(a)) return 2;
    if (pmm_state(a) != PMM_STATE_USED) return 3;
    if (pmm_refcount(a) != 1) return 4;

    /* ---- 用例 2：分配后空闲页数恰好减 1 ---- */
    free_before = pmm_free_page_count();
    b = pmm_alloc_page();
    if (b == 0) return 5;
    if (pmm_free_page_count() != free_before - 1) return 6;

    /* ---- 用例 3：释放后计数恢复 ---- */
    if (pmm_free_page(b) != PMM_OK) return 7;
    if (pmm_free_page_count() != free_before) return 8;

    /* ---- 用例 4：重复释放应被拒绝 ---- */
    if (pmm_free_page(b) != PMM_EBUSY) return 9;

    /* ---- 用例 5：连续 4 页分配，地址连续且对齐 ---- */
    c = pmm_alloc_pages(4);
    if (c == 0 || !PAGE_IS_ALIGNED(c)) return 10;
    if (pmm_state(c) != PMM_STATE_USED) return 11;
    if (pmm_state(c + 3u * PAGE_SIZE) != PMM_STATE_USED) return 12;
    if (pmm_state(c + 4u * PAGE_SIZE) == PMM_STATE_USED) return 13;
    if (pmm_free_pages(c, 4) != PMM_OK) return 14;
    if (pmm_state(c) != PMM_STATE_FREE) return 15;

    /* ---- 用例 6：非对齐地址释放被拒绝 ---- */
    if (pmm_free_page(0x1234) != PMM_EINVAL) return 16;

    /* ---- 用例 7：低端保留区不可释放 ---- */
    if (pmm_free_page(0x00001000) != PMM_ERESERVED) return 17;
    if (pmm_state(0x00001000) != PMM_STATE_RESERVED) return 18;

    /* ---- 用例 8：内核保留区不可释放 ---- */
    if (pmm_state(KERNEL_RESERVE_BASE) != PMM_STATE_RESERVED) return 19;

    /* ---- 用例 9：越界地址 ---- */
    if (pmm_state(0xFFFFFFFFu) != PMM_STATE_OOR) return 20;
    if (pmm_free_page(0xFFFFFF00u) != PMM_EINVAL) return 21;

    /* ---- 用例 10：alloc_pages(0) 与超大请求应失败 ---- */
    if (pmm_alloc_pages(0) != 0) return 22;
    if (pmm_alloc_pages(total_pages + 1u) != 0) return 23;

    /* ---- 用例 11：引用计数 ---- */
    if (pmm_ref_inc(a) != PMM_OK) return 24;
    if (pmm_refcount(a) != 2) return 25;
    if (pmm_free_page(a) != PMM_EREFCNT) return 26;
    if (pmm_ref_dec(a) != PMM_OK) return 27;
    if (pmm_refcount(a) != 1) return 28;

    /* ---- 用例 12：毒化检测（释放后页内容应为毒化值） ---- */
    if (pmm_free_page(a) != PMM_OK) return 29;
    if (pmm_poison_check(a) != 0) return 30;

    /* ---- 用例 13：批量释放的中途失败必须不产生副作用 ---- */
    {
        u32 blk = pmm_alloc_pages(3);
        if (blk == 0) return 31;
        /* 释放范围跨越到保留区 → 应整体失败且 3 页仍为已用 */
        if (pmm_free_pages(blk, total_pages) != PMM_EINVAL) return 32;
        if (pmm_state(blk) != PMM_STATE_USED) return 33;
        if (pmm_state(blk + PAGE_SIZE) != PMM_STATE_USED) return 34;
        if (pmm_state(blk + 2u * PAGE_SIZE) != PMM_STATE_USED) return 35;
        if (pmm_free_pages(blk, 3) != PMM_OK) return 36;
    }

    /* ---- 用例 14：内存耗尽路径与恢复 ---- */
    {
        u32 saved = debug_level;
        u32 n = 0;
        u32 p;

        debug_level = PMM_DBG_OFF;      /* 关闭毒化，避免逐页填充耗时 */

        while (n < total_pages + 8u) {
            u32 x = pmm_alloc_page();
            if (x == 0) break;
            n++;
        }
        if (pmm_free_page_count() != 0) return 37;
        if (pmm_alloc_page() != 0) return 38;          /* 已满，必须失败 */

        /* 全量释放已分配页，验证分配器可恢复 */
        for (p = 0; p < total_pages; p++) {
            u32 addr = PAGE_TO_PHYS(p);
            if (pmm_state(addr) == PMM_STATE_USED) {
                (void)pmm_free_page(addr);
            }
        }
        if (pmm_free_page_count() !=
            (total_pages - reserved_pages)) return 39;

        debug_level = saved;
    }

    /* ---- 用例 15：统计口径自洽 ---- */
    pmm_stats(&st);
    if (st.total_pages != total_pages) return 40;
    if (st.used_pages != used_pages) return 41;
    if (st.reserved_pages != reserved_pages) return 42;
    if (st.used_pages + st.reserved_pages + st.free_pages != total_pages) return 43;
    if (st.reserved_pages > st.total_pages) return 44;
    if (st.largest_free_run == 0) return 45;
    if (st.free_regions == 0) return 46;

    /* ---- 用例 16：连续查找不应跨越保留区 ---- */
    {
        u32 run = pmm_largest_free_run();
        u32 got;
        if (run == 0) return 47;
        got = pmm_find_contiguous(run + 1u);
        if (got != PMM_MAX_PAGES) return 48;
    }

    /* ---- 用例 17：核心不变式 ----
     * 任何被标记为空闲的页，都必须「完整」落在某个 E820 可用区间内。
     * 若 E820 可用区尾部的部分可用页被整页放出，分配器就会把保留内存
     * 交给调用方，这是最难察觉也最危险的一类缺陷，故单列一条检查。 */
    {
        u32 p;
        for (p = 0; p < total_pages; p++) {
            u64 pstart = (u64)PAGE_TO_PHYS(p);
            u64 pend   = pstart + PAGE_SIZE;
            const e820_entry_t *e;

            if (pmm_state(PAGE_TO_PHYS(p)) != PMM_STATE_FREE) continue;

            e = e820_find(pstart);
            if (!e) return 49;
            if (e->type != E820_USABLE) return 50;
            if (pend > e->base + e->length) return 51;
        }
    }

    return 0;
}


/* ==========================================================================
 * 锁与并发保护自检（03 册物理内存管理 · 第二轮重做新增）
 * 两个内核任务并发分配/释放，验证四件事：
 *   1) 帧唯一性：同一物理帧不会被同时交给两个任务（锁真实生效）；
 *   2) 分配次数守恒：2 任务 × ROUNDS 轮 × NPAGES 页全部成功；
 *   3) 空闲页守恒：并发前后 free 计数不变（无泄漏）；
 *   4) 锁真实工作：临界区进入次数 > 0。
 * 依赖：task_init 之后调用（pmm_selftest_lock 由 kmain 在调度器就绪后执行）。
 * ========================================================================== */
#define PMM_CONC_ROUNDS  96u
#define PMM_CONC_NPAGES  4u

static spinlock_t conc_stat_lock;             /* 测试自身的帧登记锁 */
static u32 conc_conflicts = 0;
static u32 conc_alloc_ok  = 0;
static u32 conc_seen[BITMAP_WORDS];           /* 分配期间已见帧位图 */

static void conc_seen_set(u32 page)
{
    spin_lock(&conc_stat_lock);
    if (test_bit(conc_seen, page)) conc_conflicts++;
    else set_bit(conc_seen, page);
    spin_unlock(&conc_stat_lock);
}

static void conc_seen_clear(u32 page)
{
    spin_lock(&conc_stat_lock);
    clear_bit(conc_seen, page);
    spin_unlock(&conc_stat_lock);
}

static void conc_bump_alloc(void)
{
    spin_lock(&conc_stat_lock);
    conc_alloc_ok++;
    spin_unlock(&conc_stat_lock);
}

static void conc_worker(void *arg)
{
    u32 rounds = (u32)(unsigned long)arg;
    u32 i;

    for (i = 0; i < rounds; i++) {
        u32 a = pmm_alloc_pages(PMM_CONC_NPAGES);
        if (a == 0) break;                    /* 分配失败（内存不足）即停止 */

        conc_bump_alloc();
        conc_seen_set(PHYS_TO_PAGE(a));
        conc_seen_set(PHYS_TO_PAGE(a) + 1u);
        conc_seen_set(PHYS_TO_PAGE(a) + 2u);
        conc_seen_set(PHYS_TO_PAGE(a) + 3u);
        /* 先清登记再释放：避免把另一任务对新帧的登记误清 */
        conc_seen_clear(PHYS_TO_PAGE(a));
        conc_seen_clear(PHYS_TO_PAGE(a) + 1u);
        conc_seen_clear(PHYS_TO_PAGE(a) + 2u);
        conc_seen_clear(PHYS_TO_PAGE(a) + 3u);

        if (pmm_free_pages(a, PMM_CONC_NPAGES) != PMM_OK) break;
        task_yield();
    }
    task_exit(0);
}

u32 pmm_selftest_lock(void)
{
    u32 free_before, free_after;
    u32 pid0, pid1, code;

    if (total_pages == 0) return 1;

    free_before = pmm_free_page_count();
    spin_lock_init(&conc_stat_lock);
    conc_conflicts = 0;
    conc_alloc_ok  = 0;
    memset(conc_seen, 0, sizeof(conc_seen));

    pid0 = task_create("pmm-c0", conc_worker,
                       (void *)(unsigned long)PMM_CONC_ROUNDS, SCHED_CFS, 0);
    if ((int)pid0 < 0) return 2;
    pid1 = task_create("pmm-c1", conc_worker,
                       (void *)(unsigned long)PMM_CONC_ROUNDS, SCHED_CFS, 0);
    if ((int)pid1 < 0) return 3;

    /* 等待两个并发任务跑完并回收 */
    (void)task_join(pid0, &code);
    (void)task_join(pid1, &code);

    if (conc_conflicts != 0) return 4;                  /* 帧冲突 → 锁失效 */
    if (conc_alloc_ok != PMM_CONC_ROUNDS * 2u) return 5; /* 分配次数守恒 */
    free_after = pmm_free_page_count();
    if (free_after != free_before) return 6;            /* 空闲页守恒（无泄漏） */
    if (pmm_lock_enter_count() == 0) return 7;          /* 锁必须真实被使用 */

    return 0;
}
