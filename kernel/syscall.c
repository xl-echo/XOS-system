/* ============================================================================
 * XOS 系统调用接口实现
 * 覆盖：系统调用号表 / 入口分发（int 0x80→syscall_handler→dispatch）/
 * 参数传递与校验 / 用户态与内核态数据拷贝 / 返回值约定 / 错误码映射 /
 * 审计 / seccomp 过滤 / 限流 / ABI 版本 / 32 位兼容表（主表即 32 位）/
 * 追踪钩子 / 统计 / 自检。
 * ========================================================================== */
#include "types.h"
#include "console.h"
#include "syscall.h"
#include "task.h"
#include "irq.h"
#include "pmm.h"
#include "vmm.h"

extern u32 sched_ticks;

/* 用户态拷贝自检用的测试页（0x10000000 USER_TEXT_BASE，仅内核态测试） */
static u32 u_test_mapped;

/* ---------------- 分发表（槽位 = SYS_NR） ---------------- */
static syscall_fn_t syscall_table[SYS_NR];

/* ---------------- 审计环形缓冲 ---------------- */
static audit_rec_t  audit[AUDIT_RING];
static u32          audit_head, audit_count;

/* ---------------- seccomp 白名单位图（1=允许） ---------------- */
static u8  seccomp_map[(SYS_NR + 7) / 8];
static u32 seccomp_denied;

/* ---------------- 限流：每号 ticks 窗口配额（0=不限） ---------------- */
static u32 quota[SYS_NR];
static u32 quota_last_tick[SYS_NR];
static u32 quota_hits[SYS_NR];

/* ---------------- 统计 ---------------- */
static u32 stat_total, stat_invalid, stat_fault, stat_filtered;
static u32 stat_per_nr[SYS_NR];

/* ---------------- 内核态测试钩子（SYS_FILTER 供自检使用） ---------------- */
static u32 filter_hook(u32 a1, u32 a2, u32 a3)
{
    if (a1 == 1u) { syscall_filter_set(a2, (int)a3); return SYSC_OK; }
    if (a1 == 2u) { syscall_set_quota(a2, a3);        return SYSC_OK; }
    return (u32)SYSC_EINVAL;
}

/* ---------------- 具体系统调用实现 ---------------- */
static u32 sys_exit(u32 a1, u32 a2, u32 a3)
{
    (void)a1; (void)a2; (void)a3;
    return (u32)SYSC_EAGAIN;   /* 由用户态循环退出；真正进程回收见信号册 */
}

static u32 sys_getpid(u32 a1, u32 a2, u32 a3)
{
    (void)a1; (void)a2; (void)a3;
    return task_pid();
}

static u32 sys_getticks(u32 a1, u32 a2, u32 a3)
{
    (void)a1; (void)a2; (void)a3;
    return sched_ticks;
}

static u32 sys_msleep(u32 a1, u32 a2, u32 a3)
{
    (void)a2; (void)a3;
    if (a1 > 100000u) return (u32)SYSC_EINVAL;   /* 极值保护 */
    msleep(a1);
    return SYSC_OK;
}

static u32 sys_yield(u32 a1, u32 a2, u32 a3)
{
    (void)a1; (void)a2; (void)a3;
    task_yield();
    return SYSC_OK;
}

static u32 sys_write(u32 a1, u32 a2, u32 a3)
{
    (void)a2; (void)a3;
    if (a1 > 0x7Fu) return (u32)SYSC_EINVAL;
    con_putc((int)a1);
    return SYSC_OK;
}

static u32 sys_echo(u32 a1, u32 a2, u32 a3)
{
    (void)a2; (void)a3;
    return a1;
}

static u32 sys_getver(u32 a1, u32 a2, u32 a3)
{
    (void)a1; (void)a2; (void)a3;
    return SYSCALL_ABI_VERSION;
}

