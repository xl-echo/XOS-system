/* ============================================================================
 * XOS 虚拟内存子系统 —— 页缓存、反向映射与内存压力回收
 *
 * 覆盖子域：S17 反向映射 rmap、S18 页缓存与回写、S20 内存压力与回收
 *
 * 设计要点
 *   1. 反向映射回答「这一物理页被哪些地址空间、哪些虚拟地址映射着」，
 *      是写时复制、页换出与共享页释放的前提。当前以定长表实现，
 *      查找 O(n)；条目上限即同时可共享的映射数，超出返回 ENOSPC。
 *   2. 页缓存以 (后备存储, 页偏移) 为键，脏页经后备存储的 write 回调回写。
 *      本阶段后备存储是 RAM 桩（第 11 册文件系统 0%，无块设备通路），
 *      键结构、脏位、回写时机与真实文件页缓存一致，接入块设备后
 *      只需注册真实读写回调，页缓存本体不动。
 *   3. 回收向 PMM 注册为 shrinker，遵循 PMM 的回收框架：
 *      先回收干净页缓存（无代价），再换出匿名页（有代价），
 *      两步都失败才由 PMM 走 OOM 通报。
 * ============================================================================ */
#include "vmm.h"
#include "vmm_internal.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * 页缓存索引树（05 册第二轮）
 * key = 页偏移 offset>>12；L1 = key>>6、L2 = key&63。
 * 树是性能索引（find 加速），权威数据仍在 vmm_pgcache_pool：
 * find 命中后校验 backing/used，冲突或陈旧时线性兜底。
 * L2 必须为 2 的幂（&63 掩码）；L1 收窄仅丢弃高桶（安全 miss）。
 * ------------------------------------------------------------------------ */
#define VMM_CACHE_L1_SIZE   32u
#define VMM_CACHE_L2_SIZE   64u
static u16 vmm_cache_l1[VMM_CACHE_L1_SIZE];                          /* 0xFFFF=空 */
static u16 vmm_cache_l2[VMM_CACHE_L1_SIZE][VMM_CACHE_L2_SIZE];       /* 0xFFFF=空 */

static void vmm_cache_tree_init(void)
{
    memset(vmm_cache_l1, 0xFF, sizeof(vmm_cache_l1));
    memset(vmm_cache_l2, 0xFF, sizeof(vmm_cache_l2));
}

static u32 vmm_cache_key(u32 offset)
{
    return offset >> PTE_SHIFT;
}

static void vmm_cache_tree_insert(u32 offset, u32 pool_idx)
{
    u32 key = vmm_cache_key(offset);
    u32 l1 = key >> 6u, l2i = key & 63u;
    if (l1 >= VMM_CACHE_L1_SIZE) return;
    if (vmm_cache_l2[l1][l2i] == 0xFFFFu) vmm_cache_l2[l1][l2i] = (u16)pool_idx;
}

static void vmm_cache_tree_remove(u32 offset)
{
    u32 key = vmm_cache_key(offset);
    u32 l1 = key >> 6u, l2i = key & 63u;
    if (l1 >= VMM_CACHE_L1_SIZE) return;
    vmm_cache_l2[l1][l2i] = 0xFFFFu;
}

/* 树查找：命中且 backing 匹配、条目在用 → 返回池索引；否则 -1 */
static int vmm_cache_tree_find(u32 offset, u32 backing)
{
    u32 key = vmm_cache_key(offset);
    u32 l1 = key >> 6u, l2i = key & 63u;
    u16 idx;
    if (l1 >= VMM_CACHE_L1_SIZE) return -1;
    idx = vmm_cache_l2[l1][l2i];
    if (idx == 0xFFFFu || idx >= VMM_MAX_PGCACHE) return -1;
    if (!vmm_pgcache_pool[idx].used) return -1;
    if (vmm_pgcache_pool[idx].backing != backing) return -1;
    if (vmm_pgcache_pool[idx].offset != offset) return -1;
    return (int)idx;
}

/* --------------------------------------------------------------------------
 * 后备存储（S18）
 * ------------------------------------------------------------------------ */
#define VMM_RAMSTORE_PAGES  64

