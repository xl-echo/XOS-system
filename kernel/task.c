/* ============================================================================
 * XOS 进程与调度子系统（单 CPU 内核级任务骨架）
 * 20 子域落地：任务描述符 / 上下文切换 / 就绪队列 / CFS 简化 / 实时策略标记 /
 * 优先级与权重 / 时间片与抢占 / 单核负载均衡接口 / 内核线程创建 / 终止与回收 /
 * 等待队列与定时唤醒 / 信号位图 / 进程组与会话 / init 与 idle / 调度统计 /
 * CPU 亲和性 / 优先级继承字段 / 节流记账 / 僵尸清理 / 调试导出。
 * ============================================================================ */
#include "task.h"
#include "kmalloc.h"
#include "irq.h"
#include "console.h"
#include "string.h"

/* ---- 内部状态 ---- */
static task_t task_pool[TASK_MAX];
static u32   pid_bmp;                     /* pid 位图：bit n = pid n 占用（0=idle 常驻） */
static task_t *current_task;
static u32   rq_head, rq_tail, rq_count;  /* 就绪队列（环形数组，槽=pid） */
static u32   rq_slots[TASK_MAX];
static task_t *zombie_head;               /* 僵尸链表 */
static u32 idle_esp_save;                 /* idle 首次切换的 esp 保存槽 */
u32         sched_ticks;                 /* 调度器 tick 计数（sync/futex 等外部使用） */
static u32   switch_total;
static u32   need_resched_flag;
static u32   last_sched_tick;
static u32   sched_delay_min = 0xFFFFFFFFu;
static u32   sched_delay_max;
static u32   task_locked;
static u32   orphan_count;                  /* 待 init 收养的孤儿数 */
static u32   daemon_count;                  /* 现存守护进程数 */
#define INIT_STACK_ADDR  0x0000C000u        /* 低1MB 固定区：stage2 顶部(0xBE00)之上、内核映像(0x10000)之下，E820 保留 0-0x80000 内 */
static u8 *const init_stack = (u8 *)INIT_STACK_ADDR; /* init(pid1) 常驻栈：低1MB E820 保留区固定地址，不占 bss（避免抬高 __bss_end 挤压启动栈） */
static void  init_thread(void *arg);

static void scheduler_pick(void);

/* ---- 时钟/锁原语（单 CPU：irq 保存恢复即原子） ---- */
static inline void lock_task(void)   { task_locked = irq_save(); }
static inline void unlock_task(void) { irq_restore(task_locked); }

task_t *task_current(void) { return current_task; }
u32 task_pid(void) { return current_task ? current_task->pid : 0u; }

/* ---- nice → weight：nice 0=1024，nice+4 权重减半（CFS 近似） ---- */
u32 nice_to_weight(i32 nice)
{
    u32 shift;
    if (nice <= 0) {
        shift = (u32)(-nice) / 4u;
        if (shift > 12u) shift = 12u;
        return 1024u << shift;
    }
    shift = (u32)nice / 4u;
    if (shift > 10u) shift = 10u;
    return 1024u >> shift;
}

/* ---- 就绪队列（环形） ---- */
static void rq_enqueue(task_t *t)
{
    if (rq_count >= TASK_MAX) return;
    rq_slots[rq_tail] = t->pid;
    rq_tail = (rq_tail + 1u) % TASK_MAX;
    rq_count++;
}

static u32 rq_dequeue_abs(u32 abs)
{
    u32 pid, k, nxt;
    pid = rq_slots[abs];
    k = abs;
    while (k != rq_tail) {
        nxt = (k + 1u) % TASK_MAX;
        rq_slots[k] = rq_slots[nxt];
        k = nxt;
    }
    if (rq_count > 0u) rq_count--;
    rq_tail = (rq_tail + TASK_MAX - 1u) % TASK_MAX;
    if (rq_count == 0u) rq_head = rq_tail;
    return pid;
}

/* ---- 任务查询 ---- */
static task_t *task_by_pid(u32 pid)
{
    if (pid < TASK_MAX && (pid_bmp & (1u << pid)))
        return &task_pool[pid];
    return NULL;
}

/* ============================================================================
 * 上下文切换入口（汇编 _switch_to 声明于 switch.S）
 * ============================================================================ */
extern void _switch_to(u32 **prev_esp, u32 **next_esp);
extern void task_entry_asm(void);
u32 task_entry_asm_addr(void) { return (u32)task_entry_asm; }

/* ============================================================================
 * 调度器：选 vruntime 最小者（CFS 近似，线性扫描 O(n)）
 * ============================================================================ */
