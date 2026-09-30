/* ============================================================================
 * XOS 虚拟内存子系统 —— 模块内部共享声明
 * 仅供 kernel/vmm*.c 之间共享资源池与内部辅助函数，不对外暴露。
 * ============================================================================ */
#ifndef __XOS_VMM_INTERNAL_H__
#define __XOS_VMM_INTERNAL_H__

#include "vmm.h"
#include "sync.h"

/* ---- 05 册第二轮：全局并发锁（三个 vmm*.c 共享） ----
 * 管理入口（mmap/munmap/mprotect/brk/expand_stack/populate/swap_out/swap_in/
 * pgcache_get/writeback/evict_clean）经 vmm_lock_enter/exit 串行化；
 * 缺页处理为中断上下文路径，锁外执行（PTE_SW_LOCK + irqsave 语义），
 * 见 vmm_mem.c 头注释。内部互调一律走 _locked 变体防自死锁。 */
extern spinlock_t vmm_global_lock;
u32  vmm_lock_enter(void);
void vmm_lock_exit(u32 eflags);
u32  vmm_lock_calls(void);

/* 跨文件 _locked 变体：vmm_mem.c 的 demand_page 需调 vmm_cache.c 的页缓存 */
u32 _vmm_pgcache_get_locked(u32 backing, u32 offset);

/* ---- 恒等映射下「物理地址 ↔ 内核指针」互转 ----
 * 内核页目录对 0..128MB 建立 4MB 大页恒等映射，因此物理地址可直接当指针用；
 * 这里显式封装成函数，避免在代码里散落隐式转换，也便于将来改直接映射基址。 */
static inline void *vmm_phys_to_ptr(u32 phys)
{
    return (void *)phys;
}

static inline u32 vmm_ptr_to_phys(const void *ptr)
{
    return (u32)ptr;
}

/* ---- 资源池（定义于 vmm.c） ---- */
extern vmm_mm_t            vmm_mm_pool[VMM_MAX_MM];
extern vmm_vma_t           vmm_vma_pool[VMM_MAX_VMA];
extern vmm_rmap_t          vmm_rmap_pool[VMM_MAX_RMAP];
extern vmm_pgcache_t       vmm_pgcache_pool[VMM_MAX_PGCACHE];
extern vmm_huge_t          vmm_huge_pool[VMM_MAX_HUGE];
extern vmm_backing_t       vmm_backing_pool[VMM_MAX_BACKING];

extern u32                 vmm_mm_used;
extern u32                 vmm_vma_used;
extern u32                 vmm_rmap_n;
extern u32                 vmm_pgcache_used;
extern u32                 vmm_huge_used;
extern u32                 vmm_backing_used;
extern u32                 vmm_pt_pages;
extern u32                 vmm_next_mm_id;

/* ---- 系统级累计事件计数（定义于 vmm.c） ----
 * 统计接口报告的是「本机自启动以来发生过多少次」，因此不能只累加存活
 * 地址空间的私有计数 —— 那样销毁一个 mm 会让历史凭空消失。 */
extern u32                 vmm_cow_total_n;
extern u32                 vmm_grow_total_n;

extern vmm_mm_t           *vmm_cur_mm;
extern u32                 vmm_tlb_gen;
extern u32                 vmm_tlb_flush_count;
extern u32                 vmm_shootdown_count;

/* ---- 页表页直接访问（恒等映射下指针即物理地址） ---- */
static inline u32 *vmm_pte_ptr(u32 pte_value)
{
    return (u32 *)(pte_value & PTE_PFN_MASK);
}

/* ---- 内部辅助（定义于 vmm.c） ---- */
u32         vmm_kernel_pt_pages(void);
void        vmm_pool_reset(void);

/* 解除一页映射并释放其物理页引用（引用计数归零才真正归还 PMM）。
 * 供地址空间销毁（vmm.c）与 munmap（vmm_mem.c）共用。 */
int         vmm_unmap_page_put(vmm_mm_t *mm, u32 vaddr);

/* 物理页引用计数 -1，归零则归还 PMM。 */
void        vmm_put_phys(u32 phys);

/* 物理页引用计数 +1（用于共享映射，如 COW）。 */
int         vmm_get_phys(u32 phys);

/* 分页是否已开启、PSE 是否可用 */
int         vmm_pse_available(void);

/* ---- 缺页日志（定义于 vmm_mem.c，供 vmm.c 转储使用） ---- */
void        vmm_fault_log_add(u32 vaddr, u32 err, u32 eip, u32 action, u32 result);

/* ---- 回收回调（定义于 vmm_cache.c） ---- */
void        vmm_reclaim_init(void);

#endif /* __XOS_VMM_INTERNAL_H__ */
