/* ============================================================================
 * XOS 虚拟内存子系统 —— 核心实现
 *
 * 覆盖子域：S01 四级页表结构、S02 页目录与页表分配、S03 虚拟地址到物理地址转换、
 *           S04 页表项标志位管理、S10 TLB 刷新与击落、S12 大页映射、
 *           S13 用户态与内核态地址空间隔离、S14 地址空间随机化 ASLR、
 *           S16 内存保护与权限位、S19 地址空间统计
 *
 * 设计要点
 *   1. 对外四级页表框架，硬件两级时折叠 PUD / PMD（索引恒 0），
 *      地址转换例程按四级通用写法，将来切 PAE / 64 位只改折叠宏。
 *   2. 恒等映射：内核页目录对 0..128MB 建 4MB 大页恒等映射，
 *      使「物理地址 == 内核指针」，页表页与数据页无需额外映射即可访问。
 *   3. 内核直接映射：0xC0000000 起再映射一份物理 0..128MB，
 *      内核代码走恒等映射，内核数据可走直接映射，两者并存不冲突。
 *   4. 安全：开启 CR0.WP，内核也不能写只读页 —— 写时复制依赖此位成立；
 *      开启分页前先确认 PSE 可用，不可用则退回 4KB 页恒等映射，
 *      绝不假设硬件能力。
 *   5. 失败可复位：任何一步失败都不进入半开状态，vmm_init 记录 enabled 标志，
 *      未开启时全部映射接口直接返回错误，不影响引导流程继续。
 * ============================================================================ */
#include "vmm.h"
#include "vmm_internal.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * 控制寄存器与 CPU 特性
 * ------------------------------------------------------------------------ */
#define CR0_PG_BIT      0x80000000u
#define CR0_WP_BIT      0x00010000u
#define CR4_PSE_BIT     0x00000010u

static inline u32 vmm_read_cr0(void)
{
    u32 v;
    __asm__ __volatile__("movl %%cr0, %0" : "=r"(v));
    return v;
}
static inline void vmm_write_cr0(u32 v)
{
    __asm__ __volatile__("movl %0, %%cr0" : : "r"(v) : "memory");
}
static inline u32 vmm_read_cr3(void)
{
    u32 v;
    __asm__ __volatile__("movl %%cr3, %0" : "=r"(v));
    return v;
}
static inline void vmm_write_cr3(u32 v)
{
    __asm__ __volatile__("movl %0, %%cr3" : : "r"(v) : "memory");
}
static inline u32 vmm_read_cr4(void)
{
    u32 v;
    __asm__ __volatile__("movl %%cr4, %0" : "=r"(v));
    return v;
}
static inline void vmm_write_cr4(u32 v)
{
    __asm__ __volatile__("movl %0, %%cr4" : : "r"(v) : "memory");
}
static inline u32 vmm_read_cr2(void)
{
    u32 v;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(v));
    return v;
}
u32 vmm_fault_addr(void) { return vmm_read_cr2(); }

/* CPUID 探测 PSE（4MB 大页）支持 */
static int vmm_cpu_has_pse(void)
{
    u32 a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1), "c"(0));
    UNUSED(b); UNUSED(c);
    return (d & (1u << 3)) ? 1 : 0;     /* EDX bit3 = PSE */
}

/* --------------------------------------------------------------------------
 * 全局并发锁（05 册第二轮）
 * ------------------------------------------------------------------------ */
spinlock_t vmm_global_lock;
static u32 vmm_lock_count = 0;

u32 vmm_lock_enter(void) { vmm_lock_count++; return spin_lock_irqsave(&vmm_global_lock); }
void vmm_lock_exit(u32 eflags) { spin_unlock_irqrestore(&vmm_global_lock, eflags); }
u32 vmm_lock_calls(void) { return vmm_lock_count; }

/* --------------------------------------------------------------------------
 * 资源池
 * ------------------------------------------------------------------------ */
vmm_mm_t        vmm_mm_pool[VMM_MAX_MM];
vmm_vma_t       vmm_vma_pool[VMM_MAX_VMA];
vmm_rmap_t      vmm_rmap_pool[VMM_MAX_RMAP];
vmm_pgcache_t   vmm_pgcache_pool[VMM_MAX_PGCACHE];
vmm_huge_t      vmm_huge_pool[VMM_MAX_HUGE];
vmm_backing_t   vmm_backing_pool[VMM_MAX_BACKING];

u32 vmm_mm_used      = 0;
u32 vmm_vma_used     = 0;
u32 vmm_rmap_n       = 0;
u32 vmm_pgcache_used = 0;
u32 vmm_huge_used    = 0;
u32 vmm_backing_used = 0;
u32 vmm_pt_pages     = 0;
u32 vmm_next_mm_id   = 1;
u32 vmm_cow_total_n  = 0;      /* 系统级累计：写时复制次数 */
u32 vmm_grow_total_n = 0;      /* 系统级累计：栈增长次数 */

u32 vmm_dbg[VMM_DBG_SLOTS] = {0};
void vmm_dbg_clear(void)
{
    u32 i;
    for (i = 0; i < VMM_DBG_SLOTS; i++) vmm_dbg[i] = 0;
}
const char *vmm_dbg_label[VMM_DBG_SLOTS] = {
    "pa", "val", "pb", "pteB", "pteC", "rfa", "rfb", "rmapA"
};

vmm_mm_t *vmm_cur_mm = NULL;
u32       vmm_tlb_gen = 0;
u32       vmm_tlb_flush_count = 0;
u32       vmm_shootdown_count = 0;

static int  vmm_paging_on = 0;
static int  vmm_pse_ok    = 0;
static u32  vmm_pt_peak   = 0;
static u32  vmm_entropy_state = 0x5A5A1234u;

/* 内核页全局目录：4KB 对齐，链接地址即物理地址（分页开启前装载） */
static u32 swapper_pg_dir[PTRS_PER_PGD] __attribute__((aligned(4096)));

/* 4KB 恒等映射回退路径占用的页表页（PSE 不可用时使用） */
static u32 *vmm_fallback_pts[32];

int  vmm_pse_available(void) { return vmm_pse_ok; }
int  vmm_enabled(void)       { return vmm_paging_on; }
u32  vmm_kernel_pt_pages(void) { return vmm_pt_pages; }
u32  vmm_pt_pages_used(void)   { return vmm_pt_pages; }