static void scheduler_pick(void)
{
    u32 k, abs, best = 0xFFFFFFFFu;
    u64 min_vr = 0xFFFFFFFFFFFFFFFFull;
    task_t *t;

    if (rq_count == 0u) {                    /* 无就绪 → 回到 idle */
        current_task = &task_pool[0];
        return;
    }

    for (k = 0; k < rq_count; k++) {
        abs = (rq_head + k) % TASK_MAX;      /* 环形队头偏移 → 绝对槽位 */
        t = &task_pool[rq_slots[abs]];
        if (t->state != TASK_READY) continue;
        if (t->throttled) continue;                /* 节流任务跳过，配额窗口到期恢复 */
        /* 实时任务（FIFO/RR 标记）优先于 CFS（简化：实时位图语义保留） */
        if (t->policy != SCHED_CFS) {
            current_task = t;
            rq_dequeue_abs(abs);
            return;
        }
        {
            u64 eff = t->vruntime;                 /* 优先级继承：有效 vruntime 修正 */
            if (t->prio_boost > 0) {
                u64 dec = (u64)(u32)t->prio_boost * 4096u;
                eff = (t->vruntime > dec) ? t->vruntime - dec : 0ull;
            } else if (t->prio_boost < 0) {
                eff = t->vruntime + (u64)(u32)(-t->prio_boost) * 4096u;
            }
            if (eff < min_vr) {
                min_vr = eff;
                best = abs;
            }
        }
    }
    if (best != 0xFFFFFFFFu) {
        current_task = &task_pool[rq_dequeue_abs(best)];
        return;
    }
    current_task = NULL;                     /* 只可能全部 WAITING（异常）→ idle */
}

void schedule(void)
{
    task_t *prev, *next;
    u32 lock;

    lock = irq_save();
    prev = current_task;

    if (prev && prev->state == TASK_RUNNING && prev->pid != 0u) {
        /* CFS 记账：切走时补记一个 tick 的虚拟运行时间。
           防止同 vruntime 任务（如 init 空转 yield）按入队顺序死锁。 */
        prev->vruntime += 1024u * 1024u / (prev->weight ? prev->weight : 1024u);
        prev->state = TASK_READY;
        rq_enqueue(prev);
    }

    scheduler_pick();
    next = current_task;

    if (next == NULL) {                      /* 无就绪任务 → 回到 idle */
        current_task = &task_pool[0];
        next = current_task;
    }
    if (prev == next || prev == NULL) {
        if (next) next->state = TASK_RUNNING;
        irq_restore(lock);
        return;
    }

    next->state = TASK_RUNNING;
    next->switches++;
    switch_total++;
    if (last_sched_tick != 0u) {
        u32 d = sched_ticks - last_sched_tick;
        if (d < sched_delay_min) sched_delay_min = d;
        if (d > sched_delay_max) sched_delay_max = d;
    }
    last_sched_tick = sched_ticks;

    /* 上下文切换：切走；本函数在任务被切回时继续执行并返回 */
    _switch_to(&prev->esp, &next->esp);
    irq_restore(lock);
}

void task_yield(void)
{
    schedule();
}

/* ---- 任务入口包装（汇编调用） ---- */
void task_exit_self(void)
{
    task_exit(0u);
}

/* ---- 创建内核线程 ---- */
int task_create(const char *name, void (*entry)(void *), void *arg,
                u32 policy, i32 nice)
{
    u32 pid, i, lock;
    u8  *stack;
    u32 *esp0;

    lock = irq_save();
    if (!entry || rq_count >= TASK_MAX - 1u) { irq_restore(lock); return -1; }
    if (nice < -20) nice = -20;
    if (nice > 19)  nice = 19;

    for (i = 1; i < TASK_MAX; i++) {
        if (!(pid_bmp & (1u << i))) break;
    }
    if (i >= TASK_MAX) { irq_restore(lock); return -2; }
    pid = i;
    irq_restore(lock);

    stack = (u8 *)kmalloc(TASK_STACK_SIZE, 8u, KMALLOC_ZERO);
    if (stack == NULL) return -3;
    memset(stack, 0xA5, TASK_STACK_SIZE);        /* 毒化栈（检测越界写） */

    esp0 = (u32 *)(stack + TASK_STACK_SIZE - 28u);
    esp0[0] = 0xDEADBEEFu;                       /* edi 占位 */
    esp0[1] = 0xDEADBEEFu;                       /* esi */
    esp0[2] = 0xDEADBEEFu;                       /* ebx */
    esp0[3] = 0xDEADBEEFu;                       /* ebp */
    esp0[4] = (u32)task_entry_asm_addr();        /* ret 目标（switch_to ret 弹出） */
    esp0[5] = (u32)entry;                        /* 入口函数 */
    esp0[6] = (u32)arg;                          /* 参数 */

    lock = irq_save();
    pid_bmp |= 1u << pid;
    memset(&task_pool[pid], 0, sizeof(task_t));
    task_pool[pid].pid = pid;
    task_pool[pid].state = TASK_READY;
    task_pool[pid].policy = policy;
    task_pool[pid].nice = nice;
    task_pool[pid].weight = nice_to_weight(nice);
    task_pool[pid].counter = TIMESLICE_TICKS;
    task_pool[pid].vruntime = 0ull;
    task_pool[pid].cpus_allowed = 1u;
    task_pool[pid].pgid = pid;
    task_pool[pid].sid = pid;
    task_pool[pid].esp = esp0;                   /* 首个切换：pop 4 寄存器占位 + ret */
    task_pool[pid].stack_base = (void *)stack;
    if (name)
        strncpy(task_pool[pid].name, name, TASK_NAME_LEN - 1u);
    else
        strcpy(task_pool[pid].name, "task");
    rq_enqueue(&task_pool[pid]);
    if (current_task && current_task->pid != 0u && current_task->pid != 1u)
        current_task->child_count++;               /* 孤儿转移语义：创建者记子 */
    irq_restore(lock);
    return (int)pid;
}

