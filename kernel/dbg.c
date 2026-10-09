/* XOS 调试与监控 —— 串口输出 / 日志缓冲 / 级别过滤 / 符号与栈回溯 / kgdb 断点
 * V2：日志时间戳 + 磁盘持久化（关机落盘、启动恢复、dmesg 查看） */
#include "dbg.h"
#include "console.h"
#include "irq.h"       /* pit_tick_count：日志时间戳 */
#include "disk.h"      /* disk_read/write_sectors：日志落盘 */

typedef struct {
    u32   level;
    u32   ticks;
    char  line[DBG_LOG_LINE_LEN];
} dbg_log_t;

static dbg_log_t g_log[DBG_LOG_ENTRIES];
static u32       g_log_wr;
static u32       g_log_count;
static u32       g_com_sent;
static u32       g_cur_level;   /* 当前过滤级别 */

/* 持久化恢复缓存：上次关机落盘的日志快照（供 dmesg 展示） */
static dbg_plog_t g_prev;
static u32        g_prev_valid;

/* 级别名 */
static const char *dbg_level_name(u32 level)
{
    switch (level) {
    case DBG_LEVEL_DEBUG: return "dbg";
    case DBG_LEVEL_INFO:  return "inf";
    case DBG_LEVEL_WARN:  return "wrn";
    case DBG_LEVEL_ERROR: return "err";
    default:              return "???";
    }
}

typedef struct {
    char  name[DBG_SYM_LEN];
    u32   addr;
    u32   defined;
} dbg_sym_t;

static dbg_sym_t g_syms[DBG_MAX_SYMS];

typedef struct {
    char  name[DBG_BP_LEN];
    u32   hits;
    u32   enabled;
} dbg_bp_t;

static dbg_bp_t g_bp[DBG_MAX_BP];

static int dbg_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)(*a) - (u8)(*b);
}

static void dbg_strncpy(char *d, const char *s, u32 n)
{
    u32 i;
    for (i = 0u; i < n && s[i]; i++) d[i] = s[i];
    if (n > 0u) d[i < n ? i : n - 1u] = 0;
}

void dbg_init(void)
{
    u32 i;
    for (i = 0u; i < DBG_LOG_ENTRIES; i++) { g_log[i].level = 0u; g_log[i].ticks = 0u; g_log[i].line[0] = 0; }
    g_log_wr = 0u; g_log_count = 0u; g_com_sent = 0u; g_cur_level = DBG_LEVEL_DEBUG;
    for (i = 0u; i < DBG_MAX_SYMS; i++) { g_syms[i].name[0] = 0; g_syms[i].defined = 0u; }
    for (i = 0u; i < DBG_MAX_BP; i++) { g_bp[i].name[0] = 0; g_bp[i].hits = 0u; g_bp[i].enabled = 0u; }
    g_prev_valid = 0u;
    g_prev.magic = 0u;
    g_prev.version = 0u;
    g_prev.seq = 0u;
    g_prev.count = 0u;
    g_prev.ticks = 0u;
    for (i = 0u; i < DBG_PLOG_MAX; i++) {
        g_prev.ent[i].ticks = 0u; g_prev.ent[i].level = 0u; g_prev.ent[i].line[0] = 0;
    }
}

/* ---------- 串口调试输出（COM1 语义模拟） ---------- */

int dbg_com_write(const u8 *buf, u32 len)
{
    u32 i;
    for (i = 0u; i < len; i++) g_com_sent++;
    return 0;
}

u32 dbg_com_sent(void) { return g_com_sent; }

/* ---------- 日志缓冲与级别过滤 ---------- */

int dbg_log_add(u32 level, const char *line)
{
    u32 i;
    if (level < g_cur_level) return 1;   /* 被过滤 */
    i = g_log_wr;
    g_log[i].level = level;
    g_log[i].ticks = pit_tick_count();
    dbg_strncpy(g_log[i].line, line, DBG_LOG_LINE_LEN - 1u);
    g_log_wr = (g_log_wr + 1u) % DBG_LOG_ENTRIES;
    if (g_log_count < DBG_LOG_ENTRIES) g_log_count++;
    return 0;
}

u32 dbg_log_count(void) { return g_log_count; }

int dbg_log_dump(char *out, u32 max)
{
    u32 last;
    if (g_log_count == 0u) return -1;
    last = (g_log_wr + DBG_LOG_ENTRIES - 1u) % DBG_LOG_ENTRIES;
    dbg_strncpy(out, g_log[last].line, max);
    return 0;
}

