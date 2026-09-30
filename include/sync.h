/* ============================================================================
 * XOS 同步原语头文件
 * 原子操作 / 自旋锁 / 互斥锁 / 读写锁 / 信号量 / 完成量 / 顺序锁 /
 * RCU（单 CPU 简化）/ futex（轮询语义）/ 等待队列 / 死锁检测 /
 * 争用统计 / 无锁 SPSC 队列 / 内存屏障
 * 全部自研，不依赖任何外部库；单 CPU（UP）阶段语义正确，多核阶段可扩展。
 * ========================================================================== */
#ifndef XOS_SYNC_H
#define XOS_SYNC_H

#include "types.h"
#include "task.h"

#define SYNC_MAGIC       0x53594E43u   /* "SYNC" */
#define SYNC_MAGIC_FREE  0x53594E00u
#define SYNC_LOCKED      1u
#define SYNC_UNLOCKED    0u

/* ---------------- 原子操作 ---------------- */
typedef volatile u32 atomic_t;

static inline u32 atomic_read(const atomic_t *a)        { return *a; }
static inline void atomic_set(atomic_t *a, u32 v)       { *a = v; }
static inline void atomic_add(atomic_t *a, u32 v)       { __asm__ __volatile__("lock addl %1, %0" : "+m"(*a) : "r"(v)); }
static inline void atomic_sub(atomic_t *a, u32 v)       { __asm__ __volatile__("lock subl %1, %0" : "+m"(*a) : "r"(v)); }
static inline void atomic_inc(atomic_t *a)              { __asm__ __volatile__("lock incl %0" : "+m"(*a)); }
static inline void atomic_dec(atomic_t *a)              { __asm__ __volatile__("lock decl %0" : "+m"(*a)); }
static inline u32 atomic_xchg(atomic_t *a, u32 v)
{
    __asm__ __volatile__("xchgl %0, %1" : "+r"(v), "+m"(*a) : : "memory");
    return v;
}
static inline u32 atomic_cmpxchg(atomic_t *a, u32 old, u32 newv)
{
    u32 ret = old;
    __asm__ __volatile__("lock cmpxchgl %2, %1"
                         : "+a"(ret), "+m"(*a) : "r"(newv) : "memory");
    return ret;
}

/* ---------------- 内存屏障 ---------------- */
#define barrier()           __asm__ __volatile__("" ::: "memory")
#define mb()                __asm__ __volatile__("mfence" ::: "memory")
#define rmb()               __asm__ __volatile__("lfence" ::: "memory")
#define wmb()               __asm__ __volatile__("sfence" ::: "memory")
#define fence_acquire()     mb()   /* 获取语义：后续读写不得越过该点提前 */
#define fence_release()     mb()   /* 释放语义：前置读写不得越过该点延后 */

/* ---------------- 多核缓存一致性：原子 RMW 与位操作 ---------------- */
#define SYNC_CACHELINE_SIZE    64u
#define SYNC_CACHELINE_ALIGNED __attribute__((aligned(SYNC_CACHELINE_SIZE)))

static inline u32 atomic_fetch_add(atomic_t *a, u32 v)
{
    __asm__ __volatile__("lock xaddl %0, %1" : "+r"(v), "+m"(*a) : : "memory");
    return v;   /* 返回交换前的旧值 */
}
static inline u32 atomic_fetch_sub(atomic_t *a, u32 v)
{
    u32 nv = (u32)(0u - v);
    __asm__ __volatile__("lock xaddl %0, %1" : "+r"(nv), "+m"(*a) : : "memory");
    return nv;  /* 返回交换前的旧值 */
}
static inline void atomic_set_bit(atomic_t *a, u32 bit)
{
    __asm__ __volatile__("lock btsl %1, %0" : "+m"(*a) : "r"(bit) : "memory");
}
static inline void atomic_clear_bit(atomic_t *a, u32 bit)
{
    __asm__ __volatile__("lock btrl %1, %0" : "+m"(*a) : "r"(bit) : "memory");
}
static inline u32 atomic_test_bit(const atomic_t *a, u32 bit)
{
    u32 r = *(const u32 *)a;
    return (r >> bit) & 1u;
}

/* 缓存行对齐自旋锁：隔离伪共享（多核阶段多线程各自持有不同实例时互不干扰） */
typedef struct {
    u32 magic;
    atomic_t locked;
    u32 owner_pid;
    u32 pad[13];   /* 补齐至 64B 缓存行 */
} SYNC_CACHELINE_ALIGNED cacheline_spinlock_t;

#define CACHELINE_SPINLOCK_INIT { SYNC_MAGIC, 0, 0xFFFFFFFFu, {0} }