void vmm_pool_reset(void)
{
    memset(vmm_mm_pool, 0, sizeof(vmm_mm_pool));
    memset(vmm_vma_pool, 0, sizeof(vmm_vma_pool));
    memset(vmm_rmap_pool, 0, sizeof(vmm_rmap_pool));
    memset(vmm_pgcache_pool, 0, sizeof(vmm_pgcache_pool));
    memset(vmm_huge_pool, 0, sizeof(vmm_huge_pool));
    memset(vmm_backing_pool, 0, sizeof(vmm_backing_pool));
    memset(swapper_pg_dir, 0, sizeof(swapper_pg_dir));
    memset(vmm_fallback_pts, 0, sizeof(vmm_fallback_pts));
    vmm_mm_used = vmm_vma_used = vmm_rmap_n = 0;
    vmm_pgcache_used = vmm_huge_used = vmm_backing_used = 0;
    vmm_pt_pages = 0;
    vmm_next_mm_id = 1;
    vmm_cur_mm = NULL;
    vmm_tlb_gen = 0;
    vmm_tlb_flush_count = 0;
    vmm_shootdown_count = 0;
    vmm_paging_on = 0;
    vmm_pse_ok = 0;
    vmm_pt_peak = 0;
}

/* --------------------------------------------------------------------------
 * 物理页引用计数辅助
 * ------------------------------------------------------------------------ */
int vmm_get_phys(u32 phys)
{
    if (!phys) return VMM_EINVAL;
    return (pmm_ref_inc(phys) == PMM_OK) ? VMM_OK : VMM_EINVAL;
}

void vmm_put_phys(u32 phys)
{
    if (!phys) return;
    if (pmm_ref_dec(phys) != PMM_OK) return;
    if (pmm_refcount(phys) == 0) {
        pmm_free_page(phys);
    }
}

/* --------------------------------------------------------------------------
 * 索引与层级偏移（折叠 PUD / PMD）
 * ------------------------------------------------------------------------ */
u32 vmm_pgd_index(u32 vaddr) { return vaddr >> PGDIR_SHIFT; }
u32 vmm_pte_index(u32 vaddr) { return (vaddr >> PTE_SHIFT) & (PTRS_PER_PTE - 1u); }

u32 *vmm_pgd_offset(vmm_mm_t *mm, u32 vaddr)
{
    if (!mm || !mm->pgd) return NULL;
    return &mm->pgd[vmm_pgd_index(vaddr)];
}

/* PUD 在两级硬件上折叠：直接返回 PGD 项地址 */
u32 *vmm_pud_offset(u32 *pgd, u32 vaddr)
{
    UNUSED(vaddr);
    return pgd;
}

/* PMD 在两级硬件上折叠：直接返回 PUD 地址 */
u32 *vmm_pmd_offset(u32 *pud, u32 vaddr)
{
    UNUSED(vaddr);
    return pud;
}

/* PTE 偏移：PMD 项指向页表页，项内按 PTE 索引定位 */
u32 *vmm_pte_offset(u32 *pmd, u32 vaddr)
{
    if (!pmd) return NULL;
    if (!(*pmd & PTE_P)) return NULL;
    if (*pmd & PTE_PS) return NULL;         /* 大页，无页表页 */
    return vmm_pte_ptr(*pmd) + vmm_pte_index(vaddr);
}

/* --------------------------------------------------------------------------
 * 页表页分配（S02）
 * ------------------------------------------------------------------------ */
u32 *vmm_alloc_pt(void)
{
    u32 phys = pmm_alloc_page_zeroed();
    if (!phys) return NULL;
    vmm_pt_pages++;
    if (vmm_pt_pages > vmm_pt_peak) vmm_pt_peak = vmm_pt_pages;
    return (u32 *)phys;
}

void vmm_free_pt(u32 *pt)
{
    if (!pt) return;
    pmm_free_page((u32)pt);
    if (vmm_pt_pages) vmm_pt_pages--;
}

u32 *vmm_alloc_pgd(void)
{
    u32 phys = pmm_alloc_page_zeroed();
    if (!phys) return NULL;
    vmm_pt_pages++;
    if (vmm_pt_pages > vmm_pt_peak) vmm_pt_peak = vmm_pt_pages;
    return (u32 *)phys;
}

void vmm_free_pgd(u32 *pgd)
{
    if (!pgd) return;
    pmm_free_page((u32)pgd);
    if (vmm_pt_pages) vmm_pt_pages--;
}

int vmm_ensure_pt(vmm_mm_t *mm, u32 vaddr)
{
    return vmm_walk(mm, vaddr, 1) ? VMM_OK : VMM_ENOMEM;
}

/* --------------------------------------------------------------------------
 * 页表遍历（S01 / S03）
 * ------------------------------------------------------------------------ */
u32 *vmm_walk(vmm_mm_t *mm, u32 vaddr, int create)
{
    u32 *pgd, *pud, *pmd, *pt;
    u32 flags = PTE_P | PTE_RW;

    if (!mm || !mm->pgd) return NULL;

    if (mm->user_access && vmm_is_user_addr(vaddr)) flags |= PTE_US;

    pgd = vmm_pgd_offset(mm, vaddr);
    pud = vmm_pud_offset(pgd, vaddr);
    pmd = vmm_pmd_offset(pud, vaddr);

    if (!(*pmd & PTE_P)) {
        if (!create) return NULL;
        pt = vmm_alloc_pt();
        if (!pt) return NULL;
        *pmd = ((u32)pt & PTE_PFN_MASK) | flags;
        mm->pt_pages++;
    } else if (*pmd & PTE_PS) {
        return NULL;        /* 该处是大页映射，不能再往下走 */
    }
    return vmm_pte_offset(pmd, vaddr);
}

/* 软件侧地址转换：给定地址空间，返回物理地址（0 表示未映射） */
u32 vmm_translate(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte;
    u32 pi;

    if (!mm || !mm->pgd) return 0;

    pi = vmm_pgd_index(vaddr);
    if (pi < KERNEL_PGD_INDEX && (mm->pgd[pi] & PTE_PS)) {
        return (mm->pgd[pi] & PTE_PFN_MASK) + (vaddr & (PGDIR_SIZE - 1u));
    }
    pte = vmm_walk(mm, vaddr, 0);
    if (!pte) return 0;
    if (!(*pte & PTE_P)) return 0;
    if (*pte & PTE_SW_SWAP) return 0;       /* 已换出，无物理页 */
    return (*pte & PTE_PFN_MASK) + (vaddr & PTE_MASK);
}