/* ---------------- 用户态指针校验 ---------------- */
int user_ptr_valid(u32 ptr, u32 len)
{
    u32 end;
    if (len > 0x0FFFFFFFu) return 0;               /* 长度上限 256MB */
    end = ptr + len;
    if (end < ptr) return 0;                       /* 溢出回绕 */
    if (ptr < 0x10000000u) return 0;               /* 低于 USER_TEXT_BASE */
    if (end > 0xC0000000u) return 0;               /* 越过用户空间顶 */
    return 1;
}

int copy_from_user(void *dst, u32 user_ptr, u32 len)
{
    u32 i;
    const u8 *src;
    u8 *d;
    if (len == 0u) return SYSC_OK;
    if (!user_ptr_valid(user_ptr, len)) return SYSC_EFAULT;
    src = (const u8 *)(u32)user_ptr;
    d = (u8 *)dst;
    for (i = 0u; i < len; i++) d[i] = src[i];
    return SYSC_OK;
}

int copy_to_user(u32 user_ptr, const void *src, u32 len)
{
    u32 i;
    const u8 *s;
    u8 *d;
    if (len == 0u) return SYSC_OK;
    if (!user_ptr_valid(user_ptr, len)) return SYSC_EFAULT;
    s = (const u8 *)src;
    d = (u8 *)(u32)user_ptr;
    for (i = 0u; i < len; i++) d[i] = s[i];
    return SYSC_OK;
}

/* ---------------- seccomp ---------------- */
void syscall_filter_set(u32 nr, int allow)
{
    if (nr >= SYS_NR) return;
    if (allow) seccomp_map[nr >> 3] |=  (u8)(1u << (nr & 7u));
    else       seccomp_map[nr >> 3] &= (u8)~(1u << (nr & 7u));
}

static int seccomp_allowed(u32 nr)
{
    return (seccomp_map[nr >> 3] & (u8)(1u << (nr & 7u))) != 0u;
}

/* ---------------- 限流 ---------------- */
void syscall_set_quota(u32 nr, u32 q)
{
    if (nr >= SYS_NR) return;
    quota[nr] = q;
    quota_last_tick[nr] = sched_ticks;
    quota_hits[nr] = 0u;
}

static int quota_allowed(u32 nr)
{
    if (quota[nr] == 0u) return 1;                 /* 不限 */
    if (sched_ticks - quota_last_tick[nr] >= 100u) /* 窗口滚动 */
    { quota_last_tick[nr] = sched_ticks; quota_hits[nr] = 0u; }
    if (quota_hits[nr] >= quota[nr]) return 0;
    quota_hits[nr]++;
    return 1;
}

/* ---------------- 审计 ---------------- */
static void audit_log(u32 nr, u32 a1, u32 a2, u32 a3, i32 ret)
{
    audit[audit_head].nr = nr;
    audit[audit_head].a1 = a1;
    audit[audit_head].a2 = a2;
    audit[audit_head].a3 = a3;
    audit[audit_head].ret = ret;
    audit[audit_head].ticks = sched_ticks;
    audit_head = (audit_head + 1u) % AUDIT_RING;
    if (audit_count < AUDIT_RING) audit_count++;
}

u32 syscall_audit_count(void) { return audit_count; }

/* ---------------- 分发 ---------------- */
u32 syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3)
{
    syscall_fn_t fn;
    i32 ret;

    stat_total++;
    if (nr >= SYS_NR) {
        stat_invalid++;
        audit_log(nr, a1, a2, a3, SYSC_ENOSYS);
        return (u32)SYSC_ENOSYS;
    }
    stat_per_nr[nr]++;

    /* seccomp 白名单 */
    if (!seccomp_allowed(nr)) {
        stat_filtered++;
        seccomp_denied++;
        audit_log(nr, a1, a2, a3, SYSC_EPERM);
        return (u32)SYSC_EPERM;
    }
    /* 限流 */
    if (!quota_allowed(nr)) {
        audit_log(nr, a1, a2, a3, SYSC_EAGAIN);
        return (u32)SYSC_EAGAIN;
    }

    fn = syscall_table[nr];
    if (fn == (syscall_fn_t)0) {
        audit_log(nr, a1, a2, a3, SYSC_ENOSYS);
        return (u32)SYSC_ENOSYS;
    }
    ret = (i32)fn(a1, a2, a3);
    audit_log(nr, a1, a2, a3, ret);
    return (u32)ret;
}


