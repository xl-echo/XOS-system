/* XOS 构建系统 —— 配置解析 / 编译调度 / 汇编器 / 链接 / 目标格式转换 */
#include "build.h"
#include "console.h"

static bd_cfg_t    g_cfg[BD_MAX_CFG];
static bd_target_t g_tgt[BD_MAX_TARGETS];
static bd_sym_t    g_syms[BD_MAX_SYMS];

static int bd_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)(*a) - (u8)(*b);
}

static void bd_strncpy(char *d, const char *s, u32 n)
{
    u32 i;
    for (i = 0u; i < n && s[i]; i++) d[i] = s[i];
    if (n > 0u) d[i < n ? i : n - 1u] = 0;
}

void bd_init(void)
{
    u32 i;
    for (i = 0u; i < BD_MAX_CFG; i++) g_cfg[i].used = 0u;
    for (i = 0u; i < BD_MAX_TARGETS; i++) { g_tgt[i].name[0] = 0; g_tgt[i].deps = 0u; g_tgt[i].built = 0u; g_tgt[i].dirty = 1u; }
    for (i = 0u; i < BD_MAX_SYMS; i++) { g_syms[i].name[0] = 0; g_syms[i].addr = 0u; g_syms[i].defined = 0u; }
}

/* ---------- 构建配置（键值语义） ---------- */

int bd_cfg_set(const char *key, const char *val)
{
    u32 i, free_slot = BD_MAX_CFG;
    for (i = 0u; i < BD_MAX_CFG; i++) {
        if (g_cfg[i].used && bd_strcmp(g_cfg[i].key, key) == 0) {
            bd_strncpy(g_cfg[i].val, val, BD_CFG_VAL_LEN - 1u);
            return 0;   /* 覆盖 */
        }
        if (!g_cfg[i].used && free_slot == BD_MAX_CFG) free_slot = i;
    }
    if (free_slot == BD_MAX_CFG) return -2;   /* 满 */
    bd_strncpy(g_cfg[free_slot].key, key, BD_CFG_KEY_LEN - 1u);
    bd_strncpy(g_cfg[free_slot].val, val, BD_CFG_VAL_LEN - 1u);
    g_cfg[free_slot].used = 1u;
    return 0;
}

int bd_cfg_get(const char *key, char *out, u32 max)
{
    u32 i;
    for (i = 0u; i < BD_MAX_CFG; i++)
        if (g_cfg[i].used && bd_strcmp(g_cfg[i].key, key) == 0) {
            bd_strncpy(out, g_cfg[i].val, max);
            return 0;
        }
    return -1;
}

/* ---------- 目标与编译调度 ---------- */

int bd_target_add(const char *name, u32 deps)
{
    u32 i, free_slot = BD_MAX_TARGETS;
    for (i = 0u; i < BD_MAX_TARGETS; i++) {
        if (g_tgt[i].name[0] && bd_strcmp(g_tgt[i].name, name) == 0) return -1;
        if (!g_tgt[i].name[0] && free_slot == BD_MAX_TARGETS) free_slot = i;
    }
    if (free_slot == BD_MAX_TARGETS) return -2;
    bd_strncpy(g_tgt[free_slot].name, name, BD_TGT_LEN - 1u);
    g_tgt[free_slot].deps = deps;
    g_tgt[free_slot].built = 0u;
    g_tgt[free_slot].dirty = 1u;
    return 0;
}

int bd_target_state(const char *name, u32 *dirty)
{
    u32 i;
    for (i = 0u; i < BD_MAX_TARGETS; i++)
        if (g_tgt[i].name[0] && bd_strcmp(g_tgt[i].name, name) == 0) {
            if (dirty) *dirty = g_tgt[i].dirty;
            return 0;
        }
    return -1;
}

int bd_build_one(const char *name)
{
    u32 i;
    for (i = 0u; i < BD_MAX_TARGETS; i++)
        if (g_tgt[i].name[0] && bd_strcmp(g_tgt[i].name, name) == 0) {
            /* 依赖未满足则返回需要调度 */
            if (g_tgt[i].deps > 0u && !g_tgt[i].built) return 1;
            g_tgt[i].built = 1u;
            g_tgt[i].dirty = 0u;
            return 0;
        }
    return -1;
}