/* 硬件侧地址转换：完全依据当前 CR3 与页表内容，不依赖任何软件状态。
 * 用于自检中「软件结论 vs 硬件真实行为」的交叉验证。 */
u32 vmm_translate_hw(u32 vaddr)
{
    u32 cr3 = vmm_read_cr3() & PTE_PFN_MASK;
    u32 *pgd = (u32 *)cr3;
    u32 *pt;
    u32 pi = vmm_pgd_index(vaddr);

    if (!(pgd[pi] & PTE_P)) return 0;
    if (pgd[pi] & PTE_PS) {
        return (pgd[pi] & PTE_PFN_MASK) + (vaddr & (PGDIR_SIZE - 1u));
    }
    pt = vmm_pte_ptr(pgd[pi]);
    if (!(pt[vmm_pte_index(vaddr)] & PTE_P)) return 0;
    return (pt[vmm_pte_index(vaddr)] & PTE_PFN_MASK) + (vaddr & PTE_MASK);
}

u32 vmm_pgd_count(vmm_mm_t *mm)
{
    u32 i, n = 0;
    if (!mm || !mm->pgd) return 0;
    for (i = 0; i < PTRS_PER_PGD; i++) {
        if (mm->pgd[i] & PTE_P) n++;
    }
    return n;
}

u32 vmm_pt_count(vmm_mm_t *mm)
{
    u32 i, n = 0;
    if (!mm || !mm->pgd) return 0;
    for (i = 0; i < PTRS_PER_PGD; i++) {
        if ((mm->pgd[i] & PTE_P) && !(mm->pgd[i] & PTE_PS)) n++;
    }
    return n;
}

/* --------------------------------------------------------------------------
 * 映射与解除映射（S02 / S07）
 * ------------------------------------------------------------------------ */
