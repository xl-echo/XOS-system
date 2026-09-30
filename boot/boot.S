/* ============================================================================
 * XOS MBR 主引导记录（严格 512 字节）
 * 职责：寄存器/栈初始化 → 自身完整性校验 → 保存启动盘号 → 清屏 →
 *       分区表解析 → 检测 EDD → 读取 Stage2 → 移交控制权
 *
 * 本版新增（相对上一版）：
 *   1) 自身完整性校验：对偏移 0..443 做 16 位字节累加和，与构建脚本写入
 *      偏移 444 的校验值比对，可发现引导扇区被篡改（引导扇区型恶意代码）
 *   2) 分区表解析：扫描 4 个分区项（扇区内偏移 446 起，每项 16 字节）
 *   3) 活动分区标志判定：要求恰好一个分区项带 0x80 活动标志，否则拒绝引导
 *   4) 活动分区项合法性校验：类型非零、起始 LBA 非零、扇区数容得下 Stage2
 *   5) Stage2 读取 LBA 取自活动分区项，不再硬编码 —— 改分区表即可改引导位置
 *   6) 统一错误出口，新增完整性错误码 0xE1、分区表错误码 0xE2
 *
 * 保留能力：EDD 扩展读检测、CHS 回退读盘、读盘失败复位重试（最多 3 次）、
 *           BIOS 错误码十六进制输出
 *
 * 内存布局约束：Stage2 装载于 0x7E00，32 扇区共 16KB，末端 0xBE00，
 *               不跨越 64KB 边界，故 CHS 路径可直接以 16 位偏移递增。
 *
 * 空间约束：代码与数据必须全部落在偏移 0..443 之内；
 *           444..445 为完整性校验值、446..509 为分区表、510..511 为引导签名，
 *           三者均不得被代码侵占。
 * ============================================================================ */
.code16
.section .text
.globl _start

.equ STAGE2_ADDR, 0x7E00
.equ STAGE2_SECS, 32
.equ MAX_RETRY,   3
.equ PT_BASE,     0x7C00 + 446      /* 分区表在内存中的地址 */
.equ PT_ENTRIES,  4
.equ PT_ACTIVE,   0x80
.equ CHECK_LEN,   444               /* 完整性校验覆盖长度（偏移 0..443） */

_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7C00, %sp
    sti

    /* ---------------- 1. 自身完整性校验 ----------------
     * 对偏移 0..443 的代码与数据做 16 位字节累加和，与偏移 444 处由构建
     * 脚本写入的校验值比对，可发现引导扇区被篡改（引导扇区型恶意代码）。
     *
     * 【顺序约束】本段必须排在所有「写内存」动作之前。boot_drive、err_code
     * 等变量都落在被校验区间内，一旦先写再校验，运行时的内容就与构建时
     * 不一致，校验必然失败。本段只读内存，只用 AX/SI/CX，DL（BIOS 传入的
     * 启动盘号）保持原值不受影响。
     *
     * 偏移 510 的 0x55AA 签名由 BIOS 先行校验，此处另做一遍，用于覆盖
     * 「被其它引导管理器链式加载、BIOS 未校验签名」的场景。
     * --------------------------------------------------- */
    movw    $0x7C00, %si
    movw    $CHECK_LEN, %cx
    xorw    %ax, %ax
.sum_loop:
    addb    (%si), %al
    adcb    $0, %ah
    incw    %si
    loop    .sum_loop
    cmpw    mbr_sum, %ax
    jne     .sig_err
    cmpw    $0xAA55, sig_word
    jne     .sig_err

    /* ---------------- 2. 保存启动盘号 + 清屏 + 横幅 ----------------
     * 保存 DL 必须在清屏之前：INT 10h 会破坏 DL
     * ------------------------------------------------------------- */
    movb    %dl, boot_drive

    /* 设置 80x25 文本模式（清屏） */
    movw    $0x0003, %ax
    int     $0x10

    movw    $msg_boot, %si
    call    puts16

    /* ---------------- 3. 分区表解析 + 4/5. 活动分区判定与校验 ----------------
     * DL 在入口已存入 boot_drive，此处借用 DL 统计活动分区个数
     * ---------------------------------------------------------------------- */
    movw    $PT_BASE, %si
    movw    $PT_ENTRIES, %cx
    xorw    %dx, %dx