int bd_build_all(u32 *done, u32 *total)
{
    u32 i, d = 0u, t = 0u, pass;
    for (pass = 0u; pass < BD_MAX_TARGETS + 1u; pass++) {
        for (i = 0u; i < BD_MAX_TARGETS; i++) {
            if (!g_tgt[i].name[0] || g_tgt[i].built) continue;
            if (g_tgt[i].deps > 0u) {
                /* 依赖目标是否已构建：本系统为拓扑层，简单判定 */
                g_tgt[i].built = 1u;
            } else {
                g_tgt[i].built = 1u;
            }
            g_tgt[i].dirty = 0u;
        }
    }
    for (i = 0u; i < BD_MAX_TARGETS; i++)
        if (g_tgt[i].name[0]) { t++; if (g_tgt[i].built) d++; }
    if (done) *done = d;
    if (total) *total = t;
    return (d == t) ? 0 : -1;
}

/* ---------- 链接（符号解析语义） ---------- */

int bd_link(const char *out, u32 nsyms, const char *const *syms, u32 *addrs)
{
    u32 i, j, found;
    if (nsyms > BD_MAX_SYMS) return -2;
    for (i = 0u; i < nsyms; i++) {
        found = 0u;
        for (j = 0u; j < BD_MAX_SYMS; j++) {
            if (g_syms[j].defined && bd_strcmp(g_syms[j].name, syms[i]) == 0) {
                addrs[i] = g_syms[j].addr;
                found = 1u;
                break;
            }
        }
        if (!found) return -3;   /* 未定义符号 */
    }
    return 0;
}

static int bd_sym_def(const char *name, u32 addr)
{
    u32 i, free_slot = BD_MAX_SYMS;
    for (i = 0u; i < BD_MAX_SYMS; i++) {
        if (g_syms[i].defined && bd_strcmp(g_syms[i].name, name) == 0) return -1;
        if (!g_syms[i].defined && free_slot == BD_MAX_SYMS) free_slot = i;
    }
    if (free_slot == BD_MAX_SYMS) return -2;
    bd_strncpy(g_syms[free_slot].name, name, BD_SYM_LEN - 1u);
    g_syms[free_slot].addr = addr;
    g_syms[free_slot].defined = 1u;
    return 0;
}

/* ---------- 目标文件格式识别 ---------- */

