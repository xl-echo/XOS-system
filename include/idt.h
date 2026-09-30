/* ============================================================================
 * XOS 中断描述符表（IDT）与 CPU 异常处理
 * 目的：为 0-31 号 CPU 异常建立处理入口，避免未处理异常导致三重故障重启，
 *       满足「故障可诊断、不损伤主机」的设计要求。
 * ============================================================================ */
#ifndef __XOS_IDT_H__
#define __XOS_IDT_H__

#include "types.h"

#define IDT_ENTRIES     256
#define KERNEL_CS       0x08        /* GDT 代码段选择子 */

/* ---- 异常向量编号 ---- */
#define EXC_DE      0       /* Divide Error            除零 */
#define EXC_DB      1       /* Debug                   调试 */
#define EXC_NMI     2       /* NMI                     不可屏蔽中断 */
#define EXC_BP      3       /* Breakpoint              断点 */
#define EXC_OF      4       /* Overflow                溢出 */
#define EXC_BR      5       /* BOUND Range Exceeded     越界 */
#define EXC_UD      6       /* Invalid Opcode          非法指令 */
#define EXC_NM      7       /* Device Not Available    设备不可用 */
#define EXC_DF      8       /* Double Fault            双重故障 */
#define EXC_CSO     9       /* Coprocessor Segment Overrun */
#define EXC_TS      10      /* Invalid TSS             无效 TSS */
#define EXC_NP      11      /* Segment Not Present     段不存在 */
#define EXC_SS      12      /* Stack-Segment Fault     栈段故障 */
#define EXC_GP      13      /* General Protection      通用保护 */
#define EXC_PF      14      /* Page Fault              缺页 */
#define EXC_RSV15   15      /* Reserved */
#define EXC_MF      16      /* x87 FPU Error           FPU 错误 */
#define EXC_AC      17      /* Alignment Check         对齐检查 */
#define EXC_MC      18      /* Machine Check           机器检查 */
#define EXC_XF      19      /* SIMD FP Exception       SIMD 浮点 */
#define EXC_VE      20      /* Virtualization Exception */
#define EXC_CP      21      /* Control Protection */

/* ---- 异常发生时压栈的寄存器现场 ----
 * 布局与 isr_stubs.S 中 isr_common_stub 的压栈顺序严格对应 */
typedef struct {
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha 结果 */
    u32 gs, fs, es, ds;                           /* 段寄存器 */
    u32 vector;                                   /* 异常号 */
    u32 err;                                      /* 错误码（无则为 0） */
    u32 eip;                                      /* 故障指令地址 */
    u32 cs;
    u32 eflags;
} PACKED isr_regs_t;

void        idt_init(void);
void        idt_set_gate(u32 n, u32 base, u16 sel, u8 flags);
u32         idt_gate_addr(u32 n);        /* 读回门描述符入口地址 */
const char *exc_name(u32 vector);
int         exc_has_error_code(u32 vector);
void        isr_handler(isr_regs_t *r);

/* 自检：校验 IDT 装载状态与关键门描述符，返回 0 表示通过 */
u32         idt_selftest(void);

#endif /* __XOS_IDT_H__ */
