/* ============================================================================
 * XOS 进程与调度子系统
 * 本阶段范围（单 CPU 内核级任务骨架）：
 *   - 任务描述符（进程=内核线程，pid 位图分配，独立内核栈）
 *   - 汇编上下文切换 switch_to（保存/恢复 callee-saved + ESP）
 *   - 就绪队列（环形数组 O(1) 入出队）
 *   - 调度策略：CFS 简化（vruntime 最小者优先）+ nice 权重表；
 *     实时标记（SCHED_FIFO/RR）保留字段与队列语义（RT 队列为空时回退 CFS）
 *   - 时间片与抢占：PIT tick 递减 counter，耗尽置 need_resched，
 *     中断返回路径执行 schedule
 *   - 等待队列与定时唤醒（msleep）
 *   - 信号：pending 位图 + kill 接口（内核线程语义）
 *   - 退出/僵尸/回收：task_exit → 僵尸链表 → task_join/自动清理
 *   - 进程组/会话/亲和性/优先级继承/节流：字段与接口（单 CPU 语义）
 *   - 调度统计：切换次数/每任务运行 tick/唤醒次数
 * 多核负载均衡与用户态 fork 归后续册（08 扩展/10 系统调用/33 虚拟化）。
 * ============================================================================ */
#ifndef __XOS_TASK_H__
#define __XOS_TASK_H__

#include "types.h"

/* ---- 状态 ---- */
#define TASK_READY      0u
#define TASK_RUNNING    1u
#define TASK_WAITING    2u
#define TASK_EXIT       3u

/* ---- 调度策略 ---- */
#define SCHED_CFS       0u
#define SCHED_FIFO      1u
#define SCHED_RR        2u

/* ---- 配额 ---- */
#define TASK_MAX        32u
#define TASK_STACK_SIZE 8192u
#define TIMESLICE_TICKS 5u            /* 默认时间片（PIT 100Hz → 50ms） */
#define TASK_NAME_LEN   16u
#define TASK_QUOTA_WINDOW 100u   /* 配额结算窗口（tick）：窗口内预算=窗口×配额% */

/* ---- 信号位（内核线程语义，位图） ---- */
#define SIG_NONE        0u
#define SIG_TERM        1u
#define SIG_KILL        2u
#define SIG_ALRM        3u

typedef struct task task_t;

struct task {
    u32   pid;
    u32   state;
    u32   policy;                    /* SCHED_CFS/FIFO/RR */
    i32   nice;                      /* -20..19 */
    u32   weight;                    /* nice → weight 映射 */
    u32   counter;                   /* 剩余时间片（tick） */
    u64   vruntime;                  /* CFS 虚拟运行时间 */
    u32   run_ticks;                 /* 累计运行 tick */
    u32   switches;                  /* 被调度次数 */
    u32   wakeups;                   /* 唤醒次数 */
    u32   pending_signals;           /* 位图 */
    u32   pgid;                      /* 进程组 */
    u32   sid;                       /* 会话 */
    u32   cpus_allowed;              /* 亲和掩码（单 CPU 恒 1） */
    i32   prio_boost;                /* 优先级继承提升量（vruntime 折算，>0 优先） */
    u32   boost_owner_pid;           /* 触发提升方 pid（诊断审计） */
    u32   quota_pct;                 /* CPU 配额 0..100（0=不限） */
    u32   quota_budget;              /* 配额窗口内剩余预算（tick，0xFFFFFFFF=不限） */
    u32   quota_used;                /* 配额窗口内已用 tick */
    u32   throttled;                 /* 1=被节流（调度器跳过，窗口到期恢复） */
    u32   throttle_until_tick;       /* 节流恢复时刻 */
    u32   is_init;                   /* 1=init 进程（孤儿收养者） */
    u32   is_daemon;                 /* 1=守护进程（脱离会话/后台语义） */
    u32   child_count;               /* 子任务计数（孤儿转移语义） */
    u32   exit_code;
    u32   wait_until_tick;           /* 定时唤醒点（0=不睡眠） */
    char  name[TASK_NAME_LEN];
    u32  *esp;                       /* 切换保存的栈指针 */
    void *stack_base;                /* kmalloc 栈基址（回收用） */
    task_t *zombie_next;             /* 僵尸链表 */
};

/* ---- 对外接口 ---- */
void task_init(void);
int  task_create(const char *name, void (*entry)(void *), void *arg,
                 u32 policy, i32 nice);
void task_yield(void);
void task_exit(u32 code);
void task_tick(void);                /* PIT 心跳：时间片递减 + 定时唤醒 + 僵尸清理 */
void task_schedule_check(void);      /* 中断返回路径：need_resched 则调度 */
void schedule(void);
int  task_kill(u32 pid, u32 sig);
u32  task_signal_pending(void);
int  task_join(u32 pid, u32 *code);
void msleep(u32 ms);
task_t *task_current(void);
u32  task_pid(void);

/* ---- 统计/调试 ---- */
typedef struct {
    u32 task_count;                  /* 现存任务数（含 idle） */
    u32 ready_count;                 /* 就绪队列长度 */
    u32 switch_total;                /* 累计切换次数 */
    u32 pid_next;                    /* 下一个 pid */
    u32 zombie_count;                /* 僵尸数 */
    u32 max_run_ticks;
    u32 sched_delay_min;             /* 调度延迟（tick 粒度） */
    u32 sched_delay_max;
    u32 ticks_total;                 /* 调度器 tick 总数 */
    u32 need_resched;                /* 抢占挂起 */
} task_stats_t;

void task_stats(task_stats_t *st);
void task_dump(void);

/* ---- 守护进程与 init（孤儿收养） ---- */
int  task_init_process(void);                        /* 创建 init(pid 1)，幂等 */
int  task_daemonize(const char *name, void (*entry)(void *), void *arg);
u32  task_orphan_count(void);                        /* 待收养孤儿数 */
void task_adopt_orphans(void);                       /* init 收养孤儿 */
u32  task_daemon_count(void);

/* ---- 优先级继承（boost/unboost） ---- */
int  task_boost(u32 pid, i32 amount);                /* amount>0 提升，<0 压低，0 清除 */
int  task_unboost(u32 pid);
i32  task_boost_amount(u32 pid);

/* ---- 调度器节流与限流 ---- */
int  task_set_quota(u32 pid, u32 pct);               /* 0..100；超限拒绝 */
u32  task_get_quota(u32 pid);
u32  task_throttled_count(void);

/* ---- 多核负载均衡（单 CPU 接口语义） ---- */
int  task_set_affinity(u32 pid, u32 mask);           /* 单 CPU 仅允许 1 */
u32  task_get_affinity(u32 pid);
u32  task_balance_hint(void);                        /* 单 CPU 恒 0 */

/* ---- 阶段 11 自检：返回 0 通过，否则唯一失败编号 ---- */
u32  task_selftest_core(void);
u32  task_selftest_sched(void);
u32  task_selftest_wait(void);
u32  task_selftest_sig(void);
u32  task_selftest_ext(void);                        /* init/daemon/orphan/boost/quota/affinity */

#endif /* __XOS_TASK_H__ */
