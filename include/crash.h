/* ============================================================================
 * XOS 崩溃转储子系统（自研）
 * 内核异常（panic）时将完整现场写入固定物理内存区，重启后可读取诊断：
 *   异常号/错误码/故障 EIP/CS/EFLAGS/ESP、EAX..EBP、CR0/CR2/CR3/CR4、
 *   系统 tick、栈前 256 字节快照、栈回溯 EIP 链、累计崩溃次数。
 * 机制：panic_regs 停机前写转储 → 下次启动 crash_dump_check() 自动报告；
 *       shell 命令 crashdump / clearcrash 可随时查看与清除。
 * 不依赖任何外部实现；全部自研。
 * ========================================================================== */
#ifndef XOS_CRASH_H
#define XOS_CRASH_H

#include "types.h"
#include "idt.h"          /* isr_regs_t */

#define CRASH_MAGIC     0x58545255u   /* "XTRU" */
#define CRASH_VERSION   1u
#define CRASH_DUMP_ADDR 0x00700000u   /* 固定物理区（内核低 16MB 恒映射内） */
#define CRASH_DUMP_SIZE 0x00001000u   /* 4KB 转储区 */
#define CRASH_STACK_W   64u           /* 栈快照 u32 数（256 字节） */
#define CRASH_TRACE_MAX 16u           /* 回溯链 EIP 数上限 */

typedef struct crash_dump {
    u32 magic;
    u32 version;
    u32 vector;                       /* 异常号 */
    u32 err;                          /* 错误码 */
    u32 eip;                          /* 故障指令地址 */
    u32 cs;                           /* 崩溃时代码段 */
    u32 eflags;
    u32 esp;                          /* 崩溃时栈指针 */
    u32 eax, ebx, ecx, edx, esi, edi, ebp;
    u32 cr0, cr2, cr3, cr4;           /* 控制寄存器 */
    u32 ticks;                        /* 崩溃时系统 tick */
    u32 crash_count;                  /* 累计崩溃次数（含本次） */
    u32 stack_len;                    /* 实际复制的栈字 */
    u32 trace_len;                    /* 实际回溯帧数 */
    u32 stack[CRASH_STACK_W];
    u32 trace[CRASH_TRACE_MAX];
    u8  name[24];                     /* 异常名称（ASCII） */
} crash_dump_t;

/* 写崩溃转储（panic 停机前调用一次） */
void crash_dump_write(const isr_regs_t *r);

/* 启动检查：内存区存在有效转储则打印报告并返回 1，否则返回 0 */
int  crash_dump_check(void);

/* 显示当前转储（shell crashdump 命令） */
void crash_dump_show(void);

/* 清除转储（shell clearcrash 命令） */
void crash_dump_clear(void);

/* 自检：模拟一次崩溃现场写入并逐字段校验，返回 0 通过 */
int  crash_selftest(void);

#endif /* XOS_CRASH_H */
