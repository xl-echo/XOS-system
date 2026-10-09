/* ============================================================================
 * XOS 用户态执行：ELF 加载到用户地址空间 + ring3 特权级进入
 * 真实链路：ELF 段页映射(PTE_US) → 拷贝 → iret 降权进入 ring3 → int 0x80 系统调用
 * ========================================================================== */
#include "user.h"
#include "app.h"
#include "cpu.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"

#define USER_MAX_PAGES 128u

static u32 g_user_pages;
static u32 g_user_pa[USER_MAX_PAGES];
static u32 g_user_va[USER_MAX_PAGES];
static u32 g_user_active;

/* 内核恢复点（用户程序退出/异常后 iret 返回地址） */
extern void user_exit_stub(void);       /* 汇编桩：isr_stubs.S */
extern void shell_interactive(void);    /* shell 交互主循环（kmain 调用） */
static u32 g_shell_entry;

void user_set_shell_entry(u32 e) { g_shell_entry = e; }

void user_after_exit(void)
{
    con_puts("  [exec] user program exited, back to kernel\n");
    user_cleanup();
    /* 重入 shell 交互循环（函数入口重新开始；栈由 iret 重置到大栈顶，不递归膨胀） */
    if (g_shell_entry) {
        void (*fn)(void) = (void (*)(void))g_shell_entry;
        fn();
    }
    for (;;) __asm__ __volatile__("hlt");
}

int user_is_active(void) { return g_user_active ? 1 : 0; }

/* 用户程序退出/ring3 异常：把 isr 帧改造成回内核的 iret 帧
 * isr_regs_t 不含 ss/esp（ring3 陷入时 CPU 压栈在 regs 之后）：
 *   f[14]=eip f[15]=cs f[16]=eflags f[17]=esp(user) f[18]=ss(user)
 * 覆写为 iret 回 user_exit_stub（cs=ring0, esp=内核大栈顶）后，
 * 由桩完成段恢复与清理，再重入 shell——避免 &&label 取址在 -O2 下的陷阱。
 */
void user_return_to_kernel(isr_regs_t *r)
{
    u32 *f;
    f = (u32 *)r;
    f[14] = (u32)&user_exit_stub;        /* eip -> 汇编恢复桩 */
    f[15] = GDT_R0_CODE;                 /* cs -> ring0 */
    f[16] = (f[16] & ~0x200u) | 0x200u;  /* eflags: IF=1 */
    f[17] = cpu_tss_get_esp0();          /* esp -> 独立内核大栈顶 */
    f[18] = GDT_R0_DATA;                 /* ss -> ring0 data */
}

/* 记录已映射页（用于退出/清理时回滚，保证可复位） */
static int user_track(u32 va, u32 pa)
{
    if (g_user_pages >= USER_MAX_PAGES) return 0;
    g_user_va[g_user_pages] = va;
    g_user_pa[g_user_pages] = pa;
    g_user_pages++;
    return 1;
}

u32 user_pages_used(void) { return g_user_pages; }

void user_cleanup(void)
{
    u32 i;
    for (i = 0u; i < g_user_pages; i++) {
        vmm_unmap_page(vmm_kernel_mm(), g_user_va[i]);
        pmm_free_page(g_user_pa[i]);
    }
    g_user_pages = 0u;
    g_user_active = 0u;
}

/* 加载 ELF：解析段 → 逐页映射(PTE_US) → 拷贝 → 返回 entry */
u32 user_load_elf(const u8 *img, u32 size, u32 *entry)
{
    app_seg_t segs[APP_MAX_SEGS];
    u32 n, i, phys, rc;

    if (!img || size < 20u) return 1u;
    n = app_elf_phdrs(img, size, segs, APP_MAX_SEGS);
    if (n == 0u) return 2u;
    if (app_elf_ehdr(img, size, entry, 0, 0, 0, 0) != 0u) return 3u;

    /* 清理上一轮残留，保证可复位 */
    if (g_user_pages > 0u) user_cleanup();

    for (i = 0u; i < n; i++) {
        u32 v, end;
        if (segs[i].type != APP_PT_LOAD) continue;
        v   = segs[i].vaddr & ~0xFFFu;
        end = (segs[i].vaddr + segs[i].memsz + 0xFFFu) & ~0xFFFu;
        if (end > USER_TOP) return 4u;              /* 越界防护 */
        while (v < end) {
            phys = pmm_alloc_page_zeroed();
            if (phys == 0u) return 5u;              /* 内存不足 */
            rc = vmm_map_page(vmm_kernel_mm(), v, phys,
                              PTE_P | PTE_RW | PTE_US);
            if (rc != VMM_OK) { pmm_free_page(phys); return 6u; }
            if (!user_track(v, phys)) { return 7u; }  /* 映射超限 */
            v += 4096u;
        }
        /* 拷贝代码/数据（filesz），bss（memsz-filesz）已由 zeroed 页清零 */
        memcpy((void *)(u32)segs[i].vaddr, img + segs[i].off, segs[i].filesz);
    }

    /* 用户栈一页（含 guard 语义：仅映射栈顶下 4KB） */
    {
        u32 sv = USER_STACK_TOP - 4096u;
        phys = pmm_alloc_page_zeroed();
        if (phys == 0u) return 8u;
        rc = vmm_map_page(vmm_kernel_mm(), sv, phys, PTE_P | PTE_RW | PTE_US);
        if (rc != VMM_OK) { pmm_free_page(phys); return 9u; }
        if (!user_track(sv, phys)) return 10u;
    }

    g_user_active = 1u;
    return 0u;
}

