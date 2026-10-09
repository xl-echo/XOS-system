/* ============================================================================
 * XOS CPU 异常入口桩（0-31 号异常 + 默认桩）
 * 栈布局约定（isr_common_stub 内统一）：
 *   [esp+ 0..31]  pusha 结果：EDI ESI EBP ESP EBX EDX ECX EAX
 *   [esp+32..44]  段寄存器：  GS FS ES DS
 *   [esp+48]      vector
 *   [esp+52]      err
 *   [esp+56]      eip
 *   [esp+60]      cs
 *   [esp+64]      eflags
 * 该布局与 include/idt.h 的 isr_regs_t 严格一致。
 * ============================================================================ */
.code32
.section .text

.extern _isr_handler

/* CPU 不压错误码的异常：补压 0 占位，统一栈布局 */
.macro ISR_NOERR num
.globl _isr\num
_isr\num:
    cli
    pushl   $0
    pushl   $\num
    jmp     isr_common_stub
.endm

/* CPU 已压入错误码的异常：只补压异常号 */
.macro ISR_ERR num
.globl _isr\num
_isr\num:
    cli
    pushl   $\num
    jmp     isr_common_stub
.endm

ISR_NOERR 0        /* #DE 除零 */
ISR_NOERR 1        /* #DB 调试 */
ISR_NOERR 2        /* NMI */
ISR_NOERR 3        /* #BP 断点 */
ISR_NOERR 4        /* #OF 溢出 */
ISR_NOERR 5        /* #BR 越界 */
ISR_NOERR 6        /* #UD 非法指令 */
ISR_NOERR 7        /* #NM 设备不可用 */
ISR_ERR   8        /* #DF 双重故障 */
ISR_NOERR 9        /* 协处理器段越界 */
ISR_ERR   10       /* #TS 无效 TSS */
ISR_ERR   11       /* #NP 段不存在 */
ISR_ERR   12       /* #SS 栈段故障 */
ISR_ERR   13       /* #GP 通用保护 */
ISR_ERR   14       /* #PF 缺页 */
ISR_NOERR 15       /* 保留 */
ISR_NOERR 16       /* #MF FPU 错误 */
ISR_ERR   17       /* #AC 对齐检查 */
ISR_NOERR 18       /* #MC 机器检查 */
ISR_NOERR 19       /* #XF SIMD 浮点 */
ISR_NOERR 20       /* #VE 虚拟化异常 */
ISR_ERR   21       /* #CP 控制保护 */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30       /* #SX 安全异常 */
ISR_NOERR 31

/* ---------------------------------------------------------------------------
 * 默认桩：覆盖 32-255 号未使用向量，防止意外中断造成三重故障
 * ------------------------------------------------------------------------- */
.globl _isr_default
_isr_default:
    cli
    pushl   $0
    pushl   $0xFFFFFFFF
    jmp     isr_common_stub

/* ---------------------------------------------------------------------------
 * 公共出口：保存现场 → 切内核数据段 → 调用 C 处理函数 → 恢复 → iret
 * ------------------------------------------------------------------------- */
isr_common_stub:
    push    %ds
    push    %es
    push    %fs
    push    %gs
    pusha

    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    pushl   %esp
    call    _isr_handler
    addl    $4, %esp

    popa
    pop     %gs
    pop     %fs
    pop     %es
    pop     %ds

    addl    $8, %esp          /* 丢弃 vector 与 err */
    iret

/* ---------------------------------------------------------------------------
 * 用户程序退出/ring3 异常后的内核恢复桩：
 * user_return_to_kernel 把 isr 帧改造成 iret 回此桩（cs=ring0, esp=内核大栈顶）。
 * 桩恢复内核数据段 → 调用 C 清理（user_after_exit）→ 重入 shell 交互循环。
 * ------------------------------------------------------------------------- */
.globl _user_exit_stub
_user_exit_stub:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    call    _user_after_exit
1:  hlt
    jmp     1b

/* ---------------------------------------------------------------------------
 * 16 路硬件中断桩（0x20-0x2F，PIC 重映射后）与软中断桩（int 0x80）
 * 栈布局与异常桩一致（vector 在 [esp+48]，err 在 [esp+52]）。
 * 中断返回需 EOI：由 C 侧 irq_dispatch 完成。
 * ------------------------------------------------------------------------- */
.macro IRQ_STUB n
.globl _irq\n
_irq\n:
    cli
    pushl   $0
    pushl   $(0x20 + \n)
    jmp     isr_common_stub
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

/* int 0x80 软中断入口 */
.globl _syscall_stub
_syscall_stub:
    cli
    pushl   $0
    pushl   $0x80
    jmp     isr_common_stub