static u32 vmm_ramstore_page[VMM_RAMSTORE_PAGES];

/* RAM 后备存储桩：读写落在按需分配的物理页上。
 * 它模拟的是「一块可随机读写、以页为单位的持久介质」，
 * 因此页缓存的读入 / 回写路径与真实块设备完全同构。 */
static int vmm_ramstore_rw(u32 offset, void *buf, u32 write)
{
    u32 page, off, phys;

    if (offset >= VMM_RAMSTORE_PAGES * PTE_SIZE) return VMM_EINVAL;
    page = offset >> PTE_SHIFT;
    off  = offset & PTE_MASK;

    if (!vmm_ramstore_page[page]) {
        if (!write) {                       /* 读未写过的区域：视为全零 */
            memset(buf, 0, PTE_SIZE);
            return VMM_OK;
        }
        phys = pmm_alloc_page_zeroed();
        if (!phys) return VMM_ENOMEM;
        vmm_ramstore_page[page] = phys;
    }
    phys = vmm_ramstore_page[page];

    if (write) memcpy((void *)(phys + off), buf, PTE_SIZE);
    else       memcpy(buf, (const void *)(phys + off), PTE_SIZE);
    return VMM_OK;
}

static int vmm_ramstore_read(u32 offset, void *buf, u32 write)
{
    if (write) return VMM_EINVAL;
    return vmm_ramstore_rw(offset, buf, 0);
}

static int vmm_ramstore_write(u32 offset, void *buf, u32 write)
{
    if (!write) return VMM_EINVAL;
    return vmm_ramstore_rw(offset, buf, 1);
}

int vmm_backing_register(const char *name, u32 size,
                         vmm_backing_rw_fn rd, vmm_backing_rw_fn wr)
{
    u32 i;
    if (!name || !rd || !wr || size == 0) return VMM_EINVAL;
    for (i = 0; i < VMM_MAX_BACKING; i++) {
        if (!vmm_backing_pool[i].used) break;
    }
    if (i >= VMM_MAX_BACKING) return VMM_ENOSPC;

    vmm_backing_pool[i].id    = i + 1u;
    vmm_backing_pool[i].name  = name;
    vmm_backing_pool[i].size  = size;
    vmm_backing_pool[i].read  = rd;
    vmm_backing_pool[i].write = wr;
    vmm_backing_pool[i].used  = 1;
    vmm_backing_used++;
    return (int)(i + 1u);
}

u32 vmm_backing_count(void) { return vmm_backing_used; }

const vmm_backing_t *vmm_backing_at(u32 id)
{
    u32 i;
    for (i = 0; i < VMM_MAX_BACKING; i++) {
        if (vmm_backing_pool[i].used && vmm_backing_pool[i].id == id) {
            return &vmm_backing_pool[i];
        }
    }
    return NULL;
}

u32 vmm_backing_read_bytes(u32 id)
{
    const vmm_backing_t *b = vmm_backing_at(id);
    return b ? b->read_count : 0;
}

u32 vmm_backing_write_bytes(u32 id)
{
    const vmm_backing_t *b = vmm_backing_at(id);
    return b ? b->write_count : 0;
}

/* 初始化 RAM 后备存储（供文件页与页缓存自检使用） */
static void vmm_backing_init(void)
{
    vmm_backing_register("ram0", VMM_RAMSTORE_PAGES * PTE_SIZE,
                         vmm_ramstore_read, vmm_ramstore_write);
}

/* --------------------------------------------------------------------------
 * 页缓存（S18）
 * ------------------------------------------------------------------------ */
static vmm_pgcache_t *vmm_pgcache_find(u32 backing, u32 offset)
{
    int ti = vmm_cache_tree_find(offset, backing);
    u32 i;
    if (ti >= 0) return &vmm_pgcache_pool[ti];
    for (i = 0; i < VMM_MAX_PGCACHE; i++) {
        vmm_pgcache_t *c = &vmm_pgcache_pool[i];
        if (c->used && c->backing == backing && c->offset == offset) return c;
    }
    return NULL;
}

/* 可写的后备存储查询：读计数 / 写计数需就地累加，
 * 对外只暴露 const 版本（vmm_backing_at），内部另用本函数。 */