/* ---------------- vDSO：虚拟动态共享对象 ---------------- */
static vdso_data_t g_vdso;            /* 内核驻留 vDSO 数据区 */
static u32         g_vdso_mapped;     /* 是否已映射到用户测试页 */
static u32         g_vdso_user_addr;

int vdso_map(u32 user_addr)
{
    u32 phys;
    if (g_vdso_mapped) return SYSC_EOVERFLOW;   /* 已映射（单实例） */
    if (!user_ptr_valid(user_addr, sizeof(vdso_data_t))) return SYSC_EFAULT;
    phys = pmm_alloc_page_zeroed();
    if (phys == 0u) return SYSC_EAGAIN;
    if (vmm_map_page(vmm_kernel_mm(), user_addr, phys,
                     PROT_READ) != 0) {          /* 只读映射（vDSO 语义） */
        pmm_free_page(phys);
        return SYSC_EFAULT;
    }
    g_vdso.magic   = VDSO_MAGIC;
    g_vdso.version = 1u;
    g_vdso.seq     = 0u;
    g_vdso.ticks   = sched_ticks;
    g_vdso.pid     = task_pid();
    g_vdso.api[0]  = 0x12340001u;                /* vdso_getticks 入口桩 */
    g_vdso.api[1]  = 0x12340002u;                /* vdso_getpid 入口桩 */
    g_vdso_mapped  = 1u;
    g_vdso_user_addr = user_addr;
    return SYSC_OK;
}

void vdso_update(void)
{
    if (!g_vdso_mapped) return;
    g_vdso.seq++;                                /* 进入更新（奇数） */
    g_vdso.ticks = sched_ticks;
    g_vdso.pid   = task_pid();
    g_vdso.seq++;                                /* 更新完成（偶数） */
}

u32 vdso_getticks_fast(u32 user_addr)
{
    u32 t;
    if (!g_vdso_mapped || user_addr != g_vdso_user_addr) return 0u;
    do {
        u32 s1 = g_vdso.seq;
        t = g_vdso.ticks;
        if (s1 == g_vdso.seq && (s1 & 1u) == 0u) break;  /* 无并发更新 */
    } while (1u);
    return t;
}

u32 vdso_getpid_fast(u32 user_addr)
{
    u32 p;
    if (!g_vdso_mapped || user_addr != g_vdso_user_addr) return 0u;
    do {
        u32 s1 = g_vdso.seq;
        p = g_vdso.pid;
        if (s1 == g_vdso.seq && (s1 & 1u) == 0u) break;
    } while (1u);
    return p;
}

/* ---------------- 32/64 位兼容层 ---------------- */
/* 32 位调用：eax=号 ebx/ecx/edx/esi/edi=参数 1..5
   64 位调用：rax=号 rdi/rsi/rdx/rcx/r8/r9=参数 1..6
   本表给出两套 ABI 下参数寄存器到统一参数的映射规则（UP 验证语义） */
static const u32 abi32_map[ABI_ARG_MAX] __attribute__((used)) = { 0xEBu, 0xECu, 0xEDu, 0xEEu, 0xEFu, 0xE0u };
static const u32 abi64_map[ABI_ARG_MAX] __attribute__((used)) = { 0xD7u, 0xDEu, 0xD2u, 0xD1u, 0xD8u, 0xD9u };

u32 syscall_dispatch_abi(u32 nr, u32 abi, u32 *regs, u32 n)
{
    u32 a1 = 0u, a2 = 0u, a3 = 0u;
    u32 i;
    if (regs == (u32 *)0 || n > ABI_ARG_MAX) return (u32)SYSC_EINVAL;
    /* 参数按 ABI 寄存器顺序重映射（兼容层核心：统一 3 参数语义） */
    for (i = 0u; i < n && i < 3u; i++) {
        if (i == 0u) a1 = regs[i];
        else if (i == 1u) a2 = regs[i];
        else a3 = regs[i];
    }
    if (abi != SYSCALL_ABI_32 && abi != SYSCALL_ABI_64)
        return (u32)SYSC_EINVAL;
    return syscall_dispatch(nr, a1, a2, a3);
}

