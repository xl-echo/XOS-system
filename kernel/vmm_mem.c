/* ============================================================================
 * XOS 虚拟内存子系统 —— 区域管理、缺页处理、写时复制与页换出
 *
 * 覆盖子域：S05 缺页异常处理、S06 写时复制 COW、S07 mmap 映射区域管理、
 *           S08 匿名页与文件页、S09 页换出与换入、S15 栈自动增长
 *
 * 设计要点
 *   1. 缺页处理是「可恢复」的：isr_stubs.S 在调用 C 处理函数后执行 iret，
 *      因此只要本模块解决掉故障原因并返回 VMM_OK，处理器就会重新执行
 *      出错指令并成功 —— 这是按需分页、COW、栈增长能够真正工作的前提。
 *   2. 防死循环：同一 (故障地址, 故障指令) 连续重复到达即判定修复未生效，
 *      立即转为拒绝并交由上层安全停机，绝不进入无限缺页风暴。
 *   3. 页换出的后备存储目前是 RAM 交换区（第 11 册文件系统为 0%，
 *      尚无块设备读写通路）；换出/换入的槽位管理、页表项编码、
 *      脏页回写时机与真实磁盘交换完全一致，接入块设备后只需替换
 *      vmm_swap_write_slot / vmm_swap_read_slot 两个函数体。
 * ============================================================================ */
#include "vmm.h"
#include "vmm_internal.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * 交换区：按需逐页分配，槽位号写入页表项 PFN 字段
 * ------------------------------------------------------------------------ */
static u32 vmm_swap_page[VMM_SWAP_SLOTS];       /* 槽位 → 物理页 */
static u32 vmm_swap_used = 0;
static u32 vmm_swap_out_n = 0;
static u32 vmm_swap_in_n  = 0;

u32 vmm_swap_total_slots(void) { return VMM_SWAP_SLOTS; }
u32 vmm_swap_used_slots(void)  { return vmm_swap_used; }
u32 vmm_swap_free_slots(void)  { return VMM_SWAP_SLOTS - vmm_swap_used; }
u32 vmm_swap_out_count(void)   { return vmm_swap_out_n; }
u32 vmm_swap_in_count(void)    { return vmm_swap_in_n; }

int vmm_swap_alloc_slot(void)
{
    u32 i;
    for (i = 0; i < VMM_SWAP_SLOTS; i++) {
        if (vmm_swap_page[i] == 0) {
            u32 phys = pmm_alloc_page_zeroed();
            if (!phys) return -1;
            vmm_swap_page[i] = phys;
            vmm_swap_used++;
            return (int)i;
        }
    }
    return -1;
}

void vmm_swap_free_slot(u32 slot)
{
    if (slot >= VMM_SWAP_SLOTS) return;
    if (!vmm_swap_page[slot]) return;
    pmm_free_page(vmm_swap_page[slot]);
    vmm_swap_page[slot] = 0;
    if (vmm_swap_used) vmm_swap_used--;
}

/* 把一页内容写入交换槽 / 从交换槽读回（后备存储抽象点） */
static int vmm_swap_write_slot(u32 slot, u32 phys)
{
    if (slot >= VMM_SWAP_SLOTS || !vmm_swap_page[slot]) return VMM_EINVAL;
    memcpy((void *)vmm_swap_page[slot], (const void *)phys, PTE_SIZE);
    return VMM_OK;
}

static int vmm_swap_read_slot(u32 slot, u32 phys)
{
    if (slot >= VMM_SWAP_SLOTS || !vmm_swap_page[slot]) return VMM_EINVAL;
    memcpy((void *)phys, (const void *)vmm_swap_page[slot], PTE_SIZE);
    return VMM_OK;
}

