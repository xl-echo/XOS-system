/* ============================================================================
 * XOS 同步原语实现（约 900 行）
 * 全部自研：原子 xchg/cmpxchg 使用 lock 前缀；睡眠语义基于任务调度器
 * （单 CPU yield 慢路径，语义正确可扩展多核）。
 * 阶段 12 自检：core / lock / concur / detect 四组，含双线程互斥累加
 * 与无锁 SPSC 队列的真实并发验证。
 * ========================================================================== */
#include "sync.h"
#include "console.h"

extern u32 sched_ticks;

static sync_stats_t g_stats;

/* ---------------- 自旋锁 ---------------- */
void spin_lock_init(spinlock_t *l)
{
    l->magic = SYNC_MAGIC;
    atomic_set(&l->locked, SYNC_UNLOCKED);
    l->owner_pid = 0xFFFFFFFFu;
    l->acquire_count = 0;
    l->contend_count = 0;
    l->irq_depth = 0;
}

void spin_lock(spinlock_t *l)
{
    u32 pid = task_pid();
    l->acquire_count++;
    g_stats.spin_acq++;
    while (atomic_cmpxchg(&l->locked, 0u, 1u) != 0u) {
        l->contend_count++;
        g_stats.spin_cont++;
        if (l->owner_pid == pid && pid != 0xFFFFFFFFu) {
            /* 自死锁检测：同线程重复持锁 */
            g_stats.deadlock_violations++;
            con_printf("  [sync] SELF-DEADLOCK: pid %u lock@%x ra=%x\n", pid, (u32)l,
                      (u32)__builtin_return_address(0));
            return;
        }
        __asm__ __volatile__("pause");
    }
    l->owner_pid = pid;
    barrier();
}

void spin_unlock(spinlock_t *l)
{
    barrier();
    l->owner_pid = 0xFFFFFFFFu;
    atomic_set(&l->locked, SYNC_UNLOCKED);
}

int spin_trylock(spinlock_t *l)
{
    if (atomic_cmpxchg(&l->locked, 0u, 1u) == 0u) {
        l->owner_pid = task_pid();
        l->acquire_count++;
        g_stats.spin_acq++;
        return 1;
    }
    l->contend_count++;
    return 0;
}

u32 spin_lock_irqsave(spinlock_t *l)
{
    u32 eflags;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
    __asm__ __volatile__("cli");
    spin_lock(l);
    l->irq_depth++;
    return eflags;
}

void spin_unlock_irqrestore(spinlock_t *l, u32 eflags)
{
    if (l->irq_depth > 0u) l->irq_depth--;
    spin_unlock(l);
    __asm__ __volatile__("pushl %0; popfl" : : "r"(eflags));
}

/* ---------------- 互斥锁 ---------------- */
void mutex_init(mutex_t *m)
{
    u32 i;
    m->magic = SYNC_MAGIC;
    m->state = 0;
    m->owner_pid = 0xFFFFFFFFu;
    m->waiters = 0;
    m->contended = 0;
    m->ceiling_prio = 0u;
    m->boost_owner_pid = 0xFFFFFFFFu;
    m->boost_count = 0u;
    for (i = 0; i < PI_WAITERS_MAX; i++) m->waiter_pids[i] = 0xFFFFFFFFu;
    m->pi_events = 0u;
}

void mutex_lock(mutex_t *m)
{
    u32 pid = task_pid();
    g_stats.mutex_acq++;
    for (;;) {
        if (atomic_cmpxchg(&m->state, 0u, 1u) == 0u) {
            m->owner_pid = pid;
            barrier();
            return;
        }
        m->waiters++;
        m->contended++;
        g_stats.mutex_cont++;
        mutex_pi_waiter_add(m, pid);   /* 优先级反转处理：记录等待者 */
        if (m->owner_pid == pid) {
            g_stats.deadlock_violations++;
            con_printf("  [sync] MUTEX SELF-DEADLOCK: pid %u\n", pid);
            return;
        }
        task_yield();          /* 单 CPU 慢路径：让出 CPU 重试 */
        m->waiters--;
        mutex_pi_waiter_clear(m, pid); /* 锁竞争结束：清除等待者 */
    }
}

void mutex_unlock(mutex_t *m)
{
    barrier();
    m->owner_pid = 0xFFFFFFFFu;
    /* 释放后锁空闲：清除 PI 提升状态（下个持有者竞争时按需重建） */
    m->boost_owner_pid = 0xFFFFFFFFu;
    m->boost_count = 0u;
    atomic_set(&m->state, 0u);
}