static vmm_backing_t *vmm_backing_mut(u32 id)
{
    u32 i;
    for (i = 0; i < VMM_MAX_BACKING; i++) {
        if (vmm_backing_pool[i].used && vmm_backing_pool[i].id == id) {
            return &vmm_backing_pool[i];
        }
    }
    return NULL;
}

u32 _vmm_pgcache_get_locked(u32 backing, u32 offset)
{
    vmm_pgcache_t *c;
    vmm_backing_t *b;
    u32 i, phys;

    b = vmm_backing_mut(backing);
    if (!b) return 0;
    if (offset + PTE_SIZE > b->size) return 0;

    c = vmm_pgcache_find(backing, offset);
    if (c) {
        if (c->phys) { c->refcount++; return c->phys; }
    } else {
        for (i = 0; i < VMM_MAX_PGCACHE; i++) {
            if (!vmm_pgcache_pool[i].used) break;
        }
        if (i >= VMM_MAX_PGCACHE) return 0;
        c = &vmm_pgcache_pool[i];
        memset(c, 0, sizeof(*c));
        c->backing = backing;
        c->offset  = offset;
        c->used    = 1;
        vmm_pgcache_used++;
        vmm_cache_tree_insert(offset, i);
    }

    phys = pmm_alloc_page_zeroed();
    if (!phys) return 0;

    if (b->read(offset, (void *)phys, 0) != VMM_OK) {
        pmm_free_page(phys);
        return 0;
    }
    b->read_count++;

    c->phys = phys;
    c->refcount = 1;
    c->dirty = 0;
    return phys;
}

int vmm_pgcache_mark_dirty(u32 phys)
{
    u32 i;
    if (!phys) return VMM_EINVAL;
    for (i = 0; i < VMM_MAX_PGCACHE; i++) {
        vmm_pgcache_t *c = &vmm_pgcache_pool[i];
        if (c->used && c->phys == phys) { c->dirty = 1; return VMM_OK; }
    }
    return VMM_ENOENT;
}

int _vmm_pgcache_writeback_locked(u32 backing, u32 offset)
{
    vmm_pgcache_t *c;
    vmm_backing_t *b;

    b = vmm_backing_mut(backing);
    if (!b) return VMM_EINVAL;
    c = vmm_pgcache_find(backing, offset);
    if (!c || !c->phys) return VMM_ENOENT;
    if (!c->dirty) return VMM_OK;

    if (b->write(offset, (void *)c->phys, 1) != VMM_OK) return VMM_EINVAL;
    b->write_count++;
    c->dirty = 0;
    c->writeback_count++;
    return VMM_OK;
}

u32 vmm_pgcache_count(void) { return vmm_pgcache_used; }

u32 vmm_pgcache_dirty_count(void)
{
    u32 i, n = 0;
    for (i = 0; i < VMM_MAX_PGCACHE; i++) {
        if (vmm_pgcache_pool[i].used && vmm_pgcache_pool[i].dirty) n++;
    }
    return n;
}

u32 vmm_pgcache_writeback_count(void)
{
    u32 i, n = 0;
    for (i = 0; i < VMM_MAX_PGCACHE; i++) n += vmm_pgcache_pool[i].writeback_count;
    return n;
}

/* 回收干净且无引用的页缓存页 */
u32 _vmm_pgcache_evict_clean_locked(u32 nr)
{
    u32 i, freed = 0;
    for (i = 0; i < VMM_MAX_PGCACHE && freed < nr; i++) {
        vmm_pgcache_t *c = &vmm_pgcache_pool[i];
        if (!c->used || !c->phys) continue;
        if (c->dirty || c->refcount > 1u) continue;
        pmm_free_page(c->phys);
        vmm_cache_tree_remove(c->offset);
        c->phys = 0;
        c->refcount = 0;
        c->used = 0;
        vmm_pgcache_used--;
        freed++;
    }
    return freed;
}

/* --------------------------------------------------------------------------
 * 反向映射 rmap（S17）
 * ------------------------------------------------------------------------ */
u32 vmm_rmap_used(void) { return vmm_rmap_n; }

