/* ============================================================================
 * XOS 崩溃转储子系统实现（自研）
 * 见 include/crash.h 设计说明。
 * ========================================================================== */
#include "types.h"
#include "crash.h"
#include "console.h"
#include "idt.h"
#include "irq.h"          /* pit_tick_count */

/* 转储区指针（低 1MB 恒映射，直接物理寻址） */
#define CRASH_BASE ((crash_dump_t *)CRASH_DUMP_ADDR)

/* 安全内存边界：栈快照仅允许复制内核栈区（0x80000-0xA0000）
 * 及用户栈区（0xB0000000 顶向下 1MB）内的内容，避免读未知物理内存 */
static int crash_addr_safe(u32 addr, u32 len)
{
    if (addr >= 0x00080000u && addr < 0x000A0000u) {
        return (addr + len) <= 0x000A0000u;
    }
    if (addr >= 0xB0000000u - 0x00100000u && addr < 0xB0000000u) {
        return (addr + len) <= 0xB0000000u;
    }
    return 0;
}

/* 安全复制栈内容 */
static u32 crash_copy_stack(u32 esp, u32 *dst, u32 max_words)
{
    u32 n = 0u;
    u32 a;
    while (n < max_words) {
        a = esp - (n << 2);
        if (!crash_addr_safe(a, 4u)) break;
        dst[n] = *(volatile u32 *)a;
        n++;
    }
    return n;
}

/* 安全遍历 EBP 链收集回溯 EIP（纯函数版，不打印） */
static u32 crash_collect_trace(u32 ebp, u32 *trace, u32 max)
{
    u32 frame = ebp;
    u32 n = 0u;
    while (n < max) {
        u32 next;
        if (frame < 0x00080000u || frame >= 0x000A0000u) break;
        if ((frame & 3u) != 0u) break;
        next = *(volatile u32 *)frame;
        if (next <= frame || next - frame > 0x00020000u) break;
        trace[n] = *(volatile u32 *)(frame + 4u);
        n++;
        frame = next;
    }
    return n;
}

static void crash_read_cr(u32 *cr0, u32 *cr2, u32 *cr3, u32 *cr4)
{
    __asm__ __volatile__("movl %%cr0, %0" : "=r"(*cr0));
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(*cr2));
    __asm__ __volatile__("movl %%cr3, %0" : "=r"(*cr3));
    __asm__ __volatile__("movl %%cr4, %0" : "=r"(*cr4));
}

/* 异常名回填（本地小表，避免依赖 idt 内部表顺序） */
static void crash_fill_name(crash_dump_t *d, u32 v)
{
    const char *n;
    switch (v) {
    case 0:  n = "Divide-by-zero"; break;
    case 1:  n = "Debug"; break;
    case 2:  n = "NMI"; break;
    case 3:  n = "Breakpoint"; break;
    case 4:  n = "Overflow"; break;
    case 5:  n = "Bound-range"; break;
    case 6:  n = "Invalid-opcode"; break;
    case 7:  n = "Device-not-available"; break;
    case 8:  n = "Double-fault"; break;
    case 9:  n = "Coprocessor-seg-overrun"; break;
    case 10: n = "Invalid-TSS"; break;
    case 11: n = "Segment-not-present"; break;
    case 12: n = "Stack-segment-fault"; break;
    case 13: n = "General-protection"; break;
    case 14: n = "Page-fault"; break;
    case 16: n = "x87-fp-error"; break;
    case 17: n = "Alignment-check"; break;
    case 18: n = "Machine-check"; break;
    case 19: n = "SIMD-fp-exception"; break;
    default: n = "Unknown"; break;
    }
    {
        u32 i;
        for (i = 0u; i < 23u && n[i] != '\0'; i++) d->name[i] = (u8)n[i];
        d->name[i] = 0u;
    }
}

