/* ============================================================================
 * XOS 内核堆分配器实现（第 06 册）
 * ============================================================================
 * 架构：
 *   1) slab 路径：16/32/64/128/256/512/1024/2048/4096 共 9 档缓存，
 *      每个缓存持有若干页面的 slab，对象以空闲链表组织；
 *   2) 大块路径：size > 4096 或对齐需求超出 slab 容量时，直接向 PMM
 *      按页分配，块头记录页数/大小/调用者；
 *   3) 内存池：固定大小对象的专用池，自带空闲链表与占用校验；
 *   4) 调试：对象头魔数 + 大小 + 头红区 + 尾红区 + canary + 调用者，
 *      释放时全项校验；重复释放/越界/非堆指针均可检测且不崩溃；
 *   5) 扩展收缩：slab 用尽自动向 PMM 取页；全空闲 slab 通过 shrinker
 *      归还给 PMM（已注册进第 03 册的回收框架）。
 * 并发：单 CPU，临界区以 IRQ 保存/恢复 + 嵌套深度计数保护。
 *
 * 对象布局（调试模式）：
 *   [hdr 24B][头红区][payload][尾红区 4B][canary 4B]
 *   stride = ALIGN_UP(objsize + 24 + 8, 8)，canary 位于 payload+size+4，
 *   保证载荷写满 size 字节也不会触碰红区与 canary。
 *   hdr.reserved 记录 payload 相对对象起点的偏移（支持任意 2 的幂对齐）。
 * ============================================================================ */
#include "kmalloc.h"
#include "pmm.h"
#include "string.h"
#include "console.h"

/* ---- 对象头（24 字节，8 对齐） ---- */
#define KALLOC_HDR_SIZE     24u
#define KALLOC_RZ_WORD      8u      /* 尾红区 4 + canary 4 */
#define KALLOC_MAGIC_ALLOC  0x4B314C4Cu  /* "K1L|" */
#define KALLOC_MAGIC_FREE   0x4B315244u  /* "K1RD" */
#define KALLOC_RZ_PAT       0xA5A5A5A5u
#define KALLOC_CANARY       0xC4A4C0DEu
#define KALLOC_PAT_ALLOC    0xC5u
#define KALLOC_PAT_FREE     0x5Au

#define KALLOC_SLABS_MAX    24u     /* 每缓存 slab 描述符上限 */
#define KALLOC_POOLS_MAX    8u
#define KALLOC_LARGE_MAX    64u
#define KALLOC_LEAK_SLOTS   32u
#define KALLOC_LEAK_TBL_SIZE 64u   /* 分配点追踪表槽位数（调试模式） */
#define KALLOC_POOL_MAX_COUNT 65536u
#define KPOOL_MAGIC          0x4B504F4Cu  /* "KPO|" 池魔数：销毁清零防悬垂 */

typedef struct kmalloc_hdr {
    u32 magic;
    u32 size;                       /* 用户载荷大小 */
    u32 flags;
    u32 rz_head;                    /* 头红区 */
    u32 caller;                     /* 调用者返回地址（泄漏追踪） */
    u32 reserved;                   /* payload 相对对象起点的偏移 */
} kmalloc_hdr_t;

typedef struct kslab {
    struct kslab *next;
    u8  *base;                      /* 本 slab 页区起始（页对齐） */
    u32 pages;
    u32 stride;                     /* 对象步长 */
    u32 total;                      /* 对象总数 */
    u32 free;                       /* 空闲对象数 */
    void *flist;                    /* 空闲对象链表头 */
    u32 state;                      /* 0 空闲槽 / 1 在用 */
} kslab_t;

typedef struct kcache {
    const char *name;
    u32 objsize;
    u32 stride;
    u32 slab_pages;
    kslab_t slabs[KALLOC_SLABS_MAX];
    kslab_t *list;
    u32 obj_total;
    u32 obj_used;
    u32 obj_free;
    u32 alloc_count;
    u32 free_count;
    u32 grow_count;
    u32 shrink_count;
    u32 page_count;                 /* 本缓存占用页数 */
    void (*ctor)(void *obj, u32 size);
    void (*dtor)(void *obj, u32 size);
    u32 ctor_calls;
    u32 dtor_calls;
    u32 hook_regs;                  /* 钩子注册次数（幂等覆盖计数，审计用） */
} kcache_t;

typedef struct klarge {
    struct klarge *next;
    u8  *base;                      /* 页对齐 */
    u32 pages;
    u32 size;
    u32 caller;
    u32 magic;
} klarge_t;

typedef struct kpool {
    const char *name;
    u32 objsize;
    u32 stride;
    u32 count;
    u32 free;
    u32 pages;
    u8  *base;                      /* 池占用的连续页区 */
    void *flist;
    u32 state;                      /* 0 空槽 / 1 在用 */
    u32 magic;                      /* 池魔数：销毁后清零，悬垂使用被拒 */
    u32 alloc_count;
    u32 free_count;
    u32 fail_count;
    u32 bytes;                      /* 在途载荷字节 */
} kpool_t;

/* ---- 全局状态 ---- */
static kcache_t  kcaches[KMALLOC_SLAB_CLASSES];
static u32      kheap_gap[64];           /* klarge_tbl 尾部越界写吸收哨兵（256B） */
static klarge_t  klarge_tbl[KALLOC_LARGE_MAX];
static klarge_t *klarge_list;
static u32       klarge_count;
static kpool_t   kpools[KALLOC_POOLS_MAX];
static u32       kpool_count;

static u32 kheap_dbg = 1u;
static u32 kheap_irq_depth;
static u32 kheap_err = KHEAP_ERR_NONE;
static u32 kheap_total_alloc;
static u32 kheap_total_free;
static u32 kheap_total_fail;
static u32 kheap_errors;
static u32 kheap_shrink_calls;
static u32 kheap_shrink_freed;
static u32 kheap_grow_calls;
static u32 kheap_histo[13];
static int  kheap_shrinker_id = -1;
static u32 kheap_leak_tbl[KALLOC_LEAK_TBL_SIZE][3];  /* [caller, count, bytes] */
static u32 kheap_leak_used;

/* ---- IRQ 保存/恢复自旋计数锁（单 CPU，无中断驱动并发） ---- */
static u32 kheap_irq_save(void)
{
    u32 eflags;
    asm volatile("pushfl; popl %0" : "=r"(eflags));
    asm volatile("cli");
    kheap_irq_depth++;
    return eflags;
}

