/* ============================================================================
 * XOS 任务上下文切换汇编
 * void switch_to(u32 *prev_esp, u32 *next_esp)
 *   保存当前 CPU 上下文到 prev_esp，从 next_esp 恢复并返回。
 *   保存/恢复顺序严格对称：push ebp/ebx/esi/edi → 存 ESP → 切 ESP →
 *   pop edi/esi/ebx/ebp → ret。
 * 新任务首次被调度：其栈由 task_create 构造为
 *   [esp+0]=edi 占位 [esp+4]=esi [esp+8]=ebx [esp+12]=ebp
 *   [esp+16]=task_entry_asm（ret 目标） [esp+20]=入口函数 [esp+24]=参数
 * ============================================================================ */
.code32
.section .text

.globl __switch_to
__switch_to:
    pushl   %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi
    movl    20(%esp), %eax     /* prev_esp：返回地址 4 + 寄存器 16 = 偏移 20 */
    movl    %esp, (%eax)
    movl    24(%esp), %eax     /* next_esp */
    movl    (%eax), %esp
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret

/* ---------------------------------------------------------------------------
 * 新任务入口包装：ret 进入后，esp 指向 [entry, arg]
 * 调用 entry(arg)；entry 返回后调用 task_exit_self 自杀（不返回）。
 * ------------------------------------------------------------------------- */
.extern _task_exit_self

.globl _task_entry_asm
_task_entry_asm:
    sti                        /* 新任务首次进入：恢复中断（切走路径的 irq_restore 不执行） */
    movl    0(%esp), %eax      /* 入口函数 */
    movl    4(%esp), %edx      /* 参数 */
    pushl   %edx
    call    *%eax
    addl    $4, %esp
    call    _task_exit_self
.hang:
    hlt
    jmp     .hang
