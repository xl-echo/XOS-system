/* ============================================================================
 * XOS 中断与异常子系统
 * 覆盖：PIC（8259A）初始化与向量重映射、16 路 IRQ 分发、PIT 定时器驱动、
 *       软中断入口（int 0x80）、softirq 下半部、中断统计、风暴防护、
 *       看门狗、中断延迟采样、异常诊断与栈回溯、panic 统一出口。
 * 安全设计：所有故障路径都收敛到「打印现场 + 停机」，绝不三重故障；
 *           看门狗默认告警模式（WARN），不擅自停机，保证不损伤主机。
 * ============================================================================ */
#ifndef __XOS_IRQ_H__
#define __XOS_IRQ_H__

#include "types.h"
#include "idt.h"

/* ---- 向量布局 ---- */
#define IRQ_BASE         0x20u      /* PIC 重映射后 IRQ0 所在向量 */
#define IRQ_COUNT        16u
#define IRQ_END          0x30u      /* 0x20..0x2F 为 16 路 IRQ */
#define SYS_CALL_VEC     0x80u      /* int 0x80 软中断入口 */

/* ---- 8259A 端口 ---- */
#define PIC1_CMD         0x20u
#define PIC1_DATA        0x21u
#define PIC2_CMD         0xA0u
#define PIC2_DATA        0xA1u
#define PIC_EOI          0x20u

/* ---- PIT 定时器 ---- */
#define PIT_CH0          0x40u
#define PIT_CMD          0x43u
#define PIT_DIV_100HZ    11932u     /* 1193182/100 ≈ 11932 → 约 100 Hz */

/* ---- IRQ 描述符标志 ---- */
#define IRQF_ENABLED     0x01u
#define IRQF_SHARED      0x02u

/* ---- 软中断（下半部）---- */
#define SOFTIRQ_MAX      8u

typedef void (*irq_handler_t)(void *arg);
typedef void (*softirq_fn_t)(void);

/* ---- IRQ 描述符 ---- */
typedef struct {
    irq_handler_t handler;
    void         *arg;
    u32           flags;
    u32           count;            /* 到达次数 */
    u32           storm_hits;       /* 风暴告警次数 */
    u32           last_tsc;         /* 上次到达的 TSC（延迟测量） */
    u32           min_delta_tsc;    /* 最小到达间隔 */
    u32           max_delta_tsc;    /* 最大到达间隔 */
} irq_desc_t;

/* ---- 中断子系统统计 ---- */
typedef struct {
    u32 total_irqs;                 /* 全部 IRQ 到达总数 */
    u32 spurious;                   /* 假中断计数 */
    u32 softirq_run;                /* softirq 执行次数 */
    u32 syscall_count;              /* int 0x80 次数 */
    u32 bp_hits;                    /* #BP 可恢复命中 */
    u32 ud_hits;                    /* #UD 可恢复命中 */
    u32 pf_recovered;               /* 缺页恢复次数（与 VMM 联动） */
    u32 nested_max;                 /* 中断嵌套深度峰值 */
    u32 wdog_serviced;              /* 看门狗喂狗次数 */
    u32 wdog_timeouts;              /* 看门狗超时告警次数 */
    u32 storm_threshold;            /* 每秒风暴阈值 */
    u32 storm_events;               /* 风暴事件次数 */
    u32 tick_count;                 /* PIT 心跳计数 */
    u32 tick_delta_min;             /* 心跳间隔最小 TSC */
    u32 tick_delta_max;             /* 心跳间隔最大 TSC */
    u32 apic_present;               /* CPUID/MSR 探测到的 APIC 状态 */
    u32 ioapic_present;
} irq_stats_t;

/* ---- 函数接口 ---- */
void irq_init(void);
void irq_install_gates(void);                                   /* PIC + PIT + 门装载 */
void irq_enable(void);                                 /* sti */
void irq_disable(void);                                /* cli */
u32  irq_save(void);                                   /* pushf+cli，返回原 EFLAGS */
void irq_restore(u32 eflags);                          /* popf */
void irq_enable_nr(u32 irq);                           /* 开某路 IRQ */
void irq_disable_nr(u32 irq);                          /* 关某路 IRQ */
int  irq_request(u32 irq, irq_handler_t h, void *arg); /* 注册 handler */
int  irq_free(u32 irq);                                /* 注销 handler */
void irq_dispatch(isr_regs_t *r);                      /* isr_handler 分发入口 */

void raise_softirq(u32 nr);
void do_softirq(void);
int  softirq_register(u32 nr, softirq_fn_t fn);

void pit_init(void);
u32  pit_tick_count(void);
void wdog_arm(u32 ticks);
void wdog_feed(void);
u32  wdog_status(void);

void exc_stack_trace(u32 ebp, u32 limit);
void panic(const char *msg);
void panic_regs(isr_regs_t *r);

void syscall_handler(isr_regs_t *r);
int  exc_recover_skip(isr_regs_t *r);

void irq_stats(irq_stats_t *st);
void irq_dump(void);

/* 阶段 10 自检：返回 0 通过，否则返回唯一失败编号 */
u32  irq_selftest(void);
u32  irq_selftest_irq(void);
u32  irq_selftest_exc(void);

#endif /* __XOS_IRQ_H__ */
