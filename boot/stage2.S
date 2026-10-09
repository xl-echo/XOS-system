/* ============================================================================
 * XOS Stage2 第二阶段引导
 * 职责：串口初始化 → 开启并验证 A20 → E820 内存探测 → 条目排序/合并 →
 *       内存保留区标记 → 加载内核 → 校验内核签名（失败回退备用副本）
 *       → 建立 GDT → 进入保护模式 → 跳转内核
 *
 * 本版新增（相对上一版）：
 *   1) 串口早期调试输出：COM1(0x3F8) 115200 8N1，引导全过程镜像到串口，
 *      便于无头环境取证；串口不可用时静默跳过，不影响引导
 *   2) 引导日志缓冲：0x0D00 起 32 条环形记录，每条含阶段号/结果/BIOS tick
 *   3) 引导阶段计时与统计：用 BIOS tick(0x40:0x6C) 记录各阶段耗时
 *   4) 内存映射条目排序与合并：按基址升序排序，同类型相邻条目合并
 *   5) 内存保留区标记：把 MBR/Stage2/内核/E820 缓冲区/EBDA 标为保留
 *   6) 引导失败回退：内核签名校验失败时改读备用副本 LBA 再试一次
 *
 * 保留能力：A20 内存回绕验证、E820 结果签名、磁盘读重试与控制器复位、
 *           AH=88h 回退探测（带 64MB 钳位）
 *
 * 低内存布局（全部标为保留，内核不得占用）：
 *   0x0000-0x04FF  BIOS 中断向量表与数据区
 *   0x04F0-0x04F3  E820 条目数（u16）
 *   0x04F4-0x04F7  E820 签名 'XOSE'（u32）
 *   0x0500-0x0AFF  E820 条目数组（64 槽 × 24 字节）
 *   0x0D00-0x0E07  引导日志缓冲（magic + count + 32 × 8 字节）
 *   0x0E10-0x0E3F  引导统计块（magic + 10 × u32）
 * ============================================================================ */
.code16
.section .text
.globl _start

.equ KERNEL_SEG,   0x1000        /* 0x1000:0x0000 = 0x10000 */
.equ KERNEL_LBA,   33
.equ KERNEL_SECS,  772           /* 自动同步：内核 386748 B -> 772 扇区 */           /* 自动同步：内核 382652 B -> 764 扇区 */           /* 自动同步：内核 378556 B -> 756 扇区 */           /* 自动同步：内核 374440 B -> 748 扇区 */           /* 自动同步：内核 370344 B -> 740 扇区 */           /* 自动同步：内核 366248 B -> 732 扇区 */           /* 自动同步：内核 362152 B -> 724 扇区 */           /* 自动同步：内核 358024 B -> 716 扇区 */           /* 自动同步：内核 353928 B -> 708 扇区 */           /* 当前内核 692 扇区(353928B)+余量；build.py 会在内核扩容时自动同步此值 */
.equ BKUP_LBA,     1057          /* 备用内核副本 LBA（与 build.py BKUP_LBA 一致），签名损坏时回退 */
.equ CHUNK_SECS,   32
.equ MAX_RETRY,    3

.equ E820_BUF,     0x0500
.equ E820_CNT,     0x04F0        /* u16 条目数 */
.equ E820_MAGIC,   0x04F4        /* u32 签名，供内核校验 */
.equ E820_MAX,     56            /* 预留 8 槽给保留区条目 */
.equ E820_USABLE,  1
.equ E820_RESERVED, 2
.equ E820_SIGVAL,  0x584F5345    /* 'XOSE' */
.equ E820_ENT,     24            /* 单条记录长度 */

.equ LOG_BASE,     0x0D00
.equ LOG_MAGIC,    0x584F534C    /* 'XOSL' */
.equ LOG_MAX,      32

.equ STAT_BASE,    0x0E10
.equ STAT_MAGIC,   0x584F5354    /* 'XOST' */

.equ KERNEL_MAGIC_ADDR, 0x10010  /* entry.S 放置签名的固定偏移（Multiboot 头后） */
.equ KERNEL_SIGVAL,     0x584F534B  /* 'XOSK' */
.equ KERNEL_MBADDR,     0x10004     /* Multiboot 头 magic 位置 */
.equ MB_MAGIC,          0x1BADB002
.equ MB_CHECKSUM,       0xE4524FFE  /* -(0x1BADB002 + 0) */

/* 引导配置块 0x0D40（16B）：magic 'CFG1' + u8 超时秒 + u8 默认项 + u8 本次选择 + pad + u32 选择时刻 tick */
.equ CFG_BASE,    0x0D40
.equ CFG_MAGIC,   0x31474643      /* 'CFG1' */
.equ MENU_TIMEOUT, 5              /* 默认 5 秒超时 */
.equ MENU_DEFAULT, 1              /* 默认引导项 1 = 主副本 */
.equ MENU_CH_BKUP, 2              /* 选择项 2 = 强制备用副本 */

/* 保护模式阶段：页目录基址 0x6000（低内存空闲区，4KB，1024 项 4MB 大页 identity map） */
.equ PGT_BASE,    0x6000
/* 数据搬运目标区：统计块副本 0x0A00（20B，与 0x0E10 原始区互为校验） */
.equ COPY_DST,    0x0A00

/* 低端保留区上界：与内核自身声明的占用上限 0x80000 一致
   （内核映像+静态数据+.bss 已随堆分配器等模块增长超过 0x40000） */
.equ RSRV_TOP,     0x80000