/* ---- 终止与回收 ---- */
void task_exit(u32 code)
{
    u32 lock = irq_save();
    task_t *t = current_task;
    if (t == NULL || t->pid == 0u) { irq_restore(lock); return; }
    t->state = TASK_EXIT;
    t->exit_code = code;
    if (t->child_count > 0u && !t->is_init) {
        orphan_count += t->child_count;            /* 子任务移交 init 收养 */
        t->child_count = 0u;
    }
    t->zombie_next = zombie_head;
    zombie_head = t;
    irq_restore(lock);
    schedule();                                  /* 不再返回（除非被误唤醒） */
    for (;;) __asm__ __volatile__("hlt");
}

int task_join(u32 pid, u32 *code)
{
    task_t *t, **pp;
    u32 lock;
    u32 t0 = 0xFFFFFFFFu;
    for (;;) {
        lock = irq_save();
        pp = &zombie_head;
        t = zombie_head;
        while (t) {
            if (t->pid == pid) break;
            pp = &t->zombie_next;
            t = t->zombie_next;
        }
        if (t) {
            *pp = t->zombie_next;
            if (code) *code = t->exit_code;
            if (t->stack_base) kfree(t->stack_base);
            pid_bmp &= ~(1u << pid);
            irq_restore(lock);
            return 0;
        }
        irq_restore(lock);
        if (task_by_pid(pid) == NULL) return -1;   /* 任务不存在 */
        if (t0 == 0xFFFFFFFFu) t0 = sched_ticks;
        if (sched_ticks > t0 + 500u)             /* 5 秒看门狗（节流任务需等待恢复） */
            return -2;
        task_yield();
    }
}

/* ---- 定时唤醒：扫描睡眠任务 ---- */
static void wake_timed(void)
{
    u32 i;
    for (i = 1; i < TASK_MAX; i++) {
        task_t *t = &task_pool[i];
        if ((pid_bmp & (1u << i)) && t->state == TASK_WAITING &&
            t->wait_until_tick != 0u && sched_ticks >= t->wait_until_tick) {
            t->wait_until_tick = 0u;
            t->state = TASK_READY;
            t->wakeups++;
            rq_enqueue(t);
        }
    }
}

/* ---- PIT 心跳：时间片 + 唤醒 + 僵尸清理 ---- */
void task_tick(void)
{
    task_t *cur;
    sched_ticks++;
    cur = current_task;
    if (cur && cur->pid != 0u && cur->state == TASK_RUNNING) {
        cur->run_ticks++;
        cur->vruntime += 1024u * 1024u / (cur->weight ? cur->weight : 1024u);
        if (cur->counter > 0u) {
            cur->counter--;
            if (cur->counter == 0u) {
                cur->counter = TIMESLICE_TICKS;
                need_resched_flag = 1u;
            }
        }
        /* 配额结算：窗口内预算用满 → 节流至窗口结束 */
        if (cur->quota_pct > 0u && cur->quota_pct < 100u) {
            cur->quota_used++;
            if (cur->quota_budget != 0xFFFFFFFFu &&
                cur->quota_used >= cur->quota_budget) {
                cur->throttled = 1u;
                cur->throttle_until_tick = sched_ticks + TASK_QUOTA_WINDOW;
                cur->quota_used = 0u;
                cur->quota_budget = (TASK_QUOTA_WINDOW * cur->quota_pct) / 100u;
                need_resched_flag = 1u;            /* 立即让出 */
            }
        }
    }
    /* 节流恢复：窗口到期解除 */
    {
        u32 i;
        for (i = 1; i < TASK_MAX; i++) {
            task_t *t = &task_pool[i];
            if ((pid_bmp & (1u << i)) && t->throttled &&
                sched_ticks >= t->throttle_until_tick) {
                t->throttled = 0u;
            }
        }
    }
    wake_timed();
}

