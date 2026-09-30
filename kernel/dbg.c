/* XOS 调试与监控 —— 串口输出 / 日志缓冲 / 级别过滤 / 符号与栈回溯 / kgdb 断点 */
#include "dbg.h"
#include "console.h"

typedef struct {
    u32   level;
    char  line[DBG_LOG_LINE_LEN];
} dbg_log_t;

static dbg_log_t g_log[DBG_LOG_ENTRIES];
static u32       g_log_wr;
static u32       g_log_count;
static u32       g_com_sent;
static u32       g_cur_level;   /* 当前过滤级别 */

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
    for (i = 0u; i < DBG_LOG_ENTRIES; i++) { g_log[i].level = 0u; g_log[i].line[0] = 0; }
    g_log_wr = 0u; g_log_count = 0u; g_com_sent = 0u; g_cur_level = DBG_LEVEL_DEBUG;
    for (i = 0u; i < DBG_MAX_SYMS; i++) { g_syms[i].name[0] = 0; g_syms[i].defined = 0u; }
    for (i = 0u; i < DBG_MAX_BP; i++) { g_bp[i].name[0] = 0; g_bp[i].hits = 0u; g_bp[i].enabled = 0u; }
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

    return 0;
}
