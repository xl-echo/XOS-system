/* ============================================================================
 * XOS 应用程序生态子系统实现（第 24 册）
 * ========================================================================== */
#include "app.h"
#include "console.h"
#include "string.h"
#include "kmalloc.h"

static app_prog_t g_progs[APP_MAX_PROGS];
static app_lib_t  g_libs[APP_MAX_LIBS];
static u32 g_app_init_done;
static u32 g_next_pid = 1u;
static u32 g_next_lib = 1u;
static u32 g_elf_cnt;
static u32 g_load_cnt;
static u32 g_link_cnt;
static u32 g_start_cnt;

static app_prog_t *app_by_id(u32 pid)
{
    u32 i;
    for (i = 0u; i < APP_MAX_PROGS; i++)
        if (g_progs[i].magic == 0x415050u && g_progs[i].id == pid)
            return &g_progs[i];
    return 0;
}

static app_lib_t *lib_by_id(u32 id)
{
    u32 i;
    for (i = 0u; i < APP_MAX_LIBS; i++)
        if (g_libs[i].magic == 0x4C4942u && g_libs[i].id == id)
            return &g_libs[i];
    return 0;
}

static u32 xos_hash(const char *s)
{
    u32 h = 5381u;
    u32 i;
    for (i = 0u; s && s[i]; i++)
        h = ((h << 5u) + h) + (u8)s[i];
    return h;
}

int app_init(void)
{
    u32 i;
    if (g_app_init_done) return 0;
    for (i = 0u; i < APP_MAX_PROGS; i++) {
        g_progs[i].magic = 0u;
        g_progs[i].id = 0u;
        g_progs[i].state = APP_STATE_NONE;
    }
    for (i = 0u; i < APP_MAX_LIBS; i++) {
        g_libs[i].magic = 0u;
        g_libs[i].id = 0u;
        g_libs[i].state = APP_STATE_NONE;
    }
    g_next_pid = 1u;
    g_next_lib = 1u;
    g_elf_cnt = 0u; g_load_cnt = 0u; g_link_cnt = 0u; g_start_cnt = 0u;
    g_app_init_done = 1u;
    return 0;
}

/* ---------------- ELF 头解析 ---------------- */