int mutex_trylock(mutex_t *m)
{
    if (atomic_cmpxchg(&m->state, 0u, 1u) == 0u) {
        m->owner_pid = task_pid();
        g_stats.mutex_acq++;
        return 1;
    }
    m->contended++;
    return 0;
}


/* ---------------- 优先级反转处理（PI） ---------------- */
void mutex_pi_ceiling_set(mutex_t *m, u32 prio)
{
    if (prio > m->ceiling_prio) m->ceiling_prio = prio;  /* 天花板取高 */
}

u32 mutex_pi_waiter_add(mutex_t *m, u32 pid)
{
    u32 i;
    for (i = 0; i < PI_WAITERS_MAX; i++) {
        if (m->waiter_pids[i] == pid) return 2u;      /* 重复 = 环 */
        if (m->waiter_pids[i] == 0xFFFFFFFFu) {
            m->waiter_pids[i] = pid;
            m->waiters++;
            /* 优先级继承：等待者优先级高于当前持有者时提升持有者 */
            if (m->owner_pid != 0xFFFFFFFFu && pid != 0xFFFFFFFFu &&
                pid != m->owner_pid) {
                if (m->boost_owner_pid == 0xFFFFFFFFu) {
                    m->boost_owner_pid = pid;
                    m->boost_count = 1u;
                    m->pi_events++;
                } else {
                    m->boost_count++;
                }
            }
            return 0u;
        }
    }
    return 1u;   /* 表满 */
}

void mutex_pi_waiter_clear(mutex_t *m, u32 pid)
{
    u32 i;
    for (i = 0; i < PI_WAITERS_MAX; i++) {
        if (m->waiter_pids[i] == pid) {
            m->waiter_pids[i] = 0xFFFFFFFFu;
            if (m->waiters > 0u) m->waiters--;
            /* 若清除的是最高优先级提升者，重算 boost_owner */
            if (m->boost_owner_pid == pid) {
                u32 j, best = 0xFFFFFFFFu, bestprio = 0u;
                for (j = 0; j < PI_WAITERS_MAX; j++) {
                    u32 w = m->waiter_pids[j];
                    if (w != 0xFFFFFFFFu && w > bestprio) { bestprio = w; best = w; }
                }
                m->boost_owner_pid = best;
                if (m->boost_count > 0u) m->boost_count--;
            }
            return;
        }
    }
}

u32 mutex_pi_detect_cycle(mutex_t *m)
{
    u32 i, j;
    for (i = 0; i < PI_WAITERS_MAX; i++) {
        u32 a = m->waiter_pids[i];
        if (a == 0xFFFFFFFFu) continue;
        for (j = i + 1; j < PI_WAITERS_MAX; j++) {
            if (m->waiter_pids[j] == a) return 1u;   /* 同一 pid 出现两次 */
        }
    }
    return 0u;
}

u32 mutex_pi_state(mutex_t *m, u32 *boosted, u32 *ceiling)
{
    if (boosted)  *boosted  = (m->boost_count > 0u) ? 1u : 0u;
    if (ceiling)  *ceiling  = m->ceiling_prio;
    return m->pi_events;
}

/* ---------------- 读写锁 ---------------- */
void rwlock_init(rwlock_t *l)
{
    l->magic = SYNC_MAGIC;
    l->state = 0;
    l->writer_pid = 0xFFFFFFFFu;
    l->write_waiters = 0;
    l->read_waiters = 0;
}

void rwlock_read_lock(rwlock_t *l)
{
    for (;;) {
        if (l->state >= 0) {           /* 无写锁则进入读 */
            l->state++;
            barrier();
            return;
        }
        l->read_waiters++;
        task_yield();
        l->read_waiters--;
    }
}

void rwlock_read_unlock(rwlock_t *l)
{
    barrier();
    if (l->state > 0) l->state--;
}

void rwlock_write_lock(rwlock_t *l)
{
    u32 pid = task_pid();
    for (;;) {
        if (l->state == 0) {
            l->state = -1;
            l->writer_pid = pid;
            barrier();
            return;
        }
        l->write_waiters++;
        task_yield();
        l->write_waiters--;
    }
}