void task_schedule_check(void)
{
    if (need_resched_flag) {
        need_resched_flag = 0u;
        schedule();
    }
}

/* ---- 信号（内核线程语义） ---- */
int task_kill(u32 pid, u32 sig)
{
    task_t *t;
    u32 lock = irq_save();
    t = task_by_pid(pid);
    if (t == NULL || t->state == TASK_EXIT) { irq_restore(lock); return -1; }
    if (sig >= 32u) { irq_restore(lock); return -2; }
    t->pending_signals |= 1u << sig;
    if (t->state == TASK_WAITING) {              /* 唤醒挂起信号的任务 */
        t->wait_until_tick = 0u;
        t->state = TASK_READY;
        t->wakeups++;
        rq_enqueue(t);
    }
    irq_restore(lock);
    return 0;
}

u32 task_signal_pending(void)
{
    task_t *t = current_task;
    if (!t) return 0u;
    return t->pending_signals;
}

void task_signal_clear(u32 sig)
{
    task_t *t = current_task;
    if (t && sig < 32u)
        t->pending_signals &= ~(1u << sig);
}

/* ---- 定时睡眠 ---- */
void msleep(u32 ms)
{
    u32 ticks, lock;
    task_t *t;
    if (ms == 0u) { task_yield(); return; }
    ticks = ms / 10u;                            /* PIT 100Hz → 10ms/tick */
    if (ticks == 0u) ticks = 1u;
    lock = irq_save();
    t = current_task;
    if (t == NULL) { irq_restore(lock); return; }
    t->state = TASK_WAITING;
    t->wait_until_tick = sched_ticks + ticks;
    irq_restore(lock);
    schedule();
    t = current_task;
    if (t) t->wait_until_tick = 0u;
}

/* ---- 统计 ---- */
void task_stats(task_stats_t *st)
{
    u32 i, n = 0, m = 0;
    if (!st) return;
    for (i = 1; i < TASK_MAX; i++)
        if (pid_bmp & (1u << i)) {
            n++;
            if (task_pool[i].run_ticks > m) m = task_pool[i].run_ticks;
        }
    st->task_count = n + 1u;                     /* + idle */
    st->ready_count = rq_count;
    st->switch_total = switch_total;
    st->pid_next = 0u;
    st->zombie_count = 0u;
    {
        task_t *z = zombie_head;
        while (z) { st->zombie_count++; z = z->zombie_next; }
    }
    st->max_run_ticks = m;
    st->sched_delay_min = (sched_delay_min == 0xFFFFFFFFu) ? 0u : sched_delay_min;
    st->sched_delay_max = sched_delay_max;
    st->ticks_total = sched_ticks;
    st->need_resched = need_resched_flag;
}

/* ---- 调试导出 ---- */
void task_dump(void)
{
    u32 i;
    task_stats_t st;
    con_printf("\n  Task scheduler dump:\n");
    con_printf("    pid  name            state pol nice weight run_ticks  sw  vruntime\n");
    for (i = 0; i < TASK_MAX; i++) {
        task_t *t = &task_pool[i];
        if (i == 0u || (pid_bmp & (1u << i))) {
            con_printf("    %2u   %s %5u %4u %4d %6u %9u %3u %08x\n",
                t->pid, t->name, t->state, t->policy, t->nice, t->weight,
                t->run_ticks, t->switches, (u32)(t->vruntime & 0xFFFFFFFFu));
        }
    }
    task_stats(&st);
    con_printf("    tasks=%u ready=%u switches=%u ticks=%u zombies=%u\n"
                   "    sched_delay min=%u max=%u need_resched=%u\n",
        st.task_count, st.ready_count, st.switch_total, st.ticks_total,
        st.zombie_count, st.sched_delay_min, st.sched_delay_max, st.need_resched);
}

/* ============================================================================
 * 阶段 11 自检
 * ============================================================================ */

static void task_core_thread(void *arg);

/* 核心自检：pid 分配/回收、就绪队列、权重、状态机、栈构造 */
static u32 wtab[41];                             /* nice -20..0..20 权重采样 */