/* iret 降权进入 ring3：cs=0x18(GDT_R3_CODE) ss=0x20(GDT_R3_DATA)
 * 返回方式：用户程序执行 int 0x80 → isr_common_stub（切 TSS.esp0 内核栈）→
 * syscall_handler → iret 恢复用户上下文（继续用户态）
 */
void user_exec(u32 entry)
{
    u32 stack = USER_STACK_TOP;
    if (!cpu_user_supported()) return;
    con_puts("  [user] iret frame eip=");
    con_put_hex32(entry);
    con_puts(" cs=0x1B ss=0x23 esp=");
    con_put_hex32(stack);
    con_puts("\n");
    con_flush();
    __asm__ __volatile__(
        "movl %0, %%eax\n"          /* entry */
        "movl %1, %%ebx\n"          /* stack */
        "movw $0x23, %%dx\n"        /* R3 data | RPL3：进入 ring3 前加载数据段，否则 DPL0 段在 CPL3 访问即 GPF */
        "movw %%dx, %%ds\n"
        "movw %%dx, %%es\n"
        "movw %%dx, %%fs\n"
        "movw %%dx, %%gs\n"
        "pushl $0x23\n"             /* ss = GDT_R3_DATA | RPL3（0x20|3） */
        "pushl %%ebx\n"             /* esp = 用户栈顶 */
        "pushfl\n"                  /* eflags（含 IF） */
        "pushl $0x1B\n"             /* cs = GDT_R3_CODE | RPL3（0x18|3） */
        "pushl %%eax\n"             /* eip = entry */
        "iret\n"
        :: "r"(entry), "r"(stack)
        : "eax", "ebx", "edx", "memory");
}

/* 自检：加载最小 ELF（复用 app_selftest 构造法）并校验映射/清理 */
int user_selftest(void)
{
    u8 img[160];
    u32 entry = 0u;

    memset(img, 0, sizeof(img));
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 1u; img[5] = 1u;
    img[16] = APP_ET_EXEC & 0xFFu; img[17] = (APP_ET_EXEC >> 8) & 0xFFu;
    img[18] = APP_EM_386 & 0xFFu; img[19] = (APP_EM_386 >> 8) & 0xFFu;
    img[24] = 0x00; img[25] = 0x00; img[26] = 0x00; img[27] = 0x08;  /* entry = 0x08000000 */
    img[28] = 52u;                              /* phoff */
    img[32] = 0u; img[44] = 1u; img[48] = 0u;
    img[52] = APP_PT_LOAD & 0xFFu; img[53] = (APP_PT_LOAD >> 8) & 0xFFu;
    img[56] = 52u;
    img[60] = 0x00; img[61] = 0x00; img[62] = 0x00; img[63] = 0x08;  /* vaddr = 0x08000000 */
    img[64] = 0x00; img[65] = 0x00; img[66] = 0x00; img[67] = 0x08;  /* paddr(忽略) */
    img[68] = 32u;                               /* filesz */
    img[72] = 0x00; img[73] = 0x10;              /* memsz = 4096 */
    img[76] = 7u;

    /* 1: 加载成功 */
    if (user_load_elf(img, sizeof(img), &entry) != 0u) return 1;
    if (entry != 0x08000000u) return 2;
    /* 2: 映射页数 = 1(段) + 1(栈) */
    if (user_pages_used() != 2u) return 3;
    /* 3: 用户页表 U/S 位校验（读回页表项） */
    {
        u32 pte = vmm_pte_get(vmm_kernel_mm(), 0x08000000u);
        if (!(pte & PTE_US)) return 4;
        if (!(pte & PTE_P))  return 5;
    }
    /* 4: 栈映射存在 */
    {
        u32 pte = vmm_pte_get(vmm_kernel_mm(), USER_STACK_TOP - 4096u);
        if (!(pte & PTE_P)) return 6;
    }
    /* 5: 清理可复位 */
    user_cleanup();
    if (user_pages_used() != 0u) return 7;
    /* 6: 清理后可重新加载 */
    if (user_load_elf(img, sizeof(img), &entry) != 0u) return 8;
    user_cleanup();
    /* 7: 坏镜像拒绝 */
    if (user_load_elf(img, 10u, &entry) != 1u) return 9;
    return 0;
}