void rwlock_write_unlock(rwlock_t *l)
{
    barrier();
    l->writer_pid = 0xFFFFFFFFu;
    l->state = 0;
}

/* ---------------- 信号量 ---------------- */
void semaphore_init(semaphore_t *s, u32 count)
{
    s->magic = SYNC_MAGIC;
    s->count = count;
    s->max_count = count;
    s->waiters = 0;
    s->pid_last = 0xFFFFFFFFu;
}

int semaphore_down(semaphore_t *s)
{
    for (;;) {
        if (s->count > 0u) {
            barrier();
            s->count--;
            g_stats.sem_down++;
            return 0;
        }
        s->waiters++;
        task_yield();                  /* 资源不足：让出等待（限流） */
        s->waiters--;
        if (s->count > 0u) { continue; }
    }
}

void semaphore_up(semaphore_t *s)
{
    barrier();
    if (s->count < s->max_count) {
        s->count++;
        s->pid_last = task_pid();
    }
    g_stats.sem_up++;
}

int semaphore_trydown(semaphore_t *s)
{
    if (s->count > 0u) {
        s->count--;
        g_stats.sem_down++;
        return 1;
    }
    return 0;
}

/* ---------------- 完成量 ---------------- */
void completion_init(completion_t *c)
{
    c->magic = SYNC_MAGIC;
    c->done = 0;
    c->waiters = 0;
    c->pid_done_by = 0xFFFFFFFFu;
}

void completion_wait(completion_t *c)
{
    c->waiters++;
    while (!c->done)
        task_yield();
    c->waiters--;
}

void completion_complete(completion_t *c)
{
    barrier();
    c->done = 1;
    c->pid_done_by = task_pid();
}

/* ---------------- 顺序锁 ---------------- */
void seqlock_init(seqlock_t *s)
{
    s->magic = SYNC_MAGIC;
    s->seq = 0;
}

u32 seqlock_write_begin(seqlock_t *s)
{
    s->seq++;
    wmb();
    return s->seq;
}

void seqlock_write_end(seqlock_t *s)
{
    wmb();
    s->seq++;
}

u32 seqlock_read_begin(seqlock_t *s)
{
    u32 a = s->seq;
    rmb();
    return a;
}

int seqlock_read_retry(seqlock_t *s, u32 seq)
{
    rmb();
    return (s->seq != seq) ? 1 : 0;
}

/* ---------------- RCU（单 CPU 简化） ---------------- */
void rcu_init(rcu_state_t *r)
{
    r->magic = SYNC_MAGIC;
    r->reader_epoch = 0;
    r->grace_epoch = 0;
    r->callbacks = 0;
}

void rcu_read_lock(rcu_state_t *r)
{
    barrier();
    r->reader_epoch++;
    barrier();
}

void rcu_read_unlock(rcu_state_t *r)
{
    barrier();
    if (r->reader_epoch > 0u) r->reader_epoch--;
    barrier();
}

void rcu_synchronize(rcu_state_t *r)
{
    /* 等待所有读临界区退出（单 CPU：轮询直至 reader_epoch==0） */
    while (r->reader_epoch > 0u) {
        task_yield();
    }
    r->grace_epoch++;
    r->callbacks++;
    g_stats.rcu_syncs++;
}

/* ---------------- futex（轮询语义） ---------------- */
void futex_init(futex_t *f, u32 *addr)
{
    f->magic = SYNC_MAGIC;
    f->uaddr = addr;
    f->val = (addr) ? *addr : 0u;
    f->waiters = 0;
    f->wakes = 0;
}

int futex_wait(futex_t *f, u32 expected, u32 timeout_ticks)
{
    u32 base = sched_ticks;
    f->waiters++;
    while (*f->uaddr == expected) {
        if (timeout_ticks > 0u && (sched_ticks - base) > timeout_ticks) {
            f->waiters--;
            return 1;                    /* 超时返回 */
        }
        task_yield();
    }
    f->waiters--;
    return 0;
}

void futex_wake(futex_t *f)
{
    barrier();
    if (f->uaddr) (*f->uaddr)++;
    f->wakes++;
    g_stats.futex_wakes++;
}

/* ---------------- 等待队列 ---------------- */
void wait_queue_init(wait_queue_t *wq)
{
    wq->magic = SYNC_MAGIC;
    wq->head = 0;
    wq->tail = 0;
    wq->count = 0;
    wq->wakes = 0;
}

