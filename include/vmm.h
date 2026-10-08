/* ============================================================================
 * XOS 虚拟内存子系统（VMM）—— 分页、地址空间与缺页处理
 *
 * 硬件前提：32 位 x86，不启用 PAE。硬件为两级页表（页目录 PD + 页表 PT）。
 * 本子系统对外呈现「四级页表框架」（PGD → PUD → PMD → PTE），
 * 在两级硬件上把 PUD / PMD 折叠（各自恒为 1 个表项、索引恒为 0），
 * 地址转换例程按四级通用写法遍历。这样上层接口与层级语义在
 * 未来切换到 PAE / 64 位时无需改动，仅折叠宏取值变化。
 *
 * 地址划分（32 位）：
 *   PGD 索引  bit 22..31   10 位   1024 项，每项覆盖 4MB
 *   PUD 索引  折叠，恒 0
 *   PMD 索引  折叠，恒 0
 *   PTE 索引  bit 12..21   10 位   1024 项，每项覆盖 4KB
 *   页内偏移  bit  0..11
 *
 * 地址空间布局：
 *   0x00000000 - 0xBFFFFFFF  用户空间（每个地址空间私有）
 *   0xC0000000 - 0xFFFFFFFF  内核空间（所有地址空间共享同一份映射）
 * ============================================================================ */
#ifndef __XOS_VMM_H__
#define __XOS_VMM_H__

#include "types.h"

/* ---- 页表层级 ---- */
#define VMM_LEVELS          4
#define PTRS_PER_PGD        1024
#define PTRS_PER_PUD        1       /* 折叠 */
#define PTRS_PER_PMD        1       /* 折叠 */
#define PTRS_PER_PTE        1024

#define PGDIR_SHIFT         22
#define PGDIR_SIZE          (1u << PGDIR_SHIFT)         /* 4 MB */
#define PGDIR_MASK          (~(PGDIR_SIZE - 1u))
#define PTE_SHIFT           12
#define PTE_SIZE            4096u
#define PTE_MASK            (PTE_SIZE - 1u)

/* ---- 内核 / 用户空间分界 ----
 * 低端 0x00000000 - 0x07FFFFFF（前 128MB）保留给内核恒等映射：
 *   内核映像位于 0x10000、内核栈位于 0x9FFFC、VGA 显存位于 0xB8000，
 *   这些地址在「每个」地址空间里都必须保持有效，否则一旦装载用户页目录，
 *   内核自身立刻取指失败。因此该区段以「超级用户页（US=0）」形式
 *   复制进所有地址空间：内核可访问，用户态无权访问。
 * 用户可映射区从 0x08000000 起，到 0xBFFFFFFF 止。 */
#define KERNEL_IDENTITY_TOP 0x08000000u                          /* 128 MB */
#define KERNEL_IDENTITY_PGDS (KERNEL_IDENTITY_TOP >> PGDIR_SHIFT) /* 32 项 */

#define USER_SPACE_BASE     KERNEL_IDENTITY_TOP
#define USER_SPACE_TOP      0xC0000000u
#define KERNEL_SPACE_BASE   0xC0000000u
#define KERNEL_SPACE_TOP    0xFFFFFFFFu
#define KERNEL_PGD_INDEX    (KERNEL_SPACE_BASE >> PGDIR_SHIFT)   /* 768 */

/* 用户区可供 mmap 的虚拟地址下界 */
#define USER_MIN_MAP_ADDR   0x08000000u
/* 代码段 / 数据段 / 堆的固定基址（位于用户区内部） */
#define USER_TEXT_BASE      0x10000000u
#define USER_DATA_BASE      0x10010000u
#define USER_HEAP_BASE      0x20000000u
/* 映射区与栈的默认基址（实际取值经 ASLR 偏移） */
#define USER_MMAP_BASE      0x40000000u
#define USER_STACK_TOP      0xB0000000u

/* ---- 页表项 / 页目录项硬件标志位 ---- */
#define PTE_P               0x001u      /* 存在 */
#define PTE_RW              0x002u      /* 可写 */
#define PTE_US              0x004u      /* 用户可访问 */
#define PTE_PWT             0x008u      /* 透写 */
#define PTE_PCD             0x010u      /* 禁止缓存 */
#define PTE_A               0x020u      /* 已访问（硬件置位） */
#define PTE_D               0x040u      /* 已脏（硬件置位） */
#define PTE_PS              0x080u      /* 大页（4MB） */
#define PTE_G               0x100u      /* 全局页（不随 CR3 失效） */

