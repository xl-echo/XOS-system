/* ============================================================================
 * XOS 内核堆分配器（第 06 册：内核堆分配器）
 * ============================================================================
 * 覆盖子域：
 *   小块分配 slab / 大块页级分配 / 内存池 mempool / 分配器缓存与热路径
 *   碎片控制 / 分配器统计 / 内存对齐保证 / 越界检测红区与 canary
 *   重复释放检测 / 内存泄漏追踪 / 分配器调试模式 / 分配失败处理
 *   内存清零与初始化 / 分配器并发保护 / 分配器自举 bootstrap
 *   分配器扩展与收缩 / 对象构造与析构钩子 / 分配器性能剖析
 *   分配器与 NUMA 亲和 / 分配器安全加固
 *
 * 设计约束（与全系统一致）：
 *   - 完全自研，不依赖任何第三方分配器实现；
 *   - 单 CPU、无中断驱动并发：临界区用 IRQ 保存/恢复自旋计数保护，
 *     为第 09 册同步原语接入后的真实锁预留接口；
 *   - 调试模式默认开启：对象头带魔数/大小/红区/canary/调用者，
 *     越界、重复释放、非堆指针释放均可检测且不崩溃；
 *   - 失败路径一律返回 NULL 或错误码，绝不 panic，保证可快速复位。
 * ============================================================================ */
#ifndef __XOS_KMALLOC_H__
#define __XOS_KMALLOC_H__

#include "types.h"

/* ---- 分配标志 ---- */
#define KMALLOC_ZERO        0x01u   /* 返回前将载荷清零 */
#define KMALLOC_NOLOCK      0x02u   /* 已在锁内调用，跳过加锁（内部用） */

/* ---- 大小类常量 ---- */
#define KMALLOC_MIN_SIZE    16u
#define KMALLOC_MAX_SLAB    4096u   /* slab 路径上限（含） */
#define KMALLOC_SLAB_CLASSES 9u     /* 16..4096 共 9 档 */

/* ---- 错误语义（返回 NULL 时的全局原因） ---- */
#define KHEAP_ERR_NONE      0u
#define KHEAP_ERR_INVAL     1u      /* 参数非法：size=0 / 对齐非 2 的幂 / 节点不存在 */
#define KHEAP_ERR_OOM       2u      /* 物理内存不足 */
#define KHEAP_ERR_NOSLOT    3u      /* slab 描述符槽位耗尽 */
#define KHEAP_ERR_ALIGN     4u      /* 无法满足对齐（理论上不会发生，防御保留） */

/* ---- 统计 ---- */
typedef struct {
    u32 total_alloc;        /* 累计分配调用（成功） */
    u32 total_free;         /* 累计释放调用 */
    u32 total_fail;         /* 累计失败调用 */
    u32 outstanding;        /* 未释放对象数 */
    u32 outstanding_bytes;  /* 未释放载荷字节数 */
    u32 slabs_active;       /* 在用 slab 数 */
    u32 slabs_pages;        /* slab 占用页数 */
    u32 large_count;        /* 大块分配数 */
    u32 large_bytes;        /* 大块占用字节数 */
    u32 pool_count;         /* 已建内存池数 */
    u32 pool_slots;         /* 内存池槽位数 */
    u32 shrink_calls;       /* 收缩回调被调用次数 */
    u32 shrink_freed;       /* 收缩累计归还页数 */
    u32 grow_calls;         /* 扩容（新 slab）次数 */
    u32 errors_detected;    /* 调试检测到的错误总数（越界/重复释放/非堆指针等） */
    u32 debug_on;           /* 调试模式开关 */
    u32 irq_depth;          /* 锁嵌套深度 */
    u32 last_err;           /* 最近一次失败原因 KHEAP_ERR_* */
    u32 profile[13];        /* 载荷大小直方图（1..4096 按 2 的幂分桶 + 大块桶） */
} kheap_stats_t;

/* ---- 生命周期 ---- */
void kheap_init(void);

/* ---- 分配与释放 ---- */
void *kmalloc(size_t size, u32 align, u32 flags);
void *kmalloc_node(size_t size, u32 align, u32 flags, u32 node);
void  kfree(void *ptr);
u32   kheap_last_err(void);

/* ---- 缓存构造/析构钩子（按大小类挂接） ---- */
void kheap_set_hooks(size_t size, void (*ctor)(void *obj, u32 size),
                     void (*dtor)(void *obj, u32 size));
void kheap_hook_info(size_t size, u32 *out_regs, u32 *out_ctor,
                     u32 *out_dtor);   /* 钩子注册/调用审计 */

/* ---- 内存池 ---- */
typedef struct kpool kpool_t;
kpool_t *kpool_create(const char *name, size_t objsize, u32 count);
void     kpool_destroy(kpool_t *pool);
void    *kpool_alloc(kpool_t *pool);
int      kpool_free(kpool_t *pool, void *obj);
u32      kpool_free_count(kpool_t *pool);
u32      kpool_used_count(kpool_t *pool);
void     kpool_stats(const kpool_t *pool, u32 *out_free, u32 *out_used,
                      u32 *out_fail, u32 *out_bytes);   /* 池级统计快照 */

/* ---- 统计 / 调试 / 泄漏 ---- */
void kheap_stats(kheap_stats_t *st);
void kheap_dump(void);
u32  kheap_leak_report(void);      /* 返回泄漏对象数（调试模式下） */
void kheap_set_debug(u32 on);
u32  kheap_get_debug(void);

/* ---- 收缩回调（注册进 PMM shrinker 框架） ---- */
u32  kheap_shrink(u32 nr_to_free, void *arg);

/* ---- 自检：0 通过；1..N 返回失败编号 ---- */
u32  kheap_selftest_core(void);     /* slab/大块/对齐/清零/红区/canary 往返 */
u32  kheap_selftest_fault(void);    /* 重复释放/越界/非堆指针/OOM/NUMA/NULL */
u32  kheap_selftest_ext(void);      /* mempool/钩子/统计/收缩/泄漏 */
u32  kheap_selftest(void);          /* 以上三组的总入口 */

#endif /* __XOS_KMALLOC_H__ */