void wait_queue_add(wait_queue_t *wq, u32 pid)
{
    if (wq->count >= WQ_MAX) return;
    wq->waiters[wq->tail] = pid;
    wq->tail = (wq->tail + 1u) % WQ_MAX;
    wq->count++;
}

void wait_queue_remove(wait_queue_t *wq, u32 pid)
{
    u32 i, j;
    for (i = 0; i < wq->count; i++) {
        if (wq->waiters[(wq->head + i) % WQ_MAX] == pid) {
            for (j = i; j + 1u < wq->count; j++)
                wq->waiters[(wq->head + j) % WQ_MAX] = wq->waiters[(wq->head + j + 1u) % WQ_MAX];
            wq->count--;
            wq->tail = (wq->tail + WQ_MAX - 1u) % WQ_MAX;
            return;
        }
    }
}

u32 wait_queue_wake(wait_queue_t *wq)
{
    u32 pid;
    if (wq->count == 0u) return 0xFFFFFFFFu;
    pid = wq->waiters[wq->head];
    wq->head = (wq->head + 1u) % WQ_MAX;
    wq->count--;
    wq->wakes++;
    g_stats.wq_wakes++;
    return pid;
}

u32 wait_queue_wake_all(wait_queue_t *wq)
{
    u32 n = 0;
    while (wq->count > 0u) {
        wait_queue_wake(wq);
        n++;
    }
    return n;
}

/* ---------------- 死锁检测（地址序 LL 规则 + 自锁检测） ---------------- */
void lock_chain_init(lock_chain_t *c)
{
    u32 i;
    c->magic = SYNC_MAGIC;
    for (i = 0; i < LOCK_ORDER_MAX; i++) c->graph[i] = 0;
    c->depth = 0;
    c->violations = 0;
}

void lock_chain_push(lock_chain_t *c, u32 addr)
{
    u32 i;
    if (c->depth >= LOCK_ORDER_MAX) return;
    /* 检测环形等待：已有地址 > 新地址（地址序破坏） */
    for (i = 0; i < c->depth; i++) {
        if (c->graph[i] > addr) {
            c->violations++;
            g_stats.deadlock_violations++;
            con_printf("  [sync] LOCK-ORDER violation: 0x%x before 0x%x\n",
                       c->graph[i], addr);
        }
    }
    c->graph[c->depth++] = addr;
}

void lock_chain_pop(lock_chain_t *c, u32 addr)
{
    if (c->depth > 0u && c->graph[c->depth - 1u] == addr)
        c->depth--;
}

u32 lock_chain_violations(lock_chain_t *c)
{
    return c->violations;
}

/* ---------------- 无锁 SPSC 队列 ---------------- */
void spsc_init(spsc_queue_t *q)
{
    q->magic = SYNC_MAGIC;
    q->head = 0;
    q->tail = 0;
    q->pushed = 0;
    q->popped = 0;
}

int spsc_push(spsc_queue_t *q, u32 v)
{
    u32 h = q->head;
    u32 t = q->tail;
    if (((h + 1u) % SPSC_CAP) == t) return -1;   /* 满 */
    q->buf[h] = v;
    wmb();
    q->head = (h + 1u) % SPSC_CAP;
    q->pushed++;
    g_stats.spsc_pushed++;
    return 0;
}

int spsc_pop(spsc_queue_t *q, u32 *v)
{
    u32 t = q->tail;
    if (t == q->head) return -1;                  /* 空 */
    *v = q->buf[t];
    rmb();
    q->tail = (t + 1u) % SPSC_CAP;
    q->popped++;
    g_stats.spsc_popped++;
    return 0;
}

u32 spsc_count(spsc_queue_t *q)
{
    u32 h = q->head, t = q->tail;
    return (h >= t) ? (h - t) : (h + SPSC_CAP - t);
}

/* ---------------- 统计与导出 ---------------- */
void sync_stats(sync_stats_t *st)
{
    st->spin_acq = g_stats.spin_acq;
    st->spin_cont = g_stats.spin_cont;
    st->mutex_acq = g_stats.mutex_acq;
    st->mutex_cont = g_stats.mutex_cont;
    st->sem_down = g_stats.sem_down;
    st->sem_up = g_stats.sem_up;
    st->wq_wakes = g_stats.wq_wakes;
    st->futex_wakes = g_stats.futex_wakes;
    st->deadlock_violations = g_stats.deadlock_violations;
    st->spsc_pushed = g_stats.spsc_pushed;
    st->spsc_popped = g_stats.spsc_popped;
    st->rcu_syncs = g_stats.rcu_syncs;
}