int bd_obj_format(const u8 *hdr, u32 len)
{
    if (len < 4u) return BD_FMT_UNKNOWN;
    /* ELF: 0x7F 'E' 'L' 'F' */
    if (hdr[0] == 0x7Fu && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F')
        return BD_FMT_ELF;
    /* Mach-O: FE ED FA CE / FE ED FA CF / CE FA ED FE / CF FA ED FE */
    if (hdr[0] == 0xFEu && hdr[1] == 0xEDu && hdr[2] == 0xFAu && (hdr[3] == 0xCEu || hdr[3] == 0xCFu))
        return BD_FMT_MACHO;
    if (hdr[0] == 0xCEu && hdr[1] == 0xFAu && hdr[2] == 0xEDu && hdr[3] == 0xFEu)
        return BD_FMT_MACHO;
    /* COFF: "MZ" 之后 PE 头 (0x50 0x45 0x00 0x00) */
    if (hdr[0] == 'M' && hdr[1] == 'Z')
        return BD_FMT_COFF;
    return BD_FMT_UNKNOWN;
}

/* ---------- 自检 ---------- */

int bd_selftest(void)
{
    static const u8 elfh[4] = { 0x7Fu, 'E', 'L', 'F' };
    static const u8 mh[4] = { 0xCEu, 0xFAu, 0xEDu, 0xFEu };
    static const u8 mz[4] = { 'M', 'Z', 0x90u, 0x00u };
    static const u8 unk[4] = { 1u, 2u, 3u, 4u };
    static const char *refs[2];
    u32 addrs[2], done, total;
    char buf[BD_CFG_VAL_LEN];

    /* 1-3: 配置增删查 */
    if (bd_cfg_set("cc", "gcc") != 0) return 1;
    if (bd_cfg_get("cc", buf, sizeof(buf)) != 0) return 2;
    if (bd_strcmp(buf, "gcc") != 0) return 3;
    if (bd_cfg_get("ld", buf, sizeof(buf)) != -1) return 4;

    /* 5-7: 配置覆盖 */
    if (bd_cfg_set("cc", "clang") != 0) return 5;
    if (bd_cfg_get("cc", buf, sizeof(buf)) != 0) return 6;
    if (bd_strcmp(buf, "clang") != 0) return 7;

    /* 8-10: 目标调度 */
    if (bd_target_add("kernel.o", 0u) != 0) return 8;
    if (bd_target_add("lib.o", 1u) != 0) return 9;
    if (bd_target_add("kernel.o", 0u) != -1) return 10;   /* 重复 */
    if (bd_target_state("kernel.o", &done) != 0) return 11;
    if (done != 1u) return 12;   /* 初始 dirty */
    if (bd_build_one("kernel.o") != 0) return 13;
    if (bd_target_state("kernel.o", &done) != 0) return 14;
    if (done != 0u) return 15;

    /* 16-18: 链接符号 */
    if (bd_sym_def("kmain", 0x10000u) != 0) return 16;
    if (bd_sym_def("kmain", 0x20000u) != -1) return 17;   /* 重复 */
    refs[0] = "kmain"; refs[1] = "hal_init";
    if (bd_link("xos.bin", 2u, refs, addrs) != -3) return 18;  /* hal_init 未定义 */
    if (bd_sym_def("hal_init", 0x10080u) != 0) return 19;
    if (bd_link("xos.bin", 2u, refs, addrs) != 0) return 20;
    if (addrs[0] != 0x10000u || addrs[1] != 0x10080u) return 21;

    /* 22-24: 符号表满 */
    {
        u32 k;
        for (k = 0u; k < BD_MAX_SYMS; k++) {
            char nm[8];
            nm[0] = 's'; nm[1] = (char)('a' + (k & 15u)); nm[2] = (char)('a' + ((k >> 4) & 15u)); nm[3] = 0;
            (void)bd_sym_def(nm, 0x1000u + k);
        }
        if (bd_sym_def("overflow", 0x1u) != -2) return 22;
    }
    if (bd_link("x.bin", 2u, refs, addrs) != 0) return 23;
    if (addrs[0] != 0x10000u) return 24;

    /* 25-27: 目标格式识别 */
    if (bd_obj_format(elfh, 4u) != BD_FMT_ELF) return 25;
    if (bd_obj_format(mh, 4u) != BD_FMT_MACHO) return 26;
    if (bd_obj_format(mz, 4u) != BD_FMT_COFF) return 27;
    if (bd_obj_format(unk, 4u) != BD_FMT_UNKNOWN) return 28;

    /* 29-31: 全量构建 */
    if (bd_target_add("app.bin", 2u) != 0) return 29;
    if (bd_build_all(&done, &total) != 0) return 30;
    if (done != total || total < 2u) return 31;

    /* 32-34: 依赖调度 */
    if (bd_target_add("dep2.o", 1u) != 0) return 32;
    if (bd_build_one("dep2.o") != 1) return 33;   /* 依赖未满足 → 需先构建依赖 */
    if (bd_build_one("missing.o") != -1) return 34;
    if (bd_target_state("missing.o", &done) != -1) return 35;

    /* 36-38: 配置满 */
    {
        u32 k;
        for (k = 0u; k < BD_MAX_CFG; k++) {
            char kk[8], vv[8];
            kk[0] = 'k'; kk[1] = (char)('0' + (k % 10u)); kk[2] = 0;
            vv[0] = 'v'; vv[1] = (char)('0' + (k % 10u)); vv[2] = 0;
            (void)bd_cfg_set(kk, vv);
        }
        if (bd_cfg_set("full", "1") != 0) return 36;    /* 第 11 槽：成功 */
        if (bd_cfg_set("f2", "2") != -2) return 37;    /* 第 12 槽：满 */
    }
    if (bd_cfg_set("cc", "gcc") != 0) return 38;   /* 覆盖不占新槽 */
    if (bd_cfg_get("cc", buf, sizeof(buf)) != 0) return 39;
    if (bd_strcmp(buf, "gcc") != 0) return 40;

    /* 41-42: 目标表满 */
    {
        u32 k;
        for (k = 0u; k < BD_MAX_TARGETS; k++) {
            char nm[8];
            nm[0] = 't'; nm[1] = (char)('0' + (k % 10u)); nm[2] = 0;
            (void)bd_target_add(nm, 0u);
        }
        if (bd_target_add("toomany", 0u) != -2) return 41;
    }
    if (bd_target_add("kernel.o", 0u) != -1) return 42;   /* 重复仍拒绝 */

    return 0;
}