.part_scan:
    cmpb    $PT_ACTIVE, (%si)
    jne     .part_next
    incb    %dl
    cmpb    $0, 4(%si)              /* 分区类型非零 */
    je      .pt_err
    movl    8(%si), %eax            /* 起始 LBA */
    testl   %eax, %eax
    jz      .pt_err
    cmpl    $STAGE2_SECS, 12(%si)   /* 扇区数须容得下 Stage2 */
    jb      .pt_err
    movl    %eax, dap_lba           /* 写入磁盘地址包 */
.part_next:
    addw    $16, %si
    loop    .part_scan
    cmpb    $1, %dl                 /* 必须恰好一个活动分区 */
    jne     .pt_err

    /* ---------------- 6. 检测 EDD 扩展读支持 ---------------- */
    movb    $0x41, %ah
    movb    boot_drive, %dl
    int     $0x13
    jc      .use_chs
    testb   $0x01, %cl          /* bit0 = 支持 LBA 访问 */
    jz      .use_chs

    /* ---------------- EDD 路径（LBA 直接寻址） ---------------- */
    movw    $MAX_RETRY, %bp
.edd_retry:
    movw    $dap, %si
    movb    boot_drive, %dl
    movb    $0x42, %ah
    int     $0x13
    jnc     .loaded_edd
    call    reset_disk
    decw    %bp
    jnz     .edd_retry
    jmp     .disk_err

.loaded_edd:
    movb    $0x01, %al          /* 引导源标志：EDD 路径，fall-through 到 .loaded */

    /* ---------------- CHS 回退路径 ---------------- */
.use_chs:
    movb    boot_drive, %dl
    movb    $0x08, %ah          /* 取驱动器几何参数 */
    int     $0x13
    jc      .disk_err
    andb    $0x3F, %cl          /* CL 低 6 位 = 每道扇区数 */
    movzbl  %cl, %eax
    testl   %eax, %eax
    jz      .disk_err
    movw    %ax, spt
    movzbl  %dh, %eax
    incl    %eax                /* DH = 最大磁头号，磁头数 = DH+1 */
    movw    %ax, heads

    movw    $MAX_RETRY, %bp
.chs_retry:
    call    chs_read_stage2
    jnc     .loaded_chs
    call    reset_disk
    decw    %bp
    jnz     .chs_retry
    jmp     .disk_err

.loaded_chs:
    movb    $0x02, %al          /* 引导源标志：CHS 路径，fall-through 到 .loaded */

    /* ---------------- 移交控制权 ----------------
     * 交接协议：移交前把「启动介质契约」写入固定内存槽 0x7DFC
     *   [0x7DFC] = 启动盘号 DL 副本（BIOS 实模式约定，供 Stage2 复用）
     *   [0x7DFD] = 引导源标志（0x01=EDD 扩展读 / 0x02=CHS 回退读）
     * Stage2 与后续阶段可据此感知引导路径，无需重新探测。
     * ------------------------------------------------ */
.loaded:
    movb    %al, 0x7DFD         /* 引导源标志（0x01=EDD / 0x02=CHS） */
    movb    boot_drive, %al
    movb    %al, 0x7DFC         /* 盘号副本 */
    ljmp    $0x0000, $STAGE2_ADDR

    /* ---------------- 统一错误出口 ---------------- */
.sig_err:
    movb    $0xE1, %al          /* 自身签名校验失败 */
    jmp     .err_out
.pt_err:
    movb    $0xE2, %al          /* 分区表非法 */
    jmp     .err_out
.disk_err:
    movb    %ah, %al            /* BIOS 磁盘错误码 */
.err_out:
    movb    %al, err_code
    movw    $msg_err, %si
    call    puts16
    movb    err_code, %al
    call    puthex8
    movw    $msg_crlf, %si
    call    puts16
.halt:
    hlt
    jmp     .halt

/* ---------------------------------------------------------------------------
 * reset_disk：复位磁盘控制器（INT13h AH=00h），清故障状态后再重试
 * 调用方在每次重试前都会重新装载 AH/DL，故无需保护任何寄存器
 * ------------------------------------------------------------------------- */
reset_disk:
    xorw    %ax, %ax
    movb    boot_drive, %dl
    int     $0x13
    ret