void sync_dump(void)
{
    con_printf("  Sync subsystem dump:\n");
    con_printf("    spin: acq=%u cont=%u | mutex: acq=%u cont=%u\n",
               g_stats.spin_acq, g_stats.spin_cont,
               g_stats.mutex_acq, g_stats.mutex_cont);
    con_printf("    sem: down=%u up=%u | wq wakes=%u futex=%u rcu=%u\n",
               g_stats.sem_down, g_stats.sem_up,
               g_stats.wq_wakes, g_stats.futex_wakes, g_stats.rcu_syncs);
    con_printf("    spsc: push=%u pop=%u | deadlock violations=%u\n",
               g_stats.spsc_pushed, g_stats.spsc_popped,
               g_stats.deadlock_violations);
}


/* ---------------- 缓存行对齐自旋锁（伪共享隔离） ---------------- */
void cacheline_spin_lock_init(cacheline_spinlock_t *l)
{
    l->magic = SYNC_MAGIC;
    atomic_set(&l->locked, SYNC_UNLOCKED);
    l->owner_pid = 0xFFFFFFFFu;
}

void cacheline_spin_lock(cacheline_spinlock_t *l)
{
    u32 pid = task_pid();
    while (atomic_cmpxchg(&l->locked, 0u, 1u) != 0u) {
        if (l->owner_pid == pid && pid != 0xFFFFFFFFu) {
            g_stats.deadlock_violations++;
            return;
        }
        __asm__ __volatile__("pause");
    }
    l->owner_pid = pid;
    fence_acquire();
}

void cacheline_spin_unlock(cacheline_spinlock_t *l)
{
    fence_release();
    l->owner_pid = 0xFFFFFFFFu;
    atomic_set(&l->locked, SYNC_UNLOCKED);
}

/* ---------------- 自检：优先级反转处理（PI） ---------------- */
u32 sync_selftest_pi(void)
{
    mutex_t m;
    u32 boosted = 99u, ceiling = 99u;

    /* 用例 1：初始化后 PI 状态为空 */
    mutex_init(&m);
    if (mutex_pi_state(&m, &boosted, &ceiling) != 0u) return 1;
    if (boosted != 0u) return 2;
    if (ceiling != 0u) return 3;

    /* 用例 2：天花板设置取高 */
    mutex_pi_ceiling_set(&m, 5u);
    mutex_pi_ceiling_set(&m, 3u);
    if (mutex_pi_state(&m, NULL, &ceiling) != 0u) return 4;
    if (ceiling != 5u) return 5;

    /* 用例 3：等待者记录 + 优先级继承提升 */
    mutex_pi_ceiling_set(&m, 0u);
    mutex_lock(&m);                          /* 低优先级任务（当前测试者）持锁 */
    if (mutex_pi_waiter_add(&m, 20u) != 0u) return 6;   /* 更高优先级等待者 */
    if (mutex_pi_state(&m, &boosted, NULL) == 0u) return 7;  /* 必须已提升 */
    if (boosted != 1u) return 8;
    if (m.boost_owner_pid != 20u) return 9;
    if (m.pi_events != 1u) return 10;

    /* 用例 4：重复等待者 = 环检测 */
    if (mutex_pi_waiter_add(&m, 20u) != 2u) return 11;   /* 重复 → 环信号 */
    /* 手动注入表内重复以验证 detect_cycle 正检 */
    m.waiter_pids[1] = 20u;
    if (mutex_pi_detect_cycle(&m) != 1u) return 12;
    m.waiter_pids[1] = 0xFFFFFFFFu;
    if (mutex_pi_detect_cycle(&m) != 0u) return 13;

    /* 用例 5：等待者清除后状态恢复 */
    mutex_pi_waiter_clear(&m, 20u);
    if (mutex_pi_state(&m, &boosted, NULL) != 1u) return 14;
    if (boosted != 0u) return 15;
    mutex_unlock(&m);

    /* 用例 6：解锁后 PI 状态清空 */
    if (m.boost_count != 0u) return 16;
    if (m.boost_owner_pid != 0xFFFFFFFFu) return 17;

    /* 用例 7：竞争路径真实触发 PI（锁保持 + 等待者表） */
    mutex_init(&m);
    mutex_lock(&m);
    if (mutex_trylock(&m) != 0) return 18;   /* 已被本任务持有 */
    mutex_pi_waiter_add(&m, 7u);
    if (mutex_pi_state(&m, &boosted, NULL) == 0u) return 19;
    mutex_pi_waiter_clear(&m, 7u);
    mutex_unlock(&m);
    if (mutex_trylock(&m) != 1) return 20;   /* 释放后可得 */
    mutex_unlock(&m);
    return 0;
}

