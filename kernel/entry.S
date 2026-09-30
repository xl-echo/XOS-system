/* ============================================================================
 * XOS 内核入口汇编桩
 * 职责：关闭中断 → 清除方向标志 → 清零 .bss（含尾部字节）→ 调用 kmain
 * 注意：本文件必须位于链接命令行的第一个，以保证 _start 落在 0x10000
 *
 * 修复记录：
 *   1) 增加 cld。GCC -O2 会把字节循环优化为 rep movsb/stosb，若 DF=1 则
 *      地址反向递减，将造成内存破坏。ABI 要求函数入口 DF=0，必须在进入
 *      C 代码前显式清除。
 *   2) .bss 清零改为「双字批量 + 尾字节补齐」。原实现仅按 4 字节循环，
 *      当 __bss_end - __bss_start 不是 4 的倍数时尾部字节不会被清零。
 * ============================================================================ */
.code32
.section .text
.globl _start
.extern _kmain
.extern __bss_start
.extern __bss_end

_start:
    jmp     .entry
    .byte   0x90, 0x90
    .long   0x1BADB002          /* Multiboot magic，固定位于 0x10004 */
    .long   0x00000000          /* Multiboot flags：无需内存信息/图形 */
    .long   0xE4524FFE          /* Multiboot checksum = -(magic+flags) */
    .long   0x584F534B          /* XOS 内核映像签名，固定位于 0x10010 */

.entry:
    cli
    cld                         /* 方向标志清零：保证字符串操作正向执行 */

    /* ---- 清零 .bss 段 ---- */
    movl    $__bss_start, %edi
    movl    $__bss_end, %ecx
    subl    %edi, %ecx          /* ECX = 待清零字节数 */
    jz      .bss_done

    movl    %ecx, %edx          /* EDX = 原始字节数（备用） */
    shrl    $2, %ecx            /* ECX = 双字数 */
    xorl    %eax, %eax
    testl   %ecx, %ecx
    jz      .bss_tail

.bss_word:
    movl    %eax, (%edi)
    addl    $4, %edi
    decl    %ecx
    jnz     .bss_word

.bss_tail:
    andl    $3, %edx            /* EDX = 尾部剩余字节数 */
    jz      .bss_done

.bss_byte:
    movb    %al, (%edi)
    incl    %edi
    decl    %edx
    jnz     .bss_byte

.bss_done:
    /* ---- 建立内核栈（Stage2 已设为 0x9FFFC，此处再次确保） ---- */
    movl    $0x9FFFC, %esp
    movl    %esp, %ebp

    call    _kmain

.hang:
    hlt
    jmp     .hang