/* ---- 软件扩展标志：借用页表项的 AVAIL 位（bit 9..11） ---- */
#define PTE_SW_COW          0x200u      /* 写时复制页，物理页被多方共享 */
#define PTE_SW_SWAP         0x400u      /* 页已换出，PFN 字段存交换槽号 */
#define PTE_SW_LOCK         0x800u      /* 页被锁定，禁止换出 */

#define PTE_AVAIL_MASK      0xE00u
#define PTE_PFN_MASK        0xFFFFF000u

/* ---- 错误码 ---- */
#define VMM_OK              0
#define VMM_ENOMEM          1
#define VMM_EINVAL          2
#define VMM_EFAULT          3
#define VMM_EEXIST          4
#define VMM_ENOENT          5
#define VMM_EPERM           6
#define VMM_EBUSY           7
#define VMM_ENOSPC          8
#define VMM_ERESVD          9       /* 保留位被置位 */
#define VMM_ELIMIT          10      /* 达到资源/次数上限（05 册第二轮） */
#define VMM_ESWAP           11      /* 无可换页候选（05 册第二轮） */

/* ---- 保护位 ---- */
#define PROT_NONE           0x0u
#define PROT_READ           0x1u
#define PROT_WRITE          0x2u
#define PROT_EXEC           0x4u

/* ---- 映射标志 ---- */
#define MAP_SHARED          0x01u
#define MAP_PRIVATE         0x02u
#define MAP_ANONYMOUS       0x04u
#define MAP_FIXED           0x08u
#define MAP_POPULATE        0x10u
#define MAP_GROWSDOWN       0x20u
#define MAP_LOCKED          0x40u
#define MAP_FILE            0x80u

/* ---- 缺页错误码（CPU 压栈的 err 字段） ---- */
#define PF_PRESENT          0x01u       /* 0=页不存在 1=权限违例 */
#define PF_WRITE            0x02u       /* 1=写访问触发 */
#define PF_USER             0x04u       /* 1=用户态访问 */
#define PF_RSVD             0x08u       /* 保留位被置位 */
#define PF_ID               0x10u       /* 取指阶段触发 */

/* ---- VMA 类型 ---- */
#define VMA_TYPE_ANON       0
#define VMA_TYPE_FILE       1
#define VMA_TYPE_STACK      2

/* ---- 页表分配统计上限 ---- */
#define VMM_MAX_MM          8
#define VMM_MAX_VMA         128
#define VMM_MAX_RMAP        256
#define VMM_MAX_PGCACHE     128
#define VMM_MAX_HUGE        16
#define VMM_MAX_FAULTLOG    32
#define VMM_MAX_BACKING     4

/* ---- 交换区 ---- */
#define VMM_SWAP_PAGES      512
#define VMM_SWAP_SLOTS      VMM_SWAP_PAGES

/* ---- 栈自动增长 ---- */
#define VMM_STACK_GAP       0x00010000u     /* 栈指针下方保护间隙 64 KB */
#define VMM_STACK_MAX       0x00800000u     /* 栈最大 8 MB */
#define VMM_STACK_GROW_MAX  64u             /* 单地址空间栈增长次数上限（05 册第二轮） */
#define VMM_STACK_GROW_STEP 0x00010000u     /* 单次栈增长步长上限 64 KB */

/* ---- 大页（4MB，PSE） ---- */
#define VMM_HUGE_SIZE       (4u * 1024u * 1024u)
#define VMM_HUGE_PAGES      (VMM_HUGE_SIZE / PTE_SIZE)

/* ============================================================================
 * 数据结构
 * ============================================================================ */

/* 虚拟内存区域描述符 */
typedef struct {
    u32 vm_start;       /* 起始虚拟地址（含） */
    u32 vm_end;         /* 结束虚拟地址（不含） */
    u32 vm_prot;        /* PROT_* 组合 */
    u32 vm_flags;       /* MAP_* 组合 */
    u32 vm_type;        /* VMA_TYPE_* */
    u32 vm_offset;      /* 文件映射内的页偏移 */
    u32 vm_backing;     /* 后备存储 id（文件映射用） */
    u32 vm_used;
    u32 vm_owner;       /* 所属地址空间 id */
} vmm_vma_t;