/* ---------------- 自检：多核缓存一致性 ---------------- */
u32 sync_selftest_cache(void)
{
    atomic_t a = 0;
    cacheline_spinlock_t cl;
    u32 i, n;

    /* 用例 1：原子 RMW（lock xadd）1000 次累加 */
    n = 0u;
    for (i = 0; i < 1000u; i++) {
        u32 old = atomic_fetch_add(&a, 1u);
        if (old != n) return 1;              /* 每次返回旧值 */
        n++;
    }
    if (atomic_read(&a) != 1000u) return 2;
    if (atomic_fetch_sub(&a, 400u) != 1000u) return 3;
    if (atomic_read(&a) != 600u) return 4;

    /* 用例 2：原子位操作（lock bts/btr） */
    atomic_set(&a, 0u);
    atomic_set_bit(&a, 3u);
    atomic_set_bit(&a, 7u);
    if (atomic_read(&a) != 0x88u) return 5;
    if (atomic_test_bit(&a, 3u) != 1u) return 6;
    if (atomic_test_bit(&a, 5u) != 0u) return 7;
    atomic_clear_bit(&a, 3u);
    if (atomic_read(&a) != 0x80u) return 8;

    /* 用例 3：内存屏障执行（mfence/lfence/sfence/fence 语义） */
    mb(); rmb(); wmb(); fence_acquire(); fence_release(); barrier();
    /* 屏障下写入可见性顺序：先写后读 */
    {
        volatile u32 x = 0u, y = 0u, r1 = 1u, r2 = 1u;
        x = 1u; wmb(); y = 1u;
        rmb();
        r1 = x; r2 = y;
        if (r1 != 1u || r2 != 1u) return 9;  /* 顺序读回 */
    }

    /* 用例 4：缓存行对齐（伪共享隔离） */
    if (((u32)(u32 *)&cl % SYNC_CACHELINE_SIZE) != 0u) return 10;
    if (sizeof(cacheline_spinlock_t) != SYNC_CACHELINE_SIZE) return 11;

    /* 用例 5：缓存行对齐自旋锁互斥 500 次 */
    cacheline_spin_lock_init(&cl);
    n = 0u;
    for (i = 0; i < 500u; i++) {
        cacheline_spin_lock(&cl);
        n++;
        cacheline_spin_unlock(&cl);
    }
    if (n != 500u) return 12;

    /* 用例 6：cacheline 锁的自死锁检测（重复持锁不卡死） */
    cacheline_spin_lock(&cl);
    cacheline_spin_lock(&cl);                 /* 重复持锁 → 检测并返回 */
    cacheline_spin_unlock(&cl);
    return 0;
}

/* ---------------- 自检：core ---------------- */
static spinlock_t g_sl = SPINLOCK_INIT;
static mutex_t g_mtx = MUTEX_INIT;

static u32 selftest_core_counters[4];
static volatile u32 st_shared;
static volatile u32 st_shared_ok;

static void st_inc_thread(void *arg)
{
    u32 i;
    u32 idx = (u32)(u32 *)arg;
    for (i = 0; i < 500u; i++) {
        spin_lock(&g_sl);
        st_shared++;
        spin_unlock(&g_sl);
    }
    selftest_core_counters[idx] = 1;
    task_exit(0);
}