_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    $0x9000, %ax
    movw    %ax, %ss
    movw    $0xFFFC, %sp
    sti

    /* 保存 BIOS 传入的启动盘号（DL）。E820 探测会改写 EDX，
     * 若不保存将导致后续 INT13h 使用无效驱动器号。 */
    movb    %dl, boot_drive

    call    serial_init
    call    cpu_detect
    call    log_init
    call    stat_init
    call    tick_now
    movl    %eax, stat_start

    movw    $msg_s2, %si
    call    puts_dual

    /* ---- 读取 MBR 交接槽（0x7DFC=盘号副本 / 0x7DFD=引导源标志），打印契约 ---- */
    movw    $0x7DFC, %bx
    movw    $msg_bootdrv, %si
    call    puts_dual
    movzbl  (%bx), %eax
    call    print_dec
    movw    $msg_bootsrc, %si
    call    puts_dual
    movzbl  1(%bx), %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_dual

    /* ---- 1) 开启并验证 A20（必须在 E820 之前，0x0500 尚空闲） ---- */
    call    enable_a20
    call    verify_a20
    testw   %ax, %ax
    jz      .a20_bad
    movw    $1, %ax
    xorw    %bx, %bx                 /* verify_a20 返回 1=成功，日志约定 0=成功 */
    call    log_event
    call    tick_now
    movl    %eax, stat_a20
    jmp     .a20_ok
.a20_bad:
    movw    $1, %ax
    movw    $1, %bx
    call    log_event
    jmp     .a20_failed
.a20_ok:

    /* ---- 2) E820 内存探测 ---- */
    call    detect_e820
    call    tick_now
    movl    %eax, stat_e820
    movw    $2, %ax
    movw    $0, %bx
    call    log_event

    /* ---- 3) 条目排序 ---- */
    call    sort_entries
    call    tick_now
    movl    %eax, stat_sort
    movw    $3, %ax
    movw    $0, %bx
    call    log_event

    /* ---- 4) 同类型相邻条目合并 ---- */
    call    merge_entries
    movw    $4, %ax
    movw    $0, %bx
    call    log_event

    /* ---- 5) 内存保留区标记 ---- */
    call    mark_reserved
    movw    $5, %ax
    movw    $0, %bx
    call    log_event

    /* ---- 5.5) 引导菜单与超时（键盘 INT16h + BIOS tick 超时） ---- */
    call    boot_menu
    /* 选择项 2 强制走备用副本 */
    cmpb    $MENU_CH_BKUP, cfg_choose
    jne     .m_load_primary
    movw    $msg_fbkup, %si
    call    puts_dual
    movl    $BKUP_LBA, %eax
    call    load_kernel
    call    verify_kernel
    testw   %ax, %ax
    jz      .kern_failed
    jmp     .kern_ok
.m_load_primary:
    /* ---- 6) 加载内核映像 ---- */
    movl    $KERNEL_LBA, %eax
    call    load_kernel
    call    tick_now
    movl    %eax, stat_kernel
    movw    $6, %ax
    movw    $0, %bx
    call    log_event

    /* ---- 7) 校验内核签名，失败则回退到备用副本 ---- */
    call    verify_kernel
    testw   %ax, %ax
    jnz     .kern_ok
    movw    $7, %ax
    movw    $1, %bx
    call    log_event
    movw    $msg_kbak, %si
    call    puts_dual
    movl    $BKUP_LBA, %eax
    call    load_kernel
    call    verify_kernel
    testw   %ax, %ax
    jz      .kern_failed

.kern_ok:
    movw    $8, %ax
    movw    $0, %bx
    call    log_event
    call    print_summary
    call    dump_log
    call    print_stats

    /* ---- 8) 进入保护模式 ---- */
    movw    $9, %ax
    movw    $0, %bx
    call    log_event
    call    tick_now
    movl    %eax, stat_end
    call    stat_store

    cli
    lgdt    gdt_desc
    movl    %cr0, %eax
    orl     $0x00000001, %eax
    movl    %eax, %cr0
    ljmp    $0x08, $pm_entry

.a20_failed:
    movw    $msg_a20err, %si
    call    puts_dual
    jmp     .halt
.kern_failed:
    movw    $msg_ksig, %si
    call    puts_dual
.halt:
    hlt
    jmp     .halt

/* ---------------------------------------------------------------------------
 * 保护模式入口：装载平坦数据段，建立内核栈，跳转内核
 * ------------------------------------------------------------------------- */
.code32
pm_entry:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $0x9FFFC, %esp

    /* ---- 页表初始化：4MB 大页 identity map @0x6000（1024 项覆盖 0..4GB） ---- */
    movl    $PGT_BASE, %edi
    xorl    %ecx, %ecx
.pgt_loop:
    movl    %ecx, %eax
    shll    $22, %eax
    orl     $0x00000083, %eax      /* P|RW|PS */
    movl    %eax, (%edi, %ecx, 4)
    incl    %ecx
    cmpl    $1024, %ecx
    jb      .pgt_loop
    movl    $PGT_BASE, %eax
    movl    %eax, %cr3
    movl    %cr4, %eax
    orl     $0x00000010, %eax      /* CR4.PSE：允许 4MB 大页 */
    movl    %eax, %cr4
    movl    %cr0, %eax
    orl     $0x80000000, %eax      /* 开启分页 PG */
    movl    %eax, %cr0
    movw    $msg_pgt, %si
    call    pm_serial_puts
    movl    $1024, %eax
    call    pm_serial_dec
    movw    $msg_crlf, %si
    call    pm_serial_puts

    /* ---- 实模式→保护模式数据搬运：统计块 0x0E10 → 0x0A00（magic+10 u32） ---- */
    movl    $STAT_BASE, %esi
    movl    $COPY_DST, %edi
    xorl    %ecx, %ecx
    xorl    %ebx, %ebx
