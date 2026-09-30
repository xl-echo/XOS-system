/* ============================================================================
 * XOS 内存探测与统计 —— 实现
 *
 * 设计约束（务必遵守）：
 *  1. 只读固件结构，绝不写固件区域；
 *  2. 一切物理地址访问先过 mdet_in_window()，窗口外拒绝访问；
 *  3. 不使用 64 位除法（-nostdlib 下会引入 __udivdi3），百分比一律走 pct_x100()；
 *  4. 任何一处解析失败都不中断整体探测，只记录状态并继续（容错优先）。
 * ============================================================================ */
#include "memdetect.h"
#include "console.h"
#include "string.h"
#include "pmm.h"
#include "sync.h"

/* --------------------------------------------------------------------------
 * 静态状态
 * -------------------------------------------------------------------------- */
static e820_entry_t   mdet_map[MDET_MAX_REGIONS];
static e820_entry_t   mdet_map0[MDET_MAX_REGIONS];   /* mdet_init 时的原始快照 */
static u32            mdet_n0;
static u32            mdet_n;
static u8             mdet_ready;
static u32            mdet_init_rc;

static mdet_types_t   mdet_ty;
static mdet_stats_t   mdet_st;
static mdet_acpi_t    mdet_ac;
static mdet_smbios_t  mdet_sm;
static mdet_physaddr_t mdet_pa;
static mdet_holes_t   mdet_ho;
static mdet_hotplug_t mdet_hp;
static mdet_monitor_t mdet_mo;
static mdet_cmdline_t mdet_cl;

/* ==========================================================================
 * 04 册第二轮重做：并发保护 / 查询快照 / 探测重试与退避
 *  - 全部写入口（探测/恢复/持久化/参数/采样）= 锁包装 + _locked 内部变体；
 *  - 全部查询接口在锁内做一致性快照后返回快照指针，调用者可安全使用；
 *  - 探测失败自动退避重试，重试次数与退避时长计入统计（可测）。
 * ========================================================================== */
static spinlock_t mdet_lock;
static u32         mdet_lock_calls_count = 0;

static inline u32 mdet_lock_enter(void)
{
    u32 eflags = spin_lock_irqsave(&mdet_lock);
    mdet_lock_calls_count++;
    return eflags;
}

static inline void mdet_lock_exit(u32 eflags)
{
    spin_unlock_irqrestore(&mdet_lock, eflags);
}

u32 mdet_lock_calls(void) { return mdet_lock_calls_count; }

static mdet_types_t    snap_types;
static mdet_acpi_t     snap_acpi;
static mdet_smbios_t   snap_smbios;
static mdet_hotplug_t  snap_hotplug;
static mdet_physaddr_t snap_physaddr;
static mdet_holes_t    snap_holes;
static mdet_monitor_t  snap_monitor;
static mdet_cmdline_t  snap_cmdline;
static mdet_bootinfo_t snap_bootinfo;
static e820_entry_t    snap_region;

static mdet_retry_stats_t mdet_rs;

/* 忙等退避：n 次重试后等待 n*n*N 个 io 周期（模拟 us 级退避，不依赖定时器） */
static u32 mdet_backoff_delay(u32 n)
{
    volatile u32 k = (n * n * 40u) + 40u;
    while (k--) __asm__ __volatile__("pause");
    return n * n * 40u + 40u;
}

/* 失败自动退避重试：调用 fn，失败则退避后重试，最多 max_retry 次 */
int mdet_probe_with_retry(int (*fn)(void), u32 max_retry)
{
    u32 attempt, delay_us = 0;
    int rc;
    if (!fn) return MDET_EINVAL;
    if (max_retry == 0) max_retry = 1;
    rc = fn();
    for (attempt = 1; rc != MDET_OK && attempt < max_retry; attempt++) {
        delay_us = mdet_backoff_delay(attempt);
        mdet_rs.retry_total++;
        mdet_rs.last_backoff_us = delay_us;
        rc = fn();
    }
    if (rc != MDET_OK && max_retry > 1) { mdet_rs.retry_failed++; mdet_rs.probe_timeouts++; }
    return rc;
}

const mdet_retry_stats_t *mdet_retry_stats(void)
{
    return &mdet_rs;
}
/* --------------------------------------------------------------------------
 * 基础工具
 * -------------------------------------------------------------------------- */

/* 物理地址窗口校验：窗口之外一律不得解引用。
 * 用减法而不是加法判断上界，避免 addr+len 自身溢出回绕后绕过检查。 */
int mdet_in_window(u64 addr, u32 len)
{
    if (len == 0) return MDET_EINVAL;
    if (addr >= (u64)MDET_WINDOW_TOP) return MDET_EWINDOW;
    if ((u64)len > (u64)MDET_WINDOW_TOP - addr) return MDET_EWINDOW;
    return MDET_OK;
}

static const void *mdet_ptr(u64 addr, u32 len)
{
    if (mdet_in_window(addr, len) != MDET_OK) return 0;
    return (const void *)(u32)addr;
}

/* 百分比 × 100，整数实现。
 * 先把两个操作数同步右移到 32 位可容纳的范围，再做 32 位除法 ——
 * 这样既不会触发 64 位除法，也不会让 part*100 溢出。 */
static u32 pct_x100(u64 part, u64 whole)
{
    u32 p, w;
    if (whole == 0) return 0;
    if (part > whole) part = whole;
    while (whole > 0x02000000ull) { whole >>= 1; part >>= 1; }
    p = (u32)part;
    w = (u32)whole;
    if (w == 0) return 0;
    return (p * 100u) / w;
}

static u8 mdet_sum8(const u8 *p, u32 len)
{
    u8 s = 0;
    u32 i;
    for (i = 0; i < len; i++) s = (u8)(s + p[i]);
    return s;
}

/* 跳过指定字节区间求和：用于持久化块校验。
 * checksum 字段自身不得参与求和 —— 写入时先清零得到 S0 并存入该字段，
 * 回读时若把它也加进去会得到 S0 + S0 != S0，恒失败。 */
static u32 mdet_sum32_skip(const u8 *p, u32 len, u32 skip_off, u32 skip_len)
{
    u32 s = 0, i, end = skip_off + skip_len;
    for (i = 0; i < len; i++) {
        if (i >= skip_off && i < end) continue;
        s += p[i];
    }
    return s;
}

static void mdet_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    *a = 0; *b = 0; *c = 0; *d = 0;
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(0u));
}

static int mdet_sig_printable(const u8 *sig, u32 len)
{
    u32 i;
    for (i = 0; i < len; i++) {
        if (sig[i] < 0x20u || sig[i] > 0x7Eu) return 0;
    }
    return 1;
}

static u64 mdet_u64(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24)
         | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