u32 task_selftest_core(void)
{
    i32 n;
    int rc;
    u32 lock;

    if (!current_task) return 1;                 /* idle 必须存在 */
    if (task_pool[0].pid != 0u) return 2;
    if (strcmp(task_pool[0].name, "idle")) return 3;

    for (n = -20; n <= 20; n++)
        wtab[n + 20] = nice_to_weight(n);
    if (!(wtab[0] > wtab[20])) return 4;         /* nice 低 → 权重大 */
    if (!(wtab[40] <= wtab[20])) return 5;       /* nice 高 → 权重小 */

    rc = task_create("t_core", task_core_thread, NULL, SCHED_CFS, 0);
    if (rc < 1) return 6;                        /* 必须拿到有效 pid */
    lock = irq_save();
    if (task_pool[rc].state != TASK_READY) { irq_restore(lock); return 7; }
    if (task_pool[rc].esp == NULL) { irq_restore(lock); return 8; }
    if (task_pool[rc].stack_base == NULL) { irq_restore(lock); return 9; }
    if (task_pool[rc].counter != TIMESLICE_TICKS) { irq_restore(lock); return 10; }
    irq_restore(lock);

    if (task_kill((u32)rc, SIG_TERM) != 0) return 11;
    if (task_join((u32)rc, NULL) != 0) return 12; /* 回收后 pid 应可复用 */
    lock = irq_save();
    if (pid_bmp & (1u << rc)) { irq_restore(lock); return 13; }
    irq_restore(lock);
    return 0;
}

static u32 core_thread_done;
static void task_core_thread(void *arg)
{
    (void)arg;
    core_thread_done = 1u;
    task_exit(7u);
}

/* 真实调度自检：3 个计数线程 + 时间片轮转 */
static u32 tA_count, tB_count, tC_count;
static u32 tA_done, tB_done, tC_done;

static void task_run_a(void *arg)
{
    u32 i;
    (void)arg;
    for (i = 0; i < 500u; i++) {
        tA_count += 3u;
        if ((i & 3u) == 3u) task_yield();
    }
    tA_done = 1u;
    task_exit(11u);
}

static void task_run_b(void *arg)
{
    u32 i;
    (void)arg;
    for (i = 0; i < 400u; i++) {
        tB_count += 5u;
        if ((i & 3u) == 3u) task_yield();
    }
    tB_done = 1u;
    task_exit(12u);
}

static void task_run_c(void *arg)
{
    u32 i;
    (void)arg;
    for (i = 0; i < 300u; i++) {
        tC_count += 7u;
        if ((i & 3u) == 3u) task_yield();
    }
    tC_done = 1u;
    task_exit(13u);
}

u32 task_selftest_sched(void)
{
    u32 i, pid_a, pid_b, pid_c;
    task_stats_t st;

    tA_count = tB_count = tC_count = 0u;
    tA_done = tB_done = tC_done = 0u;

    pid_a = task_create("sched_a", task_run_a, NULL, SCHED_CFS, 0);
    pid_b = task_create("sched_b", task_run_b, NULL, SCHED_CFS, 1);
    pid_c = task_create("sched_c", task_run_c, NULL, SCHED_CFS, -1);
    if (pid_a < 1 || pid_b < 1 || pid_c < 1) return 3;

    /* 让调度器跑：当前线程反复让出，直到三个子线程完成（上限保护） */
    for (i = 0; i < 20000u && !(tA_done && tB_done && tC_done); i++)
        task_yield();
    if (!(tA_done && tB_done && tC_done)) return 4;   /* 调度未推进 */
    if (!(tA_count > 0u && tB_count > 0u && tC_count > 0u)) return 5;
    if (tA_count != 1500u || tB_count != 2000u || tC_count != 2100u) return 6;

    task_stats(&st);
    if (st.switch_total == 0u) return 7;              /* 必须发生过真实切换 */
    if (st.task_count < 4u) return 8;                 /* idle + 3 子线程 + 当前 */
    if (st.ticks_total == 0u) return 9;

    if (task_join((u32)pid_a, NULL) != 0) return 10;
    if (task_join((u32)pid_b, NULL) != 0) return 11;
    if (task_join((u32)pid_c, NULL) != 0) return 12;
    return 0;
}

/* 等待队列自检：msleep 定时唤醒 */
static u32 sleep_done;
static u32 sleep_ticks;

static void task_sleeper(void *arg)
{
    u32 t0, t1;
    (void)arg;
    t0 = sched_ticks;
    msleep(30u);
    t1 = sched_ticks;
    sleep_ticks = t1 - t0;
    sleep_done = 1u;
    task_exit(21u);
}