void crash_dump_write(const isr_regs_t *r)
{
    crash_dump_t *d = CRASH_BASE;
    u32 cr0, cr2, cr3, cr4;

    if (r == (const isr_regs_t *)0) return;
    crash_read_cr(&cr0, &cr2, &cr3, &cr4);

    d->magic     = CRASH_MAGIC;
    d->version   = CRASH_VERSION;
    d->vector    = r->vector;
    d->err       = r->err;
    d->eip       = r->eip;
    d->cs        = r->cs;
    d->eflags    = r->eflags;
    d->esp       = r->esp;
    d->eax       = r->eax;
    d->ebx       = r->ebx;
    d->ecx       = r->ecx;
    d->edx       = r->edx;
    d->esi       = r->esi;
    d->edi       = r->edi;
    d->ebp       = r->ebp;
    d->cr0       = cr0;
    d->cr2       = cr2;
    d->cr3       = cr3;
    d->cr4       = cr4;
    d->ticks     = pit_tick_count();
    d->crash_count = d->crash_count + 1u;
    d->stack_len = crash_copy_stack(r->esp, d->stack, CRASH_STACK_W);
    d->trace_len = crash_collect_trace(r->ebp, d->trace, CRASH_TRACE_MAX);
    crash_fill_name(d, r->vector);

    con_set_color(VGA_YELLOW, VGA_BLACK);
    con_puts("  [crash] dump written to 0x00700000 (");
    con_put_dec(d->crash_count);
    con_puts(" crashes)\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_flush();
}

int crash_dump_check(void)
{
    crash_dump_t *d = CRASH_BASE;
    u32 i;

    if (d->magic != CRASH_MAGIC || d->version != CRASH_VERSION) return 0;

    con_set_color(VGA_YELLOW, VGA_BLACK);
    con_puts("\n=============================================\n");
    con_puts("  PREVIOUS CRASH DUMP (recovered)\n");
    con_puts("=============================================\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("  Vector      : ");
    con_put_dec(d->vector);
    con_puts("  (");
    con_puts((const char *)d->name);
    con_puts(")\n");
    con_puts("  EIP         : ");
    con_put_hex32(d->eip);
    con_puts("  CS/EFLAGS   : ");
    con_put_hex32(d->cs);
    con_puts(" / ");
    con_put_hex32(d->eflags);
    con_putc('\n');
    con_puts("  EAX="); con_put_hex32(d->eax);
    con_puts("  EBX="); con_put_hex32(d->ebx);
    con_puts("  ECX="); con_put_hex32(d->ecx);
    con_puts("  EDX="); con_put_hex32(d->edx);
    con_putc('\n');
    con_puts("  CR0="); con_put_hex32(d->cr0);
    con_puts("  CR2="); con_put_hex32(d->cr2);
    con_puts("  CR3="); con_put_hex32(d->cr3);
    con_puts("  CR4="); con_put_hex32(d->cr4);
    con_putc('\n');
    con_puts("  Ticks       : ");
    con_put_dec(d->ticks);
    con_puts("  Stack words : ");
    con_put_dec(d->stack_len);
    con_puts("  Trace frames: ");
    con_put_dec(d->trace_len);
    con_putc('\n');
    con_puts("  Backtrace   :\n");
    for (i = 0u; i < d->trace_len; i++) {
        con_puts("    #");
        con_put_dec(i + 1u);
        con_puts("  eip=");
        con_put_hex32(d->trace[i]);
        con_putc('\n');
    }
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_flush();
    return 1;
}

void crash_dump_show(void)
{
    crash_dump_t *d = CRASH_BASE;
    u32 i;

    if (d->magic != CRASH_MAGIC || d->version != CRASH_VERSION) {
        con_puts("  No crash dump present.\n");
        return;
    }
    con_puts("  Vector      : ");
    con_put_dec(d->vector);
    con_puts("  (");
    con_puts((const char *)d->name);
    con_puts(")  err=");
    con_put_hex32(d->err);
    con_putc('\n');
    con_puts("  EIP         : ");
    con_put_hex32(d->eip);
    con_puts("  CS=");
    con_put_hex32(d->cs);
    con_puts("  EFLAGS=");
    con_put_hex32(d->eflags);
    con_puts("  ESP=");
    con_put_hex32(d->esp);
    con_putc('\n');
    con_puts("  EAX="); con_put_hex32(d->eax);
    con_puts(" EBX="); con_put_hex32(d->ebx);
    con_puts(" ECX="); con_put_hex32(d->ecx);
    con_puts(" EDX="); con_put_hex32(d->edx);
    con_puts(" ESI="); con_put_hex32(d->esi);
    con_puts(" EDI="); con_put_hex32(d->edi);
    con_puts(" EBP="); con_put_hex32(d->ebp);
    con_putc('\n');
    con_puts("  CR0="); con_put_hex32(d->cr0);
    con_puts(" CR2="); con_put_hex32(d->cr2);
    con_puts(" CR3="); con_put_hex32(d->cr3);
    con_puts(" CR4="); con_put_hex32(d->cr4);
    con_putc('\n');
    con_puts("  Ticks=");
    con_put_dec(d->ticks);
    con_puts("  Crashes=");
    con_put_dec(d->crash_count);
    con_puts("  Stack=");
    con_put_dec(d->stack_len);
    con_puts("w  Trace=");
    con_put_dec(d->trace_len);
    con_puts("f\n");
    con_puts("  Stack head :");
    for (i = 0u; i < d->stack_len && i < 8u; i++) {
        con_puts(" ");
        con_put_hex32(d->stack[i]);
    }
    con_putc('\n');
    con_puts("  Backtrace  :");
    for (i = 0u; i < d->trace_len; i++) {
        con_puts(" ");
        con_put_hex32(d->trace[i]);
    }
    con_putc('\n');
}

void crash_dump_clear(void)
{
    crash_dump_t *d = CRASH_BASE;
    d->magic = 0u;
    con_puts("  Crash dump cleared.\n");
}

/* 自检：构造模拟崩溃现场 → 写入 → 校验字段；随后清转储避免污染启动报告 */
int crash_selftest(void)
{
    isr_regs_t r;
    crash_dump_t *d = CRASH_BASE;
    u32 saved_magic = d->magic;
    u32 saved_count = d->crash_count;
    u32 i;

    /* 模拟 #GP(13) 崩溃现场 */
    memset(&r, 0, sizeof(r));
    r.vector = 13u;
    r.err    = 0x2u;
    r.eip    = 0x00123456u;
    r.cs     = 0x08u;
    r.eflags = 0x202u;
    r.esp    = 0x0008F000u;             /* 内核栈区内 */
    r.eax = 0x11111111u; r.ebx = 0x22222222u;
    r.ecx = 0x33333333u; r.edx = 0x44444444u;
    r.esi = 0x55555555u; r.edi = 0x66666666u;
    r.ebp = 0x0008F800u;

    crash_dump_write(&r);

    /* 逐字段校验 */
    if (d->magic   != CRASH_MAGIC) return 1;
    if (d->version != CRASH_VERSION) return 2;
    if (d->vector  != 13u) return 3;
    if (d->err     != 0x2u) return 4;
    if (d->eip     != 0x00123456u) return 5;
    if (d->cs      != 0x08u) return 6;
    if (d->eax     != 0x11111111u) return 7;
    if (d->ebx     != 0x22222222u) return 8;
    if (d->ecx     != 0x33333333u) return 9;
    if (d->edx     != 0x44444444u) return 10;
    if (d->esi     != 0x55555555u) return 11;
    if (d->edi     != 0x66666666u) return 12;
    if (d->ebp     != 0x0008F800u) return 13;
    if (d->crash_count != saved_count + 1u) return 14;
    if (d->stack_len == 0u) return 15;        /* 内核栈区应可复制 */
    if (d->trace_len > CRASH_TRACE_MAX) return 16;  /* 帧数不越界 */
    if (d->cr0 == 0u || d->cr3 == 0u) return 17;   /* CR 读取有效 */
    /* name 表非空 */
    for (i = 0u; i < 24u; i++) if (d->name[i] != 0u) break;
    if (i >= 24u) return 18;

    /* 恢复现场，避免污染下次启动报告 */
    if (saved_magic != CRASH_MAGIC) d->magic = 0u;
    d->crash_count = saved_count;
    return 0;
}