u32 dbg_level_filter(u32 level)
{
    return (level < g_cur_level) ? 1u : 0u;
}

/* ---------- 符号与栈回溯 ---------- */

int dbg_sym_add(const char *name, u32 addr)
{
    u32 i, free_slot = DBG_MAX_SYMS;
    for (i = 0u; i < DBG_MAX_SYMS; i++) {
        if (g_syms[i].defined && dbg_strcmp(g_syms[i].name, name) == 0) return -1;
        if (!g_syms[i].defined && free_slot == DBG_MAX_SYMS) free_slot = i;
    }
    if (free_slot == DBG_MAX_SYMS) return -2;
    dbg_strncpy(g_syms[free_slot].name, name, DBG_SYM_LEN - 1u);
    g_syms[free_slot].addr = addr;
    g_syms[free_slot].defined = 1u;
    return 0;
}

int dbg_sym_lookup(const char *name, u32 *addr)
{
    u32 i;
    for (i = 0u; i < DBG_MAX_SYMS; i++)
        if (g_syms[i].defined && dbg_strcmp(g_syms[i].name, name) == 0) {
            if (addr) *addr = g_syms[i].addr;
            return 0;
        }
    return -1;
}

int dbg_backtrace(const u32 *fp_chain, u32 depth, u32 *out, u32 max)
{
    u32 i, n;
    if (depth > max) depth = max;
    n = 0u;
    for (i = 0u; i < depth; i++) {
        if (fp_chain[i] == 0u) break;
        out[n++] = fp_chain[i];
    }
    return (int)n;
}

/* ---------- kgdb 断点 ---------- */

int dbg_bp_set(const char *name)
{
    u32 i, free_slot = DBG_MAX_BP;
    for (i = 0u; i < DBG_MAX_BP; i++) {
        if (g_bp[i].enabled && dbg_strcmp(g_bp[i].name, name) == 0) {
            g_bp[i].hits = 0u;
            return 0;
        }
        if (!g_bp[i].enabled && free_slot == DBG_MAX_BP) free_slot = i;
    }
    if (free_slot == DBG_MAX_BP) return -2;
    dbg_strncpy(g_bp[free_slot].name, name, DBG_BP_LEN - 1u);
    g_bp[free_slot].hits = 0u;
    g_bp[free_slot].enabled = 1u;
    return 0;
}

int dbg_bp_hit(const char *name)
{
    u32 i;
    for (i = 0u; i < DBG_MAX_BP; i++)
        if (g_bp[i].enabled && dbg_strcmp(g_bp[i].name, name) == 0) {
            g_bp[i].hits++;
            return 0;
        }
    return -1;
}

u32 dbg_bp_count(void)
{
    u32 i, c = 0u;
    for (i = 0u; i < DBG_MAX_BP; i++)
        if (g_bp[i].enabled) c++;
    return c;
}

/* ============================================================================
 * 日志磁盘持久化（V2，自研）
 * 磁盘末尾固定区（末尾 DBG_PLOG_SECTS 扇区）：关机/重启前序列化当前缓冲落盘，
 * 启动恢复并缓存，dmesg 命令统一展示。不依赖文件系统（Linux pstore 语义）。
 * ========================================================================== */

/* 日志持久化区：固定位于镜像安全区（LBA 7201 = 内核主/备副本 4096+4096 扇区
 * 预留之后、镜像末尾之前；kernel.bin 上限 0x90000 → 主副本最远 ~LBA 1164，
 * 备副本最远 ~LBA 4237，7201 起 16 扇区恒为空闲） */
#define DBG_PLOG_LBA 7201u

static u32 dbg_plog_lba(void)
{
    /* 磁盘容量需覆盖日志区（探测值为 65536 扇区/32MB，足够） */
    u32 total = disk_capacity_lba(0u);
    if (total < DBG_PLOG_LBA + DBG_PLOG_SECTS) return 0u;
    return DBG_PLOG_LBA;
}