/* ---------------------------------------------------------------------------
 * lba_to_chs：LBA → CHS 换算（16 位运算：本镜像 10MB = 20480 扇区，足够）
 * 入参：AX = LBA
 * 出参：CX = 柱面(低8位于CH) | 柱面高2位(CL6-7) | 扇区号(CL0-5)，DH = 磁头
 * 破坏：AX BX CX DX SI
 * 依赖：spt（每道扇区数）、heads（磁头数）；调用方自行保护需保留的寄存器
 * ------------------------------------------------------------------------- */
lba_to_chs:
    xorw    %dx, %dx
    divw    spt                 /* AX = LBA/SPT, DX = 扇区索引(0基) */
    movw    %dx, %si
    xorw    %dx, %dx
    divw    heads               /* AX = 柱面号, DX = 磁头号 */
    movb    %dl, %dh            /* DH = 磁头号 */

    /* CX = (柱面低8位 << 8) | (柱面高2位 << 6) | (扇区号 + 1) */
    movb    %al, %ch
    movb    %ah, %cl
    andb    $0x03, %cl
    shlb    $6, %cl
    movw    %si, %ax
    incb    %al
    orb     %al, %cl
    ret

/* ---------------------------------------------------------------------------
 * chs_read_stage2：CHS 逐扇区读取 Stage2
 * 成功：CF=0；失败：CF=1 且 AH 为 BIOS 错误码
 * 调用方无需保留通用寄存器（成功即跳转、失败即重试）
 * ------------------------------------------------------------------------- */
chs_read_stage2:
    xorw    %ax, %ax
    movw    %ax, %es
    movw    $STAGE2_ADDR, %bx
    movw    dap_lba, %ax
    movw    $STAGE2_SECS, %dx
    movw    %dx, chs_left

.chs_one:
    pushw   %ax
    pushw   %bx
    call    lba_to_chs
    movb    boot_drive, %dl
    movw    $0x0201, %ax        /* AH=02h 读盘，AL=01h 扇区数 */
    int     $0x13
    jc      .chs_fail

    popw    %bx
    popw    %ax
    addw    $512, %bx
    incw    %ax
    decw    chs_left
    jnz     .chs_one
    clc
    ret

.chs_fail:
    popw    %bx
    popw    %ax
    stc
    ret

/* ---------------------------------------------------------------------------
 * puthex8：AL 以两位十六进制输出（用 DAA 技巧免分支，比查表更省空间）
 * ------------------------------------------------------------------------- */
puthex8:
    pushw   %ax
    movb    %al, %bl
    shrb    $4, %al
    call    puthexdig
    movb    %bl, %al
    call    puthexdig
    popw    %ax
    ret

puthexdig:
    andb    $0x0F, %al
    addb    $0x90, %al
    daa
    adcb    $0x40, %al
    daa
    movb    $0x0E, %ah
    int     $0x10
    ret

/* ---------------------------------------------------------------------------
 * puts16：打印 NUL 结尾字符串（BIOS INT 10h AH=0Eh）
 * 入参：%si = 字符串地址；AX/BX 不保证保留
 * ------------------------------------------------------------------------- */
puts16:
    lodsb
    testb   %al, %al
    jz      .puts_done
    movb    $0x0E, %ah
    int     $0x10
    jmp     puts16
.puts_done:
    ret

/* ---------------------------------------------------------------------------
 * 数据区
 * ------------------------------------------------------------------------- */
dap:
    .byte   0x10, 0x00
    .word   STAGE2_SECS
    .word   STAGE2_ADDR
    .word   0x0000
dap_lba:
    .long   0
    .long   0

boot_drive: .byte 0
err_code:   .byte 0
chs_left:   .word 0
spt:        .word 0
heads:      .word 0

msg_boot:
    .asciz  "XOS\r\n"
msg_err:
    .asciz  "ERR "
msg_crlf:
    .asciz  "\r\n"

/* 完整性校验值（偏移 444..445）：由构建脚本按偏移 0..443 的字节累加和写入 */
.org 444
mbr_sum:
    .word   0

/* 分区表区（偏移 446..509）：由构建脚本写入实际分区项 */
.org 446
pt_area:
    .org 510
/* 引导签名（偏移 510..511） */
sig_word:
    .word   0xAA55