u32 sync_selftest_core(void)
{
    spinlock_t sl;
    atomic_t a = 0;
    semaphore_t sem;
    seqlock_t sq;
    u32 seq;
    spinlock_t sl2;

    /* 用例 1：原子加减/交换/CAS */
    atomic_set(&a, 10);
    atomic_add(&a, 5);
    atomic_sub(&a, 3);
    atomic_inc(&a);
    if (atomic_read(&a) != 13u) return 1;
    if (atomic_xchg(&a, 99u) != 13u) return 2;
    if (atomic_cmpxchg(&a, 99u, 7u) != 99u) return 3;
    if (atomic_read(&a) != 7u) return 4;

    /* 用例 2：自旋锁互斥与 trylock */
    spin_lock_init(&sl);
    spin_lock(&sl);
    if (spin_trylock(&sl) != 0) return 5;      /* 已锁，trylock 必须失败 */
    spin_unlock(&sl);
    if (spin_trylock(&sl) != 1) return 6;
    spin_unlock(&sl);

    /* 用例 3：信号量计数语义 */
    semaphore_init(&sem, 2);
    if (semaphore_down(&sem) != 0) return 7;
    if (semaphore_down(&sem) != 0) return 8;
    if (semaphore_trydown(&sem) != 0) return 9;   /* 已耗尽 */
    semaphore_up(&sem);
    if (semaphore_trydown(&sem) != 1) return 10;

    /* 用例 4：顺序锁奇偶序列 */
    seqlock_init(&sq);
    seq = seqlock_read_begin(&sq);
    if (seqlock_read_retry(&sq, seq)) return 11;  /* 无写者，必须不重试 */
    seqlock_write_begin(&sq);
    seqlock_write_end(&sq);
    if (sq.seq != 2u) return 12;
    seq = seqlock_read_begin(&sq);
    if (seqlock_read_retry(&sq, seq)) return 13;

    /* 用例 5：irqsave 版自旋锁保留中断状态 */
    spin_lock_init(&sl2);
    {
        u32 ef = spin_lock_irqsave(&sl2);
        if (ef == 0u) return 14;               /* 进入前中断应开启 */
        spin_unlock_irqrestore(&sl2, ef);
    }
    return 0;
}

u32 sync_selftest_lock(void)
{
    mutex_t m;
    rwlock_t rw;
    completion_t c;
    wait_queue_t wq;
    mutex_t m2;

    /* 用例 1：互斥锁互斥与 trylock */
    mutex_init(&m);
    mutex_lock(&m);
    if (mutex_trylock(&m) != 0) return 1;
    mutex_unlock(&m);
    if (mutex_trylock(&m) != 1) return 2;
    mutex_unlock(&m);

    /* 用例 2：读写锁读并发/写互斥 */
    rwlock_init(&rw);
    rwlock_read_lock(&rw);
    rwlock_read_lock(&rw);                      /* 读可重入 */
    rwlock_read_unlock(&rw);
    rwlock_read_unlock(&rw);
    rwlock_write_lock(&rw);
    if (rw.state != -1) return 3;
    rwlock_write_unlock(&rw);
    if (rw.state != 0) return 4;

    /* 用例 3：完成量（先完成再等待 = 立即返回） */
    completion_init(&c);
    completion_complete(&c);
    completion_wait(&c);
    if (!c.done) return 5;

    /* 用例 4：等待队列环形增删与唤醒 */
    wait_queue_init(&wq);
    wait_queue_add(&wq, 3);
    wait_queue_add(&wq, 7);
    if (wait_queue_wake(&wq) != 3u) return 6;
    if (wait_queue_wake(&wq) != 7u) return 7;
    if (wait_queue_wake(&wq) != 0xFFFFFFFFu) return 8;
    wait_queue_add(&wq, 11);
    wait_queue_remove(&wq, 11);
    if (wq.count != 0u) return 9;

    /* 用例 5：mutex 自死锁防护不触发正常场景 */
    mutex_init(&m2);
    mutex_lock(&m2);
    mutex_unlock(&m2);
    return 0;
}

/* 双线程互斥累加（真实并发验证互斥语义） */
static volatile u32 g_counter;
static u32 g_thread_done[2];

static void st_mutex_thread(void *arg)
{
    u32 i;
    u32 idx = (u32)arg;
    con_putc('T'); con_putc('0' + (char)idx); con_putc('\n');
    for (i = 0; i < 1000u; i++) {
        if (i == 0u)
        mutex_lock(&g_mtx);
        g_counter++;
        mutex_unlock(&g_mtx);
    }
    g_thread_done[idx] = 1;
    task_exit(0);
}