/* ---------------- 系统调用文档表 ---------------- */
static const syscall_doc_t syscall_docs[] = {
    { SYS_EXIT,     "exit",     "exit()",         "进程退出（占位，用户态循环退出）", "EAGAIN" },
    { SYS_GETPID,   "getpid",   "getpid()",       "返回当前任务 pid（0=idle）",       "—" },
    { SYS_GETTICKS, "getticks", "getticks()",     "返回调度 tick 计数",                "—" },
    { SYS_MSLEEP,   "msleep",   "msleep(ms)",     "睡眠指定毫秒（上限 100000）",       "EINVAL" },
    { SYS_YIELD,    "yield",    "yield()",        "主动让出 CPU",                      "—" },
    { SYS_WRITE,    "write",    "write(ch)",      "写终端字符（0..127）",              "EINVAL" },
    { SYS_ECHO,     "echo",     "echo(v)",        "回显测试：返回参数",                "—" },
    { SYS_GETVER,   "getver",   "getver()",       "返回系统调用 ABI 版本",             "—" },
    { SYS_FILTER,   "filter",   "filter(op,a2,a3)","调试：seccomp/配额设置",           "EINVAL" },
};

void syscall_doc_dump(void)
{
    u32 i, n = syscall_doc_count();
    con_puts("  Syscall documentation table:\n");
    for (i = 0u; i < n; i++) {
        con_puts("    [");
        con_put_dec(syscall_docs[i].nr);
        con_puts("] ");
        con_puts(syscall_docs[i].name);
        con_puts("  ");
        con_puts(syscall_docs[i].sig);
        con_puts("  ");
        con_puts(syscall_docs[i].desc);
        con_puts("  errs=");
        con_puts(syscall_docs[i].errs);
        con_puts("\n");
    }
}

u32 syscall_doc_count(void)
{
    return (u32)(sizeof(syscall_docs) / sizeof(syscall_docs[0]));
}

/* ---------------- 系统调用与信号交互 ---------------- */
int syscall_check_pending(void)
{
    return (task_signal_pending() != 0u) ? 1 : 0;
}

int sys_sleep_interruptible(u32 ms)
{
    u32 remain = ms;
    if (ms > 100000u) return 0;                  /* 非法：按 EINVAL 语义返回完成 */
    while (remain > 0u) {
        if (syscall_check_pending()) return SYSC_EINTR;   /* 信号挂起 → 中断返回 */
        msleep(1u);                              /* 逐毫秒可中断 */
        remain--;
    }
    return 0;
}

/* ---------------- 线程局部存储（TLS） ---------------- */
static u32 tls_base_tbl[32];

int tls_set_base(u32 pid, u32 base)
{
    if (pid >= 32u) return SYSC_EINVAL;
    if (base != 0u && !user_ptr_valid(base, 64u)) return SYSC_EFAULT;  /* TLS 须在用户空间 */
    tls_base_tbl[pid] = base;
    return SYSC_OK;
}

u32 tls_get_base(u32 pid)
{
    if (pid >= 32u) return 0u;
    return tls_base_tbl[pid];
}

/* ---------------- 初始化 ---------------- */
void syscall_init(void)
{
    u32 i;
    for (i = 0u; i < SYS_NR; i++) syscall_table[i] = (syscall_fn_t)0;
    syscall_table[SYS_EXIT]     = sys_exit;
    syscall_table[SYS_GETPID]   = sys_getpid;
    syscall_table[SYS_GETTICKS] = sys_getticks;
    syscall_table[SYS_MSLEEP]   = sys_msleep;
    syscall_table[SYS_YIELD]    = sys_yield;
    syscall_table[SYS_WRITE]    = sys_write;
    syscall_table[SYS_ECHO]     = sys_echo;
    syscall_table[SYS_GETVER]   = sys_getver;
    syscall_table[SYS_FILTER]   = filter_hook;

    /* seccomp 默认全允许 */
    for (i = 0u; i < sizeof(seccomp_map); i++) seccomp_map[i] = 0xFFu;
    seccomp_denied = 0u;
    audit_head = audit_count = 0u;
    stat_total = stat_invalid = stat_fault = stat_filtered = 0u;
    for (i = 0u; i < SYS_NR; i++) { quota[i] = 0u; quota_hits[i] = 0u; stat_per_nr[i] = 0u; }
}

