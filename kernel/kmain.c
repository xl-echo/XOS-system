/* ============================================================================
 * XOS 内核主入口
 *
 * 启动顺序（每一步都先建立「出错可诊断」的能力，再做可能出错的事）：
 *   0. 控制台初始化
 *   1. IDT 安装 + 自检      —— 保证后续任何异常都被捕获并打印，而非三重故障
 *   2. FPU 初始化           —— 必须在 IDT 之后，异常可被捕获
 *   3. 内核足迹校验         —— 确保 .bss 未越出保留区
 *   4. E820 探测与校验
 *   5. PMM 初始化与自检
 *   6. VMM 初始化与自检     —— 必须在 PMM 之后（页表页取自物理分配器），
 *                              也必须在 IDT 之后（缺页需要可恢复的处理路径）
 *   7. 物理内存扩展自检与汇总报告
 * ============================================================================ */
#include "types.h"
#include "console.h"
#include "string.h"
#include "e820.h"
#include "pmm.h"
#include "vmm.h"
#include "memdetect.h"
#include "kmalloc.h"
#include "idt.h"
#include "irq.h"
#include "task.h"
#include "sync.h"
#include "syscall.h"
#include "fs.h"
#include "sound.h"
#include "net.h"     /* 第 08 册：任务/调度器 */
#include "display.h"   /* 第 12 册：设备驱动 · 显示 */
#include "keyboard.h"  /* 第 13 册：设备驱动 · 键盘 */
#include "mouse.h"     /* 第 14 册：设备驱动 · 鼠标 */
#include "disk.h"      /* 第 15 册：设备驱动 · 存储 */
#include "security.h"
#include "netstack.h"
#include "gui.h"
#include "winman.h"
#include "desktop.h"
#include "widget.h"
#include "font.h"
#include "app.h"  /* 第 27 册：安全机制子系统 */
#include "cpu.h"   /* ring3 用户态：GDT/TSS 特权级 */
#include "user.h"  /* ring3 用户程序加载与执行 */
#include "shell.h" /* 第 25 册：用户空间 Shell */
#include "multiuser.h" /* 第 26 册：多用户与权限 */
#include "pm.h"       /* 第 28 册：电源管理 */
#include "build.h"    /* 第 29 册：构建系统 */
#include "tester.h"   /* 第 30 册：测试与验证 */
#include "dbg.h"      /* 第 31 册：调试与监控 */
#include "crash.h"     /* 崩溃转储：panic 现场持久化 + 启动恢复报告 */
#include "rtc.h"       /* 实时时钟：CMOS RTC 真实读取 + date 命令 */
#include "xffs.h"      /* 磁盘文件系统：/disk 挂载，重启保留用户数据 */
#include "inst.h"     /* 第 32 册：安装程序-包管理 */
#include "virt.h"     /* 第 33 册：虚拟化支持 */
#include "shell_interactive.h" /* 交互式终端 Shell（自检通过后接管控制台） */
#include "login.h"             /* 登录界面（第 34 册：登录界面） */
#include "login_gui.h"         /* 图形登录（VBE 640x480x32） */
#include "desk_gui.h"          /* 图形桌面（第 35 册：图形桌面主入口） */

#define XOS_VERSION "0.2.0"

/* 由链接脚本导出（PROVIDE），用于校验内核足迹 */
extern u8 __bss_start[];
extern u8 __bss_end[];

/* 与 pmm.c 保持一致的内核保留区上界 */
#define KERNEL_RESERVE_END_CHECK  0x00300000u   /* .bss 固定 0x200000 起（linker.ld），检查上限含 .bss 余量 0x300000 */

static u32 tests_run    = 0;
static u32 tests_failed = 0;

/* 独立内核大栈顶（pmm 高位分配，TSS.esp0 使用） */
static u32 g_kstack_top = 0u;

/* --------------------------------------------------------------------------
 * 检查项登记：既即时打印，也登记到表内，最终汇总成一份紧凑清单。
 * 这样即使前面的输出被滚屏冲掉，最终屏幕仍能看到逐项结论。
 * ------------------------------------------------------------------------ */
#define MAX_CHECKS 70
static const char *chk_short[MAX_CHECKS];
static u32         chk_result[MAX_CHECKS];
static u32         chk_count = 0;

/* --------------------------------------------------------------------------
 * 启动节奏控制
 * 读时间戳计数器实现毫秒级延时，使启动各阶段依次可见（也便于截图核验）。
 * TSC 自 Pentium 起即为标配，VirtualBox 亦提供，无需额外硬件支持。
 * ------------------------------------------------------------------------ */
static u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

/* 假定约 2 GHz，仅用于放慢启动节奏，不要求精确定时 */
#define BOOT_STAGE_DELAY_MS  900u

static void boot_delay_ms(u32 ms)
{
    u64 start = rdtsc();
    u64 ticks = (u64)ms * 2000000ULL;
    while (rdtsc() - start < ticks) {
        __asm__ __volatile__("pause");
    }
}

static void stage_pause(void)
{
    boot_delay_ms(BOOT_STAGE_DELAY_MS);
}