static void kheap_irq_restore(u32 eflags)
{
    if (kheap_irq_depth > 0u) kheap_irq_depth--;
    if (eflags & 0x200u) asm volatile("sti");
}

/* ---- 大小类表 ---- */
static const u32 kalloc_sizes[KMALLOC_SLAB_CLASSES] = {
    16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

static u32 kheap_class_of(u32 size)
{
    u32 i;
    for (i = 0; i < KMALLOC_SLAB_CLASSES; i++) {
        if (size <= kalloc_sizes[i]) return i;
    }
    return KMALLOC_SLAB_CLASSES;    /* 超出 slab 上限，走大块路径 */
}

/* ---- 基本工具 ---- */
static void kheap_histo_add(u32 size)
{
    u32 b = 0;
    while (b < 12u && (1u << b) < size) b++;
    if (size > 4096u) b = 12u;
    kheap_histo[b]++;
}

static u32 kheap_leak_hash(u32 caller)
{
    return (caller >> 2) & (KALLOC_LEAK_TBL_SIZE - 1u);
}

/* 分配点追踪表：分配时累加、释放时递减，按调用者地址哈希 + 线性探测 */
static void kheap_leak_add(u32 caller, u32 size)
{
    u32 h, i;
    if (!kheap_dbg) return;
    h = kheap_leak_hash(caller);
    for (i = 0u; i < KALLOC_LEAK_TBL_SIZE; i++) {
        u32 slot = (h + i) & (KALLOC_LEAK_TBL_SIZE - 1u);
        if (kheap_leak_tbl[slot][1] == 0u) {
            kheap_leak_tbl[slot][0] = caller;
            kheap_leak_tbl[slot][1] = 1u;
            kheap_leak_tbl[slot][2] = size;
            kheap_leak_used++;
            return;
        }
        if (kheap_leak_tbl[slot][0] == caller) {
            kheap_leak_tbl[slot][1]++;
            kheap_leak_tbl[slot][2] += size;
            return;
        }
    }
}

static void kheap_leak_remove(u32 caller, u32 size)
{
    u32 h, i;
    if (!kheap_dbg) return;
    h = kheap_leak_hash(caller);
    for (i = 0u; i < KALLOC_LEAK_TBL_SIZE; i++) {
        u32 slot = (h + i) & (KALLOC_LEAK_TBL_SIZE - 1u);
        if (kheap_leak_tbl[slot][1] > 0u && kheap_leak_tbl[slot][0] == caller) {
            kheap_leak_tbl[slot][1]--;
            kheap_leak_tbl[slot][2] = (kheap_leak_tbl[slot][2] >= size) ?
                                      kheap_leak_tbl[slot][2] - size : 0u;
            if (kheap_leak_tbl[slot][1] == 0u) {
                kheap_leak_tbl[slot][0] = 0u;
                kheap_leak_tbl[slot][2] = 0u;
                kheap_leak_used--;
            }
            return;
        }
    }
}

static void kheap_report_error(const char *what, u32 detail)
{
    kheap_errors++;
    if (kheap_dbg) {
        con_puts("  [heap] DETECTED: ");
        con_puts(what);
        con_puts(" (detail=0x");
        con_put_hex32(detail);
        con_puts(")\n");
    }
}

/* ---- slab 分配器 ---- */
static int kheap_slab_grow(kcache_t *c)
{
    u32 i;
    kslab_t *s = NULL;
    u32 phys;
    for (i = 0; i < KALLOC_SLABS_MAX; i++) {
        if (c->slabs[i].state == 0u) { s = &c->slabs[i]; break; }
    }
    if (!s) { kheap_err = KHEAP_ERR_NOSLOT; return -1; }
    phys = pmm_alloc_pages(c->slab_pages);
    if (phys == 0u && c->slab_pages > 1u) phys = pmm_alloc_pages(c->slab_pages);
    if (phys == 0u) { kheap_err = KHEAP_ERR_OOM; return -1; }

    s->state = 1;
    s->base = (u8 *)phys;
    s->pages = c->slab_pages;
    s->stride = c->stride;
    s->total = (s->pages * PAGE_SIZE) / s->stride;
    s->free = s->total;
    s->next = c->list;
    c->list = s;
    c->obj_total += s->total;
    c->obj_free += s->total;
    c->page_count += s->pages;
    c->grow_count++;
    kheap_grow_calls++;

    {
        u32 n;
        u8 *o = s->base;
        void *prev = NULL;
        for (n = 0; n < s->total; n++, o += s->stride) {
            kmalloc_hdr_t *h = (kmalloc_hdr_t *)o;
            h->magic = KALLOC_MAGIC_FREE;
            *(void **)(o + 4u) = prev;   /* 链表指针存 size 字段区，不覆盖魔数 */
            prev = o;
        }
        s->flist = prev;
    }
    return 0;
}

static void kheap_slab_teardown(kslab_t *s)
{
    pmm_free_pages((u32)(u32)s->base, s->pages);
    s->state = 0;
    s->base = NULL;
    s->total = s->free = 0;
    s->flist = NULL;
}

/* 返回 (cache, slab, obj_base)；找不到返回 NULL cache */
static kcache_t *kheap_owner(void *ptr, kslab_t **out_slab, u8 **out_base)
{
    u32 i;
    for (i = 0; i < KMALLOC_SLAB_CLASSES; i++) {
        kcache_t *c = &kcaches[i];
        kslab_t *s;
        for (s = c->list; s; s = s->next) {
            u8 *b = s->base;
            if ((u8 *)ptr >= b && (u8 *)ptr < b + s->pages * PAGE_SIZE) {
                u8 *base;
                u32 off = (u32)((u8 *)ptr - b);
                base = b + (off / s->stride) * s->stride;
                if (out_slab) *out_slab = s;
                if (out_base) *out_base = base;
                return c;
            }
        }
    }
    return NULL;
}

/* ---- 分配主路径 ---- */
static void *kheap_alloc_slab(u32 size, u32 align, u32 flags, u32 caller)
{
    u32 cls = kheap_class_of(size);
    while (cls < KMALLOC_SLAB_CLASSES) {
        kcache_t *c = &kcaches[cls];
        kslab_t *s;
        u8 *obj;
        u8 *payload;
        kmalloc_hdr_t *h;
        u32 i;

        if (align > 8u) {
            u32 need = size + (align - 8u);
            if (need > c->objsize) { cls++; continue; }
        }

        s = NULL;
        for (i = 0; i < KALLOC_SLABS_MAX; i++) {
            if (c->slabs[i].state == 1u && c->slabs[i].free > 0u) {
                s = &c->slabs[i];
                break;
            }
        }
        if (!s) {
            if (kheap_slab_grow(c) != 0) return NULL;
            s = c->list;
        }

        obj = (u8 *)s->flist;
        s->flist = *(void **)(obj + 4u);
        s->free--;
        c->obj_free--;
        c->obj_used++;

        h = (kmalloc_hdr_t *)obj;
        payload = (u8 *)ALIGN_UP((u32)obj + KALLOC_HDR_SIZE, align);
        h->magic = KALLOC_MAGIC_ALLOC;
        h->size = size;
        h->flags = flags;
        h->rz_head = KALLOC_RZ_PAT;
        h->caller = caller;
        h->reserved = (u32)(payload - obj);

        if (kheap_dbg) {
            u32 off = (u32)(payload - obj);
            memset(obj + KALLOC_HDR_SIZE, KALLOC_RZ_PAT, off - KALLOC_HDR_SIZE);
            memset(payload + size, KALLOC_RZ_PAT, s->stride - off - size);
            *(u32 *)(payload + size + 4u) = KALLOC_CANARY;
        }
        if (flags & KMALLOC_ZERO) memset(payload, 0, size);
        if (c->ctor) { c->ctor(payload, size); c->ctor_calls++; }

        c->alloc_count++;
        kheap_total_alloc++;
        kheap_histo_add(size);
        return payload;
    }
    return NULL;
}

static void *kheap_alloc_large(u32 size, u32 align, u32 flags, u32 caller)
{
    u32 pages;
    u32 extra = 0u;
    u32 phys;
    klarge_t *n = NULL;
    u8 *base;
    u8 *payload;
    u32 i;

    {
        u32 hdr_off = (u32)ALIGN_UP(KALLOC_HDR_SIZE, align); /* 载荷相对块起点的最大偏移 */
        pages = (hdr_off + size + KALLOC_RZ_WORD + PAGE_SIZE - 1u) / PAGE_SIZE;
    }
    if (pages == 0u) pages = 1u;
    UNUSED(extra);

    for (i = 0; i < KALLOC_LARGE_MAX; i++) {
        if (klarge_tbl[i].magic == 0u) { n = &klarge_tbl[i]; break; }
    }
    if (!n) { kheap_err = KHEAP_ERR_NOSLOT; return NULL; }

    phys = pmm_alloc_pages(pages);
    if (phys == 0u) { kheap_err = KHEAP_ERR_OOM; return NULL; }
    base = (u8 *)phys;
    payload = (u8 *)ALIGN_UP((u32)base + KALLOC_HDR_SIZE, align);
    if ((u32)(payload + size) > (u32)base + pages * PAGE_SIZE) {
        payload = (u8 *)ALIGN_UP((u32)base + KALLOC_HDR_SIZE, PAGE_SIZE);
    }

    memset(base, KALLOC_PAT_ALLOC, KALLOC_HDR_SIZE);
    ((kmalloc_hdr_t *)base)->magic = KALLOC_MAGIC_ALLOC;
    ((kmalloc_hdr_t *)base)->size = size;
    ((kmalloc_hdr_t *)base)->flags = flags;
    ((kmalloc_hdr_t *)base)->rz_head = KALLOC_RZ_PAT;
    ((kmalloc_hdr_t *)base)->caller = caller;
    ((kmalloc_hdr_t *)base)->reserved = (u32)(payload - base);
    if (kheap_dbg) {
        memset(base + KALLOC_HDR_SIZE, KALLOC_RZ_PAT,
               (u32)(payload - base) - KALLOC_HDR_SIZE);
        memset(payload + size, KALLOC_RZ_PAT,
               pages * PAGE_SIZE - (u32)(payload - base) - size);
        *(u32 *)(payload + size + 4u) = KALLOC_CANARY;
    }
    if (flags & KMALLOC_ZERO) memset(payload, 0, size);

    n->magic = KALLOC_MAGIC_ALLOC;
    n->base = base;
    n->pages = pages;
    n->size = size;
    n->caller = caller;
    n->next = klarge_list;
    klarge_list = n;
    klarge_count++;

    kheap_total_alloc++;
    kheap_histo_add(size);
    return payload;
}

/* ---- 对象完整性校验 ---- */
static void kheap_check_object(u8 *base, u32 stride, u32 pages, u32 is_large)
{
    kmalloc_hdr_t *h = (kmalloc_hdr_t *)base;
    u32 total;
    u8 *payload;
    u8 *rz_end;
    if (is_large) {
        total = pages * PAGE_SIZE;
        payload = base + h->reserved;
    } else {
        total = stride;
        payload = base + h->reserved;
    }
    if (h->rz_head != KALLOC_RZ_PAT)
        kheap_report_error("head redzone corrupted", (u32)(u8 *)h);
    rz_end = payload + h->size;
    if ((u32)(rz_end + KALLOC_RZ_WORD) > (u32)base + total) {
        kheap_report_error("object size exceeds region", (u32)(u8 *)rz_end);
        return;
    }
    if (kheap_dbg) {
        u32 i;
        for (i = 0; i < 4u; i++) {
            if (rz_end[i] != (u8)KALLOC_RZ_PAT) {
                kheap_report_error("tail redzone corrupted", (u32)(u8 *)rz_end);
                break;
            }
        }
        if (*(u32 *)(rz_end + 4u) != KALLOC_CANARY)
            kheap_report_error("canary corrupted", (u32)(u8 *)rz_end);
    }
}

/* ---- 释放主路径 ---- */
void kfree(void *ptr)
{
    u32 eflags;
    kmalloc_hdr_t *h;
    kcache_t *c;
    kslab_t *s = NULL;
    u8 *base;

    if (ptr == NULL) return;
    eflags = kheap_irq_save();

    {
        klarge_t *n;
        for (n = klarge_list; n; n = n->next) {
            if ((u8 *)ptr >= n->base && (u8 *)ptr < n->base + n->pages * PAGE_SIZE) {
                base = n->base;
                h = (kmalloc_hdr_t *)base;
                if (h->magic != KALLOC_MAGIC_ALLOC) {
                    kheap_report_error("large block header corrupted", (u32)(u8 *)base);
                    kheap_irq_restore(eflags);
                    return;
                }
                kheap_check_object(base, 0u, n->pages, 1);
                if (kheap_dbg) memset(base, KALLOC_PAT_FREE, n->pages * PAGE_SIZE);
                n->magic = 0u;
                if (n == klarge_list) klarge_list = n->next;
                else {
                    klarge_t *p;
                    for (p = klarge_list; p; p = p->next) {
                        if (p->next == n) { p->next = n->next; break; }
                    }
                }
                klarge_count--;
                kheap_leak_remove(h->caller, h->size);
                pmm_free_pages((u32)(u32)base, n->pages);
                kheap_total_free++;
                kheap_irq_restore(eflags);
                return;
            }
        }
    }

    c = kheap_owner(ptr, &s, &base);
    if (!c) {
        kheap_report_error("free of non-heap pointer rejected", (u32)(u32)ptr);
        kheap_irq_restore(eflags);
        return;
    }
    h = (kmalloc_hdr_t *)base;
    if (h->magic == KALLOC_MAGIC_FREE) {
        kheap_report_error("double free detected", (u32)(u32)ptr);
        kheap_irq_restore(eflags);
        return;
    }
    if (h->magic != KALLOC_MAGIC_ALLOC) {
        kheap_report_error("object header corrupted", (u32)(u32)base);
        kheap_irq_restore(eflags);
        return;
    }
    kheap_check_object(base, s->stride, s->pages, 0);
    if (c->dtor) { c->dtor(ptr, h->size); c->dtor_calls++; }
    kheap_leak_remove(h->caller, h->size);

    if (kheap_dbg) memset(base, KALLOC_PAT_FREE, s->stride);
    h->magic = KALLOC_MAGIC_FREE;
    *(void **)(base + 4u) = s->flist;
    s->flist = base;
    s->free++;
    c->obj_free++;
    c->obj_used--;
    c->free_count++;
    kheap_total_free++;

    if (s->free == s->total && c->obj_total > s->total) {
        kslab_t *p;
        if (s == c->list) c->list = s->next;
        else for (p = c->list; p; p = p->next) {
            if (p->next == s) { p->next = s->next; break; }
        }
        c->obj_total -= s->total;
        c->page_count -= s->pages;
        c->obj_free = c->obj_total - c->obj_used;   /* 收缩后空闲计数与总量同步 */
        c->shrink_count++;
        kheap_shrink_freed += s->pages;
        kheap_slab_teardown(s);
    }
    kheap_irq_restore(eflags);
}

/* ---- 对外分配入口 ---- */
void *kmalloc(size_t size, u32 align, u32 flags)
{
    return kmalloc_node(size, align, flags, 0u);
}

void *kmalloc_node(size_t size, u32 align, u32 flags, u32 node)
{
    u32 eflags;
    void *r;
    u32 caller = (u32)__builtin_return_address(0);

    if (node != 0u || size == 0u || (align & (align - 1u)) != 0u) {
        kheap_err = KHEAP_ERR_INVAL;
        kheap_total_fail++;
        return NULL;
    }
    if (align < 8u) align = 8u;
    if (align > PAGE_SIZE) align = PAGE_SIZE;

    eflags = kheap_irq_save();
    if (size > KMALLOC_MAX_SLAB || align >= KMALLOC_MAX_SLAB) {
        r = kheap_alloc_large(size, align, flags, caller);
    } else {
        r = kheap_alloc_slab(size, align, flags, caller);
    }
    if (!r) kheap_total_fail++;
    else kheap_leak_add(caller, (u32)size);
    kheap_irq_restore(eflags);
    return r;
}

u32 kheap_last_err(void) { return kheap_err; }

/* ---- 缓存构造/析构钩子 ---- */
void kheap_set_hooks(size_t size, void (*ctor)(void *obj, u32 size),
                     void (*dtor)(void *obj, u32 size))
{
    u32 cls = kheap_class_of((u32)size);
    if (cls < KMALLOC_SLAB_CLASSES) {
        kcaches[cls].ctor = ctor;
        kcaches[cls].dtor = dtor;
        kcaches[cls].hook_regs++;
    }
}

/* ---- 内存池 ---- */
kpool_t *kpool_create(const char *name, size_t objsize, u32 count)
{
    u32 eflags = kheap_irq_save();
    u32 i;
    kpool_t *p = NULL;
    u32 stride, pages, phys;
    u8 *o;
    void *prev = NULL;

    if (objsize == 0u || count == 0u) { kheap_irq_restore(eflags); return NULL; }
    if (count > KALLOC_POOL_MAX_COUNT) { kheap_irq_restore(eflags); return NULL; }
    if (objsize < KMALLOC_MIN_SIZE) objsize = KMALLOC_MIN_SIZE;
    stride = (u32)ALIGN_UP(objsize + KALLOC_HDR_SIZE + KALLOC_RZ_WORD, 8u);

    for (i = 0; i < KALLOC_POOLS_MAX; i++) {
        if (kpools[i].state == 0u) { p = &kpools[i]; break; }
    }
    if (!p) { kheap_irq_restore(eflags); return NULL; }

    pages = (count * stride + PAGE_SIZE - 1u) / PAGE_SIZE;
    phys = pmm_alloc_pages(pages);
    if (phys == 0u) { kheap_irq_restore(eflags); return NULL; }

    p->state = 1;
    p->name = name;
    p->objsize = (u32)objsize;
    p->stride = stride;
    p->count = count;
    p->free = count;
    p->pages = pages;
    p->base = (u8 *)phys;
    p->magic = KPOOL_MAGIC;
    p->alloc_count = 0u;
    p->free_count = 0u;
    p->fail_count = 0u;
    p->bytes = 0u;

    o = p->base;
    for (i = 0; i < count; i++, o += stride) {
        kmalloc_hdr_t *h = (kmalloc_hdr_t *)o;
        h->magic = KALLOC_MAGIC_FREE;
        *(void **)(o + 4u) = prev;
        prev = o;
    }
    p->flist = prev;
    kpool_count++;
    kheap_irq_restore(eflags);
    return p;
}

void kpool_destroy(kpool_t *pool)
{
    u32 eflags;
    if (!pool || pool->state == 0u) return;
    eflags = kheap_irq_save();
    pmm_free_pages((u32)(u32)pool->base, pool->pages);
    pool->state = 0;
    pool->base = NULL;
    pool->flist = NULL;
    pool->magic = 0u;               /* 悬垂标记：销毁后再 alloc/free 一律拒绝 */
    kpool_count--;
    kheap_irq_restore(eflags);
}

void *kpool_alloc(kpool_t *pool)
{
    u32 eflags;
    u8 *obj;
    kmalloc_hdr_t *h;
    if (!pool || pool->state == 0u || pool->magic != KPOOL_MAGIC) return NULL;
    eflags = kheap_irq_save();
    if (pool->free == 0u) {
        pool->fail_count++;
        kheap_irq_restore(eflags);
        kheap_total_fail++;
        return NULL;
    }
    obj = (u8 *)pool->flist;
    pool->flist = *(void **)(obj + 4u);
    pool->free--;
    h = (kmalloc_hdr_t *)obj;
    h->magic = KALLOC_MAGIC_ALLOC;
    h->size = pool->objsize;
    h->rz_head = KALLOC_RZ_PAT;
    h->caller = (u32)__builtin_return_address(0);
    h->reserved = KALLOC_HDR_SIZE;
    if (kheap_dbg) {
        memset(obj + KALLOC_HDR_SIZE, KALLOC_PAT_ALLOC, pool->objsize);
        memset(obj + KALLOC_HDR_SIZE + pool->objsize, KALLOC_RZ_PAT,
               pool->stride - KALLOC_HDR_SIZE - pool->objsize);
        *(u32 *)(obj + KALLOC_HDR_SIZE + pool->objsize + 4u) = KALLOC_CANARY;
    }
    pool->alloc_count++;
    pool->bytes += pool->objsize;
    kheap_total_alloc++;
    kheap_leak_add(h->caller, pool->objsize);
    kheap_irq_restore(eflags);
    return obj + KALLOC_HDR_SIZE;
}

int kpool_free(kpool_t *pool, void *obj)
{
    u32 eflags;
    kmalloc_hdr_t *h;
    u8 *o;
    if (!pool || pool->state == 0u || pool->magic != KPOOL_MAGIC || obj == NULL) return -1;
    eflags = kheap_irq_save();
    o = (u8 *)obj - KALLOC_HDR_SIZE;
    if ((u8 *)o < pool->base || (u8 *)o >= pool->base + pool->pages * PAGE_SIZE) {
        kheap_report_error("pool free of foreign pointer", (u32)(u32)obj);
        kheap_irq_restore(eflags);
        return -1;
    }
    if (((u32)(o - pool->base) % pool->stride) != 0u) {
        kheap_report_error("pool free of unaligned pointer", (u32)(u32)obj);
        kheap_irq_restore(eflags);
        return -1;
    }
    h = (kmalloc_hdr_t *)o;
    if (h->magic == KALLOC_MAGIC_FREE) {
        kheap_report_error("pool double free", (u32)(u32)obj);
        kheap_irq_restore(eflags);
        return -1;
    }
    if (h->magic != KALLOC_MAGIC_ALLOC) {
        kheap_report_error("pool object header corrupted", (u32)(u32)o);
        kheap_irq_restore(eflags);
        return -1;
    }
    kheap_check_object(o, pool->stride, pool->pages, 0);
    kheap_leak_remove(h->caller, pool->objsize);
    if (kheap_dbg) memset(o, KALLOC_PAT_FREE, pool->stride);
    h->magic = KALLOC_MAGIC_FREE;
    *(void **)(o + 4u) = pool->flist;
    pool->flist = o;
    pool->free++;
    pool->free_count++;
    pool->bytes = (pool->bytes >= pool->objsize) ? pool->bytes - pool->objsize : 0u;
    kheap_total_free++;
    kheap_irq_restore(eflags);
    return 0;
}

u32 kpool_free_count(kpool_t *pool) { return pool ? pool->free : 0u; }
u32 kpool_used_count(kpool_t *pool) { return pool ? pool->count - pool->free : 0u; }

void kpool_stats(const kpool_t *pool, u32 *out_free, u32 *out_used,
                 u32 *out_fail, u32 *out_bytes)
{
    if (!pool || pool->state == 0u || pool->magic != KPOOL_MAGIC) {
        if (out_free) *out_free = 0u;
        if (out_used) *out_used = 0u;
        if (out_fail) *out_fail = 0u;
        if (out_bytes) *out_bytes = 0u;
        return;
    }
    if (out_free) *out_free = pool->free;
    if (out_used) *out_used = pool->count - pool->free;
    if (out_fail) *out_fail = pool->fail_count;
    if (out_bytes) *out_bytes = pool->bytes;
}

void kheap_hook_info(size_t size, u32 *out_regs, u32 *out_ctor, u32 *out_dtor)
{
    u32 cls = kheap_class_of((u32)size);
    kcache_t *c = (cls < KMALLOC_SLAB_CLASSES) ? &kcaches[cls] : NULL;
    if (out_regs) *out_regs = c ? c->hook_regs : 0u;
    if (out_ctor) *out_ctor = c ? c->ctor_calls : 0u;
    if (out_dtor) *out_dtor = c ? c->dtor_calls : 0u;
}

/* ---- 收缩回调（PMM shrinker 框架） ---- */
u32 kheap_shrink(u32 nr_to_free, void *arg)
{
    u32 eflags;
    u32 freed = 0u;
    u32 i;
    UNUSED(arg);
    eflags = kheap_irq_save();
    kheap_shrink_calls++;
    for (i = 0; i < KMALLOC_SLAB_CLASSES && freed < nr_to_free; i++) {
        kcache_t *c = &kcaches[i];
        kslab_t *s = c->list;
        while (s && freed < nr_to_free) {
            kslab_t *nx = s->next;
            if (s->free == s->total && c->obj_total > s->total) {
                if (s == c->list) c->list = nx;
                else {
                    kslab_t *p;
                    for (p = c->list; p; p = p->next) {
                        if (p->next == s) { p->next = nx; break; }
                    }
                }
                c->obj_total -= s->total;
                c->page_count -= s->pages;
                c->obj_free = c->obj_total - c->obj_used;
                c->shrink_count++;
                freed += s->pages;
                kheap_slab_teardown(s);
            }
            s = nx;
        }
    }
    kheap_shrink_freed += freed;
    kheap_irq_restore(eflags);
    return freed;
}

/* ---- 统计 ---- */
void kheap_stats(kheap_stats_t *st)
{
    u32 i;
    u32 slabs = 0u, pages = 0u;
    if (!st) return;
    for (i = 0; i < KMALLOC_SLAB_CLASSES; i++) {
        slabs += kcaches[i].grow_count - kcaches[i].shrink_count;
        pages += kcaches[i].page_count;
    }
    memset(st, 0, sizeof(*st));
    st->total_alloc = kheap_total_alloc;
    st->total_free = kheap_total_free;
    st->total_fail = kheap_total_fail;
    st->outstanding = kheap_total_alloc - kheap_total_free;
    st->slabs_active = slabs;
    st->slabs_pages = pages;
    st->large_count = klarge_count;
    st->pool_count = kpool_count;
    st->pool_slots = KALLOC_POOLS_MAX;
    st->shrink_calls = kheap_shrink_calls;
    st->shrink_freed = kheap_shrink_freed;
    st->grow_calls = kheap_grow_calls;
    st->errors_detected = kheap_errors;
    st->debug_on = kheap_dbg;
    st->irq_depth = kheap_irq_depth;
    st->last_err = kheap_err;
    for (i = 0; i < 13u; i++) st->profile[i] = kheap_histo[i];
    {
        u32 j;
        u32 slab_bytes = 0u, large_bytes = 0u;
        klarge_t *n;
        for (j = 0; j < KMALLOC_SLAB_CLASSES; j++)
            slab_bytes += kcaches[j].obj_used * kcaches[j].objsize;
        for (n = klarge_list; n; n = n->next) large_bytes += n->size;
        st->outstanding_bytes = slab_bytes + large_bytes;
        st->large_bytes = large_bytes;
    }
}

/* ---- 泄漏报告（调试模式）：返回泄漏对象数 ---- */
u32 kheap_leak_report(void)
{
    u32 leaks = 0u;
    u32 i;
    static u32 callers[KALLOC_LEAK_SLOTS][2];   /* [caller, count] */
    u32 ncallers = 0u;

    if (!kheap_dbg) return 0u;
    for (i = 0; i < KMALLOC_SLAB_CLASSES; i++) {
        kcache_t *c = &kcaches[i];
        kslab_t *s;
        for (s = c->list; s; s = s->next) {
            u32 off;
            for (off = 0u; off < s->total * s->stride; off += s->stride) {
                kmalloc_hdr_t *h = (kmalloc_hdr_t *)(s->base + off);
                if (h->magic == KALLOC_MAGIC_ALLOC) {
                    u32 k;
                    leaks++;
                    for (k = 0; k < ncallers; k++) {
                        if (callers[k][0] == h->caller) { callers[k][1]++; break; }
                    }
                    if (k == ncallers && ncallers < KALLOC_LEAK_SLOTS) {
                        callers[ncallers][0] = h->caller;
                        callers[ncallers][1] = 1u;
                        ncallers++;
                    }
                }
            }
        }
    }
    {
        klarge_t *n;
        for (n = klarge_list; n; n = n->next) leaks++;
    }
    if (leaks > 0u) {
        con_puts("  [heap] leak report: ");
        con_put_dec(leaks);
        con_puts(" objects outstanding (");
        con_put_dec(ncallers);
        con_puts(" distinct callers)\n");
        for (i = 0; i < ncallers; i++) {
            con_puts("    caller=0x");
            con_put_hex32(callers[i][0]);
            con_puts(" x ");
            con_put_dec(callers[i][1]);
            con_puts("\n");
        }
    }
    if (kheap_leak_used > 0u) {
        u32 sorted[KALLOC_LEAK_TBL_SIZE];
        u32 ns = 0u, i2, j;
        for (i2 = 0u; i2 < KALLOC_LEAK_TBL_SIZE; i2++) {
            if (kheap_leak_tbl[i2][1] > 0u) sorted[ns++] = i2;
        }
        /* 选择排序：按在途字节降序，取前 8 */
        for (i2 = 0u; i2 < ns && i2 < 8u; i2++) {
            u32 best = i2, k;
            for (k = i2 + 1u; k < ns; k++) {
                if (kheap_leak_tbl[sorted[k]][2] > kheap_leak_tbl[sorted[best]][2])
                    best = k;
            }
            j = sorted[i2]; sorted[i2] = sorted[best]; sorted[best] = j;
        }
        con_puts("  [heap] live alloc points (top by bytes):\n");
        for (i2 = 0u; i2 < ns && i2 < 8u; i2++) {
            u32 slot = sorted[i2];
            con_puts("    caller=0x");
            con_put_hex32(kheap_leak_tbl[slot][0]);
            con_puts(" n=");
            con_put_dec(kheap_leak_tbl[slot][1]);
            con_puts(" bytes=");
            con_put_dec(kheap_leak_tbl[slot][2]);
            con_puts("\n");
        }
    }
    return leaks;
}

/* ---- 转储 ---- */
void kheap_dump(void)
{
    u32 i;
    con_puts("  Heap allocator dump:\n");
    for (i = 0; i < KMALLOC_SLAB_CLASSES; i++) {
        kcache_t *c = &kcaches[i];
        con_puts("    cache ");
        con_put_dec(c->objsize);
        con_puts("B: objs ");
        con_put_dec(c->obj_used);
        con_puts("/");
        con_put_dec(c->obj_total);
        con_puts(" free ");
        con_put_dec(c->obj_free);
        con_puts(" slabs ");
        con_put_dec(c->grow_count - c->shrink_count);
        con_puts(" pages ");
        con_put_dec(c->page_count);
        con_puts(" alloc ");
        con_put_dec(c->alloc_count);
        con_puts(" free ");
        con_put_dec(c->free_count);
        con_puts(" grow ");
        con_put_dec(c->grow_count);
        con_puts(" shrink ");
        con_put_dec(c->shrink_count);
        con_puts("\n");
    }
    con_puts("    large blocks: ");
    con_put_dec(klarge_count);
    con_puts("  pools: ");
    con_put_dec(kpool_count);
    con_puts("  errors: ");
    con_put_dec(kheap_errors);
    con_puts("\n");
}

void kheap_set_debug(u32 on) { kheap_dbg = on ? 1u : 0u; }
u32 kheap_get_debug(void) { return kheap_dbg; }

/* ---- 初始化 ---- */
void kheap_init(void)
{
    u32 i;
    memset(kcaches, 0, sizeof(kcaches));
    memset(klarge_tbl, 0, sizeof(klarge_tbl));
    memset(kpools, 0, sizeof(kpools));
    klarge_list = NULL;
    klarge_count = 0u;
    kheap_gap[0] = (kheap_gap[0] + 1u) & 0x3Fu;  /* touch 哨兵 */
    kpool_count = 0u;

    for (i = 0; i < KMALLOC_SLAB_CLASSES; i++) {
        kcache_t *c = &kcaches[i];
        c->objsize = kalloc_sizes[i];
        c->stride = (u32)ALIGN_UP(c->objsize + KALLOC_HDR_SIZE + KALLOC_RZ_WORD, 8u);
        c->slab_pages = (c->stride > PAGE_SIZE) ? 2u : 1u;
        c->name = "kmalloc";
    }
    kheap_shrinker_id = pmm_register_shrinker("kheap-slabs", kheap_shrink, NULL);
}

/* ============================================================================
 * 自检：0 通过；返回唯一失败编号（1..36）
 * ============================================================================ */
static void st_ctor(void *obj, u32 size) { UNUSED(obj); UNUSED(size); }
static void st_dtor(void *obj, u32 size) { UNUSED(obj); UNUSED(size); }

static u32 st_alloc_roundtrip(void)
{
    static u32 sizes[] = { 1u, 16u, 17u, 32u, 33u, 64u, 65u, 128u, 129u, 256u,
                    257u, 512u, 513u, 1024u, 1025u, 2048u, 2049u, 4096u };
    u32 i, k;
    void *ptrs[18];
    for (i = 0; i < 18u; i++) {
        u8 *p = (u8 *)kmalloc(sizes[i], 8u, 0u);
        if (!p) return 1u;
        for (k = 0; k < sizes[i]; k++) p[k] = (u8)(k + i);
        ptrs[i] = p;
    }
    for (i = 0; i < 18u; i++) {
        u8 *p = (u8 *)ptrs[i];
        for (k = 0; k < sizes[i]; k++) {
            if (p[k] != (u8)(k + i)) return 2u;
        }
        kfree(p);
    }
    return 0u;
}

static u32 st_alignment(void)
{
    u32 aligns[] = { 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u };
    u32 i;
    for (i = 0; i < 10u; i++) {
        u8 *p = (u8 *)kmalloc(33u, aligns[i], 0u);
        if (!p) return 3u;
        if (((u32)p & (aligns[i] - 1u)) != 0u) { kfree(p); return 4u; }
        kfree(p);
    }
    return 0u;
}

static u32 st_zero(void)
{
    u8 *p = (u8 *)kmalloc(512u, 8u, KMALLOC_ZERO);
    u32 i;
    if (!p) return 5u;
    for (i = 0; i < 512u; i++) {
        if (p[i] != 0u) { kfree(p); return 6u; }
    }
    kfree(p);
    return 0u;
}

static u32 st_redzone(void)
{
    u8 *p = (u8 *)kmalloc(128u, 8u, 0u);
    u32 cls = kheap_class_of(128u);
    kcache_t *c = &kcaches[cls];
    kslab_t *s = NULL, *sl;
    u8 *base;
    kmalloc_hdr_t *h;
    u8 *rz;
    if (!p) return 7u;
    for (sl = c->list; sl; sl = sl->next) {
        if ((u8 *)p >= sl->base && (u8 *)p < sl->base + sl->pages * PAGE_SIZE) {
            s = sl; break;
        }
    }
    if (!s) { kfree(p); return 8u; }
    base = s->base + (((u32)p - (u32)s->base) / s->stride) * s->stride;
    h = (kmalloc_hdr_t *)base;
    rz = base + h->reserved + h->size;
    if (h->rz_head != KALLOC_RZ_PAT) { kfree(p); return 9u; }
    if (rz[0] != (u8)KALLOC_RZ_PAT || rz[1] != (u8)KALLOC_RZ_PAT) {
        kfree(p); return 10u;
    }
    if (*(u32 *)(rz + 4u) != KALLOC_CANARY) { kfree(p); return 11u; }
    kfree(p);
    return 0u;
}

static u32 st_double_free(void)
{
    u8 *p = (u8 *)kmalloc(64u, 8u, 0u);
    u32 before;
    if (!p) return 12u;
    kfree(p);
    before = kheap_errors;
    kfree(p);
    if (kheap_errors == before) return 13u;
    return 0u;
}

static u32 st_corruption(void)
{
    u8 *p = (u8 *)kmalloc(96u, 8u, 0u);
    u32 before;
    if (!p) return 14u;
    p[96] ^= 0xFFu;
    before = kheap_errors;
    kfree(p);
    if (kheap_errors == before) return 15u;
    return 0u;
}

static u32 st_large(void)
{
    u32 sizes[] = { 5000u, 16384u, 65536u, 262144u };
    u32 i, k;
    void *ptrs[4];
    for (i = 0; i < 4u; i++) {
        u8 *p = (u8 *)kmalloc(sizes[i], 8u, 0u);
        if (!p) return 16u;
        for (k = 0; k < 64u; k++) p[k] = (u8)(k + 1);
        ptrs[i] = p;
    }
    for (i = 0; i < 4u; i++) {
        u8 *p = (u8 *)ptrs[i];
        for (k = 0; k < 64u; k++) {
            if (p[k] != (u8)(k + 1)) return 17u;
        }
        kfree(p);
    }
    return 0u;
}

static u32 st_pool(void)
{
    kpool_t *pool = kpool_create("st-pool", 64u, 16u);
    void *objs[16];
    u32 i;
    if (!pool) return 18u;
    for (i = 0; i < 16u; i++) {
        objs[i] = kpool_alloc(pool);
        if (!objs[i]) return 19u;
    }
    if (kpool_alloc(pool) != NULL) return 20u;
    if (kpool_free(pool, objs[0]) != 0) return 21u;
    if (kpool_alloc(pool) == NULL) return 22u;
    for (i = 1; i < 16u; i++) kpool_free(pool, objs[i]);
    kpool_destroy(pool);
    return 0u;
}

static u32 st_shrink(void)
{
    u32 before_free, after_free;
    void *ptrs[64];
    u32 i;
    for (i = 0; i < 64u; i++) {
        ptrs[i] = kmalloc(512u, 8u, 0u);
        if (!ptrs[i]) return 23u;
    }
    before_free = pmm_free_page_count();
    for (i = 0; i < 64u; i++) kfree(ptrs[i]);
    kheap_shrink(16u, NULL);
    after_free = pmm_free_page_count();
    if (after_free <= before_free) return 24u;
    return 0u;
}

static u32 st_leak(void)
{
    void *a, *b;
    u32 leaks;
    a = kmalloc(128u, 8u, 0u);
    b = kmalloc(64u, 8u, 0u);
    if (!a || !b) return 25u;
    leaks = kheap_leak_report();
    kfree(a);
    kfree(b);
    if (leaks < 2u) return 26u;
    return 0u;
}

static u32 st_stats(void)
{
    kheap_stats_t s1, s2;
    void *a, *b;
    kheap_stats(&s1);
    a = kmalloc(32u, 8u, 0u);
    b = kmalloc(300u, 8u, 0u);
    if (!a || !b) return 27u;
    kheap_stats(&s2);
    if (s2.outstanding != s1.outstanding + 2u) return 28u;
    if (s2.total_alloc != s1.total_alloc + 2u) return 29u;
    kfree(a);
    kfree(b);
    return 0u;
}

static u32 st_hooks(void)
{
    kcache_t *c = &kcaches[kheap_class_of(128u)];
    void *p;
    u32 c0, d0;
    kheap_set_hooks(128u, st_ctor, st_dtor);
    c0 = c->ctor_calls;
    d0 = c->dtor_calls;
    p = kmalloc(128u, 8u, 0u);
    if (!p) return 30u;
    if (c->ctor_calls != c0 + 1u) { kfree(p); return 31u; }
    kfree(p);
    if (c->dtor_calls != d0 + 1u) return 32u;
    kheap_set_hooks(128u, NULL, NULL);
    return 0u;
}

static u32 st_pool_dangle(void)
{
    kpool_t *pool = kpool_create("st-dangle", 48u, 8u);
    if (!pool) return 40u;
    kpool_destroy(pool);
    if (kpool_alloc(pool) != NULL) return 41u;      /* 销毁后悬垂分配被拒 */
    if (kpool_free(pool, (void *)0x1234u) != -1) return 42u;
    return 0u;
}

static u32 st_pool_stats(void)
{
    kpool_t *pool = kpool_create("st-pstats", 80u, 8u);
    void *a, *b;
    u32 f, u2, fail, bytes;
    if (!pool) return 43u;
    a = kpool_alloc(pool);
    b = kpool_alloc(pool);
    if (!a || !b) { kpool_destroy(pool); return 44u; }
    kpool_stats(pool, &f, &u2, &fail, &bytes);
    if (u2 != 2u || f != 6u || bytes != 160u) { kpool_destroy(pool); return 45u; }
    if (kpool_free(pool, a) != 0) { kpool_destroy(pool); return 46u; }
    kpool_stats(pool, &f, &u2, &fail, &bytes);
    if (u2 != 1u || f != 7u || bytes != 80u) { kpool_destroy(pool); return 47u; }
    kpool_free(pool, b);
    kpool_destroy(pool);
    return 0u;
}

static u32 st_pool_redzone(void)
{
    kpool_t *pool = kpool_create("st-rz", 32u, 4u);
    u8 *o;
    u32 before = kheap_errors;
    if (!pool) return 48u;
    o = (u8 *)kpool_alloc(pool);
    if (!o) { kpool_destroy(pool); return 49u; }
    o[32] ^= 0xFFu;                     /* 破坏尾红区 */
    if (kpool_free(pool, o) != 0) { kpool_destroy(pool); return 50u; }
    if (kheap_errors == before) { kpool_destroy(pool); return 51u; }
    kpool_destroy(pool);
    return 0u;
}

static u32 st_leaktbl(void)
{
    u32 before;
    void *p;
    before = kheap_leak_used;
    p = kmalloc(256u, 8u, 0u);
    if (!p) return 52u;
    if (kheap_leak_used <= before) { kfree(p); return 53u; }
    kfree(p);
    if (kheap_leak_used != before) return 54u;
    return 0u;
}

static u32 st_hookreg(void)
{
    kcache_t *c = &kcaches[kheap_class_of(512u)];
    u32 r0, rr, rc, dc;
    kheap_set_hooks(512u, st_ctor, st_dtor);
    r0 = c->hook_regs;
    kheap_set_hooks(512u, st_ctor, st_dtor);
    kheap_hook_info(512u, &rr, &rc, &dc);
    if (rr != r0 + 1u) return 55u;
    if (c->hook_regs != r0 + 1u) return 56u;
    kheap_set_hooks(512u, NULL, NULL);
    return 0u;
}

static u32 st_numa(void)
{
    void *p = kmalloc_node(64u, 8u, 0u, 0u);
    void *q = kmalloc_node(64u, 8u, 0u, 1u);
    if (!p) return 33u;
    if (q != NULL) { kfree(p); return 34u; }
    kfree(p);
    return 0u;
}

static u32 st_oom(void)
{
    void *p = kmalloc(0x08000000u, 8u, 0u);   /* 128MB+：必失败 */
    if (p != NULL) return 35u;
    if (kheap_last_err() != KHEAP_ERR_OOM && kheap_last_err() != KHEAP_ERR_NOSLOT)
        return 36u;
    return 0u;
}

static u32 st_ownership(void)
{
    u32 before = kheap_errors;
    kfree((void *)0x00123456u);
    if (kheap_errors == before) return 37u;
    return 0u;
}

static u32 st_null(void)
{
    kfree(NULL);
    return 0u;
}

u32 kheap_selftest_core(void)
{
    u32 r;
    r = st_alloc_roundtrip();  if (r) return r;
    r = st_alignment();        if (r) return r;
    r = st_zero();             if (r) return r;
    r = st_redzone();          if (r) return r;
    r = st_large();            if (r) return r;
    return 0u;
}

u32 kheap_selftest_fault(void)
{
    u32 r;
    r = st_double_free();      if (r) return r;
    r = st_corruption();       if (r) return r;
    r = st_oom();              if (r) return r;
    r = st_ownership();        if (r) return r;
    r = st_null();             if (r) return r;
    r = st_numa();             if (r) return r;
    return 0u;
}

u32 kheap_selftest_ext(void)
{
    u32 r;
    r = st_pool();             if (r) return r;
    r = st_shrink();           if (r) return r;
    r = st_leak();             if (r) return r;
    r = st_stats();            if (r) return r;
    r = st_hooks();            if (r) return r;
    r = st_pool_dangle();      if (r) return r;
    r = st_pool_stats();       if (r) return r;
    r = st_pool_redzone();     if (r) return r;
    r = st_leaktbl();          if (r) return r;
    r = st_hookreg();          if (r) return r;
    return 0u;
}

u32 kheap_selftest(void)
{
    u32 r;
    r = kheap_selftest_core(); if (r) return r;
    r = kheap_selftest_fault(); if (r) return r;
    r = kheap_selftest_ext();  if (r) return r;
    return 0u;
}