/* 地址空间 */
typedef struct {
    u32          pgd_phys;      /* 页全局目录物理地址 */
    u32         *pgd;           /* 页全局目录的内核可见指针（恒等映射） */
    u32          id;            /* 地址空间 id */
    u32          used;
    u32          refcount;
    u32          user_access;   /* 是否允许用户态访问用户区 */

    /* 布局信息 */
    u32          mmap_base;     /* 映射区基址（受 ASLR 影响） */
    u32          brk;           /* 堆顶 */
    u32          stack_top;     /* 栈顶（受 ASLR 影响） */
    u32          stack_start;   /* 栈 VMA 当前起点 */
    u32          stack_vma;     /* 栈 VMA 索引，0xFFFFFFFF 表示无 */

    /* 计数 */
    u32          total_vm;      /* 已映射页数 */
    u32          locked_vm;     /* 已锁定页数 */
    u32          anon_vm;       /* 匿名页数 */
    u32          file_vm;       /* 文件页数 */
    u32          swap_used;     /* 已换出页数 */
    u32          pt_pages;      /* 占用的页表页数 */

    u32          fault_count;   /* 缺页次数 */
    u32          cow_count;     /* COW 复制次数 */
    u32          grow_count;    /* 栈增长次数 */
    u32          swap_out;      /* 换出次数 */
    u32          swap_in;       /* 换入次数 */
    u32          tlb_gen;       /* 已同步到的 TLB 代次 */
    u32          seq;           /* 地址空间代次（用于击落协议） */
} vmm_mm_t;

/* 反向映射项：物理页 → 一处虚拟映射 */
typedef struct {
    u32 phys;           /* 物理地址（页对齐） */
    u32 mm_id;          /* 地址空间 id */
    u32 vaddr;          /* 虚拟地址 */
    u32 used;
} vmm_rmap_t;

/* 页缓存项 */
typedef struct {
    u32 backing;        /* 后备存储 id */
    u32 offset;         /* 页偏移 */
    u32 phys;           /* 物理页地址，0 表示不在缓存 */
    u32 dirty;          /* 脏位 */
    u32 refcount;       /* 引用计数 */
    u32 used;
    u32 writeback_count;
} vmm_pgcache_t;

/* 后备存储（文件页的底层来源；本阶段以 RAM 存储桩实现） */
typedef int (*vmm_backing_rw_fn)(u32 offset, void *buf, u32 write);

typedef struct {
    u32                 id;
    const char         *name;
    u32                 size;       /* 字节数 */
    vmm_backing_rw_fn   read;
    vmm_backing_rw_fn   write;
    u32                 used;
    u32                 read_count;
    u32                 write_count;
} vmm_backing_t;

/* 缺页记录（诊断用环形日志） */
typedef struct {
    u32 seq;
    u32 vaddr;
    u32 err;
    u32 eip;
    u32 action;         /* VMM_FA_* */
    u32 result;         /* VMM_OK 或错误码 */
} vmm_fault_rec_t;

#define VMM_FA_NONE     0
#define VMM_FA_DEMAND   1   /* 按需分配 */
#define VMM_FA_COW      2   /* 写时复制 */
#define VMM_FA_SWAPIN   3   /* 换入 */
#define VMM_FA_GROW     4   /* 栈增长 */
#define VMM_FA_REFUSE   5   /* 拒绝 */

/* 大页映射记录 */
typedef struct {
    u32 vaddr;
    u32 phys;
    u32 flags;
    u32 used;
} vmm_huge_t;

/* 统计 */
typedef struct {
    u32 enabled;            /* 分页是否已开启 */
    u32 mm_count;           /* 已建地址空间数 */
    u32 vma_count;          /* 已用 VMA 数 */
    u32 rmap_count;         /* 已用反向映射项 */
    u32 pgcache_count;      /* 页缓存项数 */
    u32 pgcache_dirty;      /* 脏页数 */
    u32 pgcache_writeback;  /* 累计回写次数 */
    u32 huge_count;         /* 大页映射数 */
    u32 swap_total;         /* 交换槽总数 */
    u32 swap_free;          /* 空闲交换槽 */
    u32 swap_out_total;     /* 累计换出 */
    u32 swap_in_total;      /* 累计换入 */
    u32 fault_total;        /* 累计缺页 */
    u32 fault_resolved;     /* 累计成功解决 */
    u32 fault_refused;      /* 累计拒绝 */
    u32 cow_total;          /* 累计 COW 复制 */
    u32 grow_total;         /* 累计栈增长 */
    u32 tlb_flush_total;    /* 累计 TLB 失效 */
    u32 shootdown_total;    /* 累计击落请求 */
    u32 shootdown_pending;  /* 待处理击落数 */
    u32 pt_pages_total;     /* 累计分配页表页 */
    u32 huge_bytes;         /* 大页覆盖字节数 */
    u32 aslr_on;            /* ASLR 开关 */
    u32 entropy;            /* 最近一次熵值 */
    u32 kernel_pgd_shared;  /* 内核区共享的 PGD 项数 */
} vmm_stats_t;