int _vmm_swap_out_locked(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte;
    u32 phys, flags, slot;
    int s;

    if (!mm) return VMM_EINVAL;
    pte = vmm_walk(mm, vaddr, 0);
    if (!pte || !(*pte & PTE_P)) return VMM_ENOENT;
    if (*pte & PTE_SW_SWAP) return VMM_EEXIST;
    if (*pte & PTE_SW_LOCK) return VMM_EBUSY;      /* 锁定页不可换出 */

    phys = *pte & PTE_PFN_MASK;
    flags = *pte & ~PTE_PFN_MASK;

    s = vmm_swap_alloc_slot();
    if (s < 0) return VMM_ENOSPC;
    slot = (u32)s;

    if (vmm_swap_write_slot(slot, phys) != VMM_OK) {
        vmm_swap_free_slot(slot);
        return VMM_EINVAL;
    }

    /* 页表项改为「换出」编码：槽号存在 PFN 字段，P 位必须清零。
     * P 位若保留，处理器会认为该页仍在物理内存中，直接把槽号当物理页号
     * 使用（槽 0 即物理地址 0），既不产生缺页、也就永远不会走换入路径，
     * 且读写会落到无关物理页上 —— 这是必须避免的越界访问。 */
    *pte = (slot << PTE_SHIFT) | PTE_SW_SWAP | (flags & (PTE_US | PTE_SW_LOCK));
    vmm_flush_tlb_page(vaddr);
    vmm_rmap_remove(phys, mm->id, vaddr);
    vmm_put_phys(phys);

    mm->swap_used++;
    mm->swap_out++;
    vmm_swap_out_n++;
    return VMM_OK;
}

int _vmm_swap_in_locked(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte;
    u32 slot, phys, flags;
    vmm_vma_t *vma;

    if (!mm) return VMM_EINVAL;
    pte = vmm_walk(mm, vaddr, 0);
    if (!pte) return VMM_ENOENT;
    /* 换出页表项的 P 位为 0（见 vmm_swap_out），因此只以 SW_SWAP 判定 */
    if (!(*pte & PTE_SW_SWAP)) return VMM_ENOENT;

    slot  = (*pte & PTE_PFN_MASK) >> PTE_SHIFT;
    flags = *pte & (PTE_US | PTE_SW_LOCK);

    phys = pmm_alloc_page_zeroed();
    if (!phys) return VMM_ENOMEM;

    if (vmm_swap_read_slot(slot, phys) != VMM_OK) {
        pmm_free_page(phys);
        return VMM_EINVAL;
    }

    vma = vmm_vma_find(mm, vaddr);
    if (vma) flags |= (vma->vm_prot & PROT_WRITE) ? PTE_RW : 0u;
    else      flags |= PTE_RW;

    *pte = (phys & PTE_PFN_MASK) | flags | PTE_P;
    vmm_flush_tlb_page(vaddr);
    vmm_swap_free_slot(slot);
    vmm_rmap_add(phys, mm->id, vaddr);

    if (mm->swap_used) mm->swap_used--;
    mm->swap_in++;
    vmm_swap_in_n++;
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * VMA 区域管理（S07）
 * ------------------------------------------------------------------------ */
u32 vmm_vma_count(vmm_mm_t *mm)
{
    u32 i, n = 0;
    if (!mm) return 0;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        if (vmm_vma_pool[i].vm_used && vmm_vma_pool[i].vm_owner == mm->id) n++;
    }
    return n;
}

vmm_vma_t *vmm_vma_at(vmm_mm_t *mm, u32 idx)
{
    u32 i, n = 0;
    if (!mm) return NULL;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        if (!vmm_vma_pool[i].vm_used || vmm_vma_pool[i].vm_owner != mm->id) continue;
        if (n == idx) return &vmm_vma_pool[i];
        n++;
    }
    return NULL;
}

vmm_vma_t *vmm_vma_find(vmm_mm_t *mm, u32 vaddr)
{
    u32 i;
    if (!mm) return NULL;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        vmm_vma_t *v = &vmm_vma_pool[i];
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        if (vaddr >= v->vm_start && vaddr < v->vm_end) return v;
    }
    return NULL;
}

static int vmm_vma_overlap(vmm_mm_t *mm, u32 start, u32 end)
{
    u32 i;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        vmm_vma_t *v = &vmm_vma_pool[i];
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        if (start < v->vm_end && end > v->vm_start) return 1;
    }
    return 0;
}

int vmm_vma_insert(vmm_mm_t *mm, u32 start, u32 end, u32 prot,
                   u32 flags, u32 type, u32 backing, u32 offset)
{
    u32 i;
    vmm_vma_t *v;

    if (!mm || end <= start) return VMM_EINVAL;
    if (start < USER_MIN_MAP_ADDR) return VMM_EINVAL;   /* 不得落入内核恒等映射区 */
    if (end > USER_SPACE_TOP) return VMM_EINVAL;
    if (vmm_vma_overlap(mm, start, end)) return VMM_EEXIST;

    for (i = 0; i < VMM_MAX_VMA; i++) {
        if (!vmm_vma_pool[i].vm_used) break;
    }
    if (i >= VMM_MAX_VMA) return VMM_ENOSPC;

    v = &vmm_vma_pool[i];
    memset(v, 0, sizeof(*v));
    v->vm_start   = start;
    v->vm_end     = end;
    v->vm_prot    = prot;
    v->vm_flags   = flags;
    v->vm_type    = type;
    v->vm_backing = backing;
    v->vm_offset  = offset;
    v->vm_owner   = mm->id;
    v->vm_used    = 1;
    vmm_vma_used++;
    return VMM_OK;
}