.copy_loop:
    movl    (%esi, %ecx, 4), %eax
    movl    %eax, (%edi, %ecx, 4)
    addl    %eax, %ebx
    incl    %ecx
    cmpl    $11, %ecx
    jb      .copy_loop
    xorl    %ecx, %ecx
    xorl    %edx, %edx
.copy_verify:
    addl    (%edi, %ecx, 4), %edx
    incl    %ecx
    cmpl    $11, %ecx
    jb      .copy_verify
    cmpl    %ebx, %edx
    jne     .copy_fail
    movw    $msg_copy, %si
    call    pm_serial_puts
    jmp     .copy_done
.copy_fail:
    movw    $msg_copyf, %si
    call    pm_serial_puts
.copy_done:
    movl    $0x10000, %eax
    call    *%eax
1:  hlt
    jmp     1b

/* pm_serial_puts：保护模式串口输出（COM1 0x3F8 轮询），SI=NUL 结尾字符串 */
pm_serial_puts:
    pusha
.pm_ps_loop:
    lodsb
    testb   %al, %al
    jz      .pm_ps_done
    movb    %al, %cl
    movw    $0x3FD, %dx
.pm_ps_wait:
    inb     %dx, %al
    testb   $0x20, %al
    jz      .pm_ps_wait
    movw    $0x3F8, %dx
    movb    %cl, %al
    outb    %al, %dx
    jmp     .pm_ps_loop
.pm_ps_done:
    popa
    ret

/* pm_serial_dec：保护模式串口十进制输出，EAX=值 */
pm_serial_dec:
    pusha
    movl    %eax, %esi
    movl    $10, %ecx
    xorl    %edi, %edi
.pm_dec_div:
    movl    %esi, %eax
    xorl    %edx, %edx
    divl    %ecx
    movl    %eax, %esi
    pushl   %edx
    incl    %edi
    testl   %esi, %esi
    jnz     .pm_dec_div
.pm_dec_out:
    popl    %edx
    addb    $48, %dl
    movb    %dl, %bh
    movw    $0x3FD, %dx
.pm_dec_wait:
    inb     %dx, %al
    testb   $0x20, %al
    jz      .pm_dec_wait
    movw    $0x3F8, %dx
    movb    %bh, %al
    outb    %al, %dx
    decl    %edi
    jnz     .pm_dec_out
    popa
    ret

.code16
/* ===========================================================================
 * 串口：COM1 0x3F8，115200 8N1
 * =========================================================================== */
serial_init:
    pushw   %ax
    pushw   %dx
    movw    $0x3FB, %dx          /* LCR：置 DLAB */
    movb    $0x80, %al
    outb    %al, %dx
    movw    $0x3F8, %dx          /* DLL = 1 → 115200 */
    movb    $0x01, %al
    outb    %al, %dx
    movw    $0x3F9, %dx          /* DLM = 0 */
    xorw    %ax, %ax
    outb    %al, %dx
    movw    $0x3FB, %dx          /* 8 位、无校验、1 停止位 */
    movb    $0x03, %al
    outb    %al, %dx
    movw    $0x3FA, %dx          /* FCR：使能并清空 FIFO */
    movb    $0xC7, %al
    outb    %al, %dx
    movw    $0x3FC, %dx          /* MCR：DTR|RTS|OUT2 */
    movb    $0x0B, %al
    outb    %al, %dx
    movw    $0x3F9, %dx          /* IER：关中断，纯轮询 */
    xorw    %ax, %ax
    outb    %al, %dx
    popw    %dx
    popw    %ax
    ret

/* serial_putc：AL = 字符；发送前轮询 LSR bit5（THR 空） */
serial_putc:
    pushw   %dx
    pushw   %ax
    movw    $0x3FD, %dx
.ser_wait:
    inb     %dx, %al
    testb   $0x20, %al
    jz      .ser_wait
    movw    $0x3F8, %dx
    popw    %ax
    outb    %al, %dx
    popw    %dx
    ret

/* puts_serial：SI = NUL 结尾字符串 */
puts_serial:
    pushw   %ax
    pushw   %si
.ps_loop:
    lodsb
    testb   %al, %al
    jz      .ps_done
    call    serial_putc
    jmp     .ps_loop
.ps_done:
    popw    %si
    popw    %ax
    ret

/* puts_dual：同时输出到屏幕（INT 10h）与串口 */
puts_dual:
    pushw   %si
    call    puts16
    popw    %si
    pushw   %si
    call    puts_serial
    popw    %si
    ret

/* ===========================================================================
 * 引导日志缓冲：0x0D00 起，magic(4) + count(2) + pad(2) + 32 × 8 字节
 * 记录格式：阶段号(u16) 结果(u16) BIOS tick(u32)
 * =========================================================================== */
log_init:
    pushw   %ax
    pushw   %di
    movw    $LOG_BASE, %di
    movl    $LOG_MAGIC, (%di)
    movw    $0, 4(%di)
    movw    $0, 6(%di)
    popw    %di
    popw    %ax
    ret

/* log_event：AL = 阶段号，BX = 结果（0 成功 / 1 失败） */
log_event:
    pusha
    pushw   %es
    movw    $0x0000, %dx
    movw    %dx, %es
    movw    LOG_BASE + 4, %di
    cmpw    $LOG_MAX, %di
    jae     .log_full
    movw    %di, %si
    shlw    $3, %si              /* ×8 */
    addw    $LOG_BASE + 8, %si
    movw    %ax, (%si)
    movw    %bx, 2(%si)
    /* BIOS tick 计数器位于 0040:006C，经 DS 寻址 —— 必须改 DS 而非 ES，
     * 且改段后须先恢复 DS 再用 SI 写回，否则会写到 0040 段里去 */
    pushw   %ds
    movw    $0x0040, %dx
    movw    %dx, %ds
    movw    0x006C, %dx
    popw    %ds
    movw    %dx, 4(%si)
    incw    %di
    movw    %di, LOG_BASE + 4