/* 序列化当前环形缓冲（时间序）到持久化结构 */
static void dbg_plog_snapshot(dbg_plog_t *p)
{
    u32 n, i;
    n = (g_log_count < DBG_PLOG_MAX) ? g_log_count : DBG_PLOG_MAX;
    p->magic = DBG_PLOG_MAGIC;
    p->version = DBG_PLOG_VERSION;
    p->seq = p->seq + 1u;
    p->count = n;
    p->ticks = pit_tick_count();
    /* 环形序：从最旧（g_log_wr - count）开始取 n 条 */
    for (i = 0u; i < n; i++) {
        u32 idx = (g_log_wr + DBG_LOG_ENTRIES - n + i) % DBG_LOG_ENTRIES;
        p->ent[i].ticks = g_log[idx].ticks;
        p->ent[i].level = g_log[idx].level;
        dbg_strncpy(p->ent[i].line, g_log[idx].line, DBG_LOG_LINE_LEN - 1u);
    }
}

/* 落盘：写磁盘末尾区（reboot/poweroff 前调用） */
int dbg_log_persist(void)
{
    static dbg_plog_t p;                  /* 大缓冲静态化，避免 __chkstk_ms */
    static u8 raw[DBG_PLOG_SECTS * 512u];
    u32 lba = dbg_plog_lba();

    if (lba == 0u) return -1;
    memset(&p, 0, sizeof(p));
    dbg_plog_snapshot(&p);
    if (p.count == 0u) return 0;   /* 无可持久化内容 */
    memset(raw, 0, sizeof(raw));
    if (sizeof(p) > sizeof(raw)) return -2;   /* 结构超区，拒绝 */
    memcpy(raw, &p, sizeof(p));
    {
        int rc = disk_write_sectors(0u, lba, DBG_PLOG_SECTS, raw);
        con_puts("  [plog] persist lba=");
        con_put_dec(lba);
        con_puts(" count=");
        con_put_dec(p.count);
        con_puts(" rc=");
        con_put_dec(rc);
        con_putc('\n');
        return rc;
    }
}

/* 恢复：启动读回上次落盘日志，打印摘要并缓存供 dmesg */
int dbg_log_restore(void)
{
    static dbg_plog_t p;                  /* 大缓冲静态化 */
    static u8 raw[DBG_PLOG_SECTS * 512u];
    u32 lba = dbg_plog_lba();
    u32 i;
    int rc;

    if (lba == 0u) return -1;
    rc = disk_read_sectors(0u, lba, DBG_PLOG_SECTS, raw);
    if (rc != 0) {
        con_puts("  [plog] restore read rc=");
        con_put_dec(rc);
        con_putc('\n');
        return -1;
    }
    memcpy(&p, raw, sizeof(p));
    if (p.magic != DBG_PLOG_MAGIC || p.version != DBG_PLOG_VERSION) {
        con_puts("  [plog] restore bad magic=");
        con_put_hex32(p.magic);
        con_putc('\n');
        return -1;
    }
    if (p.count > DBG_PLOG_MAX) return -1;

    /* 缓存到 g_prev 供 dmesg */
    memcpy(&g_prev, &p, sizeof(g_prev));
    g_prev_valid = 1u;

    con_set_color(VGA_YELLOW, VGA_BLACK);
    con_puts("\n  Previous boot log recovered (seq=");
    con_put_dec(p.seq);
    con_puts(", entries=");
    con_put_dec(p.count);
    con_puts(", ticks=");
    con_put_dec(p.ticks);
    con_puts(")\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    for (i = 0u; i < p.count && i < 4u; i++) {
        con_puts("    [");
        con_puts(dbg_level_name(p.ent[i].level));
        con_puts("] ");
        con_puts(p.ent[i].line);
        con_putc('\n');
    }
    if (p.count > 4u) {
        con_puts("    ... (");
        con_put_dec(p.count - 4u);
        con_puts(" more, use 'dmesg' to view all)\n");
    }
    con_flush();
    return 0;
}

/* dmesg：级别名 + 时间戳；先显示上次恢复日志，再显示当前缓冲 */
void dbg_log_show(void)
{
    u32 i, n;

    if (g_prev_valid && g_prev.count > 0u) {
        con_set_color(VGA_YELLOW, VGA_BLACK);
        con_puts("  --- previous boot (seq ");
        con_put_dec(g_prev.seq);
        con_puts(") ---\n");
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        for (i = 0u; i < g_prev.count; i++) {
            con_puts("  [");
            con_puts(dbg_level_name(g_prev.ent[i].level));
            con_puts("] t=");
            con_put_dec(g_prev.ent[i].ticks);
            con_puts("  ");
            con_puts(g_prev.ent[i].line);
            con_putc('\n');
        }
    }

    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  --- current boot ---\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    n = g_log_count;
    for (i = 0u; i < n; i++) {
        u32 idx = (g_log_wr + DBG_LOG_ENTRIES - n + i) % DBG_LOG_ENTRIES;
        con_puts("  [");
        con_puts(dbg_level_name(g_log[idx].level));
        con_puts("] t=");
        con_put_dec(g_log[idx].ticks);
        con_puts("  ");
        con_puts(g_log[idx].line);
        con_putc('\n');
    }
    if (n == 0u && (!g_prev_valid || g_prev.count == 0u))
        con_puts("  (log empty)\n");
}