/* ============================================================================
 * 生命周期（S01 / S02）
 * ============================================================================ */
void        vmm_init(void);
int         vmm_enabled(void);
int         vmm_pse_available(void);    /* CPU 是否支持 4MB 大页（CPUID.01H:EDX bit3） */

/* ---- 故障现场快照 ----
 * 自检失败时由 vmm_test.c 填入关键中间量，kmain 在启动画面末尾统一转储。
 * 裸机无调试器，启动画面滚屏会冲掉早期输出，因此把现场固定到最后一屏。 */
#define VMM_DBG_SLOTS 8
extern u32  vmm_dbg[VMM_DBG_SLOTS];
void        vmm_dbg_clear(void);
extern const char *vmm_dbg_label[VMM_DBG_SLOTS];

/* ============================================================================
 * 页表遍历与地址转换（S01 / S03）
 * ============================================================================ */
u32        *vmm_pgd_offset(vmm_mm_t *mm, u32 vaddr);
u32        *vmm_pud_offset(u32 *pgd, u32 vaddr);
u32        *vmm_pmd_offset(u32 *pud, u32 vaddr);
u32        *vmm_pte_offset(u32 *pmd, u32 vaddr);
u32        *vmm_walk(vmm_mm_t *mm, u32 vaddr, int create);
u32         vmm_translate(vmm_mm_t *mm, u32 vaddr);
u32         vmm_translate_hw(u32 vaddr);
u32         vmm_pgd_index(u32 vaddr);
u32         vmm_pte_index(u32 vaddr);
u32         vmm_pgd_count(vmm_mm_t *mm);
u32         vmm_pt_count(vmm_mm_t *mm);

/* ============================================================================
 * 页表页分配（S02）
 * ============================================================================ */
u32        *vmm_alloc_pt(void);
void        vmm_free_pt(u32 *pt);
u32        *vmm_alloc_pgd(void);
void        vmm_free_pgd(u32 *pgd);
int         vmm_ensure_pt(vmm_mm_t *mm, u32 vaddr);
u32         vmm_pt_pages_used(void);

/* ============================================================================
 * 映射与解除映射（S02 / S07）
 * ============================================================================ */
int         vmm_map_page(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags);
int         vmm_map_device(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags);
int         vmm_map_device_huge(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags);
int         vmm_unmap_page(vmm_mm_t *mm, u32 vaddr);
int         vmm_map_range(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 len, u32 flags);
int         vmm_unmap_range(vmm_mm_t *mm, u32 vaddr, u32 len);

/* ============================================================================
 * 页表项标志位管理（S04 / S16）
 * ============================================================================ */
u32         vmm_pte_get(vmm_mm_t *mm, u32 vaddr);
int         vmm_pte_set_flags(vmm_mm_t *mm, u32 vaddr, u32 mask, u32 value);
int         vmm_pte_present(vmm_mm_t *mm, u32 vaddr);
int         vmm_pte_writable(vmm_mm_t *mm, u32 vaddr);
int         vmm_pte_user(vmm_mm_t *mm, u32 vaddr);
int         vmm_pte_cow(vmm_mm_t *mm, u32 vaddr);
int         _vmm_swap_out_locked(vmm_mm_t *mm, u32 vaddr);   /* 换页器内部（vmm_mem/vmm_cache 共用） */
int         _vmm_populate_locked(vmm_mm_t *mm, u32 addr, u32 len);
int         vmm_pte_swapped(vmm_mm_t *mm, u32 vaddr);
int         vmm_pte_locked(vmm_mm_t *mm, u32 vaddr);
u32         vmm_prot_to_flags(u32 prot, int user);
u32         vmm_flags_to_prot(u32 flags);
int         vmm_mprotect(vmm_mm_t *mm, u32 addr, u32 len, u32 prot);
int         vmm_check_access(vmm_mm_t *mm, u32 vaddr, u32 write, u32 user);