.log_full:
    popw    %es
    popa
    ret

/* ===========================================================================
 * 计时与统计
 *   cpu_detect：探测 CPUID 与 TSC 支持（EFLAGS.ID 位可翻转 → 有 CPUID；
 *               CPUID.01H:EDX bit4 → 有 TSC）
 *   tick_now  ：优先返回 TSC 低 32 位（微秒级分辨率），否则回退 BIOS tick
 *               计数器（18.2 Hz，分辨率约 55 ms）
 * =========================================================================== */
cpu_detect:
    pusha
    movb    $0, tsc_ok
    pushfl
    popl    %eax
    movl    %eax, %ebx
    xorl    $0x00200000, %eax        /* 翻转 ID 位（bit 21） */
    pushl   %eax
    popfl
    pushfl
    popl    %eax
    pushl   %ebx
    popfl                            /* 还原原始 EFLAGS */
    cmpl    %ebx, %eax
    je      .cd_done                 /* ID 位不可翻转 → 无 CPUID */
    movl    $1, %eax
    xorl    %ecx, %ecx
    cpuid
    testl   $0x00000010, %edx        /* EDX bit4 = TSC */
    jz      .cd_done
    movb    $1, tsc_ok
.cd_done:
    popa
    ret

tick_now:
    pushw   %ds
    cmpb    $0, tsc_ok
    je      .tn_bios
    rdtsc                            /* EDX:EAX = TSC，取低 32 位 */
    jmp     .tn_done
.tn_bios:
    movw    $0x0040, %ax
    movw    %ax, %ds
    movl    0x006C, %eax
.tn_done:
    popw    %ds
    ret

stat_init:
    pushw   %ax
    pushw   %di
    movw    $STAT_BASE, %di
    movl    $STAT_MAGIC, (%di)
    movl    $0, stat_merged
    movl    $0, stat_rsrv
    popw    %di
    popw    %ax
    ret

stat_store:
    pushw   %ax
    pushw   %di
    movw    $STAT_BASE + 4, %di
    movl    stat_start, %eax
    movl    %eax, (%di)
    movl    stat_a20, %eax
    movl    %eax, 4(%di)
    movl    stat_e820, %eax
    movl    %eax, 8(%di)
    movl    stat_sort, %eax
    movl    %eax, 12(%di)
    movl    stat_kernel, %eax
    movl    %eax, 16(%di)
    movl    stat_end, %eax
    movl    %eax, 20(%di)
    movw    E820_CNT, %ax
    movzwl  %ax, %eax
    movl    %eax, 24(%di)        /* 合并后条目数 */
    movl    stat_rsrv, %eax
    movl    %eax, 28(%di)        /* 追加的保留区条目数 */
    movl    stat_merged, %eax
    movl    %eax, 32(%di)        /* 合并掉的条目数 */
    popw    %di
    popw    %ax
    ret

/* ===========================================================================
 * print_summary：把统计结果打到屏幕与串口
 * =========================================================================== */
/* ===========================================================================
 * boot_menu：引导菜单与超时（键盘 INT16h + BIOS tick）
 * 流程：打印提示 → 建配置块(0x0D40) → 取起点 tick → 轮询按键：
 *   '1'=主副本(默认) '2'=强制备用副本 '3'=打印内存信息后走默认
 *   其余键/超时(5s)=默认；结果写 cfg_choose 与配置块
 * =========================================================================== */
boot_menu:
    pusha
    pushw   %es
    movw    $msg_menu, %si
    call    puts_dual
    movw    $CFG_BASE, %di
    movl    $CFG_MAGIC, %eax
    movl    %eax, (%di)
    movb    $MENU_TIMEOUT, 1(%di)
    movb    $MENU_DEFAULT, 2(%di)
    movb    $MENU_DEFAULT, 3(%di)
    movw    $0x0040, %ax
    movw    %ax, %es
    movw    %es:0x6C, %bx
.menu_wait:
    movb    $0x01, %ah
    int     $0x16
    jz      .menu_tick
    movb    $0x00, %ah
    int     $0x16
    cmpb    $'1', %al
    je      .menu_pick1
    cmpb    $'2', %al
    je      .menu_pick2
    cmpb    $'3', %al
    je      .menu_pick3
    jmp     .menu_timeout
.menu_pick1:
    movb    $1, %al
    jmp     .menu_set
.menu_pick2:
    movb    $MENU_CH_BKUP, %al
    jmp     .menu_set
.menu_pick3:
    movw    $msg_minfo, %si
    call    puts_dual
    jmp     .menu_timeout
.menu_set:
    movb    %al, 3(%di)
    jmp     .menu_done
.menu_tick:
    movw    %es:0x6C, %ax
    subw    %bx, %ax
    cmpw    $92, %ax
    jb      .menu_wait
.menu_timeout:
    movb    $MENU_DEFAULT, 3(%di)
.menu_done:
    movb    3(%di), %al
    movb    %al, cfg_choose
    movw    %es:0x6C, %ax
    movw    %ax, 6(%di)
    movw    $msg_mdone, %si
    call    puts_dual
    movzbl  cfg_choose, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_dual
    popw    %es
    popa
    ret