u32 task_selftest_wait(void)
{
    u32 i, t0;
    int pid;
    sleep_done = 0u;
    sleep_ticks = 0u;
    pid = task_create("sleeper", task_sleeper, NULL, SCHED_CFS, 0);
    if (pid < 1) return 1;
    /* init 即 idle：idle 无法入队睡眠（schedule 空队列回落自身），
     * 故用 yield（有就绪即切换）+ hlt（无任务时空转推进 PIT tick）
     * 的混合等待，直至 sleeper 完成或超时（≥10 tick）。 */
    t0 = sched_ticks;
    for (i = 0; i < 20000u && !sleep_done; i++) {
        task_yield();
        if (sched_ticks - t0 > 10u) break;
        __asm__ __volatile__("hlt");
    }
    if (!sleep_done) return 2;
    if (sleep_ticks < 2u) return 3;                  /* 30ms ≥ 3 tick @100Hz */
    if (task_join((u32)pid, NULL) != 0) return 4;
    return 0;
}

/* 信号 / 退出 / 回收自检 */
static u32 sig_observed;

static void task_signaled(void *arg)
{
    u32 i;
    (void)arg;
    for (i = 0; i < 200u; i++) {
        if (task_signal_pending() & (1u << SIG_ALRM)) {
            task_signal_clear(SIG_ALRM);
            sig_observed = 1u;
            break;
        }
        task_yield();
    }
    task_exit(31u);
}

u32 task_selftest_sig(void)
{
    int pid;
    u32 i;
    sig_observed = 0u;
    pid = task_create("signaled", task_signaled, NULL, SCHED_CFS, 0);
    if (pid < 1) return 1;
    if (task_kill((u32)pid, SIG_ALRM) != 0) return 2;
    for (i = 0; i < 20000u && !sig_observed; i++)
        task_yield();
    if (!sig_observed) return 3;
    if (task_join((u32)pid, NULL) != 0) return 4;
    return 0;
}

/* ============================================================================
 * 守护进程与 init / 优先级继承 / 节流限流 / 多核接口（08 册二轮）
 * ============================================================================ */
static void init_thread(void *arg)
{
    UNUSED(arg);
    for (;;) msleep(50u);                /* init 定时空转：睡眠-唤醒循环，不忙等占 CPU */
}

int task_init_process(void)
{
    u32 lock = irq_save();
    if (pid_bmp & 2u) { irq_restore(lock); return -1; }   /* pid 1 已存在 */
    pid_bmp |= 2u;
    memset(&task_pool[1], 0, sizeof(task_t));
    task_pool[1].pid = 1u;
    task_pool[1].state = TASK_READY;
    task_pool[1].policy = SCHED_CFS;
    task_pool[1].nice = 19;              /* 最低权重：空转线程不抢测试任务 */
    task_pool[1].weight = nice_to_weight(19);
    task_pool[1].counter = TIMESLICE_TICKS;
    task_pool[1].cpus_allowed = 1u;
    task_pool[1].pgid = 1u;
    task_pool[1].sid = 1u;
    task_pool[1].is_init = 1u;
    task_pool[1].stack_base = NULL;      /* init 常驻，不回收 */
    task_pool[1].esp = (u32 *)(init_stack + TASK_STACK_SIZE - 28u);
    task_pool[1].esp[0] = 0xDEADBEEFu;
    task_pool[1].esp[1] = 0xDEADBEEFu;
    task_pool[1].esp[2] = 0xDEADBEEFu;
    task_pool[1].esp[3] = 0xDEADBEEFu;
    task_pool[1].esp[4] = (u32)task_entry_asm_addr();
    task_pool[1].esp[5] = (u32)init_thread;
    task_pool[1].esp[6] = 0u;
    strcpy(task_pool[1].name, "init");
    rq_enqueue(&task_pool[1]);
    irq_restore(lock);
    return 0;
}

int task_daemonize(const char *name, void (*entry)(void *), void *arg)
{
    int pid = task_create(name, entry, arg, SCHED_CFS, 0);
    u32 lock;
    if (pid < 0) return pid;
    lock = irq_save();
    task_pool[pid].is_daemon = 1u;
    task_pool[pid].sid = 0u;             /* 脱离会话与进程组（后台语义） */
    task_pool[pid].pgid = 0u;
    daemon_count++;
    irq_restore(lock);
    return pid;
}

u32 task_orphan_count(void) { u32 l = irq_save(); u32 n = orphan_count; irq_restore(l); return n; }

void task_adopt_orphans(void)
{
    u32 lock = irq_save();
    if (task_pool[1].is_init && orphan_count > 0u) {
        task_pool[1].child_count += orphan_count;
        orphan_count = 0u;
    }
    irq_restore(lock);
}

u32 task_daemon_count(void) { u32 l = irq_save(); u32 n = daemon_count; irq_restore(l); return n; }

int task_boost(u32 pid, i32 amount)
{
    task_t *t;
    u32 lock = irq_save();
    t = task_by_pid(pid);
    if (!t || t->state == TASK_EXIT) { irq_restore(lock); return -1; }
    if (amount > 20) amount = 20;
    if (amount < -20) amount = -20;
    t->prio_boost = amount;
    t->boost_owner_pid = current_task ? current_task->pid : 0u;
    irq_restore(lock);
    return 0;
}