/* ============================================================================
 * 访问位与脏位跟踪（S11）
 * ============================================================================ */
int         vmm_get_accessed(vmm_mm_t *mm, u32 vaddr);
int         vmm_clear_accessed(vmm_mm_t *mm, u32 vaddr);
int         vmm_get_dirty(vmm_mm_t *mm, u32 vaddr);
int         vmm_clear_dirty(vmm_mm_t *mm, u32 vaddr);
u32         vmm_count_accessed(vmm_mm_t *mm);
u32         vmm_count_dirty(vmm_mm_t *mm);
int         vmm_scan_ad(vmm_mm_t *mm, u32 start, u32 len,
                        u32 *accessed, u32 *dirty);

/* ============================================================================
 * TLB 刷新与击落（S10）
 * ============================================================================ */
void        vmm_flush_tlb_page(u32 vaddr);
void        vmm_flush_tlb_all(void);
int         vmm_tlb_shootdown(vmm_mm_t *mm, u32 vaddr, u32 len);
u32         vmm_tlb_pending(vmm_mm_t *mm);
u32         vmm_tlb_flush_total(void);
u32         vmm_tlb_shootdown_total(void);

/* ============================================================================
 * 大页映射（S12）
 * ============================================================================ */
int         vmm_map_huge(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags);
int         vmm_unmap_huge(vmm_mm_t *mm, u32 vaddr);
u32         vmm_huge_count(void);
u32         vmm_huge_vaddr(u32 idx);
u32         vmm_huge_phys(u32 idx);
int         vmm_huge_alloc(vmm_mm_t *mm, u32 vaddr, u32 flags);
int         vmm_huge_free(vmm_mm_t *mm, u32 vaddr);
u32         vmm_huge_selftest(void);

/* ============================================================================
 * 地址空间隔离（S13）
 * ============================================================================ */
int         vmm_copy_kernel_pgd(vmm_mm_t *mm);
int         vmm_is_kernel_addr(u32 vaddr);
int         vmm_is_user_addr(u32 vaddr);
u32         vmm_kernel_pgd_shared(void);
int         vmm_set_user_access(vmm_mm_t *mm, int on);
int         vmm_check_isolation(vmm_mm_t *mm);

/* ============================================================================
 * 地址空间随机化 ASLR（S14）
 * ============================================================================ */
u32         vmm_random(void);
void        vmm_aslr_set(u32 on);
u32         vmm_aslr_get(void);
u32         vmm_aslr_mmap_base(void);
u32         vmm_aslr_stack_top(void);
u32         vmm_aslr_entropy(void);
u32         vmm_aslr_selftest(void);

/* ============================================================================
 * 地址空间生命周期（S07 / S13）
 * ============================================================================ */
vmm_mm_t   *vmm_mm_create(void);
int         vmm_mm_destroy(vmm_mm_t *mm);
vmm_mm_t   *vmm_kernel_mm(void);
vmm_mm_t   *vmm_current_mm(void);
int         vmm_mm_switch(vmm_mm_t *mm);
u32         vmm_mm_count(void);
vmm_mm_t   *vmm_mm_at(u32 idx);
u32         vmm_current_mm_id(void);

/* ============================================================================
 * VMA 与 mmap（S07 / S08 / S15）
 * ============================================================================ */
vmm_vma_t  *vmm_vma_find(vmm_mm_t *mm, u32 vaddr);
vmm_vma_t  *vmm_vma_at(vmm_mm_t *mm, u32 idx);
u32         vmm_mmap(vmm_mm_t *mm, u32 addr, u32 len, u32 prot, u32 flags,
                     u32 backing, u32 offset);
int         vmm_munmap(vmm_mm_t *mm, u32 addr, u32 len);
int         vmm_vma_insert(vmm_mm_t *mm, u32 start, u32 end, u32 prot,
                           u32 flags, u32 type, u32 backing, u32 offset);
int         vmm_vma_remove(vmm_mm_t *mm, u32 start, u32 end);
int         vmm_vma_split(vmm_mm_t *mm, u32 addr);
u32         vmm_vma_count(vmm_mm_t *mm);
int         vmm_populate(vmm_mm_t *mm, u32 addr, u32 len);
int         vmm_expand_stack(vmm_mm_t *mm, u32 vaddr);
u32         vmm_brk(vmm_mm_t *mm, u32 newbrk);
int         vmm_setup_layout(vmm_mm_t *mm);

