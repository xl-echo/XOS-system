/* ============================================================================
 * XOS 第一个真实用户程序（ring3）
 * 由内核 shell `exec` 命令加载到用户地址空间 0x08000000，iret 降权进入 ring3。
 * 通过 int 0x80 系统调用输出文本（SYS_WRITE=5），随后 SYS_EXIT=0 退出回内核。
 * ========================================================================== */
.code32
.section .text
.globl _start

_start:
    movl    $msg, %esi
1:
    lodsb                       /* al = *esi++ */
    testb   %al, %al
    jz      3f                  /* 字符串结束 */
    movzbl  %al, %ebx          /* SYS_WRITE a1 = 字符（零扩展） */
    movl    $5, %eax            /* SYS_WRITE: a1=字符 */
    int     $0x80
    jmp     1b

3:
    /* 输出完成，请求退出回内核 */
    movl    $0, %eax            /* SYS_EXIT */
    int     $0x80
4:
    jmp     4b                  /* 兜底：若 exit 未生效则自旋（不应到达） */

.section .rodata
msg:
    .asciz  "XOS user mode program: Hello from ring3!\r\nSystem calls via int 0x80 are working.\r\nExiting back to kernel now.\r\n"