int task_unboost(u32 pid) { return task_boost(pid, 0); }

i32 task_boost_amount(u32 pid)
{
    task_t *t;
    u32 lock = irq_save();
    i32 r;
    t = task_by_pid(pid);
    r = (t && t->state != TASK_EXIT) ? t->prio_boost : 0;
    irq_restore(lock);
    return r;
}

int task_set_quota(u32 pid, u32 pct)
{
    task_t *t;
    u32 lock = irq_save();
    t = task_by_pid(pid);
    if (!t || t->state == TASK_EXIT) { irq_restore(lock); return -1; }
    if (pct > 100u) { irq_restore(lock); return -2; }
    t->quota_pct = pct;
    t->quota_budget = (pct == 0u) ? 0xFFFFFFFFu : (TASK_QUOTA_WINDOW * pct) / 100u;
    t->quota_used = 0u;
    t->throttled = 0u;
    irq_restore(lock);
    return 0;
}

u32 task_get_quota(u32 pid)
{
    task_t *t;
    u32 lock = irq_save();
    u32 r;
    t = task_by_pid(pid);
    r = (t && t->state != TASK_EXIT) ? t->quota_pct : 0u;
    irq_restore(lock);
    return r;
}

u32 task_throttled_count(void)
{
    u32 i, n = 0u;
    u32 lock = irq_save();
    for (i = 1u; i < TASK_MAX; i++) {
        if ((pid_bmp & (1u << i)) && task_pool[i].throttled) n++;
    }
    irq_restore(lock);
    return n;
}

int task_set_affinity(u32 pid, u32 mask)
{
    task_t *t;
    u32 lock = irq_save();
    t = task_by_pid(pid);
    if (!t || t->state == TASK_EXIT) { irq_restore(lock); return -1; }
    if (mask != 1u) { irq_restore(lock); return -2; }   /* 单 CPU 仅允许 cpu0 */
    t->cpus_allowed = mask;
    irq_restore(lock);
    return 0;
}

u32 task_get_affinity(u32 pid)
{
    task_t *t;
    u32 lock = irq_save();
    u32 r;
    t = task_by_pid(pid);
    r = (t && t->state != TASK_EXIT) ? t->cpus_allowed : 0u;
    irq_restore(lock);
    return r;
}

u32 task_balance_hint(void) { return 0u; }   /* 单 CPU：无迁移目标 */

/* ---- 08 册扩展自检 ---- */
static u32 ext_orphan_child_pid;
static u32 ext_orphan_done;
static u32 ext_daemon_done;
static u32 ext_quota_ticks;
static volatile u64 ext_quota_spin;

static void ext_orphan_child(void *arg)
{
    UNUSED(arg);
    ext_orphan_done = 1u;
    task_exit(0);
}

static void ext_orphan_parent(void *arg)
{
    UNUSED(arg);
    ext_orphan_child_pid = (u32)task_create("ext-child", ext_orphan_child,
                                            NULL, SCHED_CFS, 0);
    task_exit(0);
}

static void ext_daemon_entry(void *arg)
{
    UNUSED(arg);
    ext_daemon_done = 1u;
    task_exit(0);
}

static void ext_quota_entry(void *arg)
{
    u32 base;
    UNUSED(arg);
    /* 确定性忙等：运行满 12 tick 再退出（预算 10 → 必然触发节流并恢复）。
       忙等期间 PIT 中断照常结算配额，节流后自动让出。 */
    base = current_task ? current_task->run_ticks : 0u;
    while (current_task && (current_task->run_ticks - base) < 12u)
        __asm__ __volatile__("pause");   /* 防优化：PIT 中断会递增 run_ticks */
    ext_quota_ticks++;
    task_exit(0);
}

static u32 st_ext_initproc(void)
{
    task_t *t = task_by_pid(1);
    if (!t || !t->is_init) return 1u;
    return 0u;
}

static u32 st_ext_daemon(void)
{
    int pid;
    u32 i;
    ext_daemon_done = 0u;
    pid = task_daemonize("ext-daemon", ext_daemon_entry, NULL);
    if (pid < 1) return 2u;
    for (i = 0u; i < 20000u && !ext_daemon_done; i++) task_yield();
    if (!ext_daemon_done) return 3u;
    if (task_daemon_count() == 0u) return 4u;
    if (task_join((u32)pid, NULL) != 0) return 5u;
    return 0u;
}