/* 持久化自检：序列化往返（内存模拟），不真实写盘以免污染下次启动 */
int dbg_selftest_plog(void)
{
    static dbg_plog_t p1, p2;             /* 大缓冲静态化 */
    static u8 raw[DBG_PLOG_SECTS * 512u];
    static u8 save[sizeof(g_log)];        /* 活日志现场快照（自检会污染环形缓冲） */
    u32 i;
    u32 saved_wr = g_log_wr, saved_count = g_log_count;
    u32 saved_prev = g_prev_valid;

    /* 构造两条日志 → 快照 → 序列化 → 反序列化 → 校验 */
    memcpy(save, g_log, sizeof(g_log));   /* 先留底 */
    g_log_wr = 0u; g_log_count = 0u;
    (void)dbg_log_add(DBG_LEVEL_INFO, "boot ok");
    (void)dbg_log_add(DBG_LEVEL_ERROR, "oops");
    memset(&p1, 0, sizeof(p1));
    dbg_plog_snapshot(&p1);
    if (p1.magic != DBG_PLOG_MAGIC) return 1;
    if (p1.count != 2u) return 2;
    if (p1.ent[0].level != DBG_LEVEL_INFO) return 3;
    if (p1.ent[1].level != DBG_LEVEL_ERROR) return 4;
    if (dbg_strcmp(p1.ent[1].line, "oops") != 0) return 5;
    if (p1.seq != 1u) return 6;               /* 首次快照 seq 从 0 → 1 */

    /* 序列化往返 */
    memset(raw, 0, sizeof(raw));
    if (sizeof(p1) > sizeof(raw)) return 7;
    memcpy(raw, &p1, sizeof(p1));
    memcpy(&p2, raw, sizeof(p2));
    if (p2.magic != DBG_PLOG_MAGIC || p2.version != DBG_PLOG_VERSION) return 8;
    if (p2.count != 2u) return 9;
    if (dbg_strcmp(p2.ent[0].line, "boot ok") != 0) return 10;
    if (dbg_strcmp(p2.ent[1].line, "oops") != 0) return 11;
    if (p2.seq != p1.seq) return 12;

    /* 边界：count 上限裁剪 */
    for (i = 0u; i < DBG_PLOG_MAX + 4u; i++) {
        char ln[16];
        ln[0] = 'P'; ln[1] = (char)('0' + (i % 10u)); ln[2] = 0;
        (void)dbg_log_add(DBG_LEVEL_WARN, ln);
    }
    memset(&p1, 0, sizeof(p1));
    dbg_plog_snapshot(&p1);
    if (p1.count != DBG_LOG_ENTRIES) return 13;   /* 环形缓冲深度上限 */

    /* 恢复现场（含环形缓冲内容，不留自检残留） */
    g_log_wr = saved_wr; g_log_count = saved_count;
    g_prev_valid = saved_prev;
    memcpy(g_log, save, sizeof(g_log));
    return 0;
}

/* ---------- 自检 ---------- */