print_summary:
    movw    $msg_e820n, %si
    call    puts_dual
    movw    E820_CNT, %ax
    movzwl  %ax, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_dual

    movw    $msg_mergn, %si
    call    puts_dual
    movl    stat_merged, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_dual

    movw    $msg_rsrvn, %si
    call    puts_dual
    movl    stat_rsrv, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_dual

    movw    $msg_tickn, %si
    call    puts_dual
    call    tick_now
    movl    %eax, %ebx
    movl    stat_start, %eax
    movl    %ebx, %ecx
    subl    %eax, %ecx           /* 总耗时 tick */
    movl    %ecx, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_dual
    ret

/* print_dec：EAX 以十进制输出到屏幕与串口 */
print_dec:
    pusha
    xorw    %cx, %cx             /* 已压栈的位数 */
    movl    $10, %esi
.pd_div:
    xorl    %edx, %edx
    divl    %esi                 /* EAX = 商, EDX = 余数 */
    pushw   %dx
    incw    %cx
    testl   %eax, %eax
    jnz     .pd_div
.pd_out:
    popw    %ax
    addb    $48, %al
    call    serial_putc
    movb    $0x0E, %ah
    pushw   %ax
    int     $0x10
    popw    %ax
    decw    %cx
    jnz     .pd_out
    popa
    ret

/* put_space：串口输出一个空格 */
put_space:
    pushw   %ax
    movb    $32, %al
    call    serial_putc
    popw    %ax
    ret

/* dump_log：把引导日志缓冲逐条输出到串口，用于无头环境取证 */
dump_log:
    pusha
    movw    $0x0000, %ax
    movw    %ax, %es
    movw    LOG_BASE + 4, %cx
    testw   %cx, %cx
    jz      .dl_done
    movw    $LOG_BASE + 8, %di
.dl_loop:
    movw    $msg_log, %si
    call    puts_serial
    movw    (%di), %ax
    movzwl  %ax, %eax
    call    print_dec
    movw    $msg_log2, %si
    call    puts_serial
    movw    2(%di), %ax
    movzwl  %ax, %eax
    call    print_dec
    movw    $msg_log3, %si
    call    puts_serial
    movw    4(%di), %ax
    movzwl  %ax, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_serial
    addw    $8, %di
    decw    %cx
    jnz     .dl_loop
.dl_done:
    popa
    ret

/* print_stats：输出各阶段 BIOS tick，验证统计块内容 */
print_stats:
    movw    $msg_tickhdr, %si
    call    puts_serial
    movl    stat_start, %eax
    call    print_dec
    call    put_space
    movl    stat_a20, %eax
    call    print_dec
    call    put_space
    movl    stat_e820, %eax
    call    print_dec
    call    put_space
    movl    stat_sort, %eax
    call    print_dec
    call    put_space
    movl    stat_kernel, %eax
    call    print_dec
    movw    $msg_crlf2, %si
    call    puts_serial
    ret

/* ===========================================================================
 * enable_a20：通过端口 0x92 快速开启 A20 地址线
 * =========================================================================== */
enable_a20:
    pusha
    inb     $0x92, %al
    orb     $0x02, %al
    andb    $0xFE, %al
    outb    %al, $0x92
    popa
    ret

/* ---------------------------------------------------------------------------
 * verify_a20：用内存回绕测试确认 A20 地址线已真正开启
 * 返回：AX = 1 表示已开启，0 表示仍关闭
 * ------------------------------------------------------------------------- */
verify_a20:
    pushw   %ds
    pushw   %es
    pushw   %si
    pushw   %bx
    pushw   %dx

    movw    $0x0000, %ax
    movw    %ax, %ds
    movw    %ax, %es

    movw    0x0500, %bx              /* 保存低地址原值 */

    movw    $0xFFFF, %ax
    movw    %ax, %es
    movw    $0x0510, %si
    movw    %es:(%si), %dx           /* 保存高地址原值 */

    movw    $0x0000, %ax
    movw    %ax, %es
    movw    $0xAAAA, 0x0500

    movw    $0xFFFF, %ax
    movw    %ax, %es
    movw    $0x0510, %si
    movw    $0x5555, %es:(%si)

    movw    $0x0000, %ax
    movw    %ax, %es
    movw    0x0500, %ax
    cmpw    $0xAAAA, %ax
    jne     .va20_off

    movw    %bx, 0x0500
    movw    $0xFFFF, %ax
    movw    %ax, %es
    movw    $0x0510, %si
    movw    %dx, %es:(%si)
    movw    $1, %ax
    jmp     .va20_done

.va20_off:
    movw    %bx, 0x0500
    xorw    %ax, %ax

.va20_done:
    popw    %dx
    popw    %bx
    popw    %si
    popw    %es
    popw    %ds
    ret

/* ---------------------------------------------------------------------------
 * verify_kernel：校验内核映像签名（entry.S 固定放在 0x10004）
 * 返回：AX = 1 校验通过，0 失败
 * ------------------------------------------------------------------------- */
verify_kernel:
    pushw   %es
    movw    $0x1000, %ax
    movw    %ax, %es
    /* Multiboot 头校验：magic(0x10004) + checksum(0x1000C) */
    movl    %es:4, %eax
    cmpl    $MB_MAGIC, %eax
    jne     .vk_bad
    movl    %es:12, %eax
    cmpl    $MB_CHECKSUM, %eax
    jne     .vk_bad
    /* XOS 签名校验：0x10010 */
    movl    %es:16, %eax
    cmpl    $KERNEL_SIGVAL, %eax
    jne     .vk_bad
    popw    %es
    movw    $1, %ax
    ret
.vk_bad:
    popw    %es
    xorw    %ax, %ax
    ret

/* ===========================================================================
 * detect_e820：INT 15h E820 内存探测
 * =========================================================================== */