static u32 st_ext_orphan(void)
{
    int ppid;
    u32 i;
    u32 oc_before = task_orphan_count();
    ext_orphan_child_pid = 0u;
    ext_orphan_done = 0u;
    ppid = task_create("ext-parent", ext_orphan_parent, NULL, SCHED_CFS, 0);
    if (ppid < 1) return 6u;
    for (i = 0u; i < 20000u && ext_orphan_child_pid == 0u; i++) task_yield();
    if (ext_orphan_child_pid == 0u) return 7u;
    while (task_by_pid((u32)ppid) && task_by_pid((u32)ppid)->state != TASK_EXIT)
        task_yield();                                  /* 等父退出 → 孤儿转移 */
    if (task_join((u32)ppid, NULL) != 0) return 8u;    /* 回收父任务（清 pid 位） */
    if (task_orphan_count() <= oc_before) return 9u;
    task_adopt_orphans();
    if (task_orphan_count() != oc_before) return 10u;
    for (i = 0u; i < 20000u && !ext_orphan_done; i++) task_yield();
    task_join(ext_orphan_child_pid, NULL);
    return 0u;
}

static u32 st_ext_boost(void)
{
    int pid = task_create("ext-boost", ext_daemon_entry, NULL, SCHED_CFS, 0);
    u32 code;
    if (pid < 1) return 10u;
    if (task_boost((u32)pid, 5) != 0) return 11u;
    if (task_boost_amount((u32)pid) != 5) return 12u;
    if (task_unboost((u32)pid) != 0) return 13u;
    if (task_boost_amount((u32)pid) != 0) return 14u;
    if (task_join((u32)pid, &code) != 0) return 15u;
    return 0u;
}

static u32 st_ext_quota(void)
{
    int pid = task_create("ext-quota", ext_quota_entry, NULL, SCHED_CFS, 0);
    u32 i, peak = 0u;
    if (pid < 1) return 20u;
    if (task_set_quota((u32)pid, 0) != 0) return 21u;
    if (task_set_quota((u32)pid, 101) != -2) return 22u;
    if (task_set_quota((u32)pid, 10) != 0) return 23u;
    if (task_get_quota((u32)pid) != 10) return 24u;
    ext_quota_ticks = 0u;
    for (i = 0u; i < 6000000u && task_by_pid((u32)pid); i++) {
        u32 th = task_throttled_count();
        if (th > peak) peak = th;
        if (task_by_pid((u32)pid)->state == TASK_EXIT) break;
        task_yield();
    }
    if (task_join((u32)pid, NULL) != 0) return 25u;    /* 任务应已跑完退出 */
    if (peak == 0u) return 26u;                        /* 节流必须真实发生 */
    return 0u;
}

static u32 st_ext_affinity(void)
{
    int pid = task_create("ext-aff", ext_daemon_entry, NULL, SCHED_CFS, 0);
    if (pid < 1) return 30u;
    if (task_set_affinity((u32)pid, 1u) != 0) return 31u;
    if (task_set_affinity((u32)pid, 2u) != -2) return 32u;
    if (task_get_affinity((u32)pid) != 1u) return 33u;
    if (task_balance_hint() != 0u) return 34u;
    if (task_join((u32)pid, NULL) != 0) return 35u;
    return 0u;
}

u32 task_selftest_ext(void)
{
    u32 r;
    r = st_ext_initproc();   if (r) return r;
    r = st_ext_daemon();     if (r) return r;
    r = st_ext_orphan();     if (r) return r;
    r = st_ext_boost();      if (r) return r;
    r = st_ext_quota();      if (r) return r;
    r = st_ext_affinity();   if (r) return r;
    return 0u;
}

/* ============================================================================
 * 初始化：idle 任务（pid 0）+ 初始上下文
 * ============================================================================ */
void task_init(void)
{
    u32 i;
    for (i = 0; i < TASK_MAX; i++)
        memset(&task_pool[i], 0, sizeof(task_t));
    rq_head = rq_tail = rq_count = 0u;
    sched_ticks = 0u;
    switch_total = 0u;
    need_resched_flag = 0u;
    last_sched_tick = 0u;
    zombie_head = NULL;
    orphan_count = 0u;
    daemon_count = 0u;

    pid_bmp = 1u;                                  /* pid 0 = idle 常驻 */
    task_pool[0].pid = 0u;
    task_pool[0].state = TASK_RUNNING;
    task_pool[0].policy = SCHED_CFS;
    task_pool[0].nice = 0;
    task_pool[0].weight = nice_to_weight(0);
    task_pool[0].counter = 0xFFFFFFFFu;            /* idle 永不耗尽 */
    task_pool[0].vruntime = 0ull;
    task_pool[0].cpus_allowed = 1u;
    task_pool[0].esp = &idle_esp_save;
    task_pool[0].stack_base = NULL;
    strcpy(task_pool[0].name, "idle");
    current_task = &task_pool[0];
}