int vmm_vma_remove(vmm_mm_t *mm, u32 start, u32 end)
{
    u32 i;
    if (!mm || end <= start) return VMM_EINVAL;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        vmm_vma_t *v = &vmm_vma_pool[i];
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        if (end <= v->vm_start || start >= v->vm_end) continue;

        if (start <= v->vm_start && end >= v->vm_end) {
            v->vm_used = 0;
            vmm_vma_used--;
        } else if (start <= v->vm_start) {
            v->vm_start = end;
        } else if (end >= v->vm_end) {
            v->vm_end = start;
        } else {
            /* 中间挖洞：拆成两段 */
            u32 os = v->vm_start, oe = v->vm_end, ooff = v->vm_offset;
            u32 rc;
            v->vm_end = start;
            rc = vmm_vma_insert(mm, end, oe, v->vm_prot, v->vm_flags,
                                v->vm_type, v->vm_backing,
                                ooff + (end - os));
            if (rc != VMM_OK) return rc;
        }
    }
    return VMM_OK;
}

int vmm_vma_split(vmm_mm_t *mm, u32 addr)
{
    u32 i;
    if (!mm) return VMM_EINVAL;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        vmm_vma_t *v = &vmm_vma_pool[i];
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        if (addr <= v->vm_start || addr >= v->vm_end) continue;
        {
            u32 oe = v->vm_end, ooff = v->vm_offset, rc;
            v->vm_end = addr;
            rc = vmm_vma_insert(mm, addr, oe, v->vm_prot, v->vm_flags,
                                v->vm_type, v->vm_backing,
                                ooff + (addr - v->vm_start));
            if (rc != VMM_OK) { v->vm_end = oe; return rc; }
        }
    }
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * mmap / munmap / populate / brk / 栈增长
 * ------------------------------------------------------------------------ */
u32 _vmm_mmap_locked(vmm_mm_t *mm, u32 addr, u32 len, u32 prot, u32 flags,
             u32 backing, u32 offset)
{
    u32 start, end, type, tries;
    int rc;

    if (!mm || len == 0) return 0;
    len = PAGE_ALIGN_UP(len);

    if (flags & MAP_FIXED) {
        if (addr & PTE_MASK) return 0;
        start = addr;
    } else {
        start = addr ? PAGE_ALIGN_UP(addr) : PAGE_ALIGN_UP(mm->mmap_base);
        if (start < USER_MIN_MAP_ADDR) start = USER_MIN_MAP_ADDR;
        /* 首次适配：与已有区域重叠就上移到该区域末尾再试 */
        for (tries = 0; tries < VMM_MAX_VMA + 2u; tries++) {
            u32 i, moved = 0;
            for (i = 0; i < VMM_MAX_VMA; i++) {
                vmm_vma_t *v = &vmm_vma_pool[i];
                if (!v->vm_used || v->vm_owner != mm->id) continue;
                if (start < v->vm_end && start + len > v->vm_start) {
                    start = PAGE_ALIGN_UP(v->vm_end);
                    moved = 1;
                }
            }
            if (!moved) break;
        }
    }

    end = start + len;
    if (end <= start) return 0;
    if (end > USER_SPACE_TOP) return 0;

    type = (flags & MAP_ANONYMOUS) ? VMA_TYPE_ANON : VMA_TYPE_FILE;
    if (type == VMA_TYPE_FILE && backing == 0) return 0;

    rc = vmm_vma_insert(mm, start, end, prot, flags, type, backing, offset);
    if (rc != VMM_OK) return 0;

    if (!(flags & MAP_FIXED)) mm->mmap_base = end;

    if (flags & MAP_POPULATE) {
        _vmm_populate_locked(mm, start, len);
    }
    return start;
}