void cacheline_spin_lock_init(cacheline_spinlock_t *l);
void cacheline_spin_lock(cacheline_spinlock_t *l);
void cacheline_spin_unlock(cacheline_spinlock_t *l);

/* ---------------- 自旋锁 ---------------- */
typedef struct {
    u32 magic;
    atomic_t locked;
    u32 owner_pid;         /* 持锁者 pid（调试/死锁检测） */
    u32 acquire_count;     /* 争用统计 */
    u32 contend_count;
    u32 irq_depth;         /* 中断上下文嵌套记录 */
} spinlock_t;

#define SPINLOCK_INIT { SYNC_MAGIC, 0, 0xFFFFFFFFu, 0, 0, 0 }

void spin_lock_init(spinlock_t *l);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
int  spin_trylock(spinlock_t *l);
u32  spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, u32 eflags);

/* ---------------- 互斥锁（睡眠语义：yield 慢路径） ---------------- */
#define PI_WAITERS_MAX 8u
typedef struct {
    u32 magic;
    volatile u32 state;    /* 0=未锁 1=锁定 */
    u32 owner_pid;
    u32 waiters;           /* 等待者计数（争用统计） */
    u32 contended;
    /* 优先级反转处理（PI）：优先级继承 + 优先级天花板 */
    u32 ceiling_prio;              /* 天花板优先级（0=未设；数值越大优先级越高） */
    u32 boost_owner_pid;           /* 触发 PI 提升的等待者（最高优先级者） */
    u32 boost_count;               /* 提升深度（嵌套提升计数） */
    u32 waiter_pids[PI_WAITERS_MAX]; /* 等待者 pid 表（环检测用） */
    u32 pi_events;                 /* PI 触发事件计数 */
} mutex_t;

#define MUTEX_INIT { SYNC_MAGIC, 0, 0xFFFFFFFFu, 0, 0, 0, 0xFFFFFFFFu, 0, {0}, 0 }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
int  mutex_trylock(mutex_t *m);
/* 优先级反转处理接口 */
void mutex_pi_ceiling_set(mutex_t *m, u32 prio);  /* 设天花板（可重复设置取高） */
u32  mutex_pi_waiter_add(mutex_t *m, u32 pid);    /* 记录等待者，返回 0=成功 1=表满 2=重复(环) */
void mutex_pi_waiter_clear(mutex_t *m, u32 pid);  /* 清除等待者（唤醒时） */
u32  mutex_pi_detect_cycle(mutex_t *m);           /* 环检测：返回 1=存在重复等待者 */
u32  mutex_pi_state(mutex_t *m, u32 *boosted, u32 *ceiling);  /* 查询 PI 状态 */

/* ---------------- 读写锁 ---------------- */
typedef struct {
    u32 magic;
    volatile i32 state;    /* -1 写锁；0 无；>0 读锁计数 */
    u32 writer_pid;
    u32 write_waiters, read_waiters;
} rwlock_t;

#define RWLOCK_INIT { SYNC_MAGIC, 0, 0xFFFFFFFFu, 0, 0 }

void rwlock_init(rwlock_t *l);
void rwlock_read_lock(rwlock_t *l);
void rwlock_read_unlock(rwlock_t *l);
void rwlock_write_lock(rwlock_t *l);
void rwlock_write_unlock(rwlock_t *l);

/* ---------------- 信号量 ---------------- */
typedef struct {
    u32 magic;
    volatile u32 count;
    u32 max_count;         /* 配额上限（资源限流） */
    u32 waiters;
    u32 pid_last;          /* 最近 up 唤醒者调试 */
} semaphore_t;

#define SEMAPHORE_INIT(cap) { SYNC_MAGIC, (cap), (cap), 0, 0xFFFFFFFFu }

void semaphore_init(semaphore_t *s, u32 count);
int  semaphore_down(semaphore_t *s);       /* P 操作，返回 0=成功 1=超时放弃 */
void semaphore_up(semaphore_t *s);         /* V 操作 */
int  semaphore_trydown(semaphore_t *s);

/* ---------------- 完成量 ---------------- */
typedef struct {
    u32 magic;
    volatile u32 done;
    u32 waiters;
    u32 pid_done_by;
} completion_t;

#define COMPLETION_INIT { SYNC_MAGIC, 0, 0, 0xFFFFFFFFu }

void completion_init(completion_t *c);
void completion_wait(completion_t *c);     /* 等待 done（yield 轮询） */
void completion_complete(completion_t *c);

/* ---------------- 顺序锁 ---------------- */
typedef struct {
    u32 magic;
    volatile u32 seq;
} seqlock_t;