int vmm_rmap_add(u32 phys, u32 mm_id, u32 vaddr)
{
    u32 i;
    phys &= PTE_PFN_MASK;
    if (!phys) return VMM_EINVAL;

    for (i = 0; i < VMM_MAX_RMAP; i++) {
        vmm_rmap_t *r = &vmm_rmap_pool[i];
        if (r->used && r->phys == phys && r->mm_id == mm_id && r->vaddr == vaddr) {
            return VMM_EEXIST;
        }
    }
    for (i = 0; i < VMM_MAX_RMAP; i++) {
        if (!vmm_rmap_pool[i].used) break;
    }
    if (i >= VMM_MAX_RMAP) return VMM_ENOSPC;

    vmm_rmap_pool[i].phys  = phys;
    vmm_rmap_pool[i].mm_id = mm_id;
    vmm_rmap_pool[i].vaddr = vaddr;
    vmm_rmap_pool[i].used  = 1;
    vmm_rmap_n++;
    return VMM_OK;
}

int vmm_rmap_remove(u32 phys, u32 mm_id, u32 vaddr)
{
    u32 i;
    phys &= PTE_PFN_MASK;
    for (i = 0; i < VMM_MAX_RMAP; i++) {
        vmm_rmap_t *r = &vmm_rmap_pool[i];
        if (r->used && r->phys == phys && r->mm_id == mm_id && r->vaddr == vaddr) {
            r->used = 0;
            vmm_rmap_n--;
            return VMM_OK;
        }
    }
    return VMM_ENOENT;
}

u32 vmm_rmap_count_for(u32 phys)
{
    u32 i, n = 0;
    phys &= PTE_PFN_MASK;
    for (i = 0; i < VMM_MAX_RMAP; i++) {
        if (vmm_rmap_pool[i].used && vmm_rmap_pool[i].phys == phys) n++;
    }
    return n;
}

u32 vmm_rmap_total_for_phys(u32 phys)
{
    u32 i, n = 0;
    phys &= PTE_PFN_MASK;
    for (i = 0; i < VMM_MAX_RMAP; i++) {
        if (vmm_rmap_pool[i].used && vmm_rmap_pool[i].phys == phys) n++;
    }
    return n;
}

int vmm_rmap_get(u32 idx, u32 *phys, u32 *mm_id, u32 *vaddr)
{
    if (idx >= VMM_MAX_RMAP || !vmm_rmap_pool[idx].used) return VMM_ENOENT;
    if (phys)   *phys   = vmm_rmap_pool[idx].phys;
    if (mm_id)  *mm_id  = vmm_rmap_pool[idx].mm_id;
    if (vaddr)  *vaddr  = vmm_rmap_pool[idx].vaddr;
    return VMM_OK;
}

int vmm_rmap_remove_phys(u32 phys)
{
    u32 i, n = 0;
    phys &= PTE_PFN_MASK;
    for (i = 0; i < VMM_MAX_RMAP; i++) {
        if (vmm_rmap_pool[i].used && vmm_rmap_pool[i].phys == phys) {
            vmm_rmap_pool[i].used = 0;
            vmm_rmap_n--;
            n++;
        }
    }
    return (int)n;
}

/* --------------------------------------------------------------------------
 * 内存压力与回收（S20）
 * ------------------------------------------------------------------------ */
static u32 vmm_shrink_calls = 0;
static u32 vmm_shrink_freed = 0;