int _vmm_munmap_locked(vmm_mm_t *mm, u32 addr, u32 len)
{
    u32 a, end;
    int rc;

    if (!mm || len == 0) return VMM_EINVAL;
    if (addr & PTE_MASK) return VMM_EINVAL;

    end = addr + PAGE_ALIGN_UP(len);

    /* 1) 释放该区间的所有页 */
    for (a = addr; a < end; a += PTE_SIZE) {
        if (vmm_pte_present(mm, a) || vmm_pte_swapped(mm, a)) {
            vmm_unmap_page_put(mm, a);
            if (mm->total_vm) mm->total_vm--;
        }
    }

    /* 2) 收缩 VMA */
    rc = vmm_vma_remove(mm, addr, end);
    if (rc != VMM_OK) return rc;

    if (mm->stack_vma != 0xFFFFFFFFu) {
        vmm_vma_t *sv = &vmm_vma_pool[mm->stack_vma];
        if (sv->vm_used && sv->vm_owner == mm->id) mm->stack_start = sv->vm_start;
    }
    return VMM_OK;
}

int _vmm_populate_locked(vmm_mm_t *mm, u32 addr, u32 len)
{
    u32 a, end;
    if (!mm) return VMM_EINVAL;
    end = addr + PAGE_ALIGN_UP(len);
    for (a = PAGE_ALIGN_DOWN(addr); a < end; a += PTE_SIZE) {
        vmm_vma_t *vma = vmm_vma_find(mm, a);
        if (!vma) return VMM_EFAULT;
        if (vmm_pte_present(mm, a)) continue;
        if (vmm_demand_page(mm, vma, a,
                vmm_prot_to_flags(vma->vm_prot, mm->user_access)) != VMM_OK) {
            return VMM_ENOMEM;
        }
    }
    return VMM_OK;
}

u32 _vmm_brk_locked(vmm_mm_t *mm, u32 newbrk)
{
    u32 i, heap_idx = 0xFFFFFFFFu;

    if (!mm) return 0;
    newbrk = PAGE_ALIGN_UP(newbrk);
    if (newbrk >= mm->mmap_base) return mm->brk;

    for (i = 0; i < VMM_MAX_VMA; i++) {
        vmm_vma_t *v = &vmm_vma_pool[i];
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        if (v->vm_type == VMA_TYPE_ANON && v->vm_start == USER_HEAP_BASE) {
            heap_idx = i;
            break;
        }
    }
    if (heap_idx == 0xFFFFFFFFu) return mm->brk;

    if (newbrk > vmm_vma_pool[heap_idx].vm_end) {
        if (vmm_vma_overlap(mm, vmm_vma_pool[heap_idx].vm_end, newbrk)) return mm->brk;
        vmm_vma_pool[heap_idx].vm_end = newbrk;
    } else if (newbrk < vmm_vma_pool[heap_idx].vm_end) {
        u32 a;
        for (a = newbrk; a < vmm_vma_pool[heap_idx].vm_end; a += PTE_SIZE) {
            if (vmm_pte_present(mm, a) || vmm_pte_swapped(mm, a)) {
                vmm_unmap_page_put(mm, a);
                if (mm->total_vm) mm->total_vm--;
            }
        }
        vmm_vma_pool[heap_idx].vm_end = newbrk;
    }
    mm->brk = newbrk;
    return mm->brk;
}

int _vmm_expand_stack_locked(vmm_mm_t *mm, u32 vaddr)
{
    vmm_vma_t *vma;
    u32 newstart;

    if (!mm) return VMM_EINVAL;
    if (mm->stack_vma == 0xFFFFFFFFu) return VMM_EFAULT;

    vma = &vmm_vma_pool[mm->stack_vma];
    if (!vma->vm_used || vma->vm_owner != mm->id) return VMM_EFAULT;
    if (!(vma->vm_flags & MAP_GROWSDOWN)) return VMM_EFAULT;

    /* 故障地址必须在栈区下方，且不超过保护间隙（防止越界静默扩张） */
    if (vaddr >= vma->vm_start) return VMM_EFAULT;
    if (vaddr + VMM_STACK_GAP < vma->vm_start) return VMM_EFAULT;
    if (mm->stack_top - vaddr > VMM_STACK_MAX) return VMM_EFAULT;
    if (vaddr < USER_MIN_MAP_ADDR) return VMM_EFAULT;
    /* 05 册第二轮：增长次数与单次步长硬上限（防栈无限扩张/单次巨量增长） */
    if (mm->grow_count >= VMM_STACK_GROW_MAX) return VMM_ELIMIT;
    if (vma->vm_start - PAGE_ALIGN_DOWN(vaddr) > VMM_STACK_GROW_STEP) return VMM_ELIMIT;

    newstart = PAGE_ALIGN_DOWN(vaddr);
    if (vmm_vma_overlap(mm, newstart, vma->vm_start)) return VMM_EFAULT;

    vma->vm_start = newstart;
    mm->stack_start = newstart;
    mm->grow_count++;
    vmm_grow_total_n++;

    return vmm_demand_page(mm, vma, vaddr,
                           vmm_prot_to_flags(vma->vm_prot, mm->user_access));
}