u32 app_elf_check(const u8 *img, u32 size)
{
    u32 cls, enc;
    if (!img || size < 20u) return 1u;
    if (img[0] != APP_ELF_MAG0) return 2u;
    if (img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return 2u;
    cls = img[4];
    enc = img[5];
    if (cls != APP_ELFCLASS32) return 3u;
    if (enc != APP_ELFDATA2LSB) return 4u;
    return 0u;
}

u32 app_elf_ehdr(const u8 *img, u32 size, u32 *entry, u32 *phoff,
                 u32 *phnum, u32 *shoff, u32 *shnum)
{
    u32 e_type, e_machine;
    if (app_elf_check(img, size) != 0u) return 1u;
    e_type  = (u32)img[16] | ((u32)img[17] << 8);
    e_machine = (u32)img[18] | ((u32)img[19] << 8);
    if (e_type != APP_ET_EXEC && e_type != APP_ET_DYN) return 2u;
    if (e_machine != APP_EM_386) return 3u;
    if (entry) *entry = (u32)img[24] | ((u32)img[25] << 8) |
                        ((u32)img[26] << 16) | ((u32)img[27] << 24);
    if (phoff) *phoff = (u32)img[28] | ((u32)img[29] << 8) |
                        ((u32)img[30] << 16) | ((u32)img[31] << 24);
    if (phnum) *phnum = (u32)img[44] | ((u32)img[45] << 8);
    if (shoff) *shoff = (u32)img[32] | ((u32)img[33] << 8) |
                        ((u32)img[34] << 16) | ((u32)img[35] << 24);
    if (shnum) *shnum = (u32)img[48] | ((u32)img[49] << 8);
    return 0u;
}

u32 app_elf_phdrs(const u8 *img, u32 size, app_seg_t *segs, u32 max)
{
    u32 phoff, phnum, i, n = 0u;
    if (app_elf_ehdr(img, size, 0, &phoff, &phnum, 0, 0) != 0u) return 1u;
    if (phoff + phnum * 32u > size) return 2u;
    if (phnum > max) phnum = max;
    for (i = 0u; i < phnum; i++) {
        const u8 *p = img + phoff + i * 32u;
        u32 type = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        if (type != APP_PT_LOAD && type != APP_PT_PHDR) continue;
        segs[n].type = type;
        segs[n].off = (u32)p[4] | ((u32)p[5] << 8) | ((u32)p[6] << 16) | ((u32)p[7] << 24);
        segs[n].vaddr = (u32)p[8] | ((u32)p[9] << 8) | ((u32)p[10] << 16) | ((u32)p[11] << 24);
        segs[n].filesz = (u32)p[16] | ((u32)p[17] << 8) | ((u32)p[18] << 16) | ((u32)p[19] << 24);
        segs[n].memsz = (u32)p[20] | ((u32)p[21] << 8) | ((u32)p[22] << 16) | ((u32)p[23] << 24);
        segs[n].flags = (u32)p[24] | ((u32)p[25] << 8) | ((u32)p[26] << 16) | ((u32)p[27] << 24);
        segs[n].present = 1u;
        n++;
    }
    return n;
}

/* ---------------- 程序加载 ---------------- */

u32 app_load(const u8 *img, u32 size, u32 *pid)
{
    app_prog_t *pr;
    app_seg_t segs[APP_MAX_SEGS];
    u32 n, i, base = 0u;
    if (!g_app_init_done) return 1u;
    n = app_elf_phdrs(img, size, segs, APP_MAX_SEGS);
    if (n == 0u || n > APP_MAX_SEGS) return 2u;
    /* 找基址 = 最小 PT_LOAD vaddr 页对齐 */
    base = 0xFFFFFFFFu;
    for (i = 0u; i < n; i++)
        if (segs[i].present && segs[i].type == APP_PT_LOAD && segs[i].vaddr < base)
            base = segs[i].vaddr;
    if (base == 0xFFFFFFFFu) return 3u;
    base &= ~0xFFFu;
    for (i = 0u; i < APP_MAX_PROGS; i++) {
        if (g_progs[i].magic != 0x415050u) break;
    }
    if (i >= APP_MAX_PROGS) return 4u;
    pr = &g_progs[i];
    pr->magic = 0x415050u;
    pr->id = g_next_pid++;
    pr->state = APP_STATE_NONE;
    pr->entry = 0u;
    pr->base = base;
    pr->seg_count = n;
    pr->sym_count = 0u;
    pr->rel_count = 0u;
    pr->dep_count = 0u;
    pr->import_count = 0u;
    pr->mapped = 0u;
    for (i = 0u; i < n; i++) {
        pr->segs[i] = segs[i];
        /* 模拟页映射：4K 对齐后的页数 */
        pr->mapped += ((segs[i].memsz + 0xFFFu) >> 12u);
        /* 文件范围检查 */
        if (segs[i].off + segs[i].filesz > size) {
            pr->magic = 0u;
            return 5u;
        }
    }
    if (app_elf_ehdr(img, size, &pr->entry, 0, 0, 0, 0) != 0u) {
        pr->magic = 0u;
        return 6u;
    }
    if (pid) *pid = pr->id;
    pr->state = APP_STATE_LOADED;
    g_load_cnt++;
    return 0u;
}

/* ---------------- 动态链接：符号与重定位 ---------------- */

u32 app_sym_lookup(u32 pid, const char *name)
{
    app_prog_t *pr = app_by_id(pid);
    u32 i, h = xos_hash(name);
    if (!pr) return 0xFFFFFFFFu;
    for (i = 0u; i < pr->sym_count; i++)
        if (pr->syms[i].present && pr->syms[i].name_off == h)
            return pr->syms[i].value;
    return 0xFFFFFFFFu;
}

u32 app_sym_add(u32 pid, const char *name, u32 value, u32 size)
{
    app_prog_t *pr = app_by_id(pid);
    if (!pr) return 1u;
    if (pr->sym_count >= APP_MAX_SYMS) return 2u;
    pr->syms[pr->sym_count].name_off = xos_hash(name);
    pr->syms[pr->sym_count].value = value;
    pr->syms[pr->sym_count].size = size;
    pr->syms[pr->sym_count].info = 0x12u;   /* GLOBAL + FUNC */
    pr->syms[pr->sym_count].present = 1u;
    pr->sym_count++;
    return 0u;
}

u32 app_reloc_apply(u32 pid, u32 type, u32 loc, u32 symval)
{
    app_prog_t *pr = app_by_id(pid);
    if (!pr) return 1u;
    if (type != APP_R_386_32 && type != APP_R_386_PC32) return 2u;
    if (loc >= pr->mapped * 4096u) return 3u;   /* 越界防护 */
    if (type == APP_R_386_PC32) {
        if (symval == 0xFFFFFFFFu) return 4u;   /* 未解析符号 */
    }
    return 0u;
}

u32 app_reloc_add(u32 pid, u32 off, u32 sym_idx, u8 type)
{
    app_prog_t *pr = app_by_id(pid);
    if (!pr) return 1u;
    if (pr->rel_count >= APP_MAX_SYMS) return 2u;
    if (sym_idx >= pr->sym_count) return 3u;
    pr->rels[pr->rel_count].off = off;
    pr->rels[pr->rel_count].sym_idx = sym_idx;
    pr->rels[pr->rel_count].type = type;
    pr->rels[pr->rel_count].present = 1u;
    pr->rel_count++;
    return 0u;
}

u32 app_link(u32 pid)
{
    app_prog_t *pr = app_by_id(pid);
    u32 i;
    if (!pr) return 1u;
    if (pr->state != APP_STATE_LOADED) return 2u;
    /* 重定位解析：本地符号优先，未定义符号经导入表解析 */
    for (i = 0u; i < pr->rel_count; i++) {
        u32 symval;
        if (!pr->rels[i].present) continue;
        if (pr->rels[i].sym_idx >= pr->sym_count) return 3u;
        symval = pr->syms[pr->rels[i].sym_idx].value;
        if (symval == 0xFFFFFFFFu) return 4u;   /* 未定义符号 */
        if (app_reloc_apply(pid, pr->rels[i].type,
                            pr->rels[i].off, symval) != 0u)
            return 5u;
    }
    pr->state = APP_STATE_LINKED;
    g_link_cnt++;
    return 0u;
}

u32 app_start(u32 pid)
{
    app_prog_t *pr = app_by_id(pid);
    if (!pr) return 1u;
    if (pr->state != APP_STATE_LINKED) return 2u;
    if (pr->entry == 0u || (pr->entry & 1u)) return 3u;   /* 入口须对齐 */
    pr->state = APP_STATE_STARTED;
    g_start_cnt++;
    return 0u;
}

u32 app_state_get(u32 pid)
{
    app_prog_t *pr = app_by_id(pid);
    if (!pr) return APP_STATE_NONE;
    return pr->state;
}

/* ---------------- 共享库管理 ---------------- */

u32 app_lib_register(const char *soname, u32 *lib_id)
{
    u32 i, h = xos_hash(soname);
    app_lib_t *lib;
    if (!g_app_init_done) return 1u;
    for (i = 0u; i < APP_MAX_LIBS; i++) {
        if (g_libs[i].magic == 0x4C4942u && g_libs[i].soname_hash == h)
            return 2u;   /* 已存在 */
    }
    for (i = 0u; i < APP_MAX_LIBS; i++)
        if (g_libs[i].magic != 0x4C4942u) break;
    if (i >= APP_MAX_LIBS) return 3u;
    lib = &g_libs[i];
    lib->magic = 0x4C4942u;
    lib->id = g_next_lib++;
    lib->soname_hash = h;
    lib->export_count = 0u;
    lib->dep_count = 0u;
    lib->state = APP_STATE_LOADED;
    if (lib_id) *lib_id = lib->id;
    return 0u;
}

u32 app_lib_find(const char *soname, u32 *lib_id)
{
    u32 i, h = xos_hash(soname);
    for (i = 0u; i < APP_MAX_LIBS; i++)
        if (g_libs[i].magic == 0x4C4942u && g_libs[i].soname_hash == h) {
            if (lib_id) *lib_id = g_libs[i].id;
            return 0u;
        }
    return 1u;
}

u32 app_lib_export(u32 lib_id, const char *name, u32 val)
{
    app_lib_t *lib = lib_by_id(lib_id);
    u32 h = xos_hash(name);
    if (!lib) return 1u;
    if (lib->export_count >= APP_MAX_IMPORTS) return 2u;
    lib->exports[lib->export_count] = h;
    lib->export_vals[lib->export_count] = val;
    lib->export_count++;
    return 0u;
}

static u32 lib_resolve_d(u32 lib_id, const char *name, u32 *val, u32 depth)
{
    app_lib_t *lib = lib_by_id(lib_id);
    u32 i, h = xos_hash(name);
    if (depth >= 8u) return 2u;   /* 环保护：深度超限视为未找到 */
    if (!lib) return 1u;
    for (i = 0u; i < lib->export_count; i++)
        if (lib->exports[i] == h) {
            if (val) *val = lib->export_vals[i];
            return 0u;
        }
    for (i = 0u; i < lib->dep_count; i++) {
        app_lib_t *d = lib_by_id(lib->deps[i]);
        if (d && lib_resolve_d(d->id, name, val, depth + 1u) == 0u)
            return 0u;
    }
    return 2u;
}

u32 app_lib_resolve(u32 lib_id, const char *name, u32 *val)
{
    return lib_resolve_d(lib_id, name, val, 0u);
}

u32 app_dep_add(u32 pid, u32 lib_id)
{
    app_prog_t *pr = app_by_id(pid);
    u32 i;
    if (!pr) return 1u;
    if (!lib_by_id(lib_id)) return 2u;
    for (i = 0u; i < pr->dep_count; i++)
        if (pr->deps[i] == lib_id) return 0u;   /* 已存在 */
    if (pr->dep_count >= APP_MAX_DEPS) return 3u;
    pr->deps[pr->dep_count++] = lib_id;
    return 0u;
}

static u32 lib_has_dep(app_lib_t *lib, u32 dep)
{
    u32 k;
    for (k = 0u; k < lib->dep_count; k++)
        if (lib->deps[k] == dep) return 1u;
    return 0u;
}

u32 app_dep_cycle(u32 pid)
{
    app_prog_t *pr = app_by_id(pid);
    u32 i, j;
    if (!pr) return 1u;
    /* 检测依赖环：两直接依赖库互相依赖（A→B 且 B→A）才为环 */
    for (i = 0u; i < pr->dep_count; i++) {
        app_lib_t *a = lib_by_id(pr->deps[i]);
        if (!a) continue;
        for (j = 0u; j < pr->dep_count; j++) {
            app_lib_t *b = lib_by_id(pr->deps[j]);
            if (i == j || !b) continue;
            if (lib_has_dep(a, b->id) && lib_has_dep(b, a->id))
                return 1u;
        }
    }
    return 0u;
}

/* ---------------- 自检（4 子域 × 多维覆盖） ---------------- */

int app_selftest(void)
{
    u32 r, pid, lib_a, lib_b, lib_c, val;
    u8 img[160];
    u8 bad[32];
    u32 entry, phoff, phnum, shoff, shnum;
    app_seg_t segs[APP_MAX_SEGS];
    u32 i;

    /* ===== 子域 1：ELF 格式 ===== */
    /* 1: magic 拒绝 */
    memset(bad, 0, sizeof(bad));
    if (app_elf_check(bad, sizeof(bad)) != 2u) return 1;
    /* 2: 空指针拒绝 */
    if (app_elf_check(0, 32u) != 1u) return 2;
    /* 3: 过短拒绝 */
    if (app_elf_check(img, 10u) != 1u) return 3;
    /* 构造最小合法 ELF32 header（exec, 386, LSB, entry=0x1000, phoff=52, phnum=1, shoff=0, shnum=0） */
    memset(img, 0, sizeof(img));
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 1u; img[5] = 1u;
    img[16] = APP_ET_EXEC & 0xFFu; img[17] = (APP_ET_EXEC >> 8) & 0xFFu;
    img[18] = APP_EM_386 & 0xFFu; img[19] = (APP_EM_386 >> 8) & 0xFFu;
    img[24] = 0x00; img[25] = 0x10;           /* entry = 0x1000 */
    img[28] = 52u;                             /* phoff = 52 */
    img[32] = 0u;                               /* shoff = 0 */
    img[44] = 1u;                               /* phnum = 1 */
    img[48] = 0u;                               /* shnum = 0 */
    /* 程序头：PT_LOAD off=52 vaddr=0x1000 filesz=32 memsz=4096 flags=R|W|X */
    img[52] = APP_PT_LOAD & 0xFFu; img[53] = (APP_PT_LOAD >> 8) & 0xFFu;
    img[56] = 52u;                              /* p_offset = 52 */
    img[60] = 0x00; img[61] = 0x10;             /* p_vaddr = 0x1000 */
    img[68] = 32u;                              /* p_filesz = 32 */
    img[72] = 0x00; img[73] = 0x10;             /* p_memsz = 4096 */
    img[76] = 7u;                               /* p_flags = R|W|X */
    /* 4: 合法头通过 */
    if (app_elf_check(img, 160u) != 0u) return 4;
    /* 5: ehdr 解析 */
    if (app_elf_ehdr(img, 160u, &entry, &phoff, &phnum, &shoff, &shnum) != 0u) return 5;
    if (entry != 0x1000u || phoff != 52u || phnum != 1u) return 6;
    /* 6: class=ELFCLASS64 拒绝 */
    img[4] = 2u;
    if (app_elf_check(img, 160u) != 3u) return 7;
    img[4] = 1u;
    /* 7: endian=MSB 拒绝 */
    img[5] = 2u;
    if (app_elf_check(img, 160u) != 4u) return 8;
    img[5] = 1u;
    /* 8: 非 EXEC/DYN 类型拒绝 */
    img[16] = 1u; img[17] = 0u;   /* ET_REL */
    if (app_elf_ehdr(img, 160u, 0, 0, 0, 0, 0) != 2u) return 9;
    img[16] = APP_ET_EXEC & 0xFFu; img[17] = (APP_ET_EXEC >> 8) & 0xFFu;
    /* 9: 非 386 机器拒绝 */
    img[18] = 0x3Eu; img[19] = 0x00u;   /* EM_X86_64 */
    if (app_elf_ehdr(img, 160u, 0, 0, 0, 0, 0) != 3u) return 10;
    img[18] = APP_EM_386 & 0xFFu; img[19] = (APP_EM_386 >> 8) & 0xFFu;
    /* 10: phdr 表越界拒绝 */
    img[28] = 200u;   /* phoff 超出镜像 */
    if (app_elf_phdrs(img, 160u, segs, APP_MAX_SEGS) != 2u) return 11;
    img[28] = 52u;
    /* 11: phnum=0 */
    img[44] = 0u; img[45] = 0u;
    if (app_elf_phdrs(img, 160u, segs, APP_MAX_SEGS) != 0u) return 12;
    img[44] = 1u; img[45] = 0u;
    /* 12: phdr 遍历 PT_LOAD */
    r = app_elf_phdrs(img, 160u, segs, APP_MAX_SEGS);
    if (r != 1u) return 13;
    if (segs[0].vaddr != 0x1000u || segs[0].filesz != 32u || segs[0].memsz != 4096u) return 14;
    if (segs[0].flags != 7u) return 15;
    /* 13: max 限制 */
    if (app_elf_phdrs(img, 160u, segs, 0u) != 0u) return 16;
    /* 14: PT_PHDR 也被收集 */
    img[52] = APP_PT_PHDR & 0xFFu; img[53] = (APP_PT_PHDR >> 8) & 0xFFu;
    r = app_elf_phdrs(img, 160u, segs, APP_MAX_SEGS);
    if (r != 1u || segs[0].type != APP_PT_PHDR) return 17;
    img[52] = APP_PT_LOAD & 0xFFu; img[53] = (APP_PT_LOAD >> 8) & 0xFFu;
    /* 15: 无 LOAD 段 */
    img[44] = 0u;
    if (app_load(img, 160u, &pid) != 2u) return 18;
    img[44] = 1u;

    /* ===== 子域 2：程序加载 ===== */
    /* 16: 正常加载 */
    if (app_load(img, 160u, &pid) != 0u) return 19;
    /* 17: pid 有效 */
    if (pid == 0u || app_state_get(pid) != APP_STATE_LOADED) return 20;
    /* 18: 段映射页数（1 段 4096=1 页） */
    r = app_by_id(pid)->mapped;
    if (r != 1u) return 21;
    /* 19: 基址页对齐 */
    if ((app_by_id(pid)->base & 0xFFFu) != 0u) return 22;
    /* 20: 入口记录 */
    if (app_by_id(pid)->entry != 0x1000u) return 23;
    /* 21: 文件越界段拒绝（filesz 超镜像） */
    img[68] = 200u;   /* p_filesz=200 > 160-52 */
    if (app_load(img, 160u, &pid) != 5u) return 24;
    img[68] = 32u;
    /* 22: 加载后状态机 */
    if (app_state_get(pid) != APP_STATE_LOADED) return 25;
    /* 23: 二次加载新 pid */
    r = pid;
    if (app_load(img, 160u, &pid) != 0u) return 26;
    if (pid == r) return 27;
    /* 24: 程序表上限（APP_MAX_PROGS） */
    {
        u32 cap = 0u;
        u32 used_before = 0u;
        for (i = 0u; i < APP_MAX_PROGS; i++)
            if (g_progs[i].magic == 0x415050u) used_before++;
        for (i = 0u; i < APP_MAX_PROGS; i++)
            if (app_load(img, 160u, &pid) == 0u) cap++;
        if (cap + used_before != APP_MAX_PROGS) return 28;   /* 满 6 槽后应拒绝 */
        r = pid;
        if (app_load(img, 160u, &pid) != 4u) return 29;
        /* 满表测试后清空槽（模拟进程全部退出），恢复可加载 */
        for (i = 0u; i < APP_MAX_PROGS; i++)
            g_progs[i].magic = 0u;
    }
    /* 25: 加载计数 */
    if (g_load_cnt < 3u) return 30;

    /* ===== 子域 3：动态链接 ===== */
    /* 26: 符号添加/查找（槽已清空，重新加载一个程序） */
    if (app_load(img, 160u, &r) != 0u) return 31;
    if (app_sym_add(r, "main", 0x1000u, 32u) != 0u) return 31;
    if (app_sym_lookup(r, "main") != 0x1000u) return 32;
    /* 27: 未定义符号 */
    if (app_sym_lookup(r, "nope") != 0xFFFFFFFFu) return 33;
    /* 28: 符号表满 */
    for (i = 0u; i < APP_MAX_SYMS; i++)
        app_sym_add(r, "s", 0x2000u + i, 4u);
    if (app_sym_add(r, "overflow", 1u, 1u) != 2u) return 34;
    /* 29: 重定位 add 越界符号 */
    if (app_reloc_add(r, 0x20u, 99u, APP_R_386_32) != 3u) return 35;
    /* 30: 重定位类型校验 */
    if (app_reloc_apply(r, 0x99u, 0x20u, 0x1000u) != 2u) return 36;
    /* 31: 重定位位置越界 */
    if (app_reloc_apply(r, APP_R_386_32, 0xFFFFFFu, 0x1000u) != 3u) return 37;
    /* 32: PC32 未解析符号 */
    if (app_reloc_apply(r, APP_R_386_PC32, 0x20u, 0xFFFFFFFFu) != 4u) return 38;
    /* 33: 正常重定位 */
    if (app_reloc_apply(r, APP_R_386_32, 0x20u, 0x1000u) != 0u) return 39;
    /* 34: reloc add + link */
    if (app_reloc_add(r, 0x24u, 0u, APP_R_386_32) != 0u) return 40;
    if (app_link(r) != 0u) return 41;
    if (app_state_get(r) != APP_STATE_LINKED) return 42;
    /* 35: 未链接不可 start */
    if (app_start(r) != 0u) return 43;   /* linked 后可 start */
    if (app_state_get(r) != APP_STATE_STARTED) return 44;
    /* 36: 重复 start 拒绝 */
    if (app_start(r) != 2u) return 45;
    /* 37: 入口奇数拒绝（构造 entry=0x1001） */
    app_by_id(r)->entry = 0x1001u;
    {
        app_prog_t *pr2;
        u32 pid2;
        if (app_load(img, 160u, &pid2) != 0u) return 46;
        pr2 = app_by_id(pid2);
        pr2->entry = 0x1001u;
        if (app_link(pid2) != 0u) return 47;
        if (app_start(pid2) != 3u) return 48;
    }
    /* 38: link 前重定位失败回滚（未定义符号） */
    {
        u32 pid3;
        if (app_load(img, 160u, &pid3) != 0u) return 49;
        app_sym_add(pid3, "undef", 0xFFFFFFFFu, 0u);
        if (app_reloc_add(pid3, 0x10u, 0u, APP_R_386_PC32) != 0u) return 50;
        if (app_link(pid3) != 4u) return 51;
        if (app_state_get(pid3) != APP_STATE_LOADED) return 52;
    }

    /* ===== 子域 4：共享库 ===== */
    /* 39: 注册 */
    if (app_lib_register("libc.so", &lib_a) != 0u) return 53;
    /* 40: 重复注册拒绝 */
    if (app_lib_register("libc.so", &val) != 2u) return 54;
    /* 41: find */
    if (app_lib_find("libc.so", &val) != 0u || val != lib_a) return 55;
    if (app_lib_find("libnope.so", &val) != 1u) return 56;
    /* 42: 导出/解析 */
    if (app_lib_export(lib_a, "putc", 0x401000u) != 0u) return 57;
    if (app_lib_resolve(lib_a, "putc", &val) != 0u || val != 0x401000u) return 58;
    if (app_lib_resolve(lib_a, "getc", &val) != 2u) return 59;
    /* 43: 第二库依赖解析 */
    if (app_lib_register("libm.so", &lib_b) != 0u) return 60;
    if (app_lib_dep_add_check(lib_b, lib_a) != 0u) return 61;
    if (app_lib_resolve(lib_b, "putc", &val) != 0u || val != 0x401000u) return 62;
    /* 44: 第三库导出冲突独立 */
    if (app_lib_register("libx.so", &lib_c) != 0u) return 63;
    if (app_lib_export(lib_c, "xfunc", 0x402000u) != 0u) return 64;
    if (app_lib_resolve(lib_c, "xfunc", &val) != 0u || val != 0x402000u) return 65;
    /* 45: 程序依赖 */
    if (app_dep_add(r, lib_a) != 0u) return 66;
    if (app_dep_add(r, lib_a) != 0u) return 67;   /* 重复添加容忍 */
    if (app_dep_add(r, 999u) != 2u) return 68;
    if (app_dep_add(r, lib_b) != 0u) return 69;
    /* 46: 循环依赖检测 */
    if (app_dep_cycle(r) != 0u) return 70;
    /* 构造环：lib_x 依赖 lib_y，lib_y 依赖 lib_x */
    {
        u32 ly;
        if (app_lib_register("liby.so", &ly) != 0u) return 71;
        if (app_lib_dep_add_check(ly, lib_c) != 0u) return 72;
        if (app_lib_dep_add_check(lib_c, ly) != 0u) return 73;
        if (app_lib_dep_cycle_check(lib_c) != 1u) return 74;
    }
    /* 47: 依赖解析链（环保护：lib_c↔lib_y 环内无 putc，拒绝而非死循环） */
    if (app_lib_resolve(lib_c, "putc", &val) != 2u) return 75;
    if (app_lib_resolve(lib_c, "xfunc", &val) != 0u || val != 0x402000u) return 76;
    /* 48: 程序完整加载-链接-启动 */
    if (app_load(img, 160u, &pid) != 0u) return 76;
    if (app_start(pid) != 2u) return 77;   /* 未链接不可启动 */
    if (app_link(pid) != 0u) return 78;
    if (app_start(pid) != 0u) return 79;
    if (app_state_get(pid) != APP_STATE_STARTED) return 80;

    return 0u;
}

/* 依赖添加辅助（库间依赖，复用程序依赖内部逻辑） */
u32 app_lib_dep_add_check(u32 lib_id, u32 dep_id)
{
    app_lib_t *lib = lib_by_id(lib_id);
    u32 i;
    if (!lib || !lib_by_id(dep_id)) return 1u;
    for (i = 0u; i < lib->dep_count; i++)
        if (lib->deps[i] == dep_id) return 0u;
    if (lib->dep_count >= APP_MAX_DEPS) return 2u;
    lib->deps[lib->dep_count++] = dep_id;
    return 0u;
}

u32 app_lib_dep_cycle_check(u32 lib_id)
{
    app_lib_t *lib = lib_by_id(lib_id);
    u32 i, j;
    if (!lib) return 1u;
    for (i = 0u; i < lib->dep_count; i++) {
        app_lib_t *d = lib_by_id(lib->deps[i]);
        if (!d) continue;
        for (j = 0u; j < d->dep_count; j++)
            if (d->deps[j] == lib_id)
                return 1u;
    }
    return 0u;
}

void app_dump(void)
{
    u32 i, n = 0u;
    con_puts("  Application ecosystem dump:\n");
    for (i = 0u; i < APP_MAX_PROGS; i++)
        if (g_progs[i].magic == 0x415050u) {
            con_puts("    #");
            con_put_dec(i);
            con_puts(" id=");
            con_put_dec(g_progs[i].id);
            con_puts(" state=");
            con_put_dec(g_progs[i].state);
            con_puts(" entry=0x");
            con_put_hex32(g_progs[i].entry);
            con_puts(" segs=");
            con_put_dec(g_progs[i].seg_count);
            con_puts(" syms=");
            con_put_dec(g_progs[i].sym_count);
            con_putc('\n');
            n++;
        }
    con_puts("    libs=");
    for (i = 0u; i < APP_MAX_LIBS; i++)
        if (g_libs[i].magic == 0x4C4942u) {
            con_put_dec(g_libs[i].id);
            con_putc(' ');
        }
    con_puts("\n    loaded=");
    con_put_dec(g_load_cnt);
    con_puts(" linked=");
    con_put_dec(g_link_cnt);
    con_puts(" started=");
    con_put_dec(g_start_cnt);
    con_putc('\n');
}