detect_e820:
    pusha
    movw    $0x0000, %ax
    movw    %ax, %es
    xorl    %ebx, %ebx
    movw    $E820_BUF, %di
    xorl    %ebp, %ebp

.e820_next:
    movl    $0xE820, %eax
    movl    $24, %ecx
    movl    $0x534D4150, %edx        /* 'SMAP' */
    int     $0x15
    jc      .e820_done
    cmpl    $0x534D4150, %eax
    jne     .e820_done
    testl   %ecx, %ecx
    jz      .e820_done

    incl    %ebp
    cmpl    $E820_MAX, %ebp
    jge     .e820_done

    addw    $24, %di
    testl   %ebx, %ebx
    jnz     .e820_next

.e820_done:
    testl   %ebp, %ebp
    jnz     .e820_store
    call    detect_fallback
    popa
    ret

.e820_store:
    movw    $E820_CNT, %di
    movw    %bp, (%di)
    movw    $E820_MAGIC, %di
    movl    $E820_SIGVAL, (%di)
    popa
    ret

/* ---------------------------------------------------------------------------
 * detect_fallback：E820 失败时用 INT 15h AH=88h 获取 1MB 以上内存量
 * 说明：返回值以 KB 为单位且上限 65535 KB（约 64MB），超过则按 64MB 钳位
 * ------------------------------------------------------------------------- */
detect_fallback:
    pusha
    movw    $0x0000, %ax
    movw    %ax, %es
    movb    $0x88, %ah
    int     $0x15
    jc      .fb_fail
    movzwl  %ax, %eax
    testl   %eax, %eax
    jz      .fb_fail
    cmpl    $0xFFFF, %eax
    jbe     .fb_ok
    movl    $0xFFFF, %eax
.fb_ok:
    movw    $E820_BUF, %di
    movl    $0x00000000, (%di)
    movl    $0x00000000, 4(%di)
    movl    $0x000A0000, 8(%di)
    movl    $0x00000000, 12(%di)
    movl    $E820_USABLE, 16(%di)
    movl    $0, 20(%di)

    addw    $24, %di
    movl    $0x00100000, (%di)
    movl    $0x00000000, 4(%di)
    shll    $10, %eax
    movl    %eax, 8(%di)
    movl    $0x00000000, 12(%di)
    movl    $E820_USABLE, 16(%di)
    movl    $0, 20(%di)

    movw    $E820_CNT, %di
    movw    $2, (%di)
    movw    $E820_MAGIC, %di
    movl    $E820_SIGVAL, (%di)
    popa
    ret

.fb_fail:
    movw    $E820_CNT, %di
    movw    $0, (%di)
    popa
    ret

/* ===========================================================================
 * sort_entries：按基址（64 位）升序冒泡排序 E820 条目
 * 破坏：AX BX CX DX SI DI
 * =========================================================================== */
sort_entries:
    pusha
    pushw   %es
    movw    $0x0000, %ax
    movw    %ax, %es
    movw    E820_CNT, %cx
    cmpw    $2, %cx
    jb      .sort_done
    decw    %cx

.sort_outer:
    pushw   %cx
    movw    $E820_BUF, %si
.sort_inner:
    movl    4(%si), %eax             /* 后项基址高 32 位 */
    movl    4+E820_ENT(%si), %ebx
    cmpl    %ebx, %eax
    jb      .sort_next
    ja      .sort_swap
    movl    (%si), %eax              /* 高 32 位相等，比低 32 位 */
    movl    E820_ENT(%si), %ebx
    cmpl    %ebx, %eax
    jbe     .sort_next

.sort_swap:
    movw    $6, %bp                  /* 24 字节 = 6 个 dword */
.swap_loop:
    movl    (%si), %eax
    movl    E820_ENT(%si), %ebx
    movl    %ebx, (%si)
    movl    %eax, E820_ENT(%si)
    addw    $4, %si
    decw    %bp
    jnz     .swap_loop
    subw    $24, %si                 /* 回到本条起点 */

.sort_next:
    addw    $E820_ENT, %si
    loop    .sort_inner
    popw    %cx
    loop    .sort_outer

.sort_done:
    popw    %es
    popa
    ret

/* ===========================================================================
 * merge_entries：合并同类型且相邻/重叠的条目
 * 仅合并基址与长度的高 32 位均为 0 的条目（4GB 以内），
 * 高位非零的条目（4GB 以上内存）保持原样不参与合并。
 * 破坏：AX BX CX DX SI DI BP
 * =========================================================================== */
merge_entries:
    pusha
    pushw   %es
    movw    $0x0000, %ax
    movw    %ax, %es
    movw    E820_CNT, %cx
    cmpw    $2, %cx
    jb      .mg_done
    decw    %cx                  /* 比较次数 = 条目数 - 1 */
    movw    $E820_BUF, %si

.mg_loop:
    movl    4(%si), %eax             /* 本项基址高 32 位 */
    orl     E820_ENT+4(%si), %eax    /* 或上后项基址高 32 位 */
    orl     12(%si), %eax            /* 或上本项长度高 32 位 */
    orl     E820_ENT+12(%si), %eax   /* 或上后项长度高 32 位 */
    testl   %eax, %eax
    jnz     .mg_advance              /* 高位非零 → 不参与合并 */

    movl    16(%si), %eax            /* 本项类型 */
    cmpl    E820_ENT+16(%si), %eax   /* 后项类型 */
    jne     .mg_advance

    /* 本项基址 + 本项长度 >= 后项基址 ？ */
    movl    (%si), %eax
    addl    8(%si), %eax
    cmpl    E820_ENT(%si), %eax
    jb      .mg_advance

    /* 合并：本项长度 = max(本项末, 后项末) - 本项基址 */
    movl    E820_ENT(%si), %ebx
    addl    E820_ENT+8(%si), %ebx    /* 后项末 */
    cmpl    %ebx, %eax
    jae     .mg_keep
    movl    %ebx, %eax