int dbg_selftest(void)
{
    char buf[DBG_LOG_LINE_LEN];
    u32 addr, out[4];
    const u32 fp[4] = { 0x10000u, 0x10020u, 0x10040u, 0u };
    u8 bytes[3] = { 0xAAu, 0xBBu, 0xCCu };
    int n;

    /* 1-3: 串口输出 */
    if (dbg_com_write(bytes, 3u) != 0) return 1;
    if (dbg_com_sent() != 3u) return 2;
    if (dbg_com_write(bytes, 2u) != 0) return 3;
    if (dbg_com_sent() != 5u) return 4;

    /* 5-7: 日志过滤 */
    if (dbg_log_add(DBG_LEVEL_ERROR, "oops") != 0) return 5;
    if (dbg_log_add(DBG_LEVEL_INFO, "boot ok") != 0) return 6;
    if (dbg_log_count() != 2u) return 7;
    if (dbg_log_dump(buf, sizeof(buf)) != 0) return 8;
    if (dbg_strcmp(buf, "boot ok") != 0) return 9;

    /* 10-11: 级别过滤 */
    if (dbg_level_filter(DBG_LEVEL_DEBUG) != 0u) return 10;
    g_cur_level = DBG_LEVEL_WARN;
    if (dbg_level_filter(DBG_LEVEL_INFO) != 1u) return 11;
    if (dbg_level_filter(DBG_LEVEL_ERROR) != 0u) return 12;
    if (dbg_log_add(DBG_LEVEL_DEBUG, "hidden") != 1) return 13;
    if (dbg_log_count() != 2u) return 14;
    g_cur_level = DBG_LEVEL_DEBUG;

    /* 15-18: 符号 */
    if (dbg_sym_add("kmain", 0x10000u) != 0) return 15;
    if (dbg_sym_add("kmain", 0x20000u) != -1) return 16;
    if (dbg_sym_lookup("kmain", &addr) != 0) return 17;
    if (addr != 0x10000u) return 18;
    if (dbg_sym_lookup("nope", &addr) != -1) return 19;

    /* 20-21: 栈回溯 */
    n = dbg_backtrace(fp, 4u, out, 4u);
    if (n != 3) return 20;
    if (out[0] != 0x10000u || out[2] != 0x10040u) return 21;

    /* 22-24: kgdb 断点 */
    if (dbg_bp_set("boot") != 0) return 22;
    if (dbg_bp_set("fault") != 0) return 23;
    if (dbg_bp_count() != 2u) return 24;
    if (dbg_bp_hit("boot") != 0) return 25;
    if (dbg_bp_hit("boot") != 0) return 26;
    if (dbg_bp_hit("missing") != -1) return 27;

    /* 28-30: 符号表满 */
    {
        u32 k;
        for (k = 0u; k < DBG_MAX_SYMS; k++) {
            char nm[8];
            nm[0] = 's'; nm[1] = (char)('a' + (k & 15u)); nm[2] = 0;
            (void)dbg_sym_add(nm, 0x1000u + k);
        }
        if (dbg_sym_add("overflow", 0x1u) != -2) return 28;
    }
    if (dbg_sym_lookup("kmain", &addr) != 0) return 29;
    if (addr != 0x10000u) return 30;

    /* 31-33: 断点表满 */
    {
        u32 k;
        for (k = 0u; k < DBG_MAX_BP; k++) {
            char nm[8];
            nm[0] = 'b'; nm[1] = (char)('0' + k); nm[2] = 0;
            (void)dbg_bp_set(nm);
        }
        if (dbg_bp_set("full") != -2) return 31;
    }
    if (dbg_bp_set("boot") != 0) return 32;   /* 重装不占槽 */
    if (dbg_bp_hit("boot") != 0) return 33;
    if (dbg_bp_count() != DBG_MAX_BP) return 34;

    /* 35-37: 日志环形回绕 */
    {
        u32 k;
        for (k = 0u; k < DBG_LOG_ENTRIES + 4u; k++) {
            char ln[16];
            ln[0] = 'L'; ln[1] = (char)('0' + (k % 10u)); ln[2] = 0;
            (void)dbg_log_add(DBG_LEVEL_INFO, ln);
        }
        if (dbg_log_count() != DBG_LOG_ENTRIES) return 35;
        if (dbg_log_dump(buf, sizeof(buf)) != 0) return 36;
        if (dbg_strcmp(buf, "L5") != 0) return 37;   /* 最近写入 k=35 -> L5 */
    }

    /* 38-40: 边界 */
    if (dbg_log_dump(buf, sizeof(buf)) != 0) return 38;
    if (dbg_backtrace(fp, 4u, out, 2u) != 2) return 39;
    if (dbg_bp_count() == 0u) return 40;

    /* 41-42: 日志持久化往返（V2） */
    {
        int prc = dbg_selftest_plog();
        if (prc != 0) {
            con_puts("  [plog] self-test rc=");
            con_put_dec(prc);
            con_puts(" (count=");
            con_put_dec(g_log_count);
            con_puts(" wr=");
            con_put_dec(g_log_wr);
            con_puts(")\n");
            return 41;
        }
    }
    if (g_prev_valid != 0u) return 42;   /* 自检不应污染恢复缓存 */

    return 0;
}