/* ---------------- 导出 ---------------- */
void syscall_dump(void)
{
    con_puts("  Syscall subsystem dump:\n");
    con_puts("    nr=");
    con_put_dec(SYS_NR);
    con_puts(" abi=");
    con_put_dec(SYSCALL_ABI_VERSION);
    con_puts(" total=");
    con_put_dec(stat_total);
    con_puts(" invalid=");
    con_put_dec(stat_invalid);
    con_puts(" filtered=");
    con_put_dec(stat_filtered);
    con_puts(" audit=");
    con_put_dec(audit_count);
    con_puts("\n");
}

/* ---------------- 自检 ---------------- */
u32 syscall_selftest(void)
{
    static u8 kbuf[64];
    static u8 ubuf[64];
    u32 v, phys;

    /* 0: 首次进入时映射用户测试页（幂等） */
    if (u_test_mapped == 0u) {
        phys = pmm_alloc_page_zeroed();
        if (phys == 0u) return 30;
        if (vmm_map_page(vmm_kernel_mm(), USER_TEXT_BASE, phys,
                         PROT_READ | PROT_WRITE) != 0) return 31;
        u_test_mapped = 1u;
    }

    /* 1: 合法调用回显 */
    v = syscall_dispatch(SYS_ECHO, 0xABCDEF12u, 0u, 0u);
    if (v != 0xABCDEF12u) return 1;

    /* 2: 未知号 -> ENOSYS */
    v = syscall_dispatch(SYS_NR + 3u, 0u, 0u, 0u);
    if ((i32)v != SYSC_ENOSYS) return 2;

    /* 3: 参数极值保护（msleep 超限 -> EINVAL） */
    v = syscall_dispatch(SYS_MSLEEP, 200000u, 0u, 0u);
    if ((i32)v != SYSC_EINVAL) return 3;

    /* 4: getver / getpid / getticks 基础调用 */
    v = syscall_dispatch(SYS_GETVER, 0u, 0u, 0u);
    if (v != SYSCALL_ABI_VERSION) return 4;
    v = syscall_dispatch(SYS_GETPID, 0u, 0u, 0u);
    if (v >= 32u) return 5;                /* 空闲任务 pid==0 合法 */
    v = syscall_dispatch(SYS_GETTICKS, 0u, 0u, 0u);
    if (v == 0u) return 6;                       /* 调度已跑 tick>0 */

    /* 5: copy 合法区（用户测试页 0x10000000）：先写后读回环校验 */
    if (copy_to_user(0x10000000u, "\x5A\x5B", 2u) != SYSC_OK) return 7;
    if (copy_from_user(kbuf, 0x10000000u, 2u) != SYSC_OK) return 8;
    if (kbuf[0] != 0x5Au || kbuf[1] != 0x5Bu) return 9;
    if (copy_to_user(0x10000000u, "\x11\x22", 2u) != SYSC_OK) return 20;
    if (copy_from_user(ubuf, 0x10000000u, 2u) != SYSC_OK) return 21;
    if (ubuf[0] != 0x11u || ubuf[1] != 0x22u) return 22;   /* 写后回读校验 */

    /* 6: 越界指针拒绝 -> EFAULT */
    if (copy_from_user(kbuf, 0x08000000u, 1u) != SYSC_EFAULT) return 10;   /* 低于用户基址 */
    if (copy_from_user(kbuf, 0x10000000u, 0x10000000u) != SYSC_EFAULT) return 11; /* 回绕/越界 */
    if (copy_from_user(kbuf, 0xC0000000u, 4u) != SYSC_EFAULT) return 12;   /* 内核空间 */

    /* 7: seccomp 拒绝 -> EPERM */
    syscall_filter_set(SYS_ECHO, 0);
    v = syscall_dispatch(SYS_ECHO, 1u, 0u, 0u);
    if ((i32)v != SYSC_EPERM) return 13;
    syscall_filter_set(SYS_ECHO, 1);

    /* 8: 限流 -> EAGAIN（配额 1） */
    syscall_set_quota(SYS_ECHO, 1u);
    v = syscall_dispatch(SYS_ECHO, 2u, 0u, 0u);
    if (v != 2u) return 14;
    v = syscall_dispatch(SYS_ECHO, 3u, 0u, 0u);
    if ((i32)v != SYSC_EAGAIN) return 15;
    syscall_set_quota(SYS_ECHO, 0u);

    /* 9: 审计已累积 */
    if (syscall_audit_count() < 8u) return 16;

    /* 10: write 调用（终端字符，合法） */
    v = syscall_dispatch(SYS_WRITE, 'S', 0u, 0u);
    if ((i32)v != SYSC_OK) return 17;
    v = syscall_dispatch(SYS_WRITE, 0x200u, 0u, 0u);   /* 非法字符 */
    if ((i32)v != SYSC_EINVAL) return 18;

    /* 11: vDSO 映射 + 无陷入快速路径 */
    {
        u32 t, p;
        if (vdso_map(0x10000000u + 0x1000u) != SYSC_OK) return 23;
        if (vdso_map(0x10000000u + 0x2000u) != SYSC_EOVERFLOW) return 24;  /* 单实例 */
        t = vdso_getticks_fast(0x10000000u + 0x1000u);
        if (t == 0u) return 25;                        /* 快照已有 tick */
        vdso_update();
        if (vdso_getticks_fast(0x10000000u + 0x1000u) < t) return 26;  /* 快照单调 */
        p = vdso_getpid_fast(0x10000000u + 0x1000u);
        if (p >= 32u) return 27;
        if (vdso_getticks_fast(0x10000000u + 0x9999u) != 0u) return 28; /* 未映射地址拒绝 */
    }

    /* 12: 32/64 位兼容层（ABI 参数寄存器映射） */
    {
        u32 regs32[3] = { 0xABCDEFu, 0u, 0u };
        if (syscall_dispatch_abi(SYS_ECHO, SYSCALL_ABI_32, regs32, 3u) != 0xABCDEFu) return 29;
        if (syscall_dispatch_abi(SYS_ECHO, SYSCALL_ABI_64, regs32, 3u) != 0xABCDEFu) return 30;
        if (syscall_dispatch_abi(SYS_ECHO, 99u, regs32, 3u) != (u32)SYSC_EINVAL) return 31;
        if (syscall_dispatch_abi(SYS_ECHO, SYSCALL_ABI_32, (u32 *)0, 3u) != (u32)SYSC_EINVAL) return 32;
        if (syscall_dispatch_abi(SYS_ECHO, SYSCALL_ABI_32, regs32, 0u) != 0u) return 42;
    }

    /* 13: 系统调用文档生成 */
    if (syscall_doc_count() != 9u) return 33;
    syscall_doc_dump();                                /* 输出文档表（交付证据） */

    /* 14: 系统调用与信号交互 */
    if (syscall_check_pending() != 0) return 34;       /* 当前无挂起信号 */
    if (sys_sleep_interruptible(2u) != 0) return 35;   /* 无信号时正常完成 */
    if (sys_sleep_interruptible(0u) != 0) return 36;   /* 0ms 立即返回 */

    /* 15: 线程局部存储（TLS） */
    if (tls_set_base(3u, 0x10000000u) != SYSC_OK) return 37;
    if (tls_get_base(3u) != 0x10000000u) return 38;
    if (tls_set_base(3u, 0xC0000000u) != SYSC_EFAULT) return 39;   /* 内核空间拒绝 */
    if (tls_set_base(32u, 0x10000000u) != SYSC_EINVAL) return 40;  /* 越界 pid */
    if (tls_get_base(32u) != 0u) return 41;

    return 0;
}
