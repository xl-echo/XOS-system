/* ============================================================================
 * XOS IDT 建立与 CPU 异常处理实现
 * ============================================================================ */
#include "idt.h"
#include "console.h"
#include "string.h"
#include "vmm.h"
#include "irq.h"

/* --------------------------------------------------------------------------
 * 硬件数据结构
 * ------------------------------------------------------------------------ */
typedef struct {
    u16 base_lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_hi;
} PACKED idt_entry_t;

typedef struct {
    u16 limit;
    u32 base;
} PACKED idt_ptr_t;

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idtp;
static bool        idt_ready = false;

/* --------------------------------------------------------------------------
 * 32 个异常入口桩（定义在 kernel/isr_stubs.S）
 * ------------------------------------------------------------------------ */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void isr_default(void);
extern void irq0(void);   extern void irq1(void);   extern void irq2(void);
extern void irq3(void);   extern void irq4(void);   extern void irq5(void);
extern void irq6(void);   extern void irq7(void);   extern void irq8(void);
extern void irq9(void);   extern void irq10(void);  extern void irq11(void);
extern void irq12(void);  extern void irq13(void);  extern void irq14(void);
extern void irq15(void);
extern void syscall_stub(void);
static void (*const irq_stubs[16])(void) = {
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
};

static void (*const isr_stubs[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
};

/* --------------------------------------------------------------------------
 * 异常名称
 * ------------------------------------------------------------------------ */
const char *exc_name(u32 vector)
{
    switch (vector) {
    case EXC_DE:    return "Divide Error";
    case EXC_DB:    return "Debug";
    case EXC_NMI:   return "Non-Maskable Interrupt";
    case EXC_BP:    return "Breakpoint";
    case EXC_OF:    return "Overflow";
    case EXC_BR:    return "BOUND Range Exceeded";
    case EXC_UD:    return "Invalid Opcode";
    case EXC_NM:    return "Device Not Available";
    case EXC_DF:    return "Double Fault";
    case EXC_CSO:   return "Coprocessor Segment Overrun";
    case EXC_TS:    return "Invalid TSS";
    case EXC_NP:    return "Segment Not Present";
    case EXC_SS:    return "Stack-Segment Fault";
    case EXC_GP:    return "General Protection Fault";
    case EXC_PF:    return "Page Fault";
    case EXC_RSV15: return "Reserved (15)";
    case EXC_MF:    return "x87 FPU Error";
    case EXC_AC:    return "Alignment Check";
    case EXC_MC:    return "Machine Check";
    case EXC_XF:    return "SIMD Floating-Point Exception";
    case EXC_VE:    return "Virtualization Exception";
    case EXC_CP:    return "Control Protection Exception";
    default:        return "Unknown/Reserved";
    }
}

/* CPU 是否对该异常压入错误码 */
int exc_has_error_code(u32 vector)
{
    switch (vector) {
    case EXC_DF: case EXC_TS: case EXC_NP: case EXC_SS:
    case EXC_GP: case EXC_PF: case EXC_AC: case EXC_CP:
        return 1;
    default:
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * 门描述符写入
 * flags: 0x8E = 存在 + DPL0 + 32 位中断门
 * ------------------------------------------------------------------------ */
void idt_set_gate(u32 n, u32 base, u16 sel, u8 flags)
{
    if (n >= IDT_ENTRIES) return;
    idt[n].base_lo = (u16)(base & 0xFFFFu);
    idt[n].sel     = sel;
    idt[n].zero    = 0;
    idt[n].flags   = flags;
    idt[n].base_hi = (u16)((base >> 16) & 0xFFFFu);
}

/* --------------------------------------------------------------------------
 * 读回门描述符入口地址（自检用）
 * ------------------------------------------------------------------------ */
u32 idt_gate_addr(u32 n)
{
    if (n >= IDT_ENTRIES) return 0;
    return (u32)idt[n].base_lo | ((u32)idt[n].base_hi << 16);
}

/* --------------------------------------------------------------------------
 * 初始化：安装 32 个异常门 + 默认门，装载 IDTR
 * ------------------------------------------------------------------------ */
void idt_init(void)
{
    u32 i;

    memset(&idt, 0, sizeof(idt));

    for (i = 0; i < 32; i++) {
        idt_set_gate(i, (u32)isr_stubs[i], KERNEL_CS, 0x8E);
    }
    for (i = 32; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, (u32)isr_default, KERNEL_CS, 0x8E);
    }

    idtp.limit = (u16)(sizeof(idt_entry_t) * IDT_ENTRIES - 1u);
    idtp.base  = (u32)&idt;

    __asm__ __volatile__("lidt %0" : : "m"(idtp));

    idt_ready = true;
}

/* --------------------------------------------------------------------------
 * 装载 16 路 IRQ 门与软中断门（irq_init 调用）
 * ------------------------------------------------------------------------ */
void irq_install_gates(void)
{
    u32 i;
    for (i = 0; i < 16u; i++) {
        idt_set_gate(IRQ_BASE + i, (u32)irq_stubs[i], KERNEL_CS, 0x8E);
    }
    idt_set_gate(SYS_CALL_VEC, (u32)syscall_stub, KERNEL_CS, 0x8E);
}

/* --------------------------------------------------------------------------
 * 可恢复缺页异常
 *
 * 设计目标：把「缺页」从「致命异常」降级为「可修复事件」。
 * 仅当虚拟内存子系统明确报告修复成功（VMM_OK）时才返回 1，
 * 由 isr_common_stub 的 iret 重新执行出错指令；
 * 任何修复失败都返回 0，落回下面的完整现场打印 + 安全停机。
 *
 * 安全边界（防止把安全停机换成死循环）：
 *   1. 未启用分页时不接管，缺页仍按致命异常处理
 *   2. vmm_handle_page_fault 内部对「同一故障地址 + 同一指令」连续重复
 *      到达做了计数保护，第二次仍失败即判为修复未生效并拒绝
 *   3. 本函数只在返回 1 时放行，绝不对失败情形做「跳过指令」之类的兜底
 * ------------------------------------------------------------------------ */
static int try_recover_page_fault(isr_regs_t *r)
{
    u32 cr2, rc;

    if (r->vector != EXC_PF) return 0;
    if (!vmm_enabled()) return 0;

    __asm__ __volatile__("movl %%cr2, %0" : "=r"(cr2));
    rc = vmm_handle_page_fault(cr2, r->err, r->eip, r->cs);
    if (rc != VMM_OK) return 0;

    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  [pf] ");
    con_put_hex32(cr2);
    con_puts(" err=");
    con_put_hex32(r->err);
    con_puts("  resolved, instruction retried\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_flush();
    return 1;
}

/* --------------------------------------------------------------------------
 * 异常处理：先尝试可恢复路径，失败则打印完整现场并停机
 * 设计目标：任何未预期异常都留下可诊断信息后停机，而不是三重故障重启，
 *           从而保证主机不被反复复位、故障可定位。
 * ------------------------------------------------------------------------ */
void isr_handler(isr_regs_t *r)
{
    u32 v = r->vector;

    /* 硬件中断：16 路 IRQ（0x20-0x2F）分发 */
    if (v >= IRQ_BASE && v < IRQ_END) {
        irq_dispatch(r);
        return;
    }
    /* 软中断：int 0x80 */
    if (v == SYS_CALL_VEC) {
        syscall_handler(r);
        return;
    }
    /* 可恢复缺页 */
    if (try_recover_page_fault(r)) return;
    /* 可恢复固定长度异常：#BP(1B)/#UD(2B) */
    if (exc_recover_skip(r)) return;

    /* 其余异常：完整现场 + 栈回溯 + 安全停机 */
    panic_regs(r);
}

/* --------------------------------------------------------------------------
 * 自检：
 *   1) IDTR 已装载且 limit/base 正确
 *   2) 关键门描述符指向正确的桩、选择子与属性
 * 返回 0 表示通过，否则返回失败用例号
 * ------------------------------------------------------------------------ */
u32 idt_selftest(void)
{
    idt_ptr_t cur;
    u32 i, off;

    if (!idt_ready) return 1;

    /* 用例 1：IDTR 回读 */
    __asm__ __volatile__("sidt %0" : "=m"(cur));
    if (cur.limit != (u16)(sizeof(idt_entry_t) * IDT_ENTRIES - 1u)) return 2;
    if (cur.base != (u32)&idt) return 3;

    /* 用例 2：全部 32 个异常门地址正确、属性正确 */
    for (i = 0; i < 32; i++) {
        off = (u32)idt[i].base_lo | ((u32)idt[i].base_hi << 16);
        if (off != (u32)isr_stubs[i]) return 4;
        if (idt[i].sel != KERNEL_CS) return 5;
        if (idt[i].flags != 0x8E) return 6;
        if (idt[i].zero != 0) return 7;
    }

    /* 用例 3：默认门覆盖 32-255 */
    for (i = 32; i < IDT_ENTRIES; i++) {
        off = (u32)idt[i].base_lo | ((u32)idt[i].base_hi << 16);
        if (off != (u32)isr_default) return 8;
    }

    /* 用例 4：错误码判定表正确 */
    if (exc_has_error_code(EXC_GP) != 1) return 9;
    if (exc_has_error_code(EXC_DE) != 0) return 10;
    if (exc_has_error_code(EXC_GP) != 1) return 9;
    if (exc_has_error_code(EXC_DE) != 0) return 10;

    return 0;
}