/* ============================================================================
 * 缺页异常处理（S05 / S06 / S09 / S15）
 * ============================================================================ */
u32         vmm_handle_page_fault(u32 vaddr, u32 err, u32 eip, u32 cs);
u32         vmm_fault_resolve(vmm_mm_t *mm, u32 vaddr, u32 err, u32 eip);
u32         vmm_demand_page(vmm_mm_t *mm, vmm_vma_t *vma, u32 vaddr, u32 flags);
u32         vmm_break_cow(vmm_mm_t *mm, u32 vaddr);
u32         vmm_cow_share(vmm_mm_t *src, u32 svaddr, vmm_mm_t *dst, u32 dvaddr);
u32         vmm_fault_count(void);
u32         vmm_fault_resolved_count(void);
u32         vmm_fault_refused_count(void);
u32         vmm_cow_count(void);
u32         vmm_fault_log_count(void);
const vmm_fault_rec_t *vmm_fault_log_at(u32 idx);
const char *vmm_fault_action_name(u32 action);

/* ============================================================================
 * 页换出与换入（S09）
 * ============================================================================ */
int         vmm_swap_alloc_slot(void);
void        vmm_swap_free_slot(u32 slot);
u32         vmm_swap_free_slots(void);
u32         vmm_swap_total_slots(void);
u32         vmm_swap_used_slots(void);
int         vmm_swap_out(vmm_mm_t *mm, u32 vaddr);
int         vmm_swap_in(vmm_mm_t *mm, u32 vaddr);
u32         vmm_swap_out_count(void);
u32         vmm_swap_in_count(void);

/* ============================================================================
 * 反向映射 rmap（S17）
 * ============================================================================ */
int         vmm_rmap_add(u32 phys, u32 mm_id, u32 vaddr);
int         vmm_rmap_remove(u32 phys, u32 mm_id, u32 vaddr);
u32         vmm_rmap_count_for(u32 phys);
u32         vmm_rmap_used(void);
int         vmm_rmap_get(u32 idx, u32 *phys, u32 *mm_id, u32 *vaddr);
u32         vmm_rmap_total_for_phys(u32 phys);
int         vmm_rmap_remove_phys(u32 phys);

/* ============================================================================
 * 页缓存与回写（S18）
 * ============================================================================ */
int         vmm_backing_register(const char *name, u32 size,
                                 vmm_backing_rw_fn rd, vmm_backing_rw_fn wr);
u32         vmm_backing_count(void);
const vmm_backing_t *vmm_backing_at(u32 id);
u32         vmm_backing_read_bytes(u32 id);
u32         vmm_backing_write_bytes(u32 id);
u32         vmm_pgcache_get(u32 backing, u32 offset);
int         vmm_pgcache_mark_dirty(u32 phys);
int         vmm_pgcache_writeback(u32 backing, u32 offset);
u32         vmm_pgcache_count(void);
u32         vmm_pgcache_dirty_count(void);
u32         vmm_pgcache_writeback_count(void);
u32         vmm_pgcache_evict_clean(u32 nr);
u32         vmm_pgcache_selftest(void);

/* ============================================================================
 * 换页调度器（05 册第二轮）
 * ============================================================================ */
#define VMM_SWAP_POLICY_FIFO   0u
#define VMM_SWAP_POLICY_CLOCK  1u
u32  vmm_swap_policy(void);
void vmm_swap_policy_set(u32 policy);
u32  vmm_swap_clock_scans(void);
u32  vmm_swap_clock_reclaimed(void);
u32  vmm_swap_clock_hand(void);
u32  vmm_page_fault_rate_x1000(void);
int  vmm_swap_select_clock(vmm_mm_t *mm, u32 *out_vaddr, u32 *out_dirty);
int  vmm_page_replace(vmm_mm_t *mm, u32 vaddr);

/* ============================================================================
 * 内存压力与回收（S20）
 * ============================================================================ */
u32         vmm_reclaim(u32 target);
u32         vmm_shrinker_freed(void);
u32         vmm_shrinker_calls(void);

/* ============================================================================
 * 统计与调试（S19）
 * ============================================================================ */
void        vmm_stats(vmm_stats_t *out);
void        vmm_dump(void);
void        vmm_mm_dump(vmm_mm_t *mm);
u32         vmm_selftest(void);
u32         vmm_fault_selftest(void);
u32         vmm_ext_selftest(void);

#endif /* __XOS_VMM_H__ */