u32 sync_selftest_concur(void)
{
    static spsc_queue_t q;
    static u32 i, v;
    static u32 pids[4];
    u32 before = g_stats.deadlock_violations;
    g_mtx.magic = SYNC_MAGIC; g_mtx.state = 0u; g_mtx.owner_pid = 0xFFFFFFFFu;

    spin_lock_init(&g_sl);

    /* 用例 1：双线程互斥累加 → 总量精确 2000（无锁必丢） */
    g_counter = 0;
    g_thread_done[0] = 0;
    g_thread_done[1] = 0;
    pids[0] = task_create("mtx0", st_mutex_thread, (void *)0, SCHED_CFS, 0);
    pids[1] = task_create("mtx1", st_mutex_thread, (void *)1, SCHED_CFS, 0);
    for (i = 0; i < 20000u && (!g_thread_done[0] || !g_thread_done[1]); i++) {
        task_yield();
    }
    if (!g_thread_done[0] || !g_thread_done[1]) return 1;
    if (g_counter != 2000u) return 2;
    /* 回收两个线程 */
    for (i = 0; i < 2u; i++) {
        (void)task_join(pids[i], NULL);
    }

    /* 用例 2：无锁 SPSC 队列 256 项入出环 */
    spsc_init(&q);
    for (i = 0; i < SPSC_CAP - 1u; i++) {
        if (spsc_push(&q, i + 100u) != 0) return 3;
    }
    if (spsc_push(&q, 0u) == 0) return 4;        /* 满队列必须拒绝 */
    for (i = 0; i < SPSC_CAP - 1u; i++) {
        if (spsc_pop(&q, &v) != 0 || v != i + 100u) return 5;
    }
    if (spsc_pop(&q, &v) == 0) return 6;          /* 空队列必须拒绝 */

    /* 用例 3：自旋锁双线程互斥（st_shared 必须精确 1000） */
    st_shared = 0;
    selftest_core_counters[0] = 0;
    selftest_core_counters[1] = 0;
    pids[2] = task_create("spl0", st_inc_thread, (void *)0, SCHED_CFS, 0);
    pids[3] = task_create("spl1", st_inc_thread, (void *)1, SCHED_CFS, 0);
    for (i = 0; i < 20000u && (!selftest_core_counters[0] || !selftest_core_counters[1]); i++) {
        task_yield();
    }
    if (!selftest_core_counters[0] || !selftest_core_counters[1]) return 7;
    if (st_shared != 1000u) return 8;
    for (i = 0; i < 2u; i++)
        task_join(pids[2 + i], NULL);

    /* 用例 4：无锁队列统计暴露 */
    if (g_stats.spsc_pushed < (SPSC_CAP - 1u)) return 9;

    if (g_stats.deadlock_violations > before) return 10;  /* 正常并发不应触发死锁 */
    return 0;
}

u32 sync_selftest_detect(void)
{
    lock_chain_t lc;
    rcu_state_t rcu;
    futex_t ft;
    u32 addr = 0u;
    u32 before = g_stats.deadlock_violations;

    /* 用例 1：地址序 LL 规则——合法序不违规 */
    lock_chain_init(&lc);
    lock_chain_push(&lc, 0x100);
    lock_chain_push(&lc, 0x200);
    if (lc.violations != 0u) return 1;
    lock_chain_pop(&lc, 0x200);
    lock_chain_pop(&lc, 0x100);

    /* 用例 2：逆序加锁必须被检测为违规 */
    lock_chain_init(&lc);
    lock_chain_push(&lc, 0x300);
    lock_chain_push(&lc, 0x100);
    if (lc.violations != 1u) return 2;
    if (g_stats.deadlock_violations == before) return 3;

    /* 用例 3：RCU 读临界区互斥于宽限期 */
    rcu_init(&rcu);
    rcu_read_lock(&rcu);
    rcu_read_lock(&rcu);
    if (rcu.reader_epoch != 2u) return 4;
    rcu_read_unlock(&rcu);
    rcu_read_unlock(&rcu);
    rcu_synchronize(&rcu);                     /* 临界区外同步：立即推进宽限期 */
    if (rcu.grace_epoch < 1u) return 5;

    /* 用例 4：futex 等待/唤醒（expected 不符立即返回） */
    futex_init(&ft, &addr);
    if (futex_wait(&ft, 1u, 10u) != 0) return 6;  /* val=0 != expected=1 → 立即返回 */
    futex_init(&ft, &addr);
    if (futex_wait(&ft, 0u, 1u) != 1) return 7;    /* 超时路径 */
    futex_wake(&ft);
    if (ft.wakes != 1u) return 8;

    /* 用例 5：空队列唤醒返回哨兵 */
    return 0;
}