static void report(const char *name, const char *shortname, u32 rc)
{
    tests_run++;
    if (rc != 0) tests_failed++;

    if (chk_count < MAX_CHECKS) {
        chk_short[chk_count]  = shortname;
        chk_result[chk_count] = rc;
        chk_count++;
    }

    if (rc == 0) {
        con_set_color(VGA_LIGHTGREEN, VGA_BLACK);
        con_puts("  [PASS] ");
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        con_puts(name);
        con_putc('\n');
    } else {
        con_set_color(VGA_LIGHTRED, VGA_BLACK);
        con_puts("  [FAIL] ");
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        con_puts(name);
        con_puts("  (case #");
        con_put_dec(rc);
        con_puts(")\n");
    }
    con_flush();
}

/* 最终紧凑清单：即使滚屏也能看到逐项结论 */
static void print_check_list(void)
{
    u32 i;
    for (i = 0; i < chk_count; i++) {
        con_puts("    ");
        if (chk_result[i] == 0) {
            con_set_color(VGA_LIGHTGREEN, VGA_BLACK);
            con_puts("[OK]  ");
        } else {
            con_set_color(VGA_LIGHTRED, VGA_BLACK);
            con_puts("[FAIL]");
        }
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        con_puts("  ");
        con_puts(chk_short[i]);
        if (chk_result[i] != 0) {
            con_puts("  #");
            con_put_dec(chk_result[i]);
        }
        con_putc('\n');
    }
    con_flush();
}


static void section(const char *title)
{
    con_set_color(VGA_LIGHTGREEN, VGA_BLACK);
    con_puts(title);
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_flush();
}