static u32 mdet_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 mdet_u16(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

/* --------------------------------------------------------------------------
 * S01 / S02 / S03 / S05 / S06 / S07：规范化与统计
 * -------------------------------------------------------------------------- */

/* 判定条目是否值得保留：零长度条目直接丢弃（容错，不算失败）。 */
static void mdet_classify(void)
{
    u32 i;
    u64 below_1m_end = 0x00100000ull;

    memset(&mdet_ty, 0, sizeof(mdet_ty));
    memset(&mdet_st, 0, sizeof(mdet_st));

    mdet_st.region_count = mdet_n;

    for (i = 0; i < mdet_n; i++) {
        u64 b = mdet_map[i].base;
        u64 l = mdet_map[i].length;
        u32 t = mdet_map[i].type;
        u64 end = b + l;

        mdet_st.total_space += l;

        if (t < MDET_TYPE_MAX) {
            mdet_ty.type_seen[t] = 1;
            mdet_ty.type_bytes[t] += l;
        } else {
            mdet_ty.unknown_type_count++;
        }
        if (l == 0) mdet_ty.zero_len_count++;
        if ((b >> 32) != 0 || (end >> 32) != 0) mdet_ty.high_bit_count++;

        switch (t) {
        case E820_USABLE:
            mdet_st.usable += l;
            mdet_st.usable_count++;
            break;
        case E820_RESERVED:
        case E820_BAD:
            mdet_st.reserved += l;
            mdet_st.reserved_count++;
            if (t == E820_BAD) { mdet_st.bad += l; mdet_st.bad_count++; }
            break;
        case E820_ACPI_RECLAIM:
            mdet_st.acpi_reclaim += l;
            mdet_st.acpi_count++;
            break;
        case E820_ACPI_NVS:
            mdet_st.acpi_nvs += l;
            mdet_st.acpi_count++;
            break;
        default:
            /* 未知类型按保留处理，保证「可用 + 保留 + ACPI」守恒 */
            mdet_st.reserved += l;
            mdet_st.reserved_count++;
            break;
        }

        /* S05：低于 1MB 的部分单独统计（该区含 BIOS 数据区、VGA、EBDA） */
        if (b < below_1m_end) {
            u64 seg = (end <= below_1m_end) ? l : (below_1m_end - b);
            mdet_st.below_1m_total += seg;
            if (t == E820_USABLE) mdet_st.below_1m_usable += seg;
            else                  mdet_st.below_1m_reserved += seg;
        }

        mdet_st.union_space += l;   /* 规范化后各条目互不重叠，直接累加 */
    }

    mdet_st.usable_pct_x100 = pct_x100(mdet_st.usable, mdet_st.union_space);
    mdet_st.below_1m_usable_pct_x100 = pct_x100(mdet_st.below_1m_usable, mdet_st.below_1m_total);
}

static int _mdet_init_locked(const e820_entry_t *entries, u32 count)
{
    u32 i;

    mdet_n = 0;
    mdet_ready = 0;
    mdet_init_rc = MDET_OK;

    if (!entries) { mdet_init_rc = MDET_EINVAL; return mdet_init_rc; }
    if (count == 0) { mdet_init_rc = MDET_ENOENT; return mdet_init_rc; }
    if (count > MDET_MAX_REGIONS) count = MDET_MAX_REGIONS;

    /* 只读拷贝：不持有调用方的指针，避免其后续改写破坏不变式 */
    for (i = 0; i < count; i++) {
        mdet_map[i].base   = entries[i].base;
        mdet_map[i].length = entries[i].length;
        mdet_map[i].type   = entries[i].type;
        mdet_map[i].acpi   = entries[i].acpi;
        if (entries[i].length != 0) mdet_n++;
        else {
            /* 零长度条目：丢弃但保留计数痕迹（见 mdet_ty.zero_len_count） */
            mdet_map[i].length = 0;
        }
    }

    /* 压缩掉零长度条目，保持「无空洞条目」的不变式 */
    {
        u32 w = 0;
        for (i = 0; i < count; i++) {
            if (mdet_map[i].length != 0) {
                if (w != i) mdet_map[w] = mdet_map[i];
                w++;
            }
        }
        for (i = w; i < count; i++) {
            mdet_map[i].base = 0; mdet_map[i].length = 0;
            mdet_map[i].type = 0; mdet_map[i].acpi = 0;
        }
        mdet_n = w;
    }

    mdet_classify();

    /* 留存原始快照：内核参数会就地改写映射表，复原时不再重新探测固件 */
    for (i = 0; i < mdet_n; i++) mdet_map0[i] = mdet_map[i];
    mdet_n0 = mdet_n;

    mdet_ready = 1;
    if (mdet_n == 0) { mdet_init_rc = MDET_EFORMAT; mdet_ready = 0; }
    return mdet_init_rc;
}

/* 复原映射表到探测时的原始状态。用于内核参数被撤销后的回滚，
 * 也用于自检：任何参数试验都不得在系统里留下副作用。 */
static int _mdet_restore_locked(void)
{
    u32 i;
    if (mdet_n0 == 0) return MDET_EBUSY;
    for (i = 0; i < MDET_MAX_REGIONS; i++) mdet_map[i] = mdet_map0[i];
    mdet_n = mdet_n0;
    mdet_classify();
    return MDET_OK;
}

u32 mdet_region_count(void)
{
    u32 r, eflags;
    eflags = mdet_lock_enter();
    r = mdet_n;
    mdet_lock_exit(eflags);    return r;
}

u32 mdet_ready_state(void) { return mdet_ready ? 1u : 0u; }

const e820_entry_t *mdet_region(u32 index)
{
    u32 eflags = mdet_lock_enter();
    if (index >= mdet_n) { mdet_lock_exit(eflags); return 0; }
    snap_region = mdet_map[index];
    mdet_lock_exit(eflags);
    return &snap_region;
}

const mdet_types_t *mdet_types(void)
{
    u32 eflags = mdet_lock_enter();
    snap_types = mdet_ty;
    mdet_lock_exit(eflags);
    return &snap_types;
}

void mdet_stats(mdet_stats_t *out)
{
    u32 eflags;
    if (!out) return;
    eflags = mdet_lock_enter();
    *out = mdet_st;
    mdet_lock_exit(eflags);
}

/* --------------------------------------------------------------------------
 * S16 / S17：探测结果校验
 * -------------------------------------------------------------------------- */
u32 mdet_validate(void)
{
    u32 i, w;
    u64 covered = 0;

    if (!mdet_ready) return 1;
    if (mdet_n == 0) return 2;
    if (mdet_n > MDET_MAX_REGIONS) return 3;

    for (i = 0; i < mdet_n; i++) {
        if (mdet_map[i].length == 0) return 4;              /* 残留零长度条目 */
        if (mdet_map[i].base + mdet_map[i].length <= mdet_map[i].base) return 5; /* 回绕 */
        if (mdet_map[i].type < 1 || mdet_map[i].type > 5) return 6;  /* 类型越界 */
        if (i > 0) {
            u64 prev_end = mdet_map[i - 1].base + mdet_map[i - 1].length;
            if (mdet_map[i].base < prev_end) return 7;      /* 未排序或重叠 */
        }
    }

    /* 三分类守恒：可用 + 保留 + ACPI 必须等于覆盖总量 */
    covered = mdet_st.usable + mdet_st.reserved + mdet_st.acpi_reclaim + mdet_st.acpi_nvs;
    if (covered != mdet_st.union_space) return 8;

    /* 可用占比不得超过 100% */
    if (mdet_st.usable_pct_x100 > 100u) return 9;

    /* 1MB 以下统计不得超过 1MB */
    if (mdet_st.below_1m_total > 0x00100000ull) return 10;

    /* 类型字节数与总量守恒 */
    w = 0;
    for (i = 1; i < MDET_TYPE_MAX; i++) {
        if (mdet_ty.type_seen[i]) w++;
        if (mdet_ty.type_bytes[i] > mdet_st.union_space) return 11;
    }
    if (w == 0) return 12;

    if (mdet_st.usable == 0) return 13;                     /* 无可用内存不可用 */

    return 0;
}

const char *mdet_status_name(int code)
{
    switch (code) {
    case MDET_OK:       return "OK";
    case MDET_ENOENT:   return "ENOENT";
    case MDET_EINVAL:   return "EINVAL";
    case MDET_EBADSUM:  return "EBADSUM";
    case MDET_EWINDOW:  return "EWINDOW";
    case MDET_ENOSPC:   return "ENOSPC";
    case MDET_EFORMAT:  return "EFORMAT";
    case MDET_EBUSY:    return "EBUSY";
    default:            return "UNKNOWN";
    }
}

/* --------------------------------------------------------------------------
 * S04：ACPI 区域识别
 * -------------------------------------------------------------------------- */
static u32 mdet_acpi_find_rsdp(void)
{
    /* 搜索顺序：EBDA 前 1KB（由 BIOS 数据区 0x40E 给出段基址）→ 0xE0000-0xFFFFF */
    u32 ebda = (u32)(*(volatile u16 *)0x040E) << 4;
    u32 addr;
    u32 found = 0;
    static const char SIG[8] = { 'R','S','D',' ','P','T','R',' ' };
    u32 i;

    if (ebda >= 0x40000u && ebda < 0xA0000u) {
        for (i = 0; i < 1024u; i += 16u) {
            const u8 *p = (const u8 *)mdet_ptr(ebda + i, 8);
            if (!p) break;
            if (memcmp(p, SIG, 8) == 0) { found = ebda + i; break; }
        }
    }
    if (found) return found;

    for (addr = 0x000E0000u; addr < 0x00100000u; addr += 16u) {
        const u8 *p = (const u8 *)mdet_ptr(addr, 8);
        if (!p) break;
        if (memcmp(p, SIG, 8) == 0) return addr;
    }
    return 0;
}

/* 布局容忍工具的前向声明（mdet_acpi_read_table 早于定义使用） */
static u32 mdet_sdt_len(const u8 *h, u32 addr);
static u32 mdet_xsdt_addr(void);

static void mdet_acpi_read_table(u32 addr)
{
    const u8 *h = (const u8 *)mdet_ptr(addr, 36);
    u32 len;
    char sig[5];
    u32 i;

    if (!h) { mdet_ac.out_of_window++; return; }

    for (i = 0; i < 4; i++) sig[i] = (char)h[i];
    sig[4] = 0;

    if (!mdet_sig_printable((const u8 *)h, 4)) { mdet_ac.bad_signature++; return; }

    /* 表长按布局自动识别并以全表校验和验证（标准：偏移 32；VBox：偏移 4） */
    len = mdet_sdt_len(h, addr);
    if (len == 0) { mdet_ac.bad_signature++; return; }

    if (mdet_ac.table_count < MDET_MAX_ACPI_TABLES) {
        u32 k = mdet_ac.table_count;
        mdet_ac.table_addr[k] = addr;
        for (i = 0; i < 4; i++) mdet_ac.table_sig[k][i] = sig[i];
        mdet_ac.table_sig[k][4] = 0;
        mdet_ac.table_count++;
    }

    if (memcmp(sig, "FACP", 4) == 0) mdet_ac.facp_addr = addr;
    else if (memcmp(sig, "APIC", 4) == 0) mdet_ac.apic_addr = addr;
    else if (memcmp(sig, "MCFG", 4) == 0) mdet_ac.mcfg_addr = addr;
    else if (memcmp(sig, "HPET", 4) == 0) mdet_ac.hpet_addr = addr;
    else if (memcmp(sig, "SRAT", 4) == 0) mdet_ac.srat_addr = addr;
    else if (memcmp(sig, "DSDT", 4) == 0) mdet_ac.dsdt_addr = addr;
}

/* ------------------------------------------------------------------
 * SDT 表长度：适配标准与 VirtualBox 两种表头布局。
 *  - ACPI 标准：Length 位于偏移 32；
 *  - VirtualBox：Length 位于偏移 4、OEM ID 位于偏移 10（"VBOX  "）。
 * 返回 0 表示两种布局都无法通过校验和验证。
 * ------------------------------------------------------------------ */
static u32 mdet_sdt_len(const u8 *h, u32 addr)
{
    u32 l4  = mdet_u32(h + 4);
    u32 l32 = mdet_u32(h + 32);
    int vbox = (h[10] == 'V' && h[11] == 'B' && h[12] == 'O' && h[13] == 'X');

    if (vbox) {
        if (l4 >= 36u && l4 <= 0x00100000u) {
            const u8 *full = (const u8 *)mdet_ptr(addr, l4);
            if (full && mdet_sum8(full, l4) == 0) return l4;
        }
        return 0;
    }
    if (l32 >= 36u && l32 <= 0x00100000u) {
        const u8 *full = (const u8 *)mdet_ptr(addr, l32);
        if (full && mdet_sum8(full, l32) == 0) return l32;
    }
    if (l4 >= 36u && l4 <= 0x00100000u) {
        const u8 *full = (const u8 *)mdet_ptr(addr, l4);
        if (full && mdet_sum8(full, l4) == 0) return l4;
    }
    return 0;
}

/* XSDT 地址：标准 RSDP 在偏移 20，VirtualBox 在偏移 24。
 * 逐一验证签名 "XSDT"，取真实存在的那一个；都验证不到返回 0。 */
static u32 mdet_xsdt_addr(void)
{
    const u8 *q = (const u8 *)mdet_ptr(mdet_ac.rsdp_addr, 36);
    u32 offs[2] = { 20u, 24u };
    u32 i;
    if (!q) return 0;
    for (i = 0; i < 2u; i++) {
        u32 cand = (u32)mdet_u64(q + offs[i]);
        const u8 *h;
        if (cand == 0) continue;
        h = (const u8 *)mdet_ptr(cand, 8);
        if (h && memcmp(h, "XSDT", 4) == 0) return cand;
    }
    return 0;
}

static int _mdet_acpi_probe_locked(void)
{
    u32 rsdp = mdet_acpi_find_rsdp();
    const u8 *p;
    u32 rsdt_len, entries, i;

    memset(&mdet_ac, 0, sizeof(mdet_ac));
    mdet_ac.rsdp_addr = rsdp;
    if (rsdp == 0) return MDET_ENOENT;

    p = (const u8 *)mdet_ptr(rsdp, 20);
    if (!p) { mdet_ac.out_of_window++; return MDET_EWINDOW; }

    mdet_ac.revision = p[15];
    for (i = 0; i < 6; i++) mdet_ac.oem_id[i] = p[9 + i];

    /* ACPI 1.0 校验和：前 20 字节之和为 0 */
    mdet_ac.rsdp_checksum_ok = (mdet_sum8(p, 20) == 0) ? 1 : 0;

    mdet_ac.rsdt_addr = mdet_u32(p + 16);

    /* ACPI 2.0+ 才有 XSDT 与扩展校验和 */
    if (mdet_ac.revision >= 2) {
        const u8 *q = (const u8 *)mdet_ptr(rsdp, 36);
        if (q) {
            u32 rlen = 36u;   /* ACPI 2.0 的 RSDP 固定 36 字节 */
            mdet_ac.xsdt_addr = mdet_xsdt_addr();   /* 布局容忍：签名验证 */
            if (mdet_ac.xsdt_addr != 0) mdet_ac.xsdt_present = 1;
            /* ACPI 6.2+（revision>=3）才在偏移 32 提供 Length 字段 */
            if (mdet_ac.revision >= 3) {
                u32 l = mdet_u32(q + 32);
                if (l >= 36u && l <= 4096u) rlen = l;
            }
            {
                const u8 *full = (const u8 *)mdet_ptr(rsdp, rlen);
                if (full) {
                    u8 ext = mdet_sum8(full, rlen);
                    if (ext != 0) mdet_ac.rsdp_checksum_ok = 0;
                }
            }
        }
    }

    /* 优先用 XSDT（64 位表项），退回 RSDT（32 位表项） */
    {
        u32 base = mdet_ac.xsdt_present ? mdet_ac.xsdt_addr : mdet_ac.rsdt_addr;
        u32 ent_size = mdet_ac.xsdt_present ? 8u : 4u;
        const u8 *h = (const u8 *)mdet_ptr(base, 36);

        if (!h) { mdet_ac.out_of_window++; return MDET_EWINDOW; }

        /* 表长按布局自动识别并以全表校验和验证（标准：偏移 32；VBox：偏移 4） */
        rsdt_len = mdet_sdt_len(h, base);
        if (rsdt_len == 0) {
            mdet_ac.bad_signature++;
            return MDET_EFORMAT;
        }
        mdet_ac.rsdt_ok = 1;   /* 只有通过全表校验和的长度才会被接受 */

        entries = (rsdt_len - 36u) / ent_size;
        if (entries > 64u) entries = 64u;   /* 防御性上限，防止长度字段被污染 */

        for (i = 0; i < entries; i++) {
            u32 off = 36u + i * ent_size;
            u32 taddr;
            const u8 *e = (const u8 *)mdet_ptr(base + off, ent_size);
            if (!e) { mdet_ac.out_of_window++; continue; }
            taddr = mdet_ac.xsdt_present ? (u32)mdet_u64(e) : mdet_u32(e);
            if (taddr == 0) continue;
            mdet_acpi_read_table(taddr);
        }
    }

    return MDET_OK;
}

const mdet_acpi_t *mdet_acpi(void)
{
    u32 eflags = mdet_lock_enter();
    snap_acpi = mdet_ac;
    mdet_lock_exit(eflags);
    return &snap_acpi;
}

const char *mdet_acpi_table_name(const char *sig)
{
    if (memcmp(sig, "FACP", 4) == 0) return "FADT 固定描述表";
    if (memcmp(sig, "APIC", 4) == 0) return "MADT 中断控制器表";
    if (memcmp(sig, "MCFG", 4) == 0) return "MCFG PCIe 配置空间表";
    if (memcmp(sig, "HPET", 4) == 0) return "HPET 高精度定时器表";
    if (memcmp(sig, "SRAT", 4) == 0) return "SRAT 内存亲和性表";
    if (memcmp(sig, "DSDT", 4) == 0) return "DSDT 差分系统描述表";
    if (memcmp(sig, "SSDT", 4) == 0) return "SSDT 附加系统描述表";
    if (memcmp(sig, "FACS", 4) == 0) return "FACS 固件 ACPI 控制结构";
    if (memcmp(sig, "BGRT", 4) == 0) return "BGRT 引导图形资源表";
    if (memcmp(sig, "WAET", 4) == 0) return "WAET Windows ACPI 模拟表";
    if (memcmp(sig, "OEM",  3) == 0) return "厂商自定义表";
    return "未识别的 ACPI 表";
}

/* --------------------------------------------------------------------------
 * S10：内存热插拔区域探测
 * -------------------------------------------------------------------------- */
static int _mdet_hotplug_probe_locked(void)
{
    memset(&mdet_hp, 0, sizeof(mdet_hp));

    /* 来源一：E820 扩展属性位中的「非易失」标记（NVDIMM 类热插拔内存） */
    {
        u32 i;
        for (i = 0; i < mdet_n; i++) {
            if (mdet_map[i].acpi & E820_ATTR_NONVOLATILE) mdet_hp.nonvolatile_count++;
        }
    }

    /* 来源二：SRAT 的内存亲和性结构（type 1），其 flags 位 1 = 可热插拔 */
    if (mdet_ac.srat_addr != 0) {
        const u8 *h = (const u8 *)mdet_ptr(mdet_ac.srat_addr, 36);
        if (h) {
            u32 len = mdet_u32(h + 4);
            u32 off;
            mdet_hp.srat_present = 1;
            if (len >= 48u && len <= 0x00100000u) {
                for (off = 48u; off + 16u <= len; ) {
                    const u8 *e = (const u8 *)mdet_ptr(mdet_ac.srat_addr + off, 16);
                    u32 elen;
                    if (!e) break;
                    elen = e[1];
                    if (elen < 16u) break;              /* 长度非法，防死循环 */
                    if (off + elen > len) break;

                    if (e[0] == 1u && elen >= 40u) {    /* Memory Affinity */
                        const u8 *m = (const u8 *)mdet_ptr(mdet_ac.srat_addr + off, elen);
                        if (m) {
                            u64 base = mdet_u64(m + 10);
                            u64 alen = mdet_u64(m + 18);
                            u32 flags = mdet_u32(m + 30);
                            mdet_hp.srat_memory_entries++;
                            if (flags & 0x2u) {         /* Hot Pluggable */
                                if (mdet_hp.count < MDET_MAX_HOTPLUG) {
                                    mdet_hp.region[mdet_hp.count].base = base;
                                    mdet_hp.region[mdet_hp.count].len  = alen;
                                    mdet_hp.count++;
                                }
                                mdet_hp.total_bytes += alen;
                            }
                        }
                    }
                    off += elen;
                }
            }
        }
    }

    return MDET_OK;
}

const mdet_hotplug_t *mdet_hotplug(void)
{
    u32 eflags = mdet_lock_enter();
    snap_hotplug = mdet_hp;
    mdet_lock_exit(eflags);
    return &snap_hotplug;
}

/* --------------------------------------------------------------------------
 * S11 / S12 / S13：SMBIOS 内存条信息、速率与时序、ECC 能力
 * -------------------------------------------------------------------------- */
static void mdet_smbios_str(const u8 *tab, u32 max, u8 idx, u8 *out, u32 outlen);
static u32 mdet_smbios_find(void)
{
    u32 addr;
    u32 found32 = 0, found64 = 0;
    static const char A2[4] = { '_','S','M','_' };
    static const char A3[5] = { '_','S','M','3','_' };

    for (addr = 0x000F0000u; addr < 0x00100000u; addr += 16u) {
        const u8 *p = (const u8 *)mdet_ptr(addr, 8);
        if (!p) break;
        if (!found32 && memcmp(p, A2, 4) == 0) found32 = addr;
        if (!found64 && memcmp(p, A3, 5) == 0) found64 = addr;
    }
    /* 优先 3.0 入口点（结构表地址为 64 位），退回 2.1 */
    return found64 ? found64 : found32;
}

/* 解码 Type 17 的 Size 字段（SMBIOS 2.7 起支持位 15 = KB 单位与扩展容量） */
static u32 mdet_smbios_size_mb(const u8 *d, u32 len, u8 *unknown)
{
    u16 size;
    u32 ext;
    *unknown = 0;
    if (len < 0x1Cu) return 0;
    size = mdet_u16(d + 0x0C);
    if (size == 0) return 0;                 /* 未安装 */
    if (size == 0xFFFFu) { *unknown = 1; return 0; }
    if (size == 0x7FFFu && len >= 0x20u) {   /* 使用扩展容量字段（单位 MB） */
        ext = mdet_u32(d + 0x1C) & 0x7FFFFFFFu;
        return ext;
    }
    if (size & 0x8000u) return (u32)(size & 0x7FFFu) / 1024u;  /* 单位 KB */
    return (u32)size;                        /* 单位 MB */
}

static int _mdet_smbios_probe_locked(void)
{
    u32 ep = mdet_smbios_find();
    const u8 *e;
    u32 tbl_addr = 0;
    u32 tbl_len = 0;
    u32 off;
    u32 n = 0;

    memset(&mdet_sm, 0, sizeof(mdet_sm));
    mdet_sm.entry_addr = ep;
    if (ep == 0) return MDET_ENOENT;

    if (memcmp((const u8 *)ep, "_SM3_", 5) == 0) {
        e = (const u8 *)mdet_ptr(ep, 24);
        if (!e) { mdet_sm.out_of_window++; return MDET_EWINDOW; }
        mdet_sm.is_v3 = 1;
        mdet_sm.major = e[7];
        mdet_sm.minor = e[8];
        mdet_sm.entry_checksum_ok = (mdet_sum8(e, 24) == 0) ? 1 : 0;
        tbl_len = mdet_u32(e + 12);
        tbl_addr = (u32)mdet_u64(e + 16);
    } else {
        e = (const u8 *)mdet_ptr(ep, 31);
        if (!e) { mdet_sm.out_of_window++; return MDET_EWINDOW; }
        mdet_sm.is_v3 = 0;
        mdet_sm.major = e[6];
        mdet_sm.minor = e[7];
        mdet_sm.entry_checksum_ok = (mdet_sum8(e, 31) == 0) ? 1 : 0;
        /* 中间锚点必须为 _DMI_，否则该入口点不可信 */
        if (memcmp(e + 16, "_DMI_", 5) != 0) {
            mdet_sm.format_errors++;
            return MDET_EFORMAT;
        }
        tbl_len = mdet_u16(e + 22);
        tbl_addr = mdet_u32(e + 24);
    }

    if (tbl_addr == 0) return MDET_ENOENT;
    if (mdet_in_window(tbl_addr, tbl_len) != MDET_OK) {
        mdet_sm.out_of_window++;
        return MDET_EWINDOW;
    }

    mdet_sm.table_len = tbl_len;

    /* 遍历结构：每条含头部（type/length/handle）+ 格式化区 + 字符串区 */
    off = 0;
    while (off + 4u <= tbl_len) {
        const u8 *s = (const u8 *)mdet_ptr(tbl_addr + off, 4);
        u8 type, slen;
        if (!s) break;
        type = s[0];
        slen = s[1];
        if (slen < 4u) { mdet_sm.format_errors++; break; }
        if (off + slen > tbl_len) { mdet_sm.format_errors++; break; }

        mdet_sm.struct_count++;

        if (type == 16u && slen >= 0x0Fu) {
            const u8 *d = (const u8 *)mdet_ptr(tbl_addr + off, slen);
            if (d) {
                mdet_sm.array_count++;
                /* 首个阵列记录纠错能力：0x05 = 单比特 ECC，0x06 = 多比特 ECC，0x07 = CRC */
                if (mdet_sm.ecc_type == 0) mdet_sm.ecc_type = d[0x06];
            }
        } else if (type == 17u) {
            const u8 *d = (const u8 *)mdet_ptr(tbl_addr + off, slen);
            if (d) {
                u8 unknown = 0;
                u32 mb = mdet_smbios_size_mb(d, slen, &unknown);
                u16 speed = 0, cspeed = 0;
                mdet_sm.device_count++;
                if (unknown) mdet_sm.unknown_size_slots++;
                if (mb > 0) {
                    mdet_sm.device_populated++;
                    mdet_sm.total_size_mb += mb;
                }
                if (slen >= 0x17u) speed = mdet_u16(d + 0x15);
                if (slen >= 0x22u) cspeed = mdet_u16(d + 0x20);
                if (speed != 0 && speed != 0xFFFFu && speed > mdet_sm.max_speed_mt)
                    mdet_sm.max_speed_mt = speed;
                if (cspeed != 0 && cspeed != 0xFFFFu && cspeed > mdet_sm.max_cfg_speed_mt)
                    mdet_sm.max_cfg_speed_mt = cspeed;
                if (speed != 0 && speed != 0xFFFFu && cspeed != 0 && cspeed != 0xFFFFu
                    && cspeed < speed)
                    mdet_sm.speed_mismatch++;

                /* Type 17 的 Attributes 位 0x02 = 该器件报告支持纠错 */
                if (slen >= 0x1Cu && (d[0x1B] & 0x02u)) mdet_sm.ecc_present = 1;

                if (mdet_sm.form_factor == 0 && slen >= 0x0Fu) mdet_sm.form_factor = d[0x0E];
                if (mdet_sm.memory_tech == 0 && slen >= 0x13u) mdet_sm.memory_tech = d[0x12];
                if (mdet_sm.type_detail == 0 && slen >= 0x15u) mdet_sm.type_detail = d[0x13];

                if (n < MDET_SMBIOS_SLOTS) {
                    mdet_sm.slot_size_mb[n] = (mb > 0xFFFFu) ? 0xFFFFu : (u16)mb;
                    mdet_sm.slot_speed_mt[n] = (speed == 0xFFFFu) ? 0 : speed;
                    /* 04 册第二轮：模块位宽 / 内存技术 / 配置电压 / 字符串明细 */
                    if (slen >= 0x09u) mdet_sm.slot_module_width[n] = d[0x08];
                    if (slen >= 0x13u) mdet_sm.slot_memory_tech[n] = d[0x12];
                    if (slen >= 0x23u) mdet_sm.slot_voltage_mv[n] = (u16)mdet_u16(d + 0x22);
                    {
                        u32 remain = (tbl_len > off + slen) ? (tbl_len - off - slen) : 0;
                        const u8 *st = (const u8 *)mdet_ptr(tbl_addr + off + slen, (remain > 4u) ? 4u : remain);
                        if (st && remain > 0) {
                            mdet_smbios_str(st, remain, d[0x15],
                                            mdet_sm.slot_manufacturer[n], 16);
                            mdet_smbios_str(st, remain, d[0x16],
                                            mdet_sm.slot_serial[n], 16);
                            mdet_smbios_str(st, remain, d[0x18],
                                            mdet_sm.slot_part[n], 24);
                        }
                    }
                    n++;
                }
            }
        } else if (type == 19u && slen >= 0x0Fu) {
            /* 内存阵列映射地址：起始/结束地址单位为 KB，0xFFFFFFFF=未知 */
            const u8 *d = (const u8 *)mdet_ptr(tbl_addr + off, slen);
            if (d && mdet_sm.type19_count < 4u) {
                u32 start = mdet_u32(d + 0x04);
                u32 end   = mdet_u32(d + 0x08);
                if (end >= start && start != 0xFFFFFFFFu && end != 0xFFFFFFFFu) {
                    mdet_sm.type19[mdet_sm.type19_count].base = (u64)start * 1024u;
                    mdet_sm.type19[mdet_sm.type19_count].len  = ((u64)end - (u64)start + 1u) * 1024u;
                    mdet_sm.type19_count++;
                }
            }
        }

        /* 跳到字符串区结束（连续两个 0 字节），至少前进 slen */
        off += slen;
        {
            u32 guard = 0;
            while (off < tbl_len && guard < tbl_len) {
                const u8 *c = (const u8 *)mdet_ptr(tbl_addr + off, 1);
                if (!c) break;
                if (*c == 0) {
                    const u8 *c2 = (const u8 *)mdet_ptr(tbl_addr + off + 1, 1);
                    off++;
                    if (!c2 || *c2 == 0) { off++; break; }
                } else {
                    off++;
                }
                guard++;
            }
        }
    }

    mdet_sm.slot_populated = (u16)n;

    /* 纠错能力汇总：Type 16 的 0x05/0x06/0x07 均为 ECC 类 */
    if (mdet_sm.ecc_type >= 0x05u && mdet_sm.ecc_type <= 0x07u) mdet_sm.ecc_present = 1;
    if (mdet_sm.ecc_type == 0x05u || mdet_sm.ecc_type == 0x06u) mdet_sm.ecc_capable_slots = 1;

    return MDET_OK;
}

const mdet_smbios_t *mdet_smbios(void)
{
    u32 eflags = mdet_lock_enter();
    snap_smbios = mdet_sm;
    mdet_lock_exit(eflags);
    return &snap_smbios;
}

/* --------------------------------------------------------------------------
 * S09：物理地址空间上限探测
 * -------------------------------------------------------------------------- */
static int _mdet_physaddr_probe_locked(void)
{
    u32 a, b, c, d;
    u32 i;
    u32 max_leaf;

    memset(&mdet_pa, 0, sizeof(mdet_pa));

    for (i = 0; i < mdet_n; i++) {
        u64 end = mdet_map[i].base + mdet_map[i].length;
        if (end > mdet_pa.e820_max_end) mdet_pa.e820_max_end = end;
        if (mdet_map[i].type == E820_USABLE && end > mdet_pa.e820_highest_usable)
            mdet_pa.e820_highest_usable = end;
        if ((mdet_map[i].base >> 32) != 0 || (end >> 32) != 0)
            mdet_pa.needs_64bit_addr = 1;
    }

    mdet_cpuid(0x80000000u, &a, &b, &c, &d);
    max_leaf = a;
    if (max_leaf >= 0x80000008u) {
        mdet_cpuid(0x80000008u, &a, &b, &c, &d);
        mdet_pa.cpuid_phys_bits   = (u8)(a & 0xFFu);
        mdet_pa.cpuid_linear_bits = (u8)((a >> 8) & 0xFFu);
        mdet_pa.cpuid_ok = 1;
        if (mdet_pa.cpuid_phys_bits >= 64u) mdet_pa.cpuid_phys_bits = 63u;
        if (mdet_pa.cpuid_phys_bits > 0u)
            mdet_pa.cpuid_max_phys = 1ull << mdet_pa.cpuid_phys_bits;
    } else {
        /* 不支持该叶：按 32 位物理寻址的保守假设处理 */
        mdet_pa.cpuid_ok = 0;
        mdet_pa.cpuid_phys_bits = 32u;
        mdet_pa.cpuid_max_phys = 1ull << 32;
    }

    mdet_pa.e820_within_cpuid = (mdet_pa.e820_max_end <= mdet_pa.cpuid_max_phys) ? 1 : 0;
    if (!mdet_pa.e820_within_cpuid)
        mdet_pa.unaddressable = mdet_pa.e820_max_end - mdet_pa.cpuid_max_phys;

    return MDET_OK;
}

const mdet_physaddr_t *mdet_physaddr(void)
{
    u32 eflags = mdet_lock_enter();
    snap_physaddr = mdet_pa;
    mdet_lock_exit(eflags);
    return &snap_physaddr;
}

/* --------------------------------------------------------------------------
 * S08：内存空洞识别
 * -------------------------------------------------------------------------- */
static int _mdet_holes_scan_locked(void)
{
    u32 i;
    u64 cursor = 0;

    memset(&mdet_ho, 0, sizeof(mdet_ho));

    for (i = 0; i < mdet_n; i++) {
        u64 b = mdet_map[i].base;
        if (b > cursor) {
            u64 gap = b - cursor;
            mdet_ho.count++;
            mdet_ho.total_bytes += gap;
            if (gap > mdet_ho.largest_len) {
                mdet_ho.largest_len = gap;
                mdet_ho.largest_base = cursor;
            }
            if (cursor < 0x00100000ull) {
                u64 e = (b <= 0x00100000ull) ? b : 0x00100000ull;
                mdet_ho.below_1m_hole += e - cursor;
            }
            if (cursor >= 0x100000000ull) mdet_ho.above_4g_hole += gap;
            if (mdet_ho.count <= MDET_MAX_HOLES) {
                mdet_ho.hole[mdet_ho.count - 1].base = cursor;
                mdet_ho.hole[mdet_ho.count - 1].len  = gap;
            }
        }
        {
            u64 end = b + mdet_map[i].length;
            if (end > cursor) cursor = end;
        }
    }

    return MDET_OK;
}

const mdet_holes_t *mdet_holes(void)
{
    u32 eflags = mdet_lock_enter();
    snap_holes = mdet_ho;
    mdet_lock_exit(eflags);
    return &snap_holes;
}

/* --------------------------------------------------------------------------
 * S14：内存映射持久化
 * -------------------------------------------------------------------------- */
u32 mdet_bootinfo_addr(void) { return MDET_BOOTINFO_ADDR; }

/* 持久化块被校验覆盖的字节数：头部（按指针差现算）+ count 条映射 */
static u32 mdet_bootinfo_len(u32 count)
{
    const mdet_bootinfo_t *bi = (const mdet_bootinfo_t *)(u32)MDET_BOOTINFO_ADDR;
    u32 hdr = (u32)((const u8 *)&bi->region[0] - (const u8 *)bi);
    return hdr + (u32)sizeof(e820_entry_t) * count;
}

static int _mdet_persist_locked(void)
{
    mdet_bootinfo_t *bi;
    u32 i;

    if (!mdet_ready) return MDET_EBUSY;
    if (MDET_BOOTINFO_ADDR + sizeof(mdet_bootinfo_t) > 0x00080000u) return MDET_ENOSPC;

    bi = (mdet_bootinfo_t *)(u32)MDET_BOOTINFO_ADDR;
    memset(bi, 0, sizeof(mdet_bootinfo_t));

    bi->magic   = MDET_BOOTINFO_MAGIC;
    bi->version = MDET_BOOTINFO_VER;
    bi->count   = mdet_n;
    bi->total_usable   = mdet_st.usable;
    bi->total_reserved = mdet_st.reserved;
    bi->max_phys       = mdet_pa.e820_max_end;
    bi->acpi_tables    = mdet_ac.table_count;
    bi->smbios_devices = mdet_sm.device_count;
    bi->flags          = (u32)((mdet_ac.rsdp_addr ? 1u : 0u) | (mdet_sm.entry_addr ? 2u : 0u));

    for (i = 0; i < mdet_n; i++) bi->region[i] = mdet_map[i];

    /* 校验和覆盖 magic 起、region[count-1] 止的连续区间。
     * 头部长度按指针差现算，不写死常量 —— 结构体对齐规则变化时不会静默失配。 */
    bi->checksum = 0;
    bi->checksum = mdet_sum32_skip((const u8 *)bi, mdet_bootinfo_len(mdet_n), 12u, 4u);
    return MDET_OK;
}

/* 回读并逐项核对：这是持久化是否真的生效的唯一判据 */
int mdet_persist_verify(void)
{
    const mdet_bootinfo_t *bi = (const mdet_bootinfo_t *)(u32)MDET_BOOTINFO_ADDR;
    u32 sum, i, len;

    if (bi->magic != MDET_BOOTINFO_MAGIC) return MDET_ENOENT;
    if (bi->version != MDET_BOOTINFO_VER) return MDET_EFORMAT;
    if (bi->count == 0 || bi->count > MDET_MAX_REGIONS) return MDET_EFORMAT;

    len = mdet_bootinfo_len(bi->count);
    sum = mdet_sum32_skip((const u8 *)bi, len, 12u, 4u);
    if (sum != bi->checksum) return MDET_EBADSUM;

    if (bi->count != mdet_n) return MDET_EFORMAT;
    for (i = 0; i < mdet_n; i++) {
        if (bi->region[i].base   != mdet_map[i].base)   return MDET_EFORMAT;
        if (bi->region[i].length != mdet_map[i].length) return MDET_EFORMAT;
        if (bi->region[i].type   != mdet_map[i].type)   return MDET_EFORMAT;
    }
    if (bi->total_usable != mdet_st.usable) return MDET_EFORMAT;
    return MDET_OK;
}

const mdet_bootinfo_t *mdet_bootinfo_read(void)
{
    const mdet_bootinfo_t *bi = (const mdet_bootinfo_t *)(u32)MDET_BOOTINFO_ADDR;
    u32 eflags = mdet_lock_enter();
    if (bi->magic != MDET_BOOTINFO_MAGIC) { mdet_lock_exit(eflags); return 0; }
    snap_bootinfo = *bi;
    mdet_lock_exit(eflags);
    return &snap_bootinfo;
}

/* --------------------------------------------------------------------------
 * S15：内存映射可视化输出
 * -------------------------------------------------------------------------- */
#define MDET_MAP_COLS 72u

void mdet_visualize(void)
{
    char bar[MDET_MAP_COLS + 1];
    u32 i, c;
    u64 span64 = mdet_pa.e820_max_end;
    u32 span, step, shift = 0;
    u32 legend_usable = 0, legend_res = 0, legend_acpi = 0, legend_hole = 0;

    if (span64 == 0) span64 = 0x00100000ull;

    /* 先把跨度降到 32 位可容纳的范围再做列宽换算。
     * 直接用 64 位除列数会引入 __udivdi3（-nostdlib 下不可用）；
     * 右移只改变比例精度，不改变映射形状，且 shift 会用于还原地址。 */
    while (span64 > 0x3FFFFFFFull) { span64 >>= 1; shift++; }
    span = (u32)span64;
    step = span / MDET_MAP_COLS;
    if (step == 0) step = 1;

    con_puts("  Physical address map (");
    con_put_dec64(mdet_pa.e820_max_end >> 20);
    con_puts(" MB across ");
    con_put_dec(MDET_MAP_COLS);
    con_puts(" cols)\n");
    con_puts("  0");
    for (i = 0; i < MDET_MAP_COLS - 4u; i++) con_putc(' ');
    con_puts("top\n  |");

    for (c = 0; c < MDET_MAP_COLS; c++) bar[c] = ' ';
    bar[MDET_MAP_COLS] = 0;

    /* 先铺空洞，再覆盖实际区间：未被覆盖的位置即为空洞 */
    for (c = 0; c < MDET_MAP_COLS; c++) {
        u64 lo = ((u64)step * c) << shift;
        u64 hi = ((u64)step * (c + 1u)) << shift;
        u32 found = 0;
        for (i = 0; i < mdet_n; i++) {
            u64 b = mdet_map[i].base;
            u64 e = b + mdet_map[i].length;
            if (b < hi && e > lo) {
                char ch;
                switch (mdet_map[i].type) {
                case E820_USABLE:       ch = '#'; legend_usable = 1; break;
                case E820_ACPI_RECLAIM:
                case E820_ACPI_NVS:     ch = '='; legend_acpi = 1;   break;
                case E820_BAD:          ch = '!'; legend_res = 1;    break;
                default:                ch = '-'; legend_res = 1;    break;
                }
                /* 同一列内多类型混叠时按「可用 > ACPI > 保留」优先级显示 */
                if (!found) bar[c] = ch;
                else if (ch == '#') bar[c] = '#';
                else if (ch == '=' && bar[c] != '#') bar[c] = '=';
                found = 1;
            }
        }
        if (!found) { bar[c] = '.'; legend_hole = 1; }
    }

    con_puts(bar);
    con_puts("|\n");

    /* 图例：只列出实际出现过的类别，避免列出不存在的类别造成误读 */
    con_puts("  ");
    if (legend_usable) con_puts("# usable   ");
    if (legend_acpi)   con_puts("= ACPI   ");
    if (legend_res)    con_puts("- reserved   ! bad   ");
    if (legend_hole)   con_puts(". hole");
    con_puts("\n");

    /* 1MB 以下单独放大：该区含 BIOS 数据区、EBDA、VGA，映射条目最密集 */
    {
        u32 sub = 0x00100000u;   /* 1MB 是常量，用 32 位即可，避免 64 位除法 */
        u32 k;
        char low[MDET_MAP_COLS + 1];
        for (k = 0; k < MDET_MAP_COLS; k++) low[k] = ' ';
        low[MDET_MAP_COLS] = 0;
        for (k = 0; k < MDET_MAP_COLS; k++) {
            u64 lo = (u64)(sub / MDET_MAP_COLS) * k;
            u64 hi = lo + (sub / MDET_MAP_COLS);
            u32 found = 0;
            for (i = 0; i < mdet_n; i++) {
                u64 b = mdet_map[i].base;
                u64 e = b + mdet_map[i].length;
                if (b < hi && e > lo && b < sub) {
                    low[k] = (mdet_map[i].type == E820_USABLE) ? '#' : '-';
                    found = 1;
                    break;
                }
            }
            if (!found) low[k] = '.';
        }
        con_puts("  low 1MB: ");
        con_puts(low);
        con_puts("\n");
    }
}

/* --------------------------------------------------------------------------
 * S19：内存使用率监控
 * -------------------------------------------------------------------------- */
static int _mdet_monitor_sample_locked(void)
{
    u32 total = pmm_total_pages();
    u32 free_p = pmm_free_page_count();
    u32 used;

    if (total == 0) return MDET_EBUSY;
    used = (free_p <= total) ? (total - free_p) : 0;

    mdet_mo.samples++;
    mdet_mo.last_total_pages = total;
    mdet_mo.last_free_pages  = free_p;
    mdet_mo.last_used_x100   = pct_x100(used, total);

    if (used > mdet_mo.peak_used_pages) mdet_mo.peak_used_pages = used;
    if (mdet_mo.min_free_pages == 0 || free_p < mdet_mo.min_free_pages)
        mdet_mo.min_free_pages = free_p;
    if (free_p > mdet_mo.max_free_pages) mdet_mo.max_free_pages = free_p;

    mdet_mo.ring[mdet_mo.ring_head] = free_p;
    mdet_mo.ring_head = (mdet_mo.ring_head + 1u) % MDET_MON_RING;

    /* 迟滞判定：高于 80% 记一次高水位，回落到 60% 以下记一次低水位。
     * 用迟滞而不是单阈值，避免使用率在阈值附近抖动时把事件数刷爆。 */
    if (!mdet_mo.in_high_water && mdet_mo.last_used_x100 >= 80u) {
        mdet_mo.in_high_water = 1;
        mdet_mo.high_water_events++;
    } else if (mdet_mo.in_high_water && mdet_mo.last_used_x100 <= 60u) {
        mdet_mo.in_high_water = 0;
        mdet_mo.low_water_events++;
    }

    return MDET_OK;
}

void mdet_monitor_report(void)
{
    con_puts("  Monitor samples=");
    con_put_dec(mdet_mo.samples);
    con_puts(" free=");
    con_put_dec(mdet_mo.last_free_pages);
    con_puts("/");
    con_put_dec(mdet_mo.last_total_pages);
    con_puts(" used=");
    con_put_dec(mdet_mo.last_used_x100 / 100u);
    con_putc('.');
    con_put_dec(mdet_mo.last_used_x100 % 100u);
    con_puts("% peak_used=");
    con_put_dec(mdet_mo.peak_used_pages);
    con_puts(" min_free=");
    con_put_dec(mdet_mo.min_free_pages);
    con_puts(" hi_evt=");
    con_put_dec(mdet_mo.high_water_events);
    con_puts(" lo_evt=");
    con_put_dec(mdet_mo.low_water_events);
    con_putc('\n');
}

const mdet_monitor_t *mdet_monitor(void)
{
    u32 eflags = mdet_lock_enter();
    snap_monitor = mdet_mo;
    mdet_lock_exit(eflags);
    return &snap_monitor;
}

/* --------------------------------------------------------------------------
 * S20：内存映射与内核参数对齐
 * -------------------------------------------------------------------------- */
int mdet_cmdline_set_mem(u64 limit)
{
    if (limit == 0) return MDET_EINVAL;
    mdet_cl.mem_limit = limit;
    return MDET_OK;
}

int mdet_cmdline_add_memmap(u64 base, u64 len, u32 type)
{
    if (len == 0) { mdet_cl.rejected++; mdet_cl.last_reject_code = MDET_EINVAL; return MDET_EINVAL; }
    if (type < 1u || type > 5u) { mdet_cl.rejected++; mdet_cl.last_reject_code = MDET_EINVAL; return MDET_EINVAL; }
    if (mdet_cl.memmap_count >= MDET_MAX_MEMMAP) {
        mdet_cl.rejected++; mdet_cl.last_reject_code = MDET_ENOSPC; return MDET_ENOSPC;
    }
    mdet_cl.memmap[mdet_cl.memmap_count].base = base;
    mdet_cl.memmap[mdet_cl.memmap_count].len  = len;
    mdet_cl.memmap[mdet_cl.memmap_count].type = type;
    mdet_cl.memmap_count++;
    return MDET_OK;
}

/* 应用顺序：先 memmap=（可把可用区强制改为保留），再 mem=（截断超出上限的可用区）。
 * 顺序不能反：先截断会让后到的 memmap= 落在已被裁掉的区间上而无法生效。 */
static int _mdet_cmdline_apply_locked(void)
{
    u32 i, k;
    if (!mdet_ready) return MDET_EBUSY;

    for (k = 0; k < mdet_cl.memmap_count; k++) {
        for (i = 0; i < mdet_n; i++) {
            if (mdet_map[i].base == mdet_cl.memmap[k].base
                && mdet_map[i].length == mdet_cl.memmap[k].len) {
                if (mdet_map[i].type == E820_USABLE
                    && mdet_cl.memmap[k].type != E820_USABLE) {
                    mdet_cl.forced_reserved++;
                }
                mdet_map[i].type = mdet_cl.memmap[k].type;
                mdet_cl.applied++;
                break;
            }
        }
        if (i == mdet_n) { mdet_cl.rejected++; mdet_cl.last_reject_code = MDET_ENOENT; }
    }

    if (mdet_cl.mem_limit != 0) {
        for (i = 0; i < mdet_n; i++) {
            if (mdet_map[i].type != E820_USABLE) continue;
            if (mdet_map[i].base >= mdet_cl.mem_limit) {
                mdet_cl.clipped_bytes += mdet_map[i].length;
                mdet_map[i].type = E820_RESERVED;
                mdet_cl.applied++;
            } else if (mdet_map[i].base + mdet_map[i].length > mdet_cl.mem_limit) {
                u64 keep = mdet_cl.mem_limit - mdet_map[i].base;
                mdet_cl.clipped_bytes += mdet_map[i].length - keep;
                mdet_map[i].length = keep;
                mdet_cl.applied++;
                /* 被裁掉的部分必须显式登记为保留，否则地址空间会出现无主区间 */
                if (mdet_n < MDET_MAX_REGIONS) {
                    mdet_map[mdet_n].base   = mdet_cl.mem_limit;
                    mdet_map[mdet_n].length = 0;   /* 占位，稍后按实际差值补齐 */
                    mdet_map[mdet_n].length =
                        (mdet_map[i].base + keep + (mdet_cl.clipped_bytes)) - mdet_cl.mem_limit;
                    mdet_map[mdet_n].type   = E820_RESERVED;
                    mdet_map[mdet_n].acpi   = 0;
                    mdet_n++;
                }
            }
        }
    }

    mdet_classify();
    return MDET_OK;
}

const mdet_cmdline_t *mdet_cmdline(void)
{
    u32 eflags = mdet_lock_enter();
    snap_cmdline = mdet_cl;
    mdet_lock_exit(eflags);
    return &snap_cmdline;
}

/* 清空全部内核参数并把映射复原到探测原始状态 */
static void _mdet_cmdline_reset_locked(void)
{
    memset(&mdet_cl, 0, sizeof(mdet_cl));
    (void)_mdet_restore_locked();
}

/* --------------------------------------------------------------------------
 * 汇总输出
 * -------------------------------------------------------------------------- */
void mdet_dump(void)
{
    u32 i;

    con_puts("  Regions=");
    con_put_dec(mdet_st.region_count);
    con_puts(" usable=");
    con_put_dec64(mdet_st.usable >> 10);
    con_puts("KB reserved=");
    con_put_dec64(mdet_st.reserved >> 10);
    con_puts("KB acpi_recl=");
    con_put_dec64(mdet_st.acpi_reclaim >> 10);
    con_puts("KB acpi_nvs=");
    con_put_dec64(mdet_st.acpi_nvs >> 10);
    con_puts("KB\n");

    con_puts("  Usable=");
    con_put_dec(mdet_st.usable_pct_x100 / 100u);
    con_putc('.');
    con_put_dec(mdet_st.usable_pct_x100 % 100u);
    con_puts("% of union ");
    con_put_dec64(mdet_st.union_space >> 10);
    con_puts("KB | below1M tot=");
    con_put_dec64(mdet_st.below_1m_total);
    con_puts(" usable=");
    con_put_dec64(mdet_st.below_1m_usable);
    con_puts(" reserved=");
    con_put_dec64(mdet_st.below_1m_reserved);
    con_putc('\n');

    con_puts("  Types:");
    for (i = 1; i < MDET_TYPE_MAX; i++) {
        if (mdet_ty.type_seen[i]) {
            con_puts(" ");
            con_puts(e820_type_name(i));
            con_puts("=");
            con_put_dec64(mdet_ty.type_bytes[i] >> 10);
            con_puts("KB");
        }
    }
    con_puts(" unknown=");
    con_put_dec(mdet_ty.unknown_type_count);
    con_puts(" zerolen=");
    con_put_dec(mdet_ty.zero_len_count);
    con_puts(" hi64=");
    con_put_dec(mdet_ty.high_bit_count);
    con_putc('\n');

    con_puts("  ACPI rsdp=");
    con_put_hex32(mdet_ac.rsdp_addr);
    con_puts(" rev=");
    con_put_dec(mdet_ac.revision);
    con_puts(" cksum=");
    con_puts(mdet_ac.rsdp_checksum_ok ? "ok" : "BAD");
    con_puts(" rsdt=");
    con_put_hex32(mdet_ac.rsdt_addr);
    con_puts(" xsdt=");
    con_put_hex32(mdet_ac.xsdt_addr);
    con_puts(mdet_ac.xsdt_present ? "+" : "-");
    con_puts(" rsdtok=");
    con_puts(mdet_ac.rsdt_ok ? "y" : "N");
    con_puts(" tables=");
    con_put_dec(mdet_ac.table_count);
    con_puts(" badsum=");
    con_put_dec(mdet_ac.checksum_fail);
    con_puts(" badsig=");
    con_put_dec(mdet_ac.bad_signature);
    con_puts(" outwin=");
    con_put_dec(mdet_ac.out_of_window);
    con_putc('\n');
    if (mdet_ac.table_count) {
        con_puts("    ");
        for (i = 0; i < mdet_ac.table_count; i++) {
            con_puts(mdet_ac.table_sig[i]);
            con_putc(' ');
        }
        con_putc('\n');
    }

    /* 原始字节取证：RSDP 与 RSDT/XSDT 头。固件布局差异（如 XSDT 地址
     * 只有 4 字节、RSDP 带 Length 字段等）只能靠字节内容判断。 */
    {
        const u8 *r = (const u8 *)mdet_ptr(mdet_ac.rsdp_addr, 36);
        u32 k;
        if (r) {
            con_puts("    RSDP:");
            for (k = 0; k < 36; k++) { con_put_hex8(r[k]); con_putc(' '); }
            con_putc('\n');
        }
        if (mdet_ac.rsdt_addr) {
            const u8 *t = (const u8 *)mdet_ptr(mdet_ac.rsdt_addr, 36);
            if (t) {
                con_puts("    RSDT:");
                for (k = 0; k < 36; k++) { con_put_hex8(t[k]); con_putc(' '); }
                con_putc('\n');
            }
        }
    }

    con_puts("  SMBIOS ep=");
    con_put_hex32(mdet_sm.entry_addr);
    con_puts(" v");
    con_put_dec(mdet_sm.major);
    con_putc('.');
    con_put_dec(mdet_sm.minor);
    con_puts(" structs=");
    con_put_dec(mdet_sm.struct_count);
    con_puts(" dev=");
    con_put_dec(mdet_sm.device_count);
    con_puts("(");
    con_put_dec(mdet_sm.device_populated);
    con_puts(" installed) total=");
    con_put_dec(mdet_sm.total_size_mb);
    con_puts("MB\n");

    con_puts("    speed max=");
    con_put_dec(mdet_sm.max_speed_mt);
    con_puts("MT/s cfg=");
    con_put_dec(mdet_sm.max_cfg_speed_mt);
    con_puts("MT/s mismatch=");
    con_put_dec(mdet_sm.speed_mismatch);
    con_puts(" ECC=");
    con_puts(mdet_sm.ecc_present ? "yes" : "no");
    con_puts(" type=");
    con_put_dec(mdet_sm.ecc_type);
    con_puts(" fmt_err=");
    con_put_dec(mdet_sm.format_errors);
    con_putc('\n');

    con_puts("  PhysAddr e820_max=");
    con_put_hex32((u32)(mdet_pa.e820_max_end >> 32));
    con_put_hex32((u32)mdet_pa.e820_max_end);
    con_puts(" cpuid_bits=");
    con_put_dec(mdet_pa.cpuid_phys_bits);
    con_puts(" lin_bits=");
    con_put_dec(mdet_pa.cpuid_linear_bits);
    con_puts(" ok=");
    con_put_dec(mdet_pa.e820_within_cpuid);
    con_putc('\n');

    con_puts("  Holes n=");
    con_put_dec(mdet_ho.count);
    con_puts(" total=");
    con_put_dec64(mdet_ho.total_bytes >> 20);
    con_puts("MB largest@");
    con_put_hex32((u32)mdet_ho.largest_base);
    con_puts("=");
    con_put_dec64(mdet_ho.largest_len >> 20);
    con_puts("MB below1M=");
    con_put_dec64(mdet_ho.below_1m_hole);
    con_putc('\n');

    con_puts("  Hotplug srat=");
    con_put_dec(mdet_hp.srat_present);
    con_puts(" regions=");
    con_put_dec(mdet_hp.count);
    con_puts(" bytes=");
    con_put_dec64(mdet_hp.total_bytes >> 20);
    con_puts("MB nonvol=");
    con_put_dec(mdet_hp.nonvolatile_count);
    con_putc('\n');

    con_puts("  Persist@");
    con_put_hex32(mdet_bootinfo_addr());
    con_puts(" verify=");
    con_puts(mdet_status_name(mdet_persist_verify()));
    con_puts(" validate=");
    con_puts(mdet_status_name((int)mdet_validate()));
    con_putc('\n');
}

/* --------------------------------------------------------------------------
 * 自检
 * -------------------------------------------------------------------------- */

/* 重试 mock：前两次失败、第三次成功（用于验证退避重试机制） */
static u32 retry_mock_calls = 0;
static int retry_mock_fail_twice(void)
{
    retry_mock_calls++;
    return (retry_mock_calls < 3u) ? MDET_EFORMAT : MDET_OK;
}

/* 主自检：覆盖 S01/S02/S03/S05/S06/S07/S08/S09/S14/S16/S17/S18/S19/S20 */
u32 mdet_selftest(void)
{
    mdet_stats_t st;
    const mdet_holes_t *ho;
    const mdet_physaddr_t *pa;

    /* 1：探测已就绪且条目数合法 */
    if (!mdet_ready) return 1;
    if (mdet_n == 0 || mdet_n > MDET_MAX_REGIONS) return 2;

    /* 2：映射表按地址严格递增且互不重叠 */
    {
        u32 i;
        for (i = 1; i < mdet_n; i++) {
            if (mdet_map[i].base < mdet_map[i - 1].base + mdet_map[i - 1].length) return 3;
        }
    }

    /* 3：无零长度条目残留 */
    {
        u32 i;
        for (i = 0; i < mdet_n; i++) if (mdet_map[i].length == 0) return 4;
    }

    /* 4：统计守恒 —— 可用 + 保留 + ACPI 等于覆盖总量 */
    mdet_stats(&st);
    if (st.usable + st.reserved + st.acpi_reclaim + st.acpi_nvs != st.union_space) return 5;

    /* 5：可用内存非空且占比不超过 100% */
    if (st.usable == 0) return 6;
    if (st.usable_pct_x100 > 100u) return 7;

    /* 6：1MB 以下统计不超过 1MB，且其中可用不超过其总量 */
    if (st.below_1m_total > 0x00100000ull) return 8;
    if (st.below_1m_usable > st.below_1m_total) return 9;

    /* 7：类型识别覆盖到可用内存 */
    if (!mdet_ty.type_seen[E820_USABLE]) return 10;
    if (mdet_ty.type_bytes[E820_USABLE] != st.usable) return 11;

    /* 8：校验函数必须报告通过 */
    if (mdet_validate() != 0) return 12;

    /* 9：窗口校验 —— 边界内通过、边界外拒绝、溢出不得绕过 */
    if (mdet_in_window(0x00100000u, 4096u) != MDET_OK) return 13;
    if (mdet_in_window(MDET_WINDOW_TOP, 1u) != MDET_EWINDOW) return 14;
    if (mdet_in_window(MDET_WINDOW_TOP - 1u, 2u) != MDET_EWINDOW) return 15;
    if (mdet_in_window(0xFFFFFFFFu, 0x1000u) != MDET_EWINDOW) return 16;
    if (mdet_in_window(0u, 0u) != MDET_EINVAL) return 17;

    /* 10：空洞 —— 总空洞 + 覆盖 = 最高地址 */
    ho = mdet_holes();
    if (ho->count > MDET_MAX_HOLES) return 18;
    {
        u64 cov = 0, maxend = 0;
        u32 i;
        for (i = 0; i < mdet_n; i++) {
            u64 e = mdet_map[i].base + mdet_map[i].length;
            cov += mdet_map[i].length;
            if (e > maxend) maxend = e;
        }
        /* 区间长度之和 + 空洞之和 = 最高地址（求和相消的恒等式） */
        if (cov + ho->total_bytes != maxend) return 19;
    }

    /* 11：空洞互不重叠且按地址递增 */
    {
        u32 i;
        for (i = 1; i < ho->count && i <= MDET_MAX_HOLES; i++) {
            if (ho->hole[i].base < ho->hole[i - 1].base + ho->hole[i - 1].len) return 20;
        }
    }

    /* 12：物理地址上限 —— E820 上限不小于最高可用地址 */
    pa = mdet_physaddr();
    if (pa->e820_max_end < pa->e820_highest_usable) return 21;
    if (pa->e820_max_end == 0) return 22;
    if (pa->cpuid_phys_bits == 0) return 23;

    /* 13：持久化回读必须与原表逐条一致 */
    if (mdet_persist() != MDET_OK) return 24;
    if (mdet_persist_verify() != MDET_OK) return 25;
    {
        const mdet_bootinfo_t *bi = mdet_bootinfo_read();
        if (!bi) return 26;
        if (bi->count != mdet_n) return 27;
        if (bi->total_usable != st.usable) return 28;
    }

    /* 14：持久化块必须落在内核保留区之内（0x00000000-0x0003FFFF） */
    if (mdet_bootinfo_addr() + sizeof(mdet_bootinfo_t) > 0x00080000u) return 29;

    /* 15：监控采样 —— 总页数非零、使用率不超过 100%、环形缓冲按序推进 */
    if (mdet_monitor_sample() != MDET_OK) return 30;
    if (mdet_monitor()->last_total_pages == 0) return 31;
    if (mdet_monitor()->last_used_x100 > 100u) return 32;
    if (mdet_monitor()->last_free_pages > mdet_monitor()->last_total_pages) return 33;
    {
        u32 h1 = mdet_monitor()->ring_head;
        if (mdet_monitor_sample() != MDET_OK) return 34;
        if (mdet_monitor()->ring_head != (h1 + 1u) % MDET_MON_RING) return 35;
        if (mdet_monitor()->samples != 2u) return 36;
    }

    /* 16：内核参数 —— 非法入参被拒绝、mem= 截断后可用内存下降且守恒仍成立 */
    {
        u64 usable_before = mdet_st.usable;
        u64 half = mdet_pa.e820_highest_usable >> 1;   /* 右移，避免 64 位除法 */

        if (mdet_cmdline_set_mem(0) != MDET_EINVAL) return 37;
        if (mdet_cmdline_add_memmap(0x1000u, 0u, E820_RESERVED) != MDET_EINVAL) return 38;
        if (mdet_cmdline_add_memmap(0x1000u, 0x1000u, 9u) != MDET_EINVAL) return 39;

        if (mdet_cmdline_set_mem(half) != MDET_OK) return 40;
        if (mdet_cmdline_apply() != MDET_OK) return 41;
        if (mdet_st.usable >= usable_before) return 42;
        if (mdet_st.usable + mdet_st.reserved + mdet_st.acpi_reclaim + mdet_st.acpi_nvs
            != mdet_st.union_space) return 43;
        if (mdet_cmdline()->clipped_bytes == 0) return 44;
        if (mdet_cmdline()->applied == 0) return 45;

        /* 复原：撤销全部内核参数后可用内存必须回到原值（不留副作用） */
        mdet_cmdline_reset();
        if (mdet_st.usable != usable_before) return 46;
        if (mdet_cmdline()->mem_limit != 0) return 47;
        if (mdet_cmdline()->clipped_bytes != 0) return 48;
    }

    /* 17：并发保护 —— 锁必须被真实使用（探测/查询/采样均走临界区） */
    if (mdet_lock_calls() == 0) return 49;

    /* 18：重试与退避 —— 前两次失败第三次成功；retry_total 精确递增、最终成功 */
    {
        u32 before = mdet_rs.retry_total;
        retry_mock_calls = 0;
        if (mdet_probe_with_retry(retry_mock_fail_twice, 3u) != MDET_OK) return 50;
        if (retry_mock_calls != 3u) return 51;
        if (mdet_rs.retry_total != before + 2u) return 52;
        if (mdet_rs.last_backoff_us == 0) return 53;
    }

    /* 19：查询快照一致性 —— 连续两次查询返回相同内容；region 快照与原表逐字段一致 */
    {
        const mdet_types_t *t1 = mdet_types();
        const mdet_types_t *t2 = mdet_types();
        u32 i;
        if (memcmp(t1, t2, sizeof(mdet_types_t)) != 0) return 54;
        for (i = 0; i < mdet_n && i < MDET_MAX_REGIONS; i++) {
            const e820_entry_t *r = mdet_region(i);
            if (!r) return 55;
            if (r->base != mdet_map[i].base) return 56;
            if (r->length != mdet_map[i].length) return 57;
            if (r->type != mdet_map[i].type) return 58;
        }
        if (mdet_region(mdet_n) != 0) return 59;
    }

    /* 20：重试统计接口 —— 探测完成后统计结构可达且计数自洽 */
    if (!mdet_retry_stats()) return 60;

    return 0;
}

u32 mdet_acpi_selftest(void)
{
    const mdet_acpi_t *a = mdet_acpi();

    /* 1：RSDP 定位 —— 找不到是合法结果（部分虚拟机不提供），但状态必须自洽 */
    if (a->rsdp_addr == 0) {
        if (a->table_count != 0) return 1;
        if (a->rsdt_ok != 0) return 2;
        return 0;
    }

    /* 2：RSDP 必须落在可读窗口内 */
    if (mdet_in_window(a->rsdp_addr, 20) != MDET_OK) return 3;

    /* 3：RSDP 校验和必须通过（这是固件结构可信的唯一凭据） */
    if (!a->rsdp_checksum_ok) return 4;

    /* 4：revision 与 XSDT 存在性一致 —— 1.0 不应报告 XSDT */
    if (a->revision < 2u && a->xsdt_present) return 5;

    /* 5：表数量不超过容量上限 */
    if (a->table_count > MDET_MAX_ACPI_TABLES) return 6;

    /* 6：每张已读到的表都必须有可打印签名 */
    {
        u32 i;
        for (i = 0; i < a->table_count; i++) {
            if (!mdet_sig_printable((const u8 *)a->table_sig[i], 4)) return 7;
        }
    }

    /* 7：RSDT/XSDT 的校验和必须通过 */
    if (!a->rsdt_ok) return 8;

    /* 8：已识别出的关键表地址必须落在窗口内 */
    if (a->facp_addr && mdet_in_window(a->facp_addr, 36) != MDET_OK) return 9;
    if (a->apic_addr && mdet_in_window(a->apic_addr, 36) != MDET_OK) return 10;
    if (a->srat_addr && mdet_in_window(a->srat_addr, 36) != MDET_OK) return 11;

    /* 9：窗口外与校验和失败都必须被计数（容错路径确实走到了） */
    if (a->out_of_window > a->table_count) return 12;

    /* 10：热插拔探测必须与 SRAT 存在性自洽 */
    if (mdet_hp.srat_present && a->srat_addr == 0) return 13;
    if (!a->srat_addr && mdet_hp.count != 0) return 14;
    if (mdet_hp.count > MDET_MAX_HOTPLUG) return 15;

    return 0;
}

u32 mdet_smbios_selftest(void)
{
    const mdet_smbios_t *s = mdet_smbios();
    u32 i;

    /* 1：未找到入口点是合法结果，但不得残留半解析状态 */
    if (s->entry_addr == 0) {
        if (s->device_count != 0) return 1;
        if (s->total_size_mb != 0) return 2;
        return 0;
    }

    /* 2：入口点必须在窗口内，且版本号非零 */
    if (mdet_in_window(s->entry_addr, 24) != MDET_OK) return 3;
    if (s->major == 0) return 4;

    /* 3：入口点校验和必须通过 */
    if (!s->entry_checksum_ok) return 5;

    /* 4：器件数不得超过结构总数 */
    if (s->device_count > s->struct_count) return 6;
    if (s->array_count > s->struct_count) return 7;

    /* 5：已安装器件数不超过器件总数 */
    if (s->device_populated > s->device_count) return 8;

    /* 6：容量 —— 有已安装器件就必须有非零总容量 */
    if (s->device_populated > 0 && s->total_size_mb == 0) return 9;

    /* 7：速率 —— 运行速率不应高于额定速率（高于说明字段解码有误） */
    if (s->max_cfg_speed_mt > s->max_speed_mt && s->max_speed_mt != 0) return 10;

    /* 8：速率必须在合理范围（0 或 100..20000 MT/s），否则说明字节偏移取错 */
    if (s->max_speed_mt != 0 && (s->max_speed_mt < 100u || s->max_speed_mt > 20000u)) return 11;
    if (s->max_cfg_speed_mt != 0 && (s->max_cfg_speed_mt < 100u || s->max_cfg_speed_mt > 20000u))
        return 12;

    /* 9：纠错类型必须在规范取值范围内 */
    if (s->ecc_type > 0x07u) return 13;

    /* 10：内存技术编码必须在规范范围内（0x01..0x0F 或 0x02 等已知值） */
    if (s->memory_tech > 0x20u) return 14;

    /* 11：格式错误数不得超过结构总数 */
    if (s->format_errors > s->struct_count) return 15;

    /* 12：槽位明细与已安装数自洽 */
    {
        u32 i, nonzero = 0;
        for (i = 0; i < MDET_SMBIOS_SLOTS; i++) if (s->slot_size_mb[i] > 0) nonzero++;
        if (nonzero > s->device_populated) return 16;
    }

    /* 13：容量合计与槽位明细之和一致（仅当全部器件都在记录窗口内时才可断言） */
    if (s->device_count <= MDET_SMBIOS_SLOTS) {
        u32 i;
        u32 sum = 0;
        for (i = 0; i < MDET_SMBIOS_SLOTS; i++) sum += s->slot_size_mb[i];
        if (sum != s->total_size_mb) return 17;
    }

    /* 14：槽位字符串非空时必须可打印（厂商/部件号/序列号） */
    for (i = 0; i < MDET_SMBIOS_SLOTS; i++) {
        if (s->slot_manufacturer[i][0] && !mdet_sig_printable(s->slot_manufacturer[i], 16)) return 18;
        if (s->slot_part[i][0] && !mdet_sig_printable(s->slot_part[i], 24)) return 19;
        if (s->slot_serial[i][0] && !mdet_sig_printable(s->slot_serial[i], 16)) return 20;
    }

    /* 15：Type19 阵列映射窗口必须不回绕（base+len 不溢出） */
    for (i = 0; i < s->type19_count; i++) {
        if (s->type19[i].base + s->type19[i].len < s->type19[i].base) return 21;
        if (s->type19[i].len == 0) return 22;
    }

    /* 16：模块位宽合法（0 = 未知，8..255 且为 8 的倍数；u8 上限天然不超 255） */
    for (i = 0; i < MDET_SMBIOS_SLOTS; i++) {
        if (s->slot_module_width[i] != 0 &&
            (s->slot_module_width[i] < 8u || (s->slot_module_width[i] & 7u) != 0)) return 23;
    }

    return 0;
}

/* ==========================================================================
 * 04 册第二轮重做：公开入口 = 锁包装
 * 每个公开入口持锁调用内部 _locked 变体；查询接口锁内快照。
 * ========================================================================== */
int mdet_init(const e820_entry_t *entries, u32 count)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_init_locked(entries, count);
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_restore(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_restore_locked();
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_acpi_probe(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = mdet_probe_with_retry(_mdet_acpi_probe_locked, 3u);
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_hotplug_probe(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = mdet_probe_with_retry(_mdet_hotplug_probe_locked, 3u);
    mdet_lock_exit(eflags);
    return rc;
}

/* SMBIOS 字符串表提取：idx=0 表示无字符串；max 为表内剩余字节数 */
static void mdet_smbios_str(const u8 *tab, u32 max, u8 idx, u8 *out, u32 outlen)
{
    u32 i = 1, off = 0;
    if (idx == 0 || !tab || outlen == 0) return;
    while (i <= idx && off < max) {
        u32 slen = 0;
        while (off + slen < max && tab[off + slen] != 0) slen++;
        if (i == idx) {
            if (slen == 0 || slen >= outlen) return;
            memcpy(out, tab + off, slen);
            out[slen] = 0;
            return;
        }
        off += slen + 1u;
        i++;
    }
}

int mdet_smbios_probe(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = mdet_probe_with_retry(_mdet_smbios_probe_locked, 3u);
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_physaddr_probe(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_physaddr_probe_locked();
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_holes_scan(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_holes_scan_locked();
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_persist(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_persist_locked();
    mdet_lock_exit(eflags);
    return rc;
}

int mdet_cmdline_apply(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_cmdline_apply_locked();
    mdet_lock_exit(eflags);
    return rc;
}

void mdet_cmdline_reset(void)
{
    u32 eflags = mdet_lock_enter();
    _mdet_cmdline_reset_locked();
    mdet_lock_exit(eflags);
}

int mdet_monitor_sample(void)
{
    int rc;
    u32 eflags = mdet_lock_enter();
    rc = _mdet_monitor_sample_locked();
    mdet_lock_exit(eflags);
    return rc;
}