int vmm_map_page(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags)
{
    u32 *pte;
    u32 val;

    if (!mm || !phys) return VMM_EINVAL;
    if (!PAGE_IS_ALIGNED(vaddr) || !PAGE_IS_ALIGNED(phys)) return VMM_EINVAL;
    if (vmm_is_kernel_addr(vaddr)) return VMM_EPERM;   /* 内核区由直接映射管，不许改 */

    pte = vmm_walk(mm, vaddr, 1);
    if (!pte) return VMM_ENOMEM;

    val = (phys & PTE_PFN_MASK) | (flags & ~PTE_PFN_MASK) | PTE_P;
    *pte = val;
    vmm_flush_tlb_page(vaddr);
    vmm_rmap_add(phys, mm->id, vaddr);
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * 设备内存映射（MMIO，S35 图形桌面 LFB）
 * 内核驱动（如 VBE 帧缓冲）需要把 PCI/MMIO 设备地址映射进高端内核区。
 * 普通映射接口因 vmm_is_kernel_addr 检查而拒绝高端地址，这里提供受控例外：
 *   - 只允许映射到高端内核区（vaddr >= KERNEL_SPACE_BASE），防止误用于用户区
 *   - 不登记反向映射（设备地址不在 RAM，不参与换页回收）
 * ------------------------------------------------------------------------ */
int vmm_map_device(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags)
{
    u32 *pte;
    u32 val;

    if (!mm || !phys) return VMM_EINVAL;
    if (!PAGE_IS_ALIGNED(vaddr) || !PAGE_IS_ALIGNED(phys)) return VMM_EINVAL;
    if (vaddr < KERNEL_SPACE_BASE) return VMM_EPERM;   /* 仅高端内核区可做设备映射 */

    pte = vmm_walk(mm, vaddr, 1);
    if (!pte) return VMM_ENOMEM;

    val = (phys & PTE_PFN_MASK) | (flags & ~PTE_PFN_MASK) | PTE_P;
    *pte = val;
    vmm_flush_tlb_page(vaddr);
    return VMM_OK;
}

/* 大页设备映射（PSE 4MB，供 2MB/4MB 对齐的帧缓冲一次建表） */
int vmm_map_device_huge(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags)
{
    u32 pi;
    u32 *pmd;

    if (!mm) return VMM_EINVAL;
    if (!vmm_pse_ok) return VMM_EPERM;
    if (vaddr & (PGDIR_SIZE - 1u)) return VMM_EINVAL;
    if (phys  & (PGDIR_SIZE - 1u)) return VMM_EINVAL;
    if (vaddr < KERNEL_SPACE_BASE) return VMM_EPERM;   /* 仅高端内核区 */

    pi = vmm_pgd_index(vaddr);
    pmd = &mm->pgd[pi];
    if (*pmd & PTE_P) return VMM_EBUSY;     /* 已有映射（含小页页表） */

    *pmd = (phys & PTE_PFN_MASK) | (flags & ~PTE_PFN_MASK) | PTE_P | PTE_PS;
    vmm_flush_tlb_all();
    return VMM_OK;
}

int vmm_unmap_page(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte;
    u32 phys, flags;

    if (!mm) return VMM_EINVAL;
    pte = vmm_walk(mm, vaddr, 0);
    if (!pte) return VMM_ENOENT;
    /* 换出页的 P 位为 0，须以 SW_SWAP 一并接受，否则槽位与映射表项泄漏 */
    if (!(*pte & PTE_P) && !(*pte & PTE_SW_SWAP)) return VMM_ENOENT;

    /* 标志位必须在清零之前取出：清零后再读 *pte 得到的恒为 0，
     * 原实现据此判断「是否换出页」，条件恒为假，映射表项永不摘除。 */
    phys  = *pte & PTE_PFN_MASK;
    flags = *pte;
    *pte  = 0;
    vmm_flush_tlb_page(vaddr);
    if (flags & PTE_SW_SWAP) {
        vmm_swap_free_slot(phys >> PTE_SHIFT);
        return VMM_OK;
    }
    vmm_rmap_remove(phys, mm->id, vaddr);
    return VMM_OK;
}

int vmm_unmap_page_put(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte;
    u32 phys, flags;

    if (!mm) return VMM_EINVAL;
    pte = vmm_walk(mm, vaddr, 0);
    if (!pte) return VMM_ENOENT;
    if (!(*pte & PTE_P) && !(*pte & PTE_SW_SWAP)) return VMM_ENOENT;

    phys  = *pte & PTE_PFN_MASK;
    flags = *pte;
    *pte  = 0;
    vmm_flush_tlb_page(vaddr);

    if (flags & PTE_SW_SWAP) {
        vmm_swap_free_slot(phys >> PTE_SHIFT);
        return VMM_OK;
    }
    vmm_rmap_remove(phys, mm->id, vaddr);
    vmm_put_phys(phys);
    return VMM_OK;
}

int vmm_map_range(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 len, u32 flags)
{
    u32 off, n;
    if (!mm) return VMM_EINVAL;
    if (!PAGE_IS_ALIGNED(vaddr) || !PAGE_IS_ALIGNED(phys)) return VMM_EINVAL;
    n = PAGE_ALIGN_UP(len) >> PTE_SHIFT;
    for (off = 0; off < n; off++) {
        int rc = vmm_map_page(mm, vaddr + (off << PTE_SHIFT),
                              phys + (off << PTE_SHIFT), flags);
        if (rc != VMM_OK) return rc;
    }
    return VMM_OK;
}

int vmm_unmap_range(vmm_mm_t *mm, u32 vaddr, u32 len)
{
    u32 off, n;
    if (!mm) return VMM_EINVAL;
    n = PAGE_ALIGN_UP(len) >> PTE_SHIFT;
    for (off = 0; off < n; off++) {
        vmm_unmap_page(mm, vaddr + (off << PTE_SHIFT));
    }
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * 页表项标志位管理（S04 / S16）
 * ------------------------------------------------------------------------ */
u32 vmm_pte_get(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte = vmm_walk(mm, vaddr, 0);
    return pte ? *pte : 0;
}

int vmm_pte_set_flags(vmm_mm_t *mm, u32 vaddr, u32 mask, u32 value)
{
    u32 *pte = vmm_walk(mm, vaddr, 0);
    if (!pte) return VMM_ENOENT;
    *pte = (*pte & ~mask) | (value & mask);
    vmm_flush_tlb_page(vaddr);
    return VMM_OK;
}

int vmm_pte_present(vmm_mm_t *mm, u32 vaddr)  { return (vmm_pte_get(mm, vaddr) & PTE_P) ? 1 : 0; }
int vmm_pte_writable(vmm_mm_t *mm, u32 vaddr) { return (vmm_pte_get(mm, vaddr) & PTE_RW) ? 1 : 0; }
int vmm_pte_user(vmm_mm_t *mm, u32 vaddr)     { return (vmm_pte_get(mm, vaddr) & PTE_US) ? 1 : 0; }
int vmm_pte_cow(vmm_mm_t *mm, u32 vaddr)      { return (vmm_pte_get(mm, vaddr) & PTE_SW_COW) ? 1 : 0; }
int vmm_pte_swapped(vmm_mm_t *mm, u32 vaddr)  { return (vmm_pte_get(mm, vaddr) & PTE_SW_SWAP) ? 1 : 0; }
int vmm_pte_locked(vmm_mm_t *mm, u32 vaddr)   { return (vmm_pte_get(mm, vaddr) & PTE_SW_LOCK) ? 1 : 0; }

u32 vmm_prot_to_flags(u32 prot, int user)
{
    u32 f = PTE_P;
    if (prot & PROT_WRITE) f |= PTE_RW;
    if (user) f |= PTE_US;
    /* 注：32 位无 PAE 时硬件无 NX 位，PROT_EXEC 无法在页表层禁止执行，
     *     取指权限由段级与后续 W^X 软件校验承担。 */
    return f;
}

u32 vmm_flags_to_prot(u32 flags)
{
    u32 p = PROT_READ;
    if (flags & PTE_RW) p |= PROT_WRITE;
    return p;
}

int _vmm_mprotect_locked(vmm_mm_t *mm, u32 addr, u32 len, u32 prot)
{
    u32 a, end;
    u32 want_rw = (prot & PROT_WRITE) ? PTE_RW : 0u;

    if (!mm) return VMM_EINVAL;
    end = addr + PAGE_ALIGN_UP(len);

    for (a = PAGE_ALIGN_DOWN(addr); a < end; a += PTE_SIZE) {
        u32 *pte = vmm_walk(mm, a, 0);
        if (!pte || !(*pte & PTE_P)) continue;
        if (want_rw) *pte |= PTE_RW;
        else         *pte &= ~PTE_RW;
        vmm_flush_tlb_page(a);
    }
    return VMM_OK;
}

/* 纯页表遍历式权限判定：不触发真实缺页，用于事前校验与自检 */
int vmm_check_access(vmm_mm_t *mm, u32 vaddr, u32 write, u32 user)
{
    u32 pte;

    if (!mm) return VMM_EINVAL;
    pte = vmm_pte_get(mm, vaddr);
    if (!(pte & PTE_P)) return VMM_EFAULT;
    if (user && !(pte & PTE_US)) return VMM_EPERM;
    if (write && !(pte & PTE_RW)) return VMM_EPERM;
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * 访问位与脏位跟踪（S11）
 * ------------------------------------------------------------------------ */
int vmm_get_accessed(vmm_mm_t *mm, u32 vaddr) { return (vmm_pte_get(mm, vaddr) & PTE_A) ? 1 : 0; }
int vmm_get_dirty(vmm_mm_t *mm, u32 vaddr)    { return (vmm_pte_get(mm, vaddr) & PTE_D) ? 1 : 0; }

int vmm_clear_accessed(vmm_mm_t *mm, u32 vaddr)
{
    int rc = vmm_pte_set_flags(mm, vaddr, PTE_A, 0);
    return rc;
}

int vmm_clear_dirty(vmm_mm_t *mm, u32 vaddr)
{
    int rc = vmm_pte_set_flags(mm, vaddr, PTE_D, 0);
    return rc;
}

int vmm_scan_ad(vmm_mm_t *mm, u32 start, u32 len, u32 *accessed, u32 *dirty)
{
    u32 a, end, na = 0, nd = 0;
    if (!mm) return VMM_EINVAL;
    end = start + PAGE_ALIGN_UP(len);
    for (a = PAGE_ALIGN_DOWN(start); a < end; a += PTE_SIZE) {
        u32 pte = vmm_pte_get(mm, a);
        if (!(pte & PTE_P)) continue;
        if (pte & PTE_A) na++;
        if (pte & PTE_D) nd++;
    }
    if (accessed) *accessed = na;
    if (dirty)    *dirty = nd;
    return VMM_OK;
}

u32 vmm_count_accessed(vmm_mm_t *mm)
{
    u32 i, j, n = 0;
    if (!mm || !mm->pgd) return 0;
    for (i = KERNEL_IDENTITY_PGDS; i < KERNEL_PGD_INDEX; i++) {
        u32 *pt;
        if (!(mm->pgd[i] & PTE_P) || (mm->pgd[i] & PTE_PS)) continue;
        pt = vmm_pte_ptr(mm->pgd[i]);
        for (j = 0; j < PTRS_PER_PTE; j++) {
            if ((pt[j] & PTE_P) && (pt[j] & PTE_A)) n++;
        }
    }
    return n;
}

u32 vmm_count_dirty(vmm_mm_t *mm)
{
    u32 i, j, n = 0;
    if (!mm || !mm->pgd) return 0;
    for (i = KERNEL_IDENTITY_PGDS; i < KERNEL_PGD_INDEX; i++) {
        u32 *pt;
        if (!(mm->pgd[i] & PTE_P) || (mm->pgd[i] & PTE_PS)) continue;
        pt = vmm_pte_ptr(mm->pgd[i]);
        for (j = 0; j < PTRS_PER_PTE; j++) {
            if ((pt[j] & PTE_P) && (pt[j] & PTE_D)) n++;
        }
    }
    return n;
}

/* --------------------------------------------------------------------------
 * TLB 刷新与击落（S10）
 * ------------------------------------------------------------------------ */
void vmm_flush_tlb_page(u32 vaddr)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
    vmm_tlb_flush_count++;
}

void vmm_flush_tlb_all(void)
{
    u32 cr3 = vmm_read_cr3();
    vmm_write_cr3(cr3);
    vmm_tlb_flush_count++;
}

/* 击落协议：当前地址空间立即失效；其他地址空间登记代次，
 * 待其下次被调度（vmm_mm_switch）时统一失效。
 * 单 CPU 环境下「远端处理器集合」为空，但协议与代次机制完整保留，
 * 多核接入时只需在远端集合上补发 IPI。 */
int vmm_tlb_shootdown(vmm_mm_t *mm, u32 vaddr, u32 len)
{
    u32 a, end;

    if (!mm) return VMM_EINVAL;
    vmm_shootdown_count++;

    if (mm == vmm_cur_mm) {
        end = vaddr + PAGE_ALIGN_UP(len ? len : 1u);
        for (a = PAGE_ALIGN_DOWN(vaddr); a < end; a += PTE_SIZE) {
            vmm_flush_tlb_page(a);
        }
        return VMM_OK;
    }

    vmm_tlb_gen++;
    return VMM_OK;
}

u32 vmm_tlb_pending(vmm_mm_t *mm)
{
    if (!mm) return 0;
    return (mm->tlb_gen < vmm_tlb_gen) ? (vmm_tlb_gen - mm->tlb_gen) : 0;
}

u32 vmm_tlb_flush_total(void)     { return vmm_tlb_flush_count; }
u32 vmm_tlb_shootdown_total(void) { return vmm_shootdown_count; }

/* --------------------------------------------------------------------------
 * 大页映射（S12，PSE 4MB）
 * ------------------------------------------------------------------------ */
u32 vmm_huge_count(void)     { return vmm_huge_used; }
u32 vmm_huge_vaddr(u32 idx)  { return (idx < VMM_MAX_HUGE) ? vmm_huge_pool[idx].vaddr : 0; }
u32 vmm_huge_phys(u32 idx)   { return (idx < VMM_MAX_HUGE) ? vmm_huge_pool[idx].phys : 0; }

int vmm_map_huge(vmm_mm_t *mm, u32 vaddr, u32 phys, u32 flags)
{
    u32 pi;
    u32 *pmd;

    if (!mm) return VMM_EINVAL;
    if (!vmm_pse_ok) return VMM_EPERM;
    if (vaddr & (PGDIR_SIZE - 1u)) return VMM_EINVAL;
    if (phys  & (PGDIR_SIZE - 1u)) return VMM_EINVAL;
    if (vmm_is_kernel_addr(vaddr)) return VMM_EPERM;

    pi = vmm_pgd_index(vaddr);
    pmd = &mm->pgd[pi];
    if (*pmd & PTE_P) return VMM_EBUSY;     /* 已有映射（含小页页表） */

    *pmd = (phys & PTE_PFN_MASK) | (flags & ~PTE_PFN_MASK) | PTE_P | PTE_PS;
    vmm_flush_tlb_all();
    return VMM_OK;
}

int vmm_unmap_huge(vmm_mm_t *mm, u32 vaddr)
{
    u32 pi;
    u32 *pmd;

    if (!mm) return VMM_EINVAL;
    if (vaddr & (PGDIR_SIZE - 1u)) return VMM_EINVAL;
    pi = vmm_pgd_index(vaddr);
    pmd = &mm->pgd[pi];
    if (!(*pmd & PTE_P) || !(*pmd & PTE_PS)) return VMM_ENOENT;
    *pmd = 0;
    vmm_flush_tlb_all();
    return VMM_OK;
}

int vmm_huge_alloc(vmm_mm_t *mm, u32 vaddr, u32 flags)
{
    u32 i, phys, slot;
    int rc;

    for (i = 0; i < VMM_MAX_HUGE; i++) {
        if (!vmm_huge_pool[i].used) break;
    }
    if (i >= VMM_MAX_HUGE) return VMM_ENOSPC;

    phys = pmm_alloc_pages(VMM_HUGE_PAGES);
    if (!phys) return VMM_ENOMEM;

    rc = vmm_map_huge(mm, vaddr, phys, flags);
    if (rc != VMM_OK) {
        pmm_free_pages(phys, VMM_HUGE_PAGES);
        return rc;
    }

    slot = i;
    vmm_huge_pool[slot].vaddr = vaddr;
    vmm_huge_pool[slot].phys  = phys;
    vmm_huge_pool[slot].flags = flags;
    vmm_huge_pool[slot].used  = 1;
    vmm_huge_used++;
    return VMM_OK;
}

int vmm_huge_free(vmm_mm_t *mm, u32 vaddr)
{
    u32 i;
    int rc = vmm_unmap_huge(mm, vaddr);
    if (rc != VMM_OK) return rc;
    for (i = 0; i < VMM_MAX_HUGE; i++) {
        if (vmm_huge_pool[i].used && vmm_huge_pool[i].vaddr == vaddr) {
            pmm_free_pages(vmm_huge_pool[i].phys, VMM_HUGE_PAGES);
            vmm_huge_pool[i].used = 0;
            vmm_huge_pool[i].vaddr = 0;
            vmm_huge_pool[i].phys = 0;
            vmm_huge_used--;
            return VMM_OK;
        }
    }
    return VMM_ENOENT;
}

/* --------------------------------------------------------------------------
 * 地址空间隔离（S13）
 * ------------------------------------------------------------------------ */
/* 内核地址：高端直接映射区，或低端恒等映射区（内核映像 / 栈 / VGA / 页表页） */
int vmm_is_kernel_addr(u32 vaddr)
{
    if (vaddr >= KERNEL_SPACE_BASE) return 1;
    if (vaddr <  KERNEL_IDENTITY_TOP) return 1;
    return 0;
}

/* 用户地址：仅供用户映射的中间区段 */
int vmm_is_user_addr(u32 vaddr)
{
    return (vaddr >= KERNEL_IDENTITY_TOP && vaddr < KERNEL_SPACE_BASE) ? 1 : 0;
}

/* 把内核页目录项复制到新地址空间：所有地址空间共享同一份内核映射。
 * 低端恒等映射（内核自身运行所必需）与高端直接映射两部分都要复制，
 * 且都必须保持 US=0 —— 内核可访问、用户态不可访问。 */
int vmm_copy_kernel_pgd(vmm_mm_t *mm)
{
    u32 i, n = 0;
    if (!mm || !mm->pgd) return VMM_EINVAL;

    for (i = 0; i < KERNEL_IDENTITY_PGDS; i++) {
        mm->pgd[i] = swapper_pg_dir[i] & ~PTE_US;
        if (swapper_pg_dir[i] & PTE_P) n++;
    }
    for (i = KERNEL_PGD_INDEX; i < PTRS_PER_PGD; i++) {
        mm->pgd[i] = swapper_pg_dir[i] & ~PTE_US;
        if (swapper_pg_dir[i] & PTE_P) n++;
    }
    return (int)n;
}

u32 vmm_kernel_pgd_shared(void)
{
    u32 i, n = 0;
    for (i = 0; i < KERNEL_IDENTITY_PGDS; i++) {
        if (swapper_pg_dir[i] & PTE_P) n++;
    }
    for (i = KERNEL_PGD_INDEX; i < PTRS_PER_PGD; i++) {
        if (swapper_pg_dir[i] & PTE_P) n++;
    }
    return n;
}

int vmm_set_user_access(vmm_mm_t *mm, int on)
{
    if (!mm) return VMM_EINVAL;
    mm->user_access = on ? 1u : 0u;
    return VMM_OK;
}

/* 隔离性校验：
 *   1) 内核恒等映射区与高端直接映射区必须与内核页目录逐项一致
 *   2) 内核区页表项不得带 US 位（用户态绝不可访问内核页）
 *   3) 用户区页表项不得指向内核恒等映射区，也不得指向低端 1MB */
int vmm_check_isolation(vmm_mm_t *mm)
{
    u32 i;

    if (!mm || !mm->pgd) return 1;

    for (i = 0; i < KERNEL_IDENTITY_PGDS; i++) {
        if (mm->pgd[i] != (swapper_pg_dir[i] & ~PTE_US)) return 2;
        if (mm->pgd[i] & PTE_US) return 4;
    }
    for (i = KERNEL_PGD_INDEX; i < PTRS_PER_PGD; i++) {
        if (mm->pgd[i] != (swapper_pg_dir[i] & ~PTE_US)) return 3;
        if (mm->pgd[i] & PTE_US) return 5;
    }
    for (i = KERNEL_IDENTITY_PGDS; i < KERNEL_PGD_INDEX; i++) {
        if (!(mm->pgd[i] & PTE_P)) continue;
        if ((mm->pgd[i] & PTE_PFN_MASK) < KERNEL_IDENTITY_TOP) return 6;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * ASLR（S14）
 * ------------------------------------------------------------------------ */
static u32 vmm_aslr_on = 1;

u32 vmm_random(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    vmm_entropy_state ^= lo;
    vmm_entropy_state ^= hi;
    vmm_entropy_state ^= vmm_entropy_state << 13;
    vmm_entropy_state ^= vmm_entropy_state >> 17;
    vmm_entropy_state ^= vmm_entropy_state << 5;
    return vmm_entropy_state;
}

void vmm_aslr_set(u32 on) { vmm_aslr_on = on ? 1u : 0u; }
u32  vmm_aslr_get(void)   { return vmm_aslr_on; }
u32  vmm_aslr_entropy(void) { return vmm_entropy_state; }

u32 vmm_aslr_mmap_base(void)
{
    u32 base = USER_MMAP_BASE;
    if (!vmm_aslr_on) return base;
    /* 12 位页号熵 → 0..16MB 随机偏移，页对齐 */
    return base + ((vmm_random() & 0xFFFu) << PTE_SHIFT);
}

u32 vmm_aslr_stack_top(void)
{
    u32 base = USER_STACK_TOP;
    if (!vmm_aslr_on) return base;
    return base - ((vmm_random() & 0xFFFu) << PTE_SHIFT);
}

/* --------------------------------------------------------------------------
 * 地址空间生命周期
 * ------------------------------------------------------------------------ */
vmm_mm_t *vmm_kernel_mm(void)
{
    if (vmm_mm_used == 0) return NULL;
    return &vmm_mm_pool[0];
}

vmm_mm_t *vmm_current_mm(void) { return vmm_cur_mm; }
u32 vmm_mm_count(void)         { return vmm_mm_used; }

vmm_mm_t *vmm_mm_at(u32 idx)
{
    if (idx >= VMM_MAX_MM || !vmm_mm_pool[idx].used) return NULL;
    return &vmm_mm_pool[idx];
}

u32 vmm_current_mm_id(void) { return vmm_cur_mm ? vmm_cur_mm->id : 0xFFFFFFFFu; }

vmm_mm_t *vmm_mm_create(void)
{
    u32 i;
    vmm_mm_t *mm = NULL;
    u32 *pgd;

    if (!vmm_paging_on) return NULL;

    for (i = 0; i < VMM_MAX_MM; i++) {
        if (!vmm_mm_pool[i].used) { mm = &vmm_mm_pool[i]; break; }
    }
    if (!mm) return NULL;

    pgd = vmm_alloc_pgd();
    if (!pgd) return NULL;

    memset(mm, 0, sizeof(*mm));
    mm->pgd      = pgd;
    mm->pgd_phys = (u32)pgd;
    mm->id       = vmm_next_mm_id++;
    mm->used     = 1;
    mm->refcount = 1;
    mm->user_access = 1;
    mm->stack_vma = 0xFFFFFFFFu;
    mm->tlb_gen  = vmm_tlb_gen;

    vmm_copy_kernel_pgd(mm);
    vmm_setup_layout(mm);

    vmm_mm_used++;
    return mm;
}

int vmm_mm_destroy(vmm_mm_t *mm)
{
    u32 i, a;
    vmm_vma_t *v;

    if (!mm || !mm->used) return VMM_EINVAL;
    if (mm == vmm_kernel_mm()) return VMM_EPERM;   /* 内核地址空间不可销毁 */
    if (mm == vmm_cur_mm) vmm_mm_switch(vmm_kernel_mm());

    /* 1) 按 VMA 释放用户区所有映射页 */
    for (i = 0; i < VMM_MAX_VMA; i++) {
        v = &vmm_vma_pool[i];
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        for (a = v->vm_start; a < v->vm_end; a += PTE_SIZE) {
            vmm_unmap_page_put(mm, a);
        }
        v->vm_used = 0;
        vmm_vma_used--;
    }

    /* 2) 释放用户区页表页与大页（内核恒等映射区与高端直接映射区不属本空间） */
    for (i = KERNEL_IDENTITY_PGDS; i < KERNEL_PGD_INDEX; i++) {
        if (!(mm->pgd[i] & PTE_P)) continue;
        if (mm->pgd[i] & PTE_PS) { mm->pgd[i] = 0; continue; }
        vmm_free_pt(vmm_pte_ptr(mm->pgd[i]));
        mm->pgd[i] = 0;
    }

    /* 3) 释放页全局目录 */
    vmm_free_pgd(mm->pgd);
    mm->pgd = NULL;
    mm->used = 0;
    vmm_mm_used--;
    return VMM_OK;
}

int vmm_mm_switch(vmm_mm_t *mm)
{
    if (!mm || !mm->used) return VMM_EINVAL;

    /* 该地址空间有未同步的击落请求 → 装载前统一失效 */
    if (mm->tlb_gen < vmm_tlb_gen) {
        mm->tlb_gen = vmm_tlb_gen;
    }
    vmm_cur_mm = mm;
    vmm_write_cr3(mm->pgd_phys);
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------------ */
/* PSE 可用：4MB 大页恒等映射，无需额外页表页 */
static void vmm_build_identity_pse(void)
{
    u32 i;
    /* 固定映射 0-128MB 物理：高位含 ACPI 表（0x7FF0000 等），
     * PMM 仅管理 96MB（PMM_MAX_PHYS），多映射区不分配、无冲突。 */
    u32 max_pgd = (128u * 1024u * 1024u) >> PGDIR_SHIFT;      /* 128MB / 4MB = 32 */

    for (i = 0; i < max_pgd; i++) {
        u32 phys = i << PGDIR_SHIFT;
        swapper_pg_dir[i] = phys | PTE_P | PTE_RW | PTE_PS;
        swapper_pg_dir[KERNEL_PGD_INDEX + i] = phys | PTE_P | PTE_RW | PTE_PS;
    }
}

/* PSE 不可用：退回 4KB 页恒等映射，需为每个 4MB 窗口分配一个页表页 */
static int vmm_build_identity_4k(void)
{
    u32 i, j;
    /* 同 PSE 路径：恒等映射覆盖 0-128MB（含高位 ACPI 区） */
    u32 max_pgd = (128u * 1024u * 1024u) >> PGDIR_SHIFT;

    for (i = 0; i < max_pgd; i++) {
        u32 *pt = vmm_alloc_pt();
        if (!pt) return VMM_ENOMEM;
        vmm_fallback_pts[i] = pt;
        for (j = 0; j < PTRS_PER_PTE; j++) {
            u32 phys = (i << PGDIR_SHIFT) + (j << PTE_SHIFT);
            pt[j] = phys | PTE_P | PTE_RW;
        }
        swapper_pg_dir[i] = ((u32)pt & PTE_PFN_MASK) | PTE_P | PTE_RW;
        swapper_pg_dir[KERNEL_PGD_INDEX + i] = swapper_pg_dir[i];
    }
    return VMM_OK;
}

void vmm_init(void)
{
    spin_lock_init(&vmm_global_lock);
    u32 cr4, cr0;

    vmm_pool_reset();

    /* 1) 能力探测：不假设硬件支持 4MB 大页 */
    vmm_pse_ok = vmm_cpu_has_pse();

    /* 2) 建立恒等映射与内核直接映射 */
    if (vmm_pse_ok) {
        vmm_build_identity_pse();
    } else {
        if (vmm_build_identity_4k() != VMM_OK) {
            con_set_color(VGA_LIGHTRED, VGA_BLACK);
            con_puts("  VMM: cannot build identity map, paging left disabled\n");
            con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
            return;                 /* 保持分页关闭：系统仍可安全运行 */
        }
    }

    /* 3) 使能 PSE（仅当硬件支持） */
    if (vmm_pse_ok) {
        cr4 = vmm_read_cr4();
        cr4 |= CR4_PSE_BIT;
        vmm_write_cr4(cr4);
    }

    /* 4) 装载页全局目录 */
    vmm_write_cr3((u32)swapper_pg_dir);

    /* 5) 开启分页与写保护（WP 使内核也不能写只读页，COW 依赖此位） */
    cr0 = vmm_read_cr0();
    cr0 |= CR0_PG_BIT | CR0_WP_BIT;
    vmm_write_cr0(cr0);

    vmm_paging_on = 1;

    /* 6) 内核地址空间登记为 0 号 */
    memset(&vmm_mm_pool[0], 0, sizeof(vmm_mm_pool[0]));
    vmm_mm_pool[0].pgd      = swapper_pg_dir;
    vmm_mm_pool[0].pgd_phys = (u32)swapper_pg_dir;
    vmm_mm_pool[0].id       = 0;
    vmm_mm_pool[0].used     = 1;
    vmm_mm_pool[0].refcount = 1;
    vmm_mm_pool[0].user_access = 1;   /* 用户程序(ring3)映射进内核地址空间：页表页 PMD 需带 US 位 */
    vmm_mm_pool[0].stack_vma = 0xFFFFFFFFu;
    vmm_mm_used = 1;
    vmm_cur_mm  = &vmm_mm_pool[0];

    /* 7) ASLR 熵源与回收框架 */
    vmm_entropy_state ^= vmm_random();
    vmm_reclaim_init();
}

/* --------------------------------------------------------------------------
 * 统计与转储（S19）
 * ------------------------------------------------------------------------ */
void vmm_stats(vmm_stats_t *out)
{
    u32 i;
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->enabled   = (u32)vmm_paging_on;
    out->mm_count  = vmm_mm_used;
    out->vma_count = vmm_vma_used;
    out->rmap_count = vmm_rmap_n;
    out->huge_count = vmm_huge_used;
    out->pt_pages_total = vmm_pt_peak;
    out->huge_bytes = vmm_huge_used * VMM_HUGE_SIZE;
    out->aslr_on = vmm_aslr_on;
    out->entropy = vmm_entropy_state;
    out->kernel_pgd_shared = vmm_kernel_pgd_shared();

    for (i = 0; i < VMM_MAX_PGCACHE; i++) {
        if (!vmm_pgcache_pool[i].used) continue;
        out->pgcache_count++;
        if (vmm_pgcache_pool[i].dirty) out->pgcache_dirty++;
        out->pgcache_writeback += vmm_pgcache_pool[i].writeback_count;
    }
    /* 累计事件一律取系统级计数：存活地址空间的私有计数会在
     * vmm_mm_destroy 后消失，用它会让统计口径随对象生命周期漂移
     * （销毁一个 mm 就少算一段历史）。 */
    out->fault_total    = vmm_fault_count();
    out->cow_total      = vmm_cow_total_n;
    out->grow_total     = vmm_grow_total_n;
    out->swap_out_total = vmm_swap_out_count();
    out->swap_in_total  = vmm_swap_in_count();
    out->fault_resolved = vmm_fault_resolved_count();
    out->fault_refused  = vmm_fault_refused_count();
    out->swap_total     = vmm_swap_total_slots();
    out->swap_free      = vmm_swap_free_slots();
    out->tlb_flush_total = vmm_tlb_flush_count;
    out->shootdown_total = vmm_shootdown_count;
    out->shootdown_pending = (vmm_cur_mm && vmm_cur_mm->tlb_gen < vmm_tlb_gen)
                           ? (vmm_tlb_gen - vmm_cur_mm->tlb_gen) : 0;
}

void vmm_mm_dump(vmm_mm_t *mm)
{
    if (!mm) return;
    con_puts("    mm#"); con_put_dec(mm->id);
    con_puts(" pgd "); con_put_hex32(mm->pgd_phys);
    con_puts("  pgd_used "); con_put_dec(vmm_pgd_count(mm));
    con_puts("  pt "); con_put_dec(mm->pt_pages);
    con_puts("  vm "); con_put_dec(mm->total_vm);
    con_puts(" (anon "); con_put_dec(mm->anon_vm);
    con_puts(" file "); con_put_dec(mm->file_vm);
    con_puts(")  faults "); con_put_dec(mm->fault_count);
    con_puts("  cow "); con_put_dec(mm->cow_count);
    con_puts("  grow "); con_put_dec(mm->grow_count);
    con_putc('\n');
}

void vmm_dump(void)
{
    u32 i;
    u32 mmc = vmm_mm_used;
    u32 vc  = vmm_vma_used;
    u32 rc  = vmm_rmap_n;
    u32 hc  = vmm_huge_used;
    u32 pc  = 0, pd = 0, pw = 0;
    for (i = 0; i < VMM_MAX_PGCACHE; i++) {
        if (!vmm_pgcache_pool[i].used) continue;
        pc++;
        if (vmm_pgcache_pool[i].dirty) pd++;
        pw += vmm_pgcache_pool[i].writeback_count;
    }

    con_puts("  Paging               : ");
    con_puts(vmm_paging_on ? "enabled" : "disabled");
    con_puts("   PSE 4MB pages ");
    con_puts(vmm_pse_ok ? "available" : "unavailable");
    con_putc('\n');

    con_puts("  Kernel/user split    : ");
    con_put_hex32(KERNEL_SPACE_BASE);
    con_puts("   shared PGD entries ");
    con_put_dec(vmm_kernel_pgd_shared());
    con_putc('\n');

    con_puts("  Address spaces       : ");
    con_put_dec(mmc);
    con_puts("   current mm#");
    con_put_dec(vmm_current_mm_id());
    con_putc('\n');

    con_puts("  VMAs / rmap entries  : ");
    con_put_dec(vc);
    con_puts(" / ");
    con_put_dec(rc);
    con_putc('\n');

    con_puts("  Page table pages     : ");
    con_put_dec(vmm_pt_peak);
    con_puts(" peak   huge pages ");
    con_put_dec(hc);
    con_puts(" (");
    con_put_dec(hc * 4u);
    con_puts(" MB)\n");

    con_puts("  Page cache           : ");
    con_put_dec(pc);
    con_puts(" pages   dirty ");
    con_put_dec(pd);
    con_puts("   writeback ");
    con_put_dec(pw);
    con_putc('\n');

    con_puts("  Swap slots           : ");
    con_put_dec(vmm_swap_total_slots() - vmm_swap_free_slots());
    con_puts(" / ");
    con_put_dec(vmm_swap_total_slots());
    con_puts("   out ");
    con_put_dec(vmm_swap_out_count());
    con_puts("  in ");
    con_put_dec(vmm_swap_in_count());
    con_putc('\n');

    con_puts("  Faults               : ");
    con_put_dec(vmm_fault_count());
    con_puts("   resolved ");
    con_put_dec(vmm_fault_resolved_count());
    con_puts("   refused ");
    con_put_dec(vmm_fault_refused_count());
    con_puts("   COW ");
    con_put_dec(vmm_cow_count());
    con_putc('\n');

    con_puts("  TLB                  : flush ");
    con_put_dec(vmm_tlb_flush_total());
    con_puts("   shootdown ");
    con_put_dec(vmm_tlb_shootdown_total());
    con_putc('\n');

    con_puts("  ASLR                 : ");
    con_puts(vmm_aslr_get() ? "on" : "off");
    con_puts("   entropy ");
    con_put_hex32(vmm_aslr_entropy());
    con_putc('\n');

    for (i = 0; i < mmc && i < 3; i++) {
        if (vmm_mm_pool[i].used) vmm_mm_dump(&vmm_mm_pool[i]);
    }
}


/* 05 册第二轮：vmm_mprotect 并发保护包装（内部互调走 _locked 变体） */
int vmm_mprotect(vmm_mm_t *mm, u32 addr, u32 len, u32 prot)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_mprotect_locked(mm, addr, len, prot);
    vmm_lock_exit(eflags);
    return rc;
}