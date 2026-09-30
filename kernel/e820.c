/* ============================================================================
 * XOS E820 物理内存探测与内存映射管理实现
 * 对应功能点 1-30
 *
 * 本版修复（针对审查发现的缺陷）：
 *   1) 原实现直接信任 0x04F0 处的条目数，若 Stage2 写入失败会读到垃圾。
 *      现增加签名（0x04F4 = 'XOSE'）校验与条目合法性校验。
 *   2) e820_get / e820_table 原先返回可写指针，调用方可绕过排序与合并
 *      直接改表，破坏不变式。现统一改为 const 返回。
 *   3) e820_stats 的 total_bytes 为各条目长度直接累加，遇到跨类型重叠
 *      区间会重复计数。现额外提供去重后的 union_bytes。
 *   4) e820_dump 原把 64 位基址/长度截断为 32 位输出，超过 4GB 的区间
 *      显示错误。现完整输出 64 位。
 *   5) 移除赋值后从未使用的 inited 死变量，改为真正生效的就绪标志。
 *   6) 新增 e820_find（地址→条目）与 e820_validate（映射一致性校验）。
 * ============================================================================ */
#include "e820.h"
#include "console.h"
#include "string.h"

/* 从 Stage2 固定的物理地址读取（低端内存，恒等映射可访问） */
static const e820_entry_t *const raw_table =
    (const e820_entry_t *)E820_BUF_ADDR;
static const u16 *const raw_count = (const u16 *)E820_CNT_ADDR;
static const u32 *const raw_magic = (const u32 *)E820_MAGIC_ADDR;

/* 内核内部规范化后的映射表 */
static e820_entry_t map[E820_MAX_ENTRIES];
static u32  map_count = 0;
static bool e820_is_ready = false;

/* --------------------------------------------------------------------------
 * 类型名称
 * ------------------------------------------------------------------------ */
const char *e820_type_name(u32 type)
{
    switch (type) {
    case E820_USABLE:       return "Usable";
    case E820_RESERVED:     return "Reserved";
    case E820_ACPI_RECLAIM: return "ACPI-Reclaim";
    case E820_ACPI_NVS:     return "ACPI-NVS";
    case E820_BAD:          return "Bad";
    default:                return "Unknown";
    }
}

bool e820_ready(void) { return e820_is_ready; }

/* --------------------------------------------------------------------------
 * 探测入口：把 BIOS 结果规范化进内核表
 * ------------------------------------------------------------------------ */
void e820_init(void)
{
    u32 n, i;

    e820_is_ready = false;
    map_count = 0;

    /* 签名校验：Stage2 未能写入则数据不可信，直接放弃 */
    if (*raw_magic != E820_SIGVAL) {
        memset(map, 0, sizeof(map));
        return;
    }

    n = (u32)(*raw_count);
    if (n > E820_MAX_ENTRIES) n = E820_MAX_ENTRIES;

    for (i = 0; i < n; i++) {
        const e820_entry_t *e = &raw_table[i];

        /* 过滤长度非法的条目 */
        if (e->length == 0) continue;
        /* 过滤非规范类型 */
        if (e->type == 0 || e->type > E820_BAD) continue;
        /* 过滤长度导致地址回绕的条目 */
        if (e->base + e->length < e->base) continue;

        map[map_count] = *e;
        map_count++;
    }

    e820_is_ready = true;
    e820_sort();
    e820_merge();
}

u32 e820_count(void) { return map_count; }

const e820_entry_t *e820_get(u32 index)
{
    if (index >= map_count) return NULL;
    return &map[index];
}

const e820_entry_t *e820_table(void) { return map; }

/* --------------------------------------------------------------------------
 * 按物理地址查找所属条目（区间为 [base, base+length)）
 * ------------------------------------------------------------------------ */