/* PMM 回收框架回调：先干净页缓存，后匿名页换出 */
static u32 vmm_shrink(u32 nr_to_free, void *arg)
{
    u32 freed = 0, i, j;
    vmm_mm_t *mm = (vmm_mm_t *)arg;

    vmm_shrink_calls++;

    /* 第一步：回收干净页缓存页（无数据丢失风险） */
    freed += _vmm_pgcache_evict_clean_locked(nr_to_free - freed);
    if (freed >= nr_to_free) goto done;

    /* 第二步：把未锁定的匿名页换出到交换区 */
    if (mm && mm->pgd) {
        for (i = KERNEL_IDENTITY_PGDS; i < KERNEL_PGD_INDEX && freed < nr_to_free; i++) {
            u32 *pt;
            if (!(mm->pgd[i] & PTE_P) || (mm->pgd[i] & PTE_PS)) continue;
            pt = vmm_pte_ptr(mm->pgd[i]);
            for (j = 0; j < PTRS_PER_PTE && freed < nr_to_free; j++) {
                u32 vaddr = (i << PGDIR_SHIFT) + (j << PTE_SHIFT);
                u32 *pte = &pt[j];
                if (!(*pte & PTE_P)) continue;   /* 含已换出页（P=0），无可回收物理页 */
                if (*pte & PTE_SW_SWAP) continue;
                if (*pte & PTE_SW_LOCK) continue;
                if (!vmm_vma_find(mm, vaddr)) continue;
                if (_vmm_swap_out_locked(mm, vaddr) == VMM_OK) freed++;
            }
        }
    }

done:
    vmm_shrink_freed += freed;
    return freed;
}

void vmm_reclaim_init(void)
{
    vmm_shrink_calls = 0;
    vmm_shrink_freed = 0;
    vmm_cache_tree_init();
    vmm_backing_init();
    pmm_register_shrinker("vmm-pages", vmm_shrink, (void *)vmm_cur_mm);
}

u32 vmm_shrinker_calls(void) { return vmm_shrink_calls; }
u32 vmm_shrinker_freed(void) { return vmm_shrink_freed; }

u32 vmm_reclaim(u32 target)
{
    u32 before = pmm_free_page_count();
    pmm_reclaim(target);
    return pmm_free_page_count() - before;
}

/* --------------------------------------------------------------------------
 * 页缓存自检：真实写入 → 标脏 → 回写 → 重新读入一致性
 * ------------------------------------------------------------------------ */
u32 vmm_pgcache_selftest(void)
{
    u32 backing, phys, phys2;
    const u32 off = 0x2000u;
    u32 *p;
    u32 i;

    if (!vmm_backing_used) return 1;
    backing = vmm_backing_pool[0].id;

    phys = vmm_pgcache_get(backing, off);
    if (!phys) return 2;

    /* 写入特征图案 */
    p = (u32 *)phys;
    for (i = 0; i < PTE_SIZE / 4u; i++) p[i] = 0xA5A50000u + i;

    if (vmm_pgcache_mark_dirty(phys) != VMM_OK) return 3;
    if (!vmm_pgcache_dirty_count()) return 4;
    if (vmm_pgcache_writeback(backing, off) != VMM_OK) return 5;
    if (vmm_pgcache_dirty_count()) return 6;

    /* 丢弃缓存副本，强制从后备存储重新读入 */
    {
        u32 j;
        for (j = 0; j < VMM_MAX_PGCACHE; j++) {
            vmm_pgcache_t *c = &vmm_pgcache_pool[j];
            if (c->used && c->backing == backing && c->offset == off) {
                pmm_free_page(c->phys);
                vmm_cache_tree_remove(c->offset);
                c->used = 0;
                c->phys = 0;
                vmm_pgcache_used--;
                break;
            }
        }
    }

    phys2 = vmm_pgcache_get(backing, off);
    if (!phys2) return 7;
    p = (u32 *)phys2;
    for (i = 0; i < PTE_SIZE / 4u; i++) {
        if (p[i] != 0xA5A50000u + i) return 8;
    }
    return 0;
}


/* 05 册第二轮：vmm_pgcache_get 并发保护包装（内部互调走 _locked 变体） */
u32 vmm_pgcache_get(u32 backing, u32 offset)
{
    u32 rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_pgcache_get_locked(backing, offset);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_pgcache_writeback 并发保护包装（内部互调走 _locked 变体） */
int vmm_pgcache_writeback(u32 backing, u32 offset)
{
    int rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_pgcache_writeback_locked(backing, offset);
    vmm_lock_exit(eflags);
    return rc;
}

/* 05 册第二轮：vmm_pgcache_evict_clean 并发保护包装（内部互调走 _locked 变体） */
u32 vmm_pgcache_evict_clean(u32 nr)
{
    u32 rc;
    u32 eflags = vmm_lock_enter();
    rc = _vmm_pgcache_evict_clean_locked(nr);
    vmm_lock_exit(eflags);
    return rc;
}