int vmm_setup_layout(vmm_mm_t *mm)
{
    u32 sz, rc;

    if (!mm) return VMM_EINVAL;

    mm->mmap_base  = vmm_aslr_mmap_base();
    mm->brk        = USER_HEAP_BASE + PTE_SIZE;
    mm->stack_top  = vmm_aslr_stack_top();
    sz             = 0x00010000u;                 /* 初始栈 64 KB */
    mm->stack_start = mm->stack_top - sz;
    mm->stack_vma  = 0xFFFFFFFFu;

    /* 代码段 / 数据段 / 堆 / 栈：建立基础区域。
     * 注意全部位于 0x08000000 之上 —— 低端 128MB 是内核恒等映射区，
     * 任何用户区域都不得落入其中。 */
    vmm_vma_insert(mm, USER_TEXT_BASE, USER_TEXT_BASE + 0x00010000u,
                   PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                   VMA_TYPE_ANON, 0, 0);
    vmm_vma_insert(mm, USER_DATA_BASE, USER_DATA_BASE + 0x00010000u,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   VMA_TYPE_ANON, 0, 0);
    vmm_vma_insert(mm, USER_HEAP_BASE, mm->brk,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   VMA_TYPE_ANON, 0, 0);

    rc = vmm_vma_insert(mm, mm->stack_start, mm->stack_top,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN,
                        VMA_TYPE_STACK, 0, 0);
    if (rc == VMM_OK) {
        u32 i;
        for (i = 0; i < VMM_MAX_VMA; i++) {
            vmm_vma_t *v = &vmm_vma_pool[i];
            if (v->vm_used && v->vm_owner == mm->id &&
                v->vm_start == mm->stack_start) {
                mm->stack_vma = i;
                break;
            }
        }
    }
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * 按需分页与写时复制（S05 / S06 / S08）
 * ------------------------------------------------------------------------ */
u32 vmm_demand_page(vmm_mm_t *mm, vmm_vma_t *vma, u32 vaddr, u32 flags)
{
    u32 phys, rc;

    if (!mm || !vma) return VMM_EINVAL;

    if (vma->vm_type == VMA_TYPE_FILE) {
        u32 off = vma->vm_offset + (PAGE_ALIGN_DOWN(vaddr) - vma->vm_start);
        phys = _vmm_pgcache_get_locked(vma->vm_backing, off);
        if (!phys) return VMM_ENOMEM;
        mm->file_vm++;
    } else {
        phys = pmm_alloc_page_zeroed();
        if (!phys) return VMM_ENOMEM;
        mm->anon_vm++;
    }

    rc = vmm_map_page(mm, PAGE_ALIGN_DOWN(vaddr), phys, flags);
    if (rc != VMM_OK) {
        vmm_put_phys(phys);
        return rc;
    }
    mm->total_vm++;
    return VMM_OK;
}

u32 vmm_break_cow(vmm_mm_t *mm, u32 vaddr)
{
    u32 *pte;
    u32 old, fresh, flags;

    if (!mm) return VMM_EINVAL;
    pte = vmm_walk(mm, vaddr, 0);
    if (!pte || !(*pte & PTE_P)) return VMM_ENOENT;
    if (!(*pte & PTE_SW_COW)) return VMM_EPERM;

    old   = *pte & PTE_PFN_MASK;
    flags = *pte & ~PTE_PFN_MASK;

    fresh = pmm_alloc_page_zeroed();
    if (!fresh) return VMM_ENOMEM;

    /* 复制原页内容到新页，解除共享 */
    memcpy((void *)fresh, (const void *)old, PTE_SIZE);

    *pte = (fresh & PTE_PFN_MASK) | (flags & ~PTE_SW_COW) | PTE_RW | PTE_P;
    vmm_flush_tlb_page(vaddr);

    vmm_rmap_remove(old, mm->id, vaddr);
    vmm_rmap_add(fresh, mm->id, vaddr);
    vmm_put_phys(old);              /* 原页引用计数 -1 */

    mm->cow_count++;
    vmm_cow_total_n++;
    mm->anon_vm++;
    return VMM_OK;
}

/* 建立共享只读映射：同一物理页在两个地址空间中都以 COW 方式映射 */
u32 vmm_cow_share(vmm_mm_t *src, u32 svaddr, vmm_mm_t *dst, u32 dvaddr)
{
    u32 phys, rc;

    if (!src || !dst) return VMM_EINVAL;
    phys = vmm_translate(src, svaddr);
    if (!phys) return VMM_ENOENT;

    if (vmm_get_phys(phys) != VMM_OK) return VMM_EINVAL;

    rc = vmm_map_page(dst, dvaddr, phys,
                      PTE_P | PTE_RW | PTE_US | PTE_SW_COW);
    if (rc != VMM_OK) { vmm_put_phys(phys); return rc; }
    /* 双方都置为只读 + COW，任何一方写入都会触发复制 */
    vmm_pte_set_flags(src, svaddr, PTE_RW | PTE_SW_COW, PTE_SW_COW);
    vmm_pte_set_flags(dst, dvaddr, PTE_RW | PTE_SW_COW, PTE_SW_COW);

    dst->total_vm++;
    dst->anon_vm++;
    return VMM_OK;
}

/* --------------------------------------------------------------------------
 * 缺页日志（诊断）
 * ------------------------------------------------------------------------ */
static vmm_fault_rec_t vmm_fault_log[VMM_MAX_FAULTLOG];
static u32 vmm_fault_log_n = 0;
static u32 vmm_fault_log_head = 0;
static u32 vmm_fault_total_n = 0;
static u32 vmm_fault_resolved_n = 0;
static u32 vmm_fault_refused_n = 0;

u32 vmm_fault_count(void)          { return vmm_fault_total_n; }
u32 vmm_fault_resolved_count(void) { return vmm_fault_resolved_n; }
u32 vmm_fault_refused_count(void)  { return vmm_fault_refused_n; }
u32 vmm_cow_count(void)
{
    u32 i, n = 0;
    for (i = 0; i < VMM_MAX_MM; i++) if (vmm_mm_pool[i].used) n += vmm_mm_pool[i].cow_count;
    return n;
}

void vmm_fault_log_add(u32 vaddr, u32 err, u32 eip, u32 action, u32 result)
{
    vmm_fault_rec_t *r = &vmm_fault_log[vmm_fault_log_head];
    r->seq    = vmm_fault_total_n;
    r->vaddr  = vaddr;
    r->err    = err;
    r->eip    = eip;
    r->action = action;
    r->result = result;
    vmm_fault_log_head = (vmm_fault_log_head + 1u) % VMM_MAX_FAULTLOG;
    if (vmm_fault_log_n < VMM_MAX_FAULTLOG) vmm_fault_log_n++;
}

u32 vmm_fault_log_count(void) { return vmm_fault_log_n; }

const vmm_fault_rec_t *vmm_fault_log_at(u32 idx)
{
    if (idx >= vmm_fault_log_n) return NULL;
    return &vmm_fault_log[idx];
}

const char *vmm_fault_action_name(u32 action)
{
    switch (action) {
    case VMM_FA_DEMAND: return "demand-alloc";
    case VMM_FA_COW:    return "cow-copy";
    case VMM_FA_SWAPIN: return "swap-in";
    case VMM_FA_GROW:   return "stack-grow";
    case VMM_FA_REFUSE: return "refused";
    default:            return "none";
    }
}

/* --------------------------------------------------------------------------
 * 缺页解决核心（S05）
 * 返回 VMM_OK 表示已修复，处理器将重新执行出错指令。
 * ------------------------------------------------------------------------ */
u32 vmm_fault_resolve(vmm_mm_t *mm, u32 vaddr, u32 err, u32 eip)
{
    vmm_vma_t *vma;
    u32 action = VMM_FA_REFUSE;
    u32 rc = VMM_EFAULT;

    if (!mm) return VMM_EFAULT;

    /* 1) 保留位被置位：页表项已损坏，补页解决不了，必须拒绝 */
    if (err & PF_RSVD) {
        mm->fault_count++;
        vmm_fault_log_add(vaddr, err, eip, VMM_FA_REFUSE, VMM_ERESVD);
        return VMM_ERESVD;
    }

    /* 2) 用户态访问内核区：越权，拒绝 */
    if ((err & PF_USER) && vmm_is_kernel_addr(vaddr)) {
        mm->fault_count++;
        vmm_fault_log_add(vaddr, err, eip, VMM_FA_REFUSE, VMM_EPERM);
        return VMM_EPERM;
    }

    /* 3) 内核态访问内核区却缺页：内核映射应始终存在，属真实内核缺陷 */
    if (!(err & PF_USER) && vmm_is_kernel_addr(vaddr)) {
        mm->fault_count++;
        vmm_fault_log_add(vaddr, err, eip, VMM_FA_REFUSE, VMM_EFAULT);
        return VMM_EFAULT;
    }

    mm->fault_count++;
    vma = vmm_vma_find(mm, vaddr);

    if (!vma) {
        /* 4) 无区域覆盖：唯一合法可能是栈向下增长 */
        rc = _vmm_expand_stack_locked(mm, vaddr);
        if (rc == VMM_OK) action = VMM_FA_GROW;
        else              rc = VMM_EFAULT;
    } else if ((err & PF_WRITE) && vmm_pte_present(mm, vaddr)) {
        /* 5) 页存在但不可写：COW 则复制，否则是真实权限违例 */
        if (vmm_pte_cow(mm, vaddr)) {
            rc = vmm_break_cow(mm, vaddr);
            action = VMM_FA_COW;
        } else {
            rc = VMM_EPERM;
        }
    } else if (vmm_pte_swapped(mm, vaddr)) {
        /* 6) 页已换出：换回 */
        rc = _vmm_swap_in_locked(mm, vaddr);
        action = VMM_FA_SWAPIN;
    } else if (vmm_pte_present(mm, vaddr)) {
        /* 7) 页存在仍缺页（用户态访问特权页等） */
        rc = VMM_EPERM;
    } else {
        /* 8) 正常按需分页 */
        if (vaddr < vma->vm_start || vaddr >= vma->vm_end) {
            rc = VMM_EFAULT;
        } else {
            rc = vmm_demand_page(mm, vma, vaddr,
                                 vmm_prot_to_flags(vma->vm_prot, mm->user_access));
            action = VMM_FA_DEMAND;
        }
    }

    if (rc == VMM_OK) {
        vmm_fault_resolved_n++;
    } else {
        vmm_fault_refused_n++;
        vmm_fault_log_add(vaddr, err, eip, VMM_FA_REFUSE, rc);
        return rc;
    }
    vmm_fault_log_add(vaddr, err, eip, action, VMM_OK);
    return VMM_OK;
}

/* 由 kernel/idt.c 的异常处理调用；返回 VMM_OK 表示已修复可继续执行 */
u32 vmm_handle_page_fault(u32 vaddr, u32 err, u32 eip, u32 cs)
{
    static u32 last_vaddr = 0xFFFFFFFFu;
    static u32 last_eip   = 0xFFFFFFFFu;
    static u32 streak     = 0;
    vmm_mm_t *mm;
    u32 rc;

    UNUSED(cs);

    if (!vmm_enabled()) return VMM_EFAULT;

    vmm_fault_total_n++;

    /* 防死循环：同一地址 + 同一指令连续重复到达，说明上一轮修复未生效 */
    if (vaddr == last_vaddr && eip == last_eip) {
        streak++;
        if (streak >= 2u) {
            vmm_fault_refused_n++;
            vmm_fault_log_add(vaddr, err, eip, VMM_FA_REFUSE, VMM_EBUSY);
            return VMM_EBUSY;
        }
    } else {
        streak = 0;
    }
    last_vaddr = vaddr;
    last_eip   = eip;

    mm = vmm_is_user_addr(vaddr) ? vmm_cur_mm : vmm_kernel_mm();
    if (!mm) return VMM_EFAULT;

    rc = vmm_fault_resolve(mm, vaddr, err, eip);
    return rc;
}


/* 05 册第二轮：vmm_mmap 并发保护包装（内部互调走 _locked 变体） */
u32 vmm_mmap(vmm_mm_t *mm, u32 addr, u32 len, u32 prot, u32 flags,
             u32 backing, u32 offset)
{
    u32 rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_mmap_locked(mm, addr, len, prot, flags, backing, offset);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_munmap 并发保护包装（内部互调走 _locked 变体） */
int vmm_munmap(vmm_mm_t *mm, u32 addr, u32 len)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_munmap_locked(mm, addr, len);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_brk 并发保护包装（内部互调走 _locked 变体） */
u32 vmm_brk(vmm_mm_t *mm, u32 newbrk)
{
    u32 rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_brk_locked(mm, newbrk);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_expand_stack 并发保护包装（内部互调走 _locked 变体） */
int vmm_expand_stack(vmm_mm_t *mm, u32 vaddr)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_expand_stack_locked(mm, vaddr);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_populate 并发保护包装（内部互调走 _locked 变体） */
int vmm_populate(vmm_mm_t *mm, u32 addr, u32 len)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_populate_locked(mm, addr, len);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_swap_out 并发保护包装（内部互调走 _locked 变体） */
int vmm_swap_out(vmm_mm_t *mm, u32 vaddr)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_swap_out_locked(mm, vaddr);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_swap_in 并发保护包装（内部互调走 _locked 变体） */
int vmm_swap_in(vmm_mm_t *mm, u32 vaddr)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_swap_in_locked(mm, vaddr);
    vmm_lock_exit(eflags);
    return rc;
}
/* --------------------------------------------------------------------------
 * 换页调度器（05 册第二轮）
 * -------------------------------------------------------------------------- */
static u32 vmm_swap_policy_cur = VMM_SWAP_POLICY_CLOCK;
static u32 vmm_clock_hand_n    = 0;      /* 时钟指针（VMA 池游标） */
static u32 vmm_clock_scans_n   = 0;
static u32 vmm_clock_reclaim_n = 0;

u32 vmm_swap_policy(void)            { return vmm_swap_policy_cur; }
void vmm_swap_policy_set(u32 policy) { vmm_swap_policy_cur = (policy == VMM_SWAP_POLICY_FIFO) ? VMM_SWAP_POLICY_FIFO : VMM_SWAP_POLICY_CLOCK; }
u32 vmm_swap_clock_scans(void)       { return vmm_clock_scans_n; }
u32 vmm_swap_clock_reclaimed(void)   { return vmm_clock_reclaim_n; }
u32 vmm_swap_clock_hand(void)        { return vmm_clock_hand_n; }

/* 页面错误率（×1000）：缺页数 / (缺页数 + 时钟扫描页数 × 16 采样当量) */
u32 vmm_page_fault_rate_x1000(void)
{
    u64 f = vmm_fault_count(), s = (u64)vmm_clock_scans_n * 16u + f;
    if (s == 0) return 0;
    return (u32)((f * 1000u) / s);
}

/* 时钟置换候选选择：遍历本地址空间的 VMA 池（环形），
 * 对每个已映射 VMA 的首页检查 PTE 访问位：
 *   - A=1：清访问位（第二次机会）并前进；
 *   - A=0：候选页；脏位经 out_dirty 上报（换出前需回写）。
 * 返回 VMM_OK 且 *out_vaddr 有效；无候选返回 VMM_ESWAP。 */
int vmm_swap_select_clock(vmm_mm_t *mm, u32 *out_vaddr, u32 *out_dirty)
{
    u32 i, start;
    if (!mm || !out_vaddr) return VMM_EINVAL;

    vmm_clock_scans_n++;
    start = vmm_clock_hand_n % VMM_MAX_VMA;
    for (i = 0; i < VMM_MAX_VMA; i++) {
        u32 idx = (start + i) % VMM_MAX_VMA;
        vmm_vma_t *v = &vmm_vma_pool[idx];
        u32 pte;
        if (!v->vm_used || v->vm_owner != mm->id) continue;
        if (v->vm_flags & MAP_LOCKED) continue;
        pte = vmm_pte_get(mm, v->vm_start);
        if (!(pte & PTE_P)) continue;          /* 已换出或未映射，跳过 */
        if (pte & PTE_A) {                     /* 访问位置位：第二次机会 */
            vmm_clear_accessed(mm, v->vm_start);
            continue;
        }
        vmm_clock_hand_n = (idx + 1u) % VMM_MAX_VMA;
        if (out_dirty) *out_dirty = (pte & PTE_D) ? 1 : 0;
        *out_vaddr = v->vm_start;
        return VMM_OK;
    }
    return VMM_ESWAP;                          /* 全被二次机会或无可换页 */
}

/* 页面置换：FIFO 直接换出目标页并换入；CLOCK 先选候选再换出换入。
 * 换出前若候选脏位被置位，由 swap_out 的写回路径负责落盘。 */
int vmm_page_replace(vmm_mm_t *mm, u32 vaddr)
{
    int rc;
    u32 target = vaddr;
    if (!mm) return VMM_EINVAL;
    if (vmm_swap_policy_cur == VMM_SWAP_POLICY_CLOCK) {
        u32 dirty = 0;
        rc = vmm_swap_select_clock(mm, &target, &dirty);
        if (rc != VMM_OK) return rc;
    }
    rc = vmm_swap_out(mm, target);
    if (rc != VMM_OK) return rc;
    rc = vmm_swap_in(mm, target);
    if (rc == VMM_OK) vmm_clock_reclaim_n++;
    return rc;
}