#define SEQLOCK_INIT { SYNC_MAGIC, 0 }

void seqlock_init(seqlock_t *s);
u32  seqlock_write_begin(seqlock_t *s);
void seqlock_write_end(seqlock_t *s);
u32  seqlock_read_begin(seqlock_t *s);
int  seqlock_read_retry(seqlock_t *s, u32 seq);

/* ---------------- RCU（单 CPU 简化：读临界区 + 同步推进） ---------------- */
typedef struct {
    u32 magic;
    volatile u32 reader_epoch;   /* 进行中读临界区计数 */
    volatile u32 grace_epoch;    /* 已完成的宽限期计数 */
    u32 callbacks;               /* 延迟回调计数 */
} rcu_state_t;

#define RCU_STATE_INIT { SYNC_MAGIC, 0, 0, 0 }

void rcu_init(rcu_state_t *r);
void rcu_read_lock(rcu_state_t *r);
void rcu_read_unlock(rcu_state_t *r);
void rcu_synchronize(rcu_state_t *r);   /* 等待所有读临界区退出 */

/* ---------------- futex（轮询语义，UP 可验证） ---------------- */
typedef struct {
    u32 magic;
    volatile u32 *uaddr;     /* 用户地址（调试，UP 下可为内核映射） */
    volatile u32 val;
    u32 waiters;
    u32 wakes;
} futex_t;

void futex_init(futex_t *f, u32 *addr);
int  futex_wait(futex_t *f, u32 expected, u32 timeout_ticks);
void futex_wake(futex_t *f);

/* ---------------- 等待队列 ---------------- */
#define WQ_MAX 32
typedef struct {
    u32 magic;
    u32 waiters[WQ_MAX];
    u32 head, tail, count;
    u32 wakes;
} wait_queue_t;

#define WAIT_QUEUE_INIT { SYNC_MAGIC, {0}, 0, 0, 0, 0 }

void wait_queue_init(wait_queue_t *wq);
void wait_queue_add(wait_queue_t *wq, u32 pid);
void wait_queue_remove(wait_queue_t *wq, u32 pid);
u32  wait_queue_wake(wait_queue_t *wq);   /* 唤醒一个等待者 */
u32  wait_queue_wake_all(wait_queue_t *wq);

/* ---------------- 死锁检测 ---------------- */
#define LOCK_ORDER_MAX 16
typedef struct {
    u32 magic;
    u32 graph[LOCK_ORDER_MAX];   /* 加锁顺序记录（当前线程持有链） */
    u32 depth;
    u32 violations;              /* 顺序违规计数（环形等待检测） */
} lock_chain_t;

void lock_chain_init(lock_chain_t *c);
void lock_chain_push(lock_chain_t *c, u32 addr);
void lock_chain_pop(lock_chain_t *c, u32 addr);
u32  lock_chain_violations(lock_chain_t *c);

/* ---------------- 无锁 SPSC 队列（单生产者单消费者环形） ---------------- */
#define SPSC_CAP 256
typedef struct {
    u32 magic;
    u32 buf[SPSC_CAP];
    volatile u32 head;       /* 生产者写入位置 */
    volatile u32 tail;       /* 消费者读取位置 */
    u32 pushed, popped;
} spsc_queue_t;

#define SPSC_INIT { SYNC_MAGIC, {0}, 0, 0, 0, 0 }

void spsc_init(spsc_queue_t *q);
int  spsc_push(spsc_queue_t *q, u32 v);
int  spsc_pop(spsc_queue_t *q, u32 *v);
u32  spsc_count(spsc_queue_t *q);

/* ---------------- 全局统计与调试导出 ---------------- */
typedef struct {
    u32 spin_acq, spin_cont, mutex_acq, mutex_cont;
    u32 sem_down, sem_up, wq_wakes, futex_wakes;
    u32 deadlock_violations, spsc_pushed, spsc_popped;
    u32 rcu_syncs;
} sync_stats_t;

void sync_stats(sync_stats_t *st);
void sync_dump(void);

/* 自检入口 */
u32 sync_selftest_core(void);      /* 原子/自旋/信号量/顺序锁/屏障 */
u32 sync_selftest_lock(void);      /* 互斥/读写锁/完成量/等待队列 */
u32 sync_selftest_concur(void);    /* 双线程互斥累加 + 无锁队列 */
u32 sync_selftest_detect(void);    /* 死锁检测/RCU/futex */
u32 sync_selftest_pi(void);        /* 优先级反转处理（继承/天花板/环检测） */
u32 sync_selftest_cache(void);     /* 多核缓存一致性（对齐/原子RMW/位操作/伪共享） */

#endif /* XOS_SYNC_H */