.mg_keep:
    subl    (%si), %eax
    movl    %eax, 8(%si)
    movl    $0, 12(%si)

    /* 删除后项：把其后的条目整体前移 24 字节
     * 需移动的条目数 = count - i - 1，其中 i = (SI - E820_BUF) / 24 */
    movw    %si, %ax
    subw    $E820_BUF, %ax
    movw    $E820_ENT, %bx
    xorw    %dx, %dx
    divw    %bx                      /* AX = i */
    movw    E820_CNT, %bp
    subw    %ax, %bp
    decw    %bp                      /* BP = 需移动条目数 */

    movw    %si, %di
    addw    $E820_ENT, %di           /* DI 指向被删项 */
    testw   %bp, %bp
    jz      .mg_shifted
.mg_shift:
    movw    %di, %bx
    addw    $E820_ENT, %bx           /* BX 指向来源 */
    movl    (%bx), %eax
    movl    %eax, (%di)
    movl    4(%bx), %eax
    movl    %eax, 4(%di)
    movl    8(%bx), %eax
    movl    %eax, 8(%di)
    movl    12(%bx), %eax
    movl    %eax, 12(%di)
    movl    16(%bx), %eax
    movl    %eax, 16(%di)
    movl    20(%bx), %eax
    movl    %eax, 20(%di)
    addw    $E820_ENT, %di
    decw    %bp
    jnz     .mg_shift

.mg_shifted:
    movw    E820_CNT, %ax
    decw    %ax
    movw    %ax, E820_CNT
    movl    stat_merged, %eax
    incl    %eax
    movl    %eax, stat_merged
    decw    %cx                      /* 条目少一条，剩余比较次数减一 */
    testw   %cx, %cx
    jg      .mg_loop                 /* SI 不动，原地再比 */
    jmp     .mg_done

.mg_advance:
    addw    $E820_ENT, %si
    decw    %cx
    testw   %cx, %cx
    jg      .mg_loop

.mg_done:
    popw    %es
    popa
    ret

/* ===========================================================================
 * mark_reserved：把低端保留区 [0, RSRV_TOP) 从可用区间中切除并标为保留
 *
 * 内核侧的契约（e820_selftest / e820_validate）要求映射表满足：
 *   ① 基址单调不减 ② 任何类型都不得重叠 ③ 不得有零长度条目
 * 因此不能简单追加保留条目（必然与可用条目重叠），必须先裁剪再追加。
 *
 * RSRV_TOP 取 0x80000，与内核自身声明的占用上限一致，覆盖：
 *   0x00000-0x00FFF  IVT/BDA/E820 缓冲/日志/统计块
 *   0x07C00-0x09E00  MBR 与 Stage2 自身
 *   0x10000-0x80000  内核映像与 .bss
 * =========================================================================== */
mark_reserved:
    pusha
    pushw   %es
    movw    $0x0000, %ax
    movw    %ax, %es
    movl    $0, stat_rsrv

    /* ---- 1. 裁剪可用条目 ---- */
    movw    E820_CNT, %cx
    movw    $E820_BUF, %si
.mr_scan:
    movl    16(%si), %eax            /* 类型 */
    cmpl    $E820_USABLE, %eax
    jne     .mr_next
    movl    4(%si), %eax             /* 基址高 32 位 */
    testl   %eax, %eax
    jnz     .mr_next                 /* 4GB 以上，不处理 */
    movl    (%si), %eax              /* 基址低 32 位 */
    cmpl    $RSRV_TOP, %eax
    jae     .mr_next                 /* 基址已在保留区之上 */

    movl    8(%si), %ebx             /* 长度低 32 位 */
    movl    12(%si), %edx            /* 长度高 32 位 */
    addl    %eax, %ebx               /* EBX = 末端低 32 位 */
    adcl    $0, %edx                 /* EDX = 末端高 32 位 */

    testl   %edx, %edx
    jnz     .mr_clip                 /* 末端远高于 4GB，必有尾部 */
    cmpl    $RSRV_TOP, %ebx
    ja      .mr_clip                 /* 末端越过保留区，保留尾部 */

    /* 整条落在保留区内 → 改判为保留类型，稍后与保留条目合并 */
    movl    $E820_RESERVED, 16(%si)
    jmp     .mr_next

.mr_clip:
    /* 保留尾部：基址上移到 RSRV_TOP，长度相应缩短 */
    movl    $RSRV_TOP, (%si)
    movl    $0, 4(%si)
    movl    %ebx, %eax
    subl    $RSRV_TOP, %eax
    movl    %eax, 8(%si)
    movl    $0, 12(%si)

.mr_next:
    addw    $E820_ENT, %si
    decw    %cx
    jnz     .mr_scan

    /* ---- 2. 追加保留区条目 [0, RSRV_TOP) ---- */
    movw    E820_CNT, %ax
    cmpw    $63, %ax
    ja      .mr_done                 /* 缓冲区已满，放弃追加 */
    movw    $E820_ENT, %cx
    mulw    %cx                      /* DX:AX = count × 24 */
    movw    %ax, %di
    addw    $E820_BUF, %di
    movl    $0, (%di)
    movl    $0, 4(%di)
    movl    $RSRV_TOP, 8(%di)
    movl    $0, 12(%di)
    movl    $E820_RESERVED, 16(%di)
    movl    $0, 20(%di)
    movw    E820_CNT, %ax
    incw    %ax
    movw    %ax, E820_CNT
    movl    $1, stat_rsrv

    /* ---- 3. 重新排序并合并同类型相邻条目 ---- */
    call    sort_entries
    call    merge_entries