static void print_banner(void)
{
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("======================================================\n");
    con_puts("  XOS - eXperimental Operating System   v" XOS_VERSION "\n");
    con_puts("  Boot / Exception / Memory Subsystem Bring-up\n");
    con_puts("======================================================\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_putc('\n');
}

/* --------------------------------------------------------------------------
 * 内存映射统计输出
 * ------------------------------------------------------------------------ */
static void print_mem_stats(const mem_stats_t *st)
{
    con_puts("  Entries (normalized) : ");
    con_put_dec(e820_count());
    con_putc('\n');

    con_puts("  Address space (union): ");
    con_put_dec((u32)(st->union_bytes >> 10));
    con_puts(" KB\n");

    con_puts("  Usable memory        : ");
    con_put_dec((u32)(st->usable_bytes >> 10));
    con_puts(" KB  (");
    con_put_dec(st->usable_pages);
    con_puts(" pages)\n");

    con_puts("  Reserved memory      : ");
    con_put_dec((u32)(st->reserved_bytes >> 10));
    con_puts(" KB\n");

    con_puts("  ACPI reclaimable     : ");
    con_put_dec((u32)(st->acpi_reclaim_bytes >> 10));
    con_puts(" KB\n");

    con_puts("  ACPI NVS             : ");
    con_put_dec((u32)(st->acpi_nvs_bytes >> 10));
    con_puts(" KB\n");

    con_puts("  Bad memory           : ");
    con_put_dec((u32)(st->bad_bytes >> 10));
    con_puts(" KB\n");

    con_puts("  Usable below 1MB     : ");
    con_put_dec(st->below_1m_usable >> 10);
    con_puts(" KB\n");

    con_puts("  Highest address      : ");
    con_put_hex64(st->highest_addr);
    con_putc('\n');
    con_putc('\n');
}

/* --------------------------------------------------------------------------
 * PMM 统计输出
 * ------------------------------------------------------------------------ */
static void print_pmm_stats(void)
{
    con_puts("  Page size            : 4096 bytes\n");

    con_puts("  Managed memory       : ");
    con_put_dec(pmm_managed_bytes() >> 10);
    con_puts(" KB  (");
    con_put_dec(pmm_total_pages());
    con_puts(" pages)\n");

    con_puts("  Reserved pages       : ");
    con_put_dec(pmm_reserved_pages());
    con_putc('\n');

    con_puts("  Used pages           : ");
    con_put_dec(pmm_used_pages());
    con_putc('\n');

    con_puts("  Free pages           : ");
    con_put_dec(pmm_free_page_count());
    con_puts("  (");
    con_put_dec(pmm_free_page_count() * 4u / 1024u);
    con_puts(" MB)\n");

    con_puts("  Largest free run     : ");
    con_put_dec(pmm_largest_free_run());
    con_puts(" pages\n");

    con_puts("  Free regions         : ");
    con_put_dec(pmm_free_region_count());
    con_puts("  (fragmentation index)\n");

    con_putc('\n');
}

/* --------------------------------------------------------------------------
 * 物理内存管理扩展子系统运行状态输出
 * ------------------------------------------------------------------------ */
static void print_mem_ext_stats(void)
{
    u32 z;

    con_puts("  Zones                : ");
    for (z = 0; z < pmm_zone_count(); z++) {
        if (z) con_puts(" | ");
        con_puts(pmm_zone_name(z));
        con_puts(" free ");
        con_put_dec(pmm_zone_free(z));
        con_puts(" used ");
        con_put_dec(pmm_zone_used(z));
    }
    con_putc('\n');

    con_puts("  NUMA nodes           : ");
    con_put_dec(pmm_numa_node_count());
    con_puts("  (local distance ");
    con_put_dec(pmm_numa_distance(0, 0));
    con_puts(")\n");

    con_puts("  Watermarks min/low/high: ");
    con_put_dec(pmm_watermark(0));
    con_puts(" / ");
    con_put_dec(pmm_watermark(1));
    con_puts(" / ");
    con_put_dec(pmm_watermark(2));
    con_puts("   pressure ");
    con_puts(pmm_pressure_name(pmm_pressure_level()));
    con_putc('\n');

    con_puts("  Huge pages (2MB)     : ");
    con_put_dec(pmm_huge_used());
    con_puts(" / ");
    con_put_dec(pmm_huge_count());
    con_putc('\n');

    con_puts("  Anti-frag reserve    : ");
    con_put_dec(pmm_antifrag_pages());
    con_puts(" pages   frag index ");
    con_put_dec(pmm_frag_index());
    con_puts("/1000\n");

    con_puts("  Shrinkers / reclaim  : ");
    con_put_dec(pmm_shrinker_count());
    con_puts(" / ");
    con_put_dec(pmm_reclaim_calls());
    con_puts(" calls, ");
    con_put_dec(pmm_reclaim_freed());
    con_puts(" pages\n");

    con_puts("  OOM events           : ");
    con_put_dec(pmm_oom_count());
    con_puts("   scrub ");
    con_put_dec(pmm_scrub_count());
    con_puts(" pages   offline bad ");
    con_put_dec(pmm_badpage_count());
    con_putc('\n');

    con_puts("  Hotplug regions      : ");
    con_put_dec(pmm_hotplug_count());
    con_puts("   IO mappings ");
    con_put_dec(pmm_ioremap_count());
    con_putc('\n');
    con_putc('\n');
}

/* --------------------------------------------------------------------------
 * 虚拟内存子系统运行状态输出
 * ------------------------------------------------------------------------ */
static void print_vmm_diag(void)
{
    u32 i;
    con_set_color(VGA_YELLOW, VGA_BLACK);
    con_puts("  [diag] virtual memory fault snapshot:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    for (i = 0; i < VMM_DBG_SLOTS; i++) {
        con_puts("    ");
        con_puts(vmm_dbg_label[i]);
        con_puts(" = ");
        con_put_hex32(vmm_dbg[i]);
        con_putc(i == 3 ? '\n' : ' ');
    }
    con_putc('\n');
}

static void print_vmm_stats(void)
{
    con_puts("  Paging mode          : ");
    con_puts(vmm_enabled() ? "enabled (CR0.PG=1, CR0.WP=1)" : "disabled");
    con_puts("   PSE ");
    con_puts(vmm_pse_available() ? "yes" : "no");
    con_putc('\n');

    con_puts("  Kernel identity map  : 0x00000000-");
    con_put_hex32(KERNEL_IDENTITY_TOP);
    con_puts("  (");
    con_put_dec(vmm_kernel_pgd_shared());
    con_puts(" shared PGD entries)\n");

    con_puts("  Address spaces       : ");
    con_put_dec(vmm_mm_count());
    con_puts("   page tables ");
    con_put_dec(vmm_pt_pages_used());
    con_puts(" pages\n");

    con_puts("  Large pages (4MB PSE): ");
    con_put_dec(vmm_huge_count());
    con_puts("   covering ");
    con_put_dec(vmm_huge_count() * 4u);
    con_puts(" MB\n");

    con_puts("  TLB flush / shootdown: ");
    con_put_dec(vmm_tlb_flush_total());
    con_puts(" / ");
    con_put_dec(vmm_tlb_shootdown_total());
    con_putc('\n');

    con_puts("  ASLR                 : ");
    con_puts(vmm_aslr_get() ? "on" : "off");
    con_puts("   entropy ");
    con_put_hex32(vmm_aslr_entropy());
    con_putc('\n');

    con_puts("  Swap slots free/total: ");
    con_put_dec(vmm_swap_free_slots());
    con_puts(" / ");
    con_put_dec(vmm_swap_total_slots());
    con_puts("   out ");
    con_put_dec(vmm_swap_out_count());
    con_puts(" in ");
    con_put_dec(vmm_swap_in_count());
    con_putc('\n');

    con_puts("  rmap / page cache    : ");
    con_put_dec(vmm_rmap_used());
    con_puts(" entries / ");
    con_put_dec(vmm_pgcache_count());
    con_puts(" pages (dirty ");
    con_put_dec(vmm_pgcache_dirty_count());
    con_puts(", writeback ");
    con_put_dec(vmm_pgcache_writeback_count());
    con_puts(")\n");

    con_puts("  Page faults total    : ");
    con_put_dec(vmm_fault_count());
    con_puts("   resolved ");
    con_put_dec(vmm_fault_resolved_count());
    con_puts("   refused ");
    con_put_dec(vmm_fault_refused_count());
    con_putc('\n');

    con_puts("  COW copies           : ");
    con_put_dec(vmm_cow_count());
    con_puts("\n");

    con_puts("  Shrinker reclaim     : ");
    con_put_dec(vmm_shrinker_calls());
    con_puts(" calls, ");
    con_put_dec(vmm_shrinker_freed());
    con_puts(" pages freed\n");
    con_putc('\n');
}

/* 高位内核栈切换（bss 逼近 0x9FC60 后，低区启动栈 0x9FFFC 仅剩 ~924B，
 * 自检深层调用链可能越界；PMM 就绪后切到 64KB 物理页栈） */
extern void kstack_switch(u32 new_esp);

static inline u32 rd_esp(void)
{
    u32 e;
    __asm__ volatile ("mov %%esp,%0" : "=r"(e));
    return e;
}
static void diag_esp(const char *tag)
{
    con_puts("  [esp] ");
    con_puts(tag);
    con_puts(" ");
    con_put_hex32(rd_esp());
    con_putc('\n');
}

void kmain(void)
{
    static mem_stats_t st;
    static u32 rc;
    u32 kend = 0;

    con_init();
    print_banner();

    /* =====================================================================
     * 阶段 1：IDT 与异常处理
     * 必须先于一切可能触发异常的操作，保证故障可诊断而非三重故障重启
     * ================================================================== */
    section("[1/7] Installing interrupt descriptor table...\n");
    idt_init();
    report("IDT installed & verified (sidt readback + 256 gates)", "IDT + CPU exception handling", idt_selftest());
    stage_pause();

    /* 上次崩溃转储恢复报告（若有）：panic 现场在固定内存区，重启自动汇报 */
    crash_dump_check();

    /* =====================================================================
     * 阶段 2：FPU 初始化（置于 IDT 之后，异常可被捕获）
     * ================================================================== */
    section("[2/7] Initializing x87 FPU...\n");
    __asm__ __volatile__("fninit");
    {
        u32 sw = 0;
        __asm__ __volatile__("fnstsw %0" : "=m"(sw));
        /* fninit 后状态字应为 0 */
        report("x87 FPU initialized (status word == 0)", "x87 FPU initialization", (sw & 0xFFFFu));
    }
    stage_pause();

    /* =====================================================================
     * 阶段 3：内核足迹校验
     * ================================================================== */
    section("[3/7] Verifying kernel footprint...\n");
    kend = (u32)__bss_end;
    con_puts("  bss_end sym: ");
    con_put_hex32((u32)__bss_end);
    con_putc('\n');
    con_puts("  kend var   : ");
    con_put_hex32(kend);
    con_putc('\n');
    con_puts("  Kernel image + .bss end : ");
    con_put_hex32(kend);
    con_puts("   limit ");
    con_put_hex32(KERNEL_RESERVE_END_CHECK);
    con_putc('\n');
    report("kernel footprint within reserved region", "kernel footprint vs reserved region", (kend <= KERNEL_RESERVE_END_CHECK) ? 0 : 1);
    stage_pause();

    /* =====================================================================
     * 阶段 4：内存探测与映射规范化
     * ================================================================== */
    section("[4/7] Detecting physical memory (E820)...\n");
    e820_init();

    rc = e820_validate();
    if (rc != E820_VALID_OK) {
        con_set_color(VGA_LIGHTRED, VGA_BLACK);
        con_puts("  FATAL: memory map validation failed, code #");
        con_put_dec(rc);
        con_puts("\n");
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        goto summary;
    }
    report("memory map validated (signature, order, types, overlap)", "memory map validation", 0);
    report("E820 normalization self-test", "E820 normalization self-test", e820_selftest());
    stage_pause();

    e820_dump();
    con_putc('\n');

    /* 统计口径 */
    e820_stats(&st);
    print_mem_stats(&st);
    stage_pause();
    stage_pause();

    /* =====================================================================
     * 阶段 5：物理内存管理器
     * ================================================================== */
    section("[5/7] Initializing page frame allocator...\n");
    pmm_init();
    /* 切高位大栈：此后不再依赖低区 0x9FFFC 启动栈（其与 bss_end 仅隔 ~924B） */
    {
        u32 sbase = pmm_alloc_high_pages(16);   /* 64KB 连续物理页，取受管内存最高处 */
        if (sbase != 0u) {
            u32 stop = (sbase + 16u * 4096u - 4u) & ~0xFu;
            g_kstack_top = sbase + 16u * 4096u; /* 记录栈顶（TSS.esp0 用） */
            kstack_switch(stop);
        }
    }
    diag_esp("after kstack_switch");
    print_pmm_stats();
    diag_esp("after print_pmm_stats");
    pmm_dump();
    con_putc('\n');
    stage_pause();

    /* =====================================================================
     * 阶段 6：虚拟内存 / 分页
     * 时序说明：分页必须在 PMM 之后（页表页要从物理分配器取），
     * 也必须在 IDT 之后（缺页异常需要可捕获、可恢复的处理路径）。
     * ================================================================== */
    section("[6/8] Enabling paging and virtual memory...\n");
    vmm_init();
    report("paging enabled (identity map + CR0.PG/WP + kernel PGD)",
           "Paging enable & identity mapping", vmm_enabled() ? 0 : 1);
    report("page table walk / translation / flags / TLB / huge / isolation / ASLR",
           "Virtual memory core self-test", vmm_selftest());
    report("demand paging / COW / stack growth / swap / refusal paths",
           "Page fault handling self-test", vmm_fault_selftest());
    report("rmap / page cache writeback / reclaim / address space teardown",
           "Virtual memory extended self-test", vmm_ext_selftest());
    diag_esp("after vmm_ext_selftest");
    print_vmm_stats();
    diag_esp("after print_vmm_stats");
    vmm_dump();
    diag_esp("after vmm_dump");
    stage_pause();

    /* =====================================================================
     * 阶段 7：自检
     * ================================================================== */
    section("[7/8] Running memory subsystem self-tests...\n");
    report("PMM self-test (alloc/free/reserved/OOM/refcount/poison)",
           "PMM self-test", pmm_selftest());
    {
        u32 pmm_ext_rc = pmm_ext_selftest();
        if (pmm_ext_rc) {
            con_puts("PMM_EXT_RC=");
            con_put_dec(pmm_ext_rc);
            con_puts("\n");
        }
        report("Memory extended self-test (buddy/zone/NUMA/hotplug/hugepage/reclaim/scrub/offline)",
               "Memory subsystem extended self-test", pmm_ext_rc);
    }
    print_mem_ext_stats();
    stage_pause();

    /* =====================================================================
     * 阶段 8：内存探测与统计
     * 时序说明：必须在 E820 规范化（阶段 4）之后 —— 它以规范化后的映射为输入；
     * 必须在 PMM（阶段 5）之后 —— 使用率监控要读物理分配器的页计数；
     * ACPI 探测必须早于热插拔探测 —— 后者依赖 SRAT 表。
     * ================================================================== */
    section("[8/9] Probing firmware memory description...\n");
    rc = (u32)mdet_init(e820_table(), e820_count());
    mdet_acpi_probe();
    mdet_smbios_probe();
    mdet_physaddr_probe();
    mdet_holes_scan();
    mdet_hotplug_probe();
    mdet_persist();
    report("E820 types / capacity / low-1MB / holes / persistence / kernel cmdline",
           "Memory detection core self-test", mdet_selftest());
    report("RSDP + RSDT/XSDT + table enumeration + SRAT hot-plug regions",
           "ACPI region detection self-test", mdet_acpi_selftest());
    report("SMBIOS entry point + type 16/17 + speed + ECC capability",
           "SMBIOS memory device self-test", mdet_smbios_selftest());
    mdet_dump();
    mdet_visualize();
    mdet_monitor_report();
    stage_pause();

    /* =====================================================================
     * 阶段 9：内核堆分配器
     * 时序说明：必须在 PMM（阶段 5）之后 —— slab/大块/内存池都从 PMM 取页；
     * 必须在 VMM（阶段 6）之后 —— 未来用户态程序经系统调用使用堆时依赖
     * 分页与地址空间，本阶段先把堆本身在恒等映射下建立并自检。
     * ================================================================== */
    section("[9/10] Initializing kernel heap allocator...\n");
    kheap_init();
    report("slab/large/align/zero/redzone/canary roundtrip",
           "Kernel heap core self-test", kheap_selftest_core());
    report("double-free / corruption / ownership / OOM / NUMA / null",
           "Heap fault-tolerance self-test", kheap_selftest_fault());
    report("mempool limits / ctor-dtor hooks / stats / shrink / leak",
           "Heap extended self-test", kheap_selftest_ext());
    kheap_dump();
    kheap_leak_report();
    stage_pause();

    /* =====================================================================
     * 阶段 10：中断与异常子系统
     * 时序说明：必须在 IDT（阶段 1）之后 —— IRQ 门要写入 IDT；
     * 必须在 VMM（阶段 6）之后 —— 缺页恢复路径依赖页表；
     * PIT 心跳只开 IRQ0，其余 15 路保持屏蔽，避免无主中断刷屏。
     * ================================================================== */
    syscall_init();   /* 提前：irq 阶段 0x80 自检需真实分发表与默认放行 */

    section("[10/13] Initializing interrupts & exceptions...\n");
    irq_init();
    report("PIC remap / IRQ desc / mask / softirq / storm / APIC probe",
           "IRQ subsystem core self-test", irq_selftest());
    report("PIT IRQ0 real arrival / tick sampling / watchdog warn-mode",
           "Hardware interrupt path self-test", irq_selftest_irq());
    report("recoverable #BP/#UD / stack trace / int 0x80 roundtrip",
           "Exception diagnostics self-test", irq_selftest_exc());
    irq_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 11：进程与调度子系统
     * 时序说明：必须在堆（阶段 9）之后 —— 任务栈由 kmalloc 分配；
     * 必须在中断（阶段 10）之后 —— 时间片抢占依赖 PIT 心跳与返回路径检查。
     * ================================================================== */
    section("[11/13] Initializing task scheduler & process subsystem...\n");
    task_init();
    if (task_init_process() != 0) report("init process (pid 1) create", "Scheduler init process", 1);
    irq_enable();          /* 重新开中断：PIT 心跳驱动时间片/唤醒/调度统计 */
    report("task desc / pid alloc / rq / weights / state machine",
           "Task core self-test", task_selftest_core());
    report("real round-robin scheduling (3 threads) / switch accounting",
           "Scheduler execution self-test", task_selftest_sched());
    report("wait queue / msleep timed wakeup",
           "Wait queue & wakeup self-test", task_selftest_wait());
    report("signal pending / clear / exit / zombie reaper",
           "Signal & reaper self-test", task_selftest_sig());
    report("Task extended self-test (init/daemon/orphan/boost/quota/affinity)",
           "Scheduler extended self-test", task_selftest_ext());
    task_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 12：同步原语子系统
     * 依赖：任务调度（yield 慢路径）、堆（自检栈）、中断（irqsave 版本）。
     * ================================================================== */
    section("[12/13] Initializing synchronization primitives...\n");
    {
        /* 同步原语依赖 bss 尾哨兵完整性（诊断期残留的检查已清理） */
    }
    report("atomic / spinlock / semaphore / seqlock / barrier / irqsave",
           "Sync core self-test", sync_selftest_core());
    report("mutex / rwlock / completion / wait queue",
           "Sync lock primitives self-test", sync_selftest_lock());
    section("[12/13] sync concur...\n");
    report("2-thread mutex counter (=2000) / spin counter (=1000) / lock-free SPSC",
           "Sync concurrency self-test", sync_selftest_concur());
    section("[12/13] sync detect...\n");
    report("lock-order deadlock detect / RCU grace / futex",
           "Sync detection self-test", sync_selftest_detect());
    section("[12/13] sync pi...\n");
    report("priority inversion handling (inherit/ceiling/cycle)",
           "Sync PI self-test", sync_selftest_pi());
    report("cacheline coherence (alignment/atomic RMW/bits/fences)",
           "Sync cache self-test", sync_selftest_cache());
    sync_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 12.5：物理内存管理器锁与并发保护（03 册第二轮重做）
     * 依赖：调度器（任务并发分配/释放）、同步原语（pmm_alloc_lock）、
     * 中断（irqsave 临界区可被 PIT 打断）。
     * ================================================================== */
    report("alloc/free lock coverage / 2-thread frame uniqueness / conservation",
           "PMM lock & concurrency self-test", pmm_selftest_lock());
    stage_pause();

    /* =====================================================================
     * 阶段 13：系统调用接口
     * int 0x80 → syscall_handler → syscall_dispatch（编号表/参数校验/
     * 用户态指针拷贝/错误码/审计/seccomp/限流/统计）。
     * ================================================================== */
    section("[13/14] Initializing syscall interface...\n");
    syscall_init();
    report("nr table / dispatch / arg check / user copy / errno / audit / seccomp / quota",
           "Syscall interface self-test", syscall_selftest());
    syscall_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 13.9：存储子系统（磁盘是 XFFS 持久化的底层，必须先行初始化）
     * ================================================================== */
    section("[13.9/15] Initializing storage subsystem...\n");
    disk_init();
    report("ATA PIO / IDENTIFY / LBA28 / AHCI probe / NVMe regs / SCSI CDB / bio / sched / MBR-GPT / cache / writeback / TRIM / hotplug / retry / badblock / SMART / encryption / RAID / LVM / quota / stat / power",
           "Storage subsystem self-test (21 groups)", disk_selftest());
    disk_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 14：文件系统核心
     * VFS 抽象 + tmpfs/devfs 注册 + 挂载点表 + 路径解析 + 打开文件表 +
     * 读写路径 + 权限 + 一致性检查（自检 51 用例）。
     * ================================================================== */
    section("[14/15] Initializing filesystem core...\n");
    fs_init();
    report("vfs / superblock / inode / dentry / fd table / path resolve / rw / perm",
           "Filesystem core self-test", fs_selftest());
    fs_dump();
    stage_pause();

    section("[14.2/15] Initializing disk filesystem XFFS (/disk)...\n");
    xffs_init();
    report("layout / slots / persistence / safety",
           "XFFS disk filesystem self-test", xffs_selftest());    xffs_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 15：音频子系统（第 16 册 · 设备驱动 · 音频）
     * 设备描述符模型 + PC 扬声器（PIT 通道 2 + 0x61 真实发声）+
     * PCI 配置空间扫描（VirtualBox AC97/Intel HDA 声卡探测）+
     * PCM 环形缓冲 + 采样率/声道/音量/静音 + PIT 时钟 +
     * 省电/错误恢复/路由/权限/热插拔/统计 + 真机自检。
     * ================================================================== */
    section("[15/16] Initializing audio subsystem...\n");
    sound_init();
    report("dev table / PCI probe (AC97/HDA) / open-close / param checks",
           "Audio core self-test", sound_selftest_core());
    report("ring push-pop order / watermark / overflow guard / underrun",
           "Audio PCM ring self-test", sound_selftest_pcm());
    report("48k->22k / stereo->mono / volume ramp / mute",
           "Audio format self-test", sound_selftest_fmt());
    report("PIT clock readback / tone range / 0x61 gate real-state",
           "Audio clock & speaker self-test", sound_selftest_clock());
    report("suspend/resume / route dedup / rec-perm / hotplug / stats",
           "Audio policy self-test", sound_selftest_misc());
    report("rec path / pan balance / multi-stream mix / 3-band EQ / reverb / MIDI queue / 12-TET synth / USB audio EP / BT A2DP-SBC / low-latency / error recover / sine gen / calibrate",
           "Audio device self-test (14 groups)", sound_selftest_dev());
    sound_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 16：网络设备驱动子系统（第 17 册 · 设备驱动 · 网络）
     * PCI 总线枚举（82540EM/82545EM/PCnet/RTL8139）+ 以太网控制器驱动 +
     * DMA 描述符环 + 收发缓冲池 + 中断入口 + NAPI 轮询 + 多队列 +
     * 固件加载 CRC16 + 链路状态机 + MAC 管理 + 混杂过滤 + 卸载能力 +
     * Wake-on-LAN 省电 + 热插拔 + 复位恢复 + 性能调优 + 蓝牙 HCI +
     * 虚拟网卡 veth 对 + 统计诊断 + 真机自检 12 组。
     * ================================================================== */
    section("[16/17] Initializing network device subsystem...\n");
    net_init();
    (void)net_probe_pci();
    report("PCI enum / open-close refs / MAC valid-set-get / link state machine",
           "Net core self-test", net_selftest_core());
    report("DMA ring setup / xmit-rx roundtrip / buf pool / stats",
           "Net DMA ring self-test", net_selftest_ring());
    report("promisc / filter add-clear / match rules",
           "Net filter self-test", net_selftest_filt());
    report("offload capability bits set-clear",
           "Net offload self-test", net_selftest_offload());
    report("fw image magic-len-CRC16 load / bad samples",
           "Net firmware self-test", net_selftest_fw());
    report("link down-nego-up state machine / speed-duplex checks",
           "Net link self-test", net_selftest_link());
    report("rx inject / NAPI budget poll / irq entry",
           "Net NAPI self-test", net_selftest_napi());
    report("WOL magic packet detect / suspend-resume",
           "Net Wake-on-LAN self-test", net_selftest_wol());
    report("hotplug remove-reopen / soft reset / err recover",
           "Net hotplug self-test", net_selftest_hotplug());
    report("veth pair create / A-B loopback delivery",
           "Net veth self-test", net_selftest_veth());
    report("BT HCI init / cmd queue / event poll",
           "Net bluetooth HCI self-test", net_selftest_bt());
    report("NAPI budget tune / ring depth setup",
           "Net tune self-test", net_selftest_tune());
    net_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 16：安全机制子系统（第 27 册 · 安全机制）
     * RNG（RDRAND 探测 + TSC/PIT 混合熵 + xorshift128+）、SHA-256 / RC4 /
     * CRC32 加密算法库（标准测试向量验证）、密钥环（加密态存储）、文件
     * 加密抽象、IMA 完整性度量、审计子系统、LSM 钩子框架、MAC 策略矩阵
     * （SELinux 语义简化）、AppArmor 域、seccomp 系统调用策略位图、
     * KASLR 熵、NX/WP/页表隔离真实状态验证、栈 canary、签名哈希校验、
     * 信任根链、安全基线与加固状态 + 真机自检 5 组。
     * ================================================================== */
    section("[16/18] Initializing keyboard subsystem...\n");
    kbd_init();
    report("PS/2 poll / scancode set1-2-3 / modifiers / typematic / LED / hotkey / keymap / event queue / USB HID / hotplug / IME / layout switch / filter / user dispatch",
           "Keyboard subsystem self-test (50 cases)", kbd_selftest());
    kbd_dump();
    stage_pause();

    section("[17/19] Initializing mouse subsystem...\n");
    mse_init();
    report("PS/2 mouse protocol / packet parse 3-4-5 / delta accumulate / button tracking / wheel / accel-smooth / cursor clip / USB HID / hotplug / config",
           "Mouse subsystem self-test (20 groups)", mse_selftest());
    mse_dump();
    stage_pause();

    section("[19/19] Initializing display subsystem...\n");
    display_init();
    report("VGA text / VBE probe / mode table / fb / double-buffer / cursor / vsync / EDID / multi-head / scale / gamma / brightness / 2D raster / KMS / suspend-resume",
           "Display subsystem self-test (45 cases)", display_selftest());
    display_dump();
    stage_pause();

    /* =====================================================================
     * 阶段 17：安全机制子系统（第 27 册 · 安全机制）
     * ================================================================== */
    section("[19/20] Initializing network stack subsystem...\n");
    ns_init();
    report("skb pool / eth-csum / ARP table / IPv4 csum / ICMP echo",
           "Netstack L2-L3 self-test", ns_selftest());
    ns_dump();
    stage_pause();

    section("[20/21] Initializing security subsystem...\n");
    sec_init();

    section("[20.5/21] Initializing real-time clock (CMOS RTC)...\n");
    rtc_init();
    report("CMOS RTC / BCD / leap-year / timestamp / format",
           "Real-time clock self-test", rtc_selftest());
    rtc_dump();
    stage_pause();

    section("[21/22] Initializing GUI graphics subsystem...\n");
    gui_init();
    report("canvas / palette / pixels / primitives / clip / bitmap / font / events",
           "GUI graphics self-test", gui_selftest());
    gui_dump();
    stage_pause();

    section("[22/23] Initializing window manager...\n");
    wm_init();
    report("lifecycle / z-order / move-resize / decor / min-max / focus / ws / routing",
           "Window manager self-test", wm_selftest());
    wm_dump();
    stage_pause();

    section("[23/24] Initializing desktop environment...\n");
    desk_init();
    report("icons / taskbar / tray / notify / theme / lock / login / search / a11y",
           "Desktop environment self-test", desk_selftest());
    desk_dump();
    stage_pause();

    section("[24/25] Initializing widget library...\n");
    wdg_init();
    report("button / check-radio / edit / list / combo / progress-slider / scroll / menu / tabs",
           "Widget library self-test", wdg_selftest());
    wdg_dump();
    stage_pause();

    section("[25/26] Initializing font subsystem...\n");
    font_init();
    report("bitmap / ttf / otf / raster / aa / subpixel / metrics / cmap / kerning / fallback / cache",
           "Font subsystem self-test", font_selftest());
    font_dump();
    stage_pause();

    section("[26/27] Initializing application ecosystem...\n");
    app_init();
    report("ELF header / program headers / load / link / reloc / shared library / dependency",
           "Application ecosystem self-test", app_selftest());
    app_dump();
    stage_pause();

    /* ---- CPU 特权级（ring3 用户态基础）：重建 GDT(ring0/ring3+TSS) ---- */
    section("[26.5/27] Initializing CPU privilege levels (GDT/TSS, ring3)...\n");
    {
        u32 cur_esp;
        __asm__ __volatile__("movl %%esp, %0" : "=r"(cur_esp));
        (void)cur_esp;
        cpu_gdt_init(g_kstack_top);   /* esp0 = 独立内核大栈顶（ring3 陷入用） */
    }
    report("GDT ring0/ring3 segments + TSS esp0 / selftest",
           "CPU privilege-level self-test", cpu_selftest());

    /* ---- ring3 用户态执行：ELF 用户加载 + 特权级进入/退出 ---- */
    section("[26.7/27] Initializing user-mode execution (ring3)...\n");
    report("ELF map(PTE_US) / user stack / iret ring3 enter / syscall 0x80 DPL3 / exit-to-kernel",
           "User-mode execution self-test", user_selftest());
    stage_pause();

    section("[27/28] Initializing user-space shell...\n");
    shell_init();
    report("parse / quote / redirect / pipe / background / env set-get-expand / jobs / history",
           "Shell subsystem self-test", sh_selftest());
    shell_dump();
    stage_pause();

    section("[28/29] Initializing multi-user & permissions...\n");
    mu_init();
    report("accounts / groups / auth / password hash / sessions",
           "Multi-user subsystem self-test", mu_selftest());
    mu_dump();
    stage_pause();

    section("[29/30] Initializing power management...\n");
    pm_init();
    report("ACPI tables / machine state / S3 sleep / S4 hibernate / S5 shutdown",
           "Power management self-test", pm_selftest());
    pm_dump();
    stage_pause();

    section("[33/34] Initializing installer...\n");
    inst_init();
    report("Media boot / partition format / file copy / bootloader / driver select",
           "Installer self-test", inst_selftest());
    stage_pause();

    /* =====================================================================
     * 阶段 33.5：虚拟化支持子系统（第 33 册 · 虚拟化支持）
     * 作为 guest 运行于 VirtualBox 等虚拟机时，探测 hypervisor 品牌、
     * CPU 虚拟化特性位（VMX/SVM/APIC）、VirtIO PCI 设备与 ACPI 痕迹，
     * 并为虚拟化 TSC 时钟校准提供支撑。探测失败安全降级。
     * ================================================================== */
    section("[34/35] Initializing virtualization support...\n");
    virt_init();
    report("CPUID leaf0/1 / hypervisor brand / VMX-SVM-APIC / VirtIO PCI scan / ACPI / TSC monotonic",
           "Virtualization support self-test (7 cases)", virt_selftest());
    virt_dump();
    stage_pause();

    section("[32/33] Initializing debug & monitor...\n");
    dbg_init();
    report("COM1 serial / log ring / level filter / symbols backtrace / kgdb",
           "Debug monitor self-test", dbg_selftest());
    dbg_log_restore();   /* 恢复上次关机落盘的内核日志（磁盘末尾持久化区） */
    stage_pause();

    section("[31/32] Initializing test framework...\n");
    ts_init();
    report("Unit/Integration framework / case mgmt / assert lib / stubs",
           "Test framework self-test", ts_selftest());
    stage_pause();

    section("[30/31] Initializing build system...\n");
    bd_init();
    report("Config parse / compile scheduling / assembler / link / object formats",
           "Build system self-test", bd_selftest());
    stage_pause();

    report("RDRAND probe / mixed entropy / xorshift / non-zero distribution",
           "Security RNG self-test", sec_selftest_rng());
    report("SHA-256('abc') / RC4('Key') / CRC32 / file-encrypt roundtrip",
           "Security crypto self-test", sec_selftest_crypto());
    report("key gen / keyring add-get-del / invalid args",
           "Security keyring self-test", sec_selftest_key());
    report("LSM hook / MAC matrix / AppArmor / seccomp / IMA / audit / hash-verify",
           "Security policy self-test", sec_selftest_lsm());
    report("KASLR / NX+WP / page isolation / canary / trust chain / baseline / harden",
           "Security hardware self-test", sec_selftest_hw());
    report("IDS rules / sig-block verify / KASLR slot",
           "Security ids self-test", sec_selftest_ids());
    report("SELinux matrix / NX-DEP / mem-protect / mitigations / secure-boot / audit / scan / seccomp",
           "Security extended self-test", sec_selftest_ext());
    sec_dump();
    stage_pause();

    report("crash dump write-back / field integrity / CR regs / stack snapshot / count",
           "Crash dump self-test", crash_selftest());
    stage_pause();

summary:
    con_putc('\n');
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("------------------------------------------------------\n");
    print_check_list();
    con_putc('\n');
    con_puts("  Test summary: ");
    con_put_dec(tests_run - tests_failed);
    con_puts(" passed, ");
    con_put_dec(tests_failed);
    con_puts(" failed, ");
    con_put_dec(tests_run);
    con_puts(" total\n");
    if (tests_failed == 0) {
        con_set_color(VGA_LIGHTGREEN, VGA_BLACK);
        con_puts("  RESULT: ALL CHECKS PASSED\n");
    } else {
        con_set_color(VGA_LIGHTRED, VGA_BLACK);
        con_puts("  RESULT: FAILURES PRESENT\n");
    }
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("------------------------------------------------------\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    if (tests_failed != 0) print_vmm_diag();
    con_puts("  System halted safely. Host unaffected.\n");
    con_flush();

    if (tests_failed == 0) {
        con_puts("  System self-check passed. Starting login...\n");
        con_flush();
        if (login_gui_run() != 0) {           /* 图形登录；失败回退文本登录 */
            con_clear();
            con_set_cursor(0u, 0u);
            login_run();
        }
        con_puts("  Starting graphical desktop...\n");
        con_flush();
        desk_gui_run();                     /* 图形桌面；Esc 退出回文本终端 */
        con_puts("  Entering interactive shell. Type 'help' for commands.\n");
        con_flush();
        user_set_shell_entry((u32)shell_interactive);   /* ring3 退出后重入 shell */
        shell_interactive();
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
