/* ============================================================================
 * XOS CPU 特权级支持：重建 GDT（ring0/ring3 + TSS）与任务状态段
 * 目标：为 ring3 用户态程序提供硬件级隔离基础（CS/SS 特权切换、esp0 内核栈）
 * ========================================================================== */
#include "cpu.h"
#include "console.h"
#include "string.h"

typedef struct {
    u16 limit;
    u32 base;
} PACKED gdt_ptr_t;

/* TSS（32 位，按 Intel 文档布局，共 104 字节 + IO 位图偏移） */
typedef struct {
    u32 link;
    u32 esp0; u32 ss0;
    u32 esp1; u32 ss1;
    u32 esp2; u32 ss2;
    u32 cr3;
    u32 eip; u32 eflags;
    u32 eax; u32 ecx; u32 edx; u32 ebx;
    u32 esp; u32 ebp; u32 esi; u32 edi;
    u32 es; u32 cs; u32 ss; u32 ds; u32 fs; u32 gs;
    u32 ldt;
    u16 trap; u16 iomap;
} PACKED tss_t;

static u64   g_gdt[6];           /* 0 null / 1 R0CODE / 2 R0DATA / 3 R3CODE / 4 R3DATA / 5 TSS */
static tss_t g_tss;
static u32   g_ready;

void cpu_gdt_init(u32 kernel_esp0)
{
    /* 段 0：null */
    g_gdt[0] = 0;
    /* 段 1：ring0 代码 0x08 —— 与 Stage2/中断桩硬编码一致（0x00CF9A 平铺） */
    g_gdt[1] = 0x00CF9A000000FFFFull;
    /* 段 2：ring0 数据 0x10（isr_common_stub 硬编码 movw $0x10） */
    g_gdt[2] = 0x00CF92000000FFFFull;
    /* 段 3：ring3 代码 0x18：P=1 DPL=3 S=1 type=A(execute/read) DB=1 G=1 */
    g_gdt[3] = 0x00CFFA000000FFFFull;
    /* 段 4：ring3 数据 0x20：P=1 DPL=3 S=1 type=2(read/write) DB=1 G=1 */
    g_gdt[4] = 0x00CFF2000000FFFFull;

    /* 段 5：TSS 0x28（可用 32 位 TSS，DPL=0） */
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.esp0 = kernel_esp0;
    g_tss.ss0  = GDT_R0_DATA;
    g_tss.cs   = GDT_R0_CODE;
    g_tss.ds   = GDT_R0_DATA;
    g_tss.ss   = GDT_R0_DATA;
    g_tss.es   = GDT_R0_DATA;
    g_tss.fs   = GDT_R0_DATA;
    g_tss.gs   = GDT_R0_DATA;
    g_tss.iomap = (u16)sizeof(tss_t);
    {
        u32 base = (u32)&g_tss;
        u32 lim  = (u32)sizeof(tss_t) - 1u;
        g_gdt[5]  = (u64)(lim & 0xFFFFu);
        g_gdt[5] |= (u64)((base & 0xFFFFFFu)) << 16;
        g_gdt[5] |= (u64)(0x89u) << 40;              /* type=9 available 32-bit TSS, P=1, DPL=0 */
        g_gdt[5] |= (u64)((lim >> 16) & 0xFu) << 48;
        g_gdt[5] |= (u64)((base >> 24) & 0xFFu) << 56;
    }

    {
        gdt_ptr_t gdtp;
        gdtp.limit = (u16)(sizeof(g_gdt) - 1u);
        gdtp.base  = (u32)g_gdt;
        __asm__ __volatile__("lgdt %0" : : "m"(gdtp));
        __asm__ __volatile__("ltr %%ax" : : "a"(GDT_TSS));
    }

    g_ready = 1u;
}

void cpu_set_esp0(u32 esp0)
{
    g_tss.esp0 = esp0;
}

u32 cpu_tss_get_esp0(void)
{
    return g_tss.esp0;
}

int cpu_user_supported(void)
{
    return g_ready ? 1 : 0;
}

int cpu_selftest(void)
{
    u32 esp0 = 0x1234000u;
    u32 saved = g_tss.esp0;
    if (!g_ready) return 1;
    /* 1: esp0 读写（测试后恢复原值，避免污染真实 esp0） */
    cpu_set_esp0(esp0);
    if (cpu_tss_get_esp0() != esp0) return 2;
    cpu_set_esp0(saved);
    /* 2: GDT 用户段属性正确（读回验证）——DPL 占位 45-46，掩码必须 0x3（0xF 会带入 P 位） */
    if (((g_gdt[3] >> 45) & 0x3u) != 3u) return 3;   /* R3 code DPL=3 */
    if (((g_gdt[4] >> 45) & 0x3u) != 3u) return 4;   /* R3 data DPL=3 */
    if (((g_gdt[1] >> 45) & 0x3u) != 0u) return 5;   /* R0 code DPL=0 */
    /* 3: TSS 描述符类型=available 或 busy（ltr 后 CPU 自动置 busy 位 0x89->0x8B） */
    {
        u32 t = (u32)((g_gdt[5] >> 40) & 0xFFu);
        if (t != 0x89u && t != 0x8Bu) return 6;
    }
    /* 4: TSS 段选择子 */
    if (GDT_R3_CODE != 0x18u || GDT_R3_DATA != 0x20u || GDT_TSS != 0x28u) return 7;
    /* 5: 内核段选择子保持与中断桩一致 */
    if (GDT_R0_CODE != 0x08u || GDT_R0_DATA != 0x10u) return 8;
    return 0;
}