const e820_entry_t *e820_find(u64 addr)
{
    u32 i;
    for (i = 0; i < map_count; i++) {
        if (addr >= map[i].base && addr < map[i].base + map[i].length) {
            return &map[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * 按基址升序排序（插入排序，条目数很少）
 * ------------------------------------------------------------------------ */
void e820_sort(void)
{
    u32 i, j;
    for (i = 1; i < map_count; i++) {
        e820_entry_t key = map[i];
        j = i;
        while (j > 0 && map[j - 1].base > key.base) {
            map[j] = map[j - 1];
            j--;
        }
        map[j] = key;
    }
}

/* --------------------------------------------------------------------------
 * 合并相邻/重叠且同类型的区间
 * 返回合并后减少的条目数
 * ------------------------------------------------------------------------ */
u32 e820_merge(void)
{
    u32 i, w;
    u32 before = map_count;

    if (map_count < 2) return 0;

    w = 0;
    for (i = 1; i < map_count; i++) {
        e820_entry_t *cur = &map[w];
        u64 cur_end = cur->base + cur->length;
        u64 new_end = map[i].base + map[i].length;

        /* 同类型 且 区间相邻或重叠 → 合并为并集 */
        if (map[i].type == cur->type && map[i].base <= cur_end) {
            if (new_end > cur_end) {
                cur->length = new_end - cur->base;
            }
            continue;
        }
        w++;
        map[w] = map[i];
    }
    map_count = w + 1;
    return before - map_count;
}

/* --------------------------------------------------------------------------
 * 映射一致性校验
 * 返回 E820_VALID_* 之一，0 表示通过
 * ------------------------------------------------------------------------ */
u32 e820_validate(void)
{
    u32 i;
    bool has_usable = false;

    if (*raw_magic != E820_SIGVAL) return E820_VALID_NOMAGIC;
    if (map_count == 0) return E820_VALID_EMPTY;
    if (map_count > E820_MAX_ENTRIES) return E820_VALID_COUNT;

    for (i = 0; i < map_count; i++) {
        const e820_entry_t *e = &map[i];

        if (e->length == 0) return E820_VALID_LENGTH;
        if (e->type == 0 || e->type > E820_BAD) return E820_VALID_TYPE;
        if (e->type == E820_USABLE) has_usable = true;

        /* 排序与重叠检查 */
        if (i > 0) {
            const e820_entry_t *p = &map[i - 1];
            u64 pend = p->base + p->length;
            if (e->base < p->base) return E820_VALID_ORDER;
            /* 同类型已在 merge 中合并；此处只剩跨类型重叠 */
            if (e->base < pend && e->type != p->type) {
                return E820_VALID_OVERLAP;
            }
        }
    }

    if (!has_usable) return E820_VALID_NORAM;
    return E820_VALID_OK;
}

/* --------------------------------------------------------------------------
 * 内存统计
 * ------------------------------------------------------------------------ */
void e820_stats(mem_stats_t *out)
{
    u32 i;

    if (!out) return;

    memset(out, 0, sizeof(*out));
    out->entries = map_count;

    for (i = 0; i < map_count; i++) {
        const e820_entry_t *e = &map[i];
        u64 end = e->base + e->length;

        out->total_bytes += e->length;
        if (end > out->highest_addr) out->highest_addr = end;

        switch (e->type) {
        case E820_USABLE:
            out->usable_bytes += e->length;
            out->usable_entries++;
            if (e->base < 0x100000u) {
                u64 top = (end < 0x100000u) ? end : 0x100000u;
                u64 low = (e->base < top) ? (top - e->base) : 0;
                out->below_1m_usable += (u32)low;
            }
            break;
        case E820_RESERVED:     out->reserved_bytes += e->length;     break;
        case E820_ACPI_RECLAIM: out->acpi_reclaim_bytes += e->length; break;
        case E820_ACPI_NVS:     out->acpi_nvs_bytes += e->length;     break;
        case E820_BAD:          out->bad_bytes += e->length;          break;
        default: break;
        }
    }

    /* 去重覆盖长度：表已按基址排序，顺序扫描取并集 */
    {
        u64 cur_start = 0, cur_end = 0;
        bool first = true;
        for (i = 0; i < map_count; i++) {
            u64 b = map[i].base;
            u64 e = map[i].base + map[i].length;
            if (first) {
                cur_start = b; cur_end = e; first = false;
                continue;
            }
            if (b <= cur_end) {
                if (e > cur_end) cur_end = e;
            } else {
                out->union_bytes += (cur_end - cur_start);
                cur_start = b; cur_end = e;
            }
        }
        if (!first) out->union_bytes += (cur_end - cur_start);
    }

    out->usable_pages = (u32)(out->usable_bytes >> 12);
}

/* --------------------------------------------------------------------------
 * 调试输出：打印整张内存映射表（64 位完整输出）
 * ------------------------------------------------------------------------ */
static void put_hex64_plain(u64 v)
{
    con_put_hex((u32)(v >> 32), 8);
    con_put_hex((u32)(v & 0xFFFFFFFFu), 8);
}

void e820_dump(void)
{
    u32 i;

    con_puts("  No  Base (64-bit)      Length (64-bit)    Type\n");
    con_puts("  --- ------------------ ------------------ ---------------\n");
    for (i = 0; i < map_count; i++) {
        const e820_entry_t *e = &map[i];
        char nbuf[12];

        con_puts("  ");
        utoa(i, nbuf, 10);
        con_puts(nbuf);
        {
            u32 l = (u32)strlen(nbuf);
            u32 pad = (l < 3) ? (3u - l) : 0u;
            while (pad--) con_putc(' ');
        }
        con_putc(' ');
        put_hex64_plain(e->base);
        con_putc(' ');
        put_hex64_plain(e->length);
        con_putc(' ');
        con_puts(e820_type_name(e->type));
        if (e->type == E820_USABLE && (e->acpi & E820_ATTR_ENABLED)) {
            con_puts(" (enabled)");
        }
        con_putc('\n');
    }
}

/* --------------------------------------------------------------------------
 * 自检：验证规范化后的映射表满足全部不变式
 * 返回 0 表示通过，否则返回失败用例号
 * ------------------------------------------------------------------------ */
u32 e820_selftest(void)
{
    u32 i;

    if (!e820_is_ready) return 1;
    if (map_count == 0) return 2;
    if (map_count > E820_MAX_ENTRIES) return 3;

    /* 用例 1：严格升序（相邻条目起点单调不减，且不重叠） */
    for (i = 1; i < map_count; i++) {
        if (map[i].base < map[i - 1].base) return 4;
        if (map[i].base < map[i - 1].base + map[i - 1].length) {
            if (map[i].type == map[i - 1].type) return 5;  /* 同类型应已合并 */
        }
    }

    /* 用例 2：不存在零长度或非法类型 */
    for (i = 0; i < map_count; i++) {
        if (map[i].length == 0) return 6;
        if (map[i].type == 0 || map[i].type > E820_BAD) return 7;
    }

    /* 用例 3：至少存在一个可用区间 */
    {
        bool found = false;
        for (i = 0; i < map_count; i++) {
            if (map[i].type == E820_USABLE) { found = true; break; }
        }
        if (!found) return 8;
    }

    /* 用例 4：校验函数与状态一致 */
    if (e820_validate() != E820_VALID_OK) return 9;

    /* 用例 5：统计口径自洽 */
    {
        mem_stats_t st;
        e820_stats(&st);
        if (st.entries != map_count) return 10;
        if (st.usable_bytes == 0) return 11;
        if (st.highest_addr == 0) return 12;
        if (st.union_bytes == 0) return 13;
        if (st.union_bytes > st.total_bytes) return 14;
        if (st.usable_bytes > st.union_bytes) return 15;
    }

    /* 用例 6：地址查找正确 —— 取第一个可用区间的起点，应能命中 */
    {
        const e820_entry_t *e = NULL;
        for (i = 0; i < map_count; i++) {
            if (map[i].type == E820_USABLE) { e = &map[i]; break; }
        }
        if (!e) return 16;
        if (e820_find(e->base) != e) return 17;
        if (e820_find(e->base + e->length) == e) return 18; /* 右开区间 */
    }

    /* 用例 7：越界访问返回 NULL */
    if (e820_get(map_count) != NULL) return 19;
    if (e820_get(0xFFFFFFFFu) != NULL) return 20;

    return 0;
}