.mr_done:
    popw    %es
    popa
    ret

/* ===========================================================================
 * load_kernel：分批读取内核映像到 0x10000，每批最多 CHUNK_SECS 扇区
 *   入参：EAX = 起始 LBA
 *   失败时复位磁盘控制器并重试，最多 MAX_RETRY 次
 * =========================================================================== */
load_kernel:
    pusha
    movl    %eax, lk_lba             /* 先保存入参 LBA —— 后续 AX 会被复用 */
    movw    $MAX_RETRY, %bp

.lk_restart:
    movw    $KERNEL_SEG, %ax
    movw    %ax, %es
    xorw    %bx, %bx
    movw    $KERNEL_SECS, %cx
    movl    lk_lba, %eax             /* 每次重试都从入参 LBA 重新开始 */
    movl    %eax, dap_k_lba

.lk_loop:
    movw    %cx, %ax
    cmpw    $CHUNK_SECS, %ax
    jbe     1f
    movw    $CHUNK_SECS, %ax
1:
    movw    %ax, dap_k_count
    movw    %bx, dap_k_off
    movw    %es, dap_k_seg

    movw    $dap_kernel, %si
    movb    boot_drive, %dl
    movb    $0x42, %ah
    int     $0x13
    jnc     2f

    pushw   %ax
    xorw    %ax, %ax
    movb    boot_drive, %dl
    int     $0x13
    popw    %ax
    decw    %bp
    jnz     .lk_restart
    jmp     .lk_err
2:
    movw    dap_k_count, %ax
    subw    %ax, %cx

    movw    dap_k_count, %ax
    shlw    $5, %ax
    movw    %es, %dx
    addw    %ax, %dx
    movw    %dx, %es

    movl    dap_k_lba, %eax
    movzwl  dap_k_count, %edx
    addl    %edx, %eax
    movl    %eax, dap_k_lba

    testw   %cx, %cx
    jnz     .lk_loop
    popa
    ret

.lk_err:
    movw    $msg_kerr, %si
    call    puts_dual
1:  hlt
    jmp     1b

/* ---------------------------------------------------------------------------
 * puts16：打印 NUL 结尾字符串（BIOS INT 10h AH=0Eh）
 * ------------------------------------------------------------------------- */
puts16:
    pushw   %ax
    pushw   %bx
    xorw    %bx, %bx
1:
    lodsb
    testb   %al, %al
    jz      2f
    movb    $0x0E, %ah
    int     $0x10
    jmp     1b
2:
    popw    %bx
    popw    %ax
    ret

/* ===========================================================================
 * 全局描述符表（平坦模型，4GB）
 * =========================================================================== */
.align 8
gdt:
    .quad   0x0000000000000000       /* 0x00 空描述符 */
    .quad   0x00CF9A000000FFFF       /* 0x08 代码段 */
    .quad   0x00CF92000000FFFF       /* 0x10 数据段 */
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1
    .long   gdt

/* ===========================================================================
 * 内核读取用磁盘地址包（字段可被改写）
 * =========================================================================== */
.align 4
dap_kernel:
dap_k_size:
    .byte   0x10
dap_k_rsv:
    .byte   0x00
dap_k_count:
    .word   0
dap_k_off:
    .word   0
dap_k_seg:
    .word   0
dap_k_lba:
    .long   0
    .long   0

/* BIOS 启动盘号（由 _start 从 DL 保存） */
boot_drive:
    .byte   0

/* 阶段计时（BIOS tick）与统计计数 */
stat_start:  .long 0
stat_a20:    .long 0
stat_e820:   .long 0
stat_sort:   .long 0
stat_kernel: .long 0
stat_end:    .long 0
stat_merged: .long 0
stat_rsrv:   .long 0
lk_lba:      .long 0
tsc_ok:      .byte 0     /* 1 = CPU 支持 TSC */

cfg_choose: .byte 0
msg_menu:
    .asciz  "Press 1/2/3 <5s>: "
msg_minfo:
    .asciz  "MEM-INFO\r\n"
msg_mdone:
    .asciz  "MENU CHOOSE="
msg_fbkup:
    .asciz  "FORCE-BACKUP\r\n"
msg_pgt:
    .asciz  "PM PGT=6000 EN="
msg_copy:
    .asciz  "PM COPY=44 OK\r\n"
msg_copyf:
    .asciz  "PM COPY FAIL\r\n"
msg_crlf:
    .asciz  "\r\n"
msg_s2:
    .asciz  "XOS Stage2\r\n"
msg_bootdrv:
    .asciz  "BOOTDRV="
msg_bootsrc:
    .asciz  " SRC="
msg_kerr:
    .asciz  "Kernel load error!\r\n"
msg_a20err:
    .asciz  "A20 gate not enabled!\r\n"
msg_ksig:
    .asciz  "Kernel image signature mismatch!\r\n"
msg_kbak:
    .asciz  "Primary kernel bad, trying backup...\r\n"
msg_e820n:
    .asciz  "E820 entries: "
msg_mergn:
    .asciz  "Merged away : "
msg_rsrvn:
    .asciz  "Reserved add: "
msg_tickn:
    .asciz  "Boot ticks  : "
msg_log:
    .asciz  "LOG stage="
msg_log2:
    .asciz  " res="
msg_log3:
    .asciz  " tick="
msg_tickhdr:
    .asciz  "TICK start a20 e820 sort kern = "
msg_crlf2:
    .asciz  "\r\n"
