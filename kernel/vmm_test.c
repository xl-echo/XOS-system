/* ============================================================================
 * XOS 虚拟内存子系统 —— 自检
 *
 * 测试口径（不是「编译通过即视为通过」）：
 *   1. 交叉验证：软件遍历页表得到的物理地址，必须与「完全依据当前 CR3 与
 *      硬件页表内容」重新走一遍得到的地址一致（vmm_translate vs
 *      vmm_translate_hw）。两者不一致即说明页表被写坏或软件模型错。
 *   2. 真实访问：映射之后真的用虚拟地址读写，读回值必须与写入值一致，
 *      且相邻未映射页不得被波及（证明 4KB 页边界真实生效）。
 *   3. 真实缺页：按需分页 / 写时复制 / 栈增长 / 换入 四类故障都是
 *      「真的触发 #PF → 处理器修复 → iret 重试 → 访问成功」的完整链路，
 *      并在缺页日志中留下可核对的记录（地址、错误码、指令指针、处置动作）。
 *   4. 拒绝路径：权限违例、空洞访问、越权访问内核页、保留位置位
 *      四类不可修复故障直接调用解析器断言其返回码，
 *      避免在自检中真的把处理器打进安全停机。
 *   5. 每个用例失败返回唯一编号，便于在启动画面上直接定位到具体断言。
 * ============================================================================ */
#include "vmm.h"
#include "vmm_internal.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

#define T_VADDR_A   0x50000000u     /* 1.25 GB，用户区 */
#define T_VADDR_B   0x50001000u
#define T_VADDR_C   0x60000000u     /* 1.5 GB */
#define T_VADDR_H   0x80000000u     /* 2 GB，4MB 对齐，供大页使用 */

/* --------------------------------------------------------------------------
 * 核心自检：页表结构、地址转换、标志位、TLB、大页、隔离、ASLR
 * ------------------------------------------------------------------------ */
u32 vmm_selftest(void)
{
    vmm_mm_t *mm = NULL;
    vmm_mm_t *kmm;
    u32 rc = 0;
    u32 phys = 0, hw, sw;
    u32 *p;
    u32 fl_before;

    kmm = vmm_kernel_mm();
    if (!kmm) return 1;

    vmm_dbg_clear();

    /* ---- 用例 1：分页已开启 ---- */
    if (!vmm_enabled()) return 1;

    /* ---- 用例 2：恒等映射生效（内核映像自身地址） ---- */
    if (vmm_translate_hw(0x10000u) != 0x10000u) return 2;

    /* ---- 用例 3：内核直接映射 0xC0000000 → 物理 0 ---- */
    if (vmm_translate_hw(KERNEL_SPACE_BASE) != 0u) return 3;

    /* ---- 用例 4：内核栈所在页可转换 ---- */
    if (vmm_translate_hw(0x9F000u) != 0x9F000u) return 4;

    /* ---- 用例 5：建立用户地址空间 ---- */
    mm = vmm_mm_create();
    if (!mm) return 5;

    /* ---- 用例 6：内核区共享且隔离性成立 ---- */
    if (vmm_kernel_pgd_shared() == 0) return 6;
    if (vmm_check_isolation(mm) != 0) return 7;

    /* ---- 用例 7：大页（4MB PSE）映射 ---- */
    if (vmm_pse_available()) {
        if (vmm_huge_alloc(mm, T_VADDR_H, PTE_P | PTE_RW | PTE_US) != VMM_OK) {
            /* 4MB 连续物理内存不可得：先整理碎片再试一次 */
            pmm_compact();
            if (vmm_huge_alloc(mm, T_VADDR_H, PTE_P | PTE_RW | PTE_US) != VMM_OK) {
                return 8;
            }
        }
        if (vmm_huge_count() == 0) return 9;
    }

    /* ---- 用例 8：映射一页并校验软硬件转换一致 ---- */
    phys = pmm_alloc_page_zeroed();
    if (!phys) { rc = 10; goto out; }
    if (vmm_map_page(mm, T_VADDR_A, phys, PTE_P | PTE_RW | PTE_US) != VMM_OK) {
        rc = 11; goto out;
    }
    sw = vmm_translate(mm, T_VADDR_A);
    if (sw != phys) { rc = 12; goto out; }

    /* ---- 用例 9：装载该地址空间，内核必须仍能继续运行 ---- */
    if (vmm_mm_switch(mm) != VMM_OK) { rc = 13; goto out; }
    if (vmm_translate_hw(0x10000u) != 0x10000u) { rc = 14; goto out; }

    /* ---- 用例 10：硬件转换与软件转换一致 ---- */
    hw = vmm_translate_hw(T_VADDR_A);
    if (hw != phys) { rc = 15; goto out; }
    if (hw != sw)   { rc = 16; goto out; }

    /* ---- 用例 11：真实读写虚拟地址 ---- */
    p = (u32 *)T_VADDR_A;
    p[0] = 0x12345678u;
    p[1023] = 0x9ABCDEF0u;
    if (p[0] != 0x12345678u)    { rc = 17; goto out; }
    if (p[1023] != 0x9ABCDEF0u) { rc = 18; goto out; }
    if (*(u32 *)phys != 0x12345678u) { rc = 19; goto out; }   /* 确实落在该物理页 */

    /* ---- 用例 12：页边界不得越界（下一页未映射） ---- */
    if (vmm_translate_hw(T_VADDR_A + PTE_SIZE) != 0u) { rc = 20; goto out; }
    if (vmm_translate_hw(T_VADDR_A - PTE_SIZE) != 0u) { rc = 21; goto out; }

    /* ---- 用例 13：硬件访问位置位 ---- */
    if (!vmm_get_accessed(mm, T_VADDR_A)) { rc = 22; goto out; }
    if (!vmm_get_dirty(mm, T_VADDR_A))    { rc = 23; goto out; }

    /* ---- 用例 14：清访问位后重新访问，硬件应再次置位 ---- */
    vmm_clear_accessed(mm, T_VADDR_A);
    if (vmm_get_accessed(mm, T_VADDR_A)) { rc = 24; goto out; }
    (void)(*(volatile u32 *)T_VADDR_A);
    if (!vmm_get_accessed(mm, T_VADDR_A)) { rc = 25; goto out; }

    /* ---- 用例 15：只读保护（清 RW 位） ---- */
    if (vmm_pte_set_flags(mm, T_VADDR_A, PTE_RW, 0) != VMM_OK) { rc = 26; goto out; }
    if (vmm_pte_writable(mm, T_VADDR_A)) { rc = 27; goto out; }
    if (vmm_check_access(mm, T_VADDR_A, 1, 1) != VMM_EPERM) { rc = 28; goto out; }
    if (vmm_check_access(mm, T_VADDR_A, 0, 1) != VMM_OK)    { rc = 29; goto out; }
    if (vmm_pte_set_flags(mm, T_VADDR_A, PTE_RW, PTE_RW) != VMM_OK) { rc = 30; goto out; }
    if (!vmm_pte_writable(mm, T_VADDR_A)) { rc = 31; goto out; }

    /* ---- 用例 16：用户位与超级用户位 ---- */
    if (!vmm_pte_user(mm, T_VADDR_A)) { rc = 32; goto out; }
    if (kmm->pgd[0] & PTE_US) { rc = 33; goto out; }   /* 内核恒等映射不得带 US */

    /* ---- 用例 17：解除映射后硬件转换立即失效（TLB 已刷新） ---- */
    fl_before = vmm_tlb_flush_total();
    if (vmm_unmap_page(mm, T_VADDR_A) != VMM_OK) { rc = 34; goto out; }
    if (vmm_translate_hw(T_VADDR_A) != 0u) { rc = 35; goto out; }
    if (vmm_tlb_flush_total() <= fl_before) { rc = 36; goto out; }

    /* ---- 用例 18：页表项与页表页计数 ---- */
    if (vmm_pt_count(mm) == 0) { rc = 37; goto out; }
    if (vmm_pgd_count(mm) == 0) { rc = 38; goto out; }

    /* ---- 用例 19：ASLR 熵源非退化 ---- */
    {
        u32 e1 = vmm_random();
        u32 e2 = vmm_random();
        u32 e3 = vmm_random();
        if (e1 == e2 && e2 == e3) { rc = 39; goto out; }
        vmm_aslr_set(0);
        if (vmm_aslr_mmap_base() != USER_MMAP_BASE) { rc = 40; goto out; }
        vmm_aslr_set(1);
        if (vmm_aslr_mmap_base() < USER_MMAP_BASE) { rc = 41; goto out; }
    }

    /* ---- 用例 20：回收干净页后切回内核地址空间 ---- */
    if (vmm_mm_switch(kmm) != VMM_OK) { rc = 42; goto out; }
    if (vmm_translate_hw(T_VADDR_A) != 0u) { rc = 43; goto out; }  /* 用户映射不共享 */

out:
    if (mm) {
        if (vmm_current_mm() == mm) vmm_mm_switch(kmm);
        if (phys && vmm_pte_present(mm, T_VADDR_A)) vmm_unmap_page_put(mm, T_VADDR_A);
        vmm_mm_destroy(mm);
    }
    if (vmm_current_mm() != kmm) vmm_mm_switch(kmm);
    return rc;
}

/* --------------------------------------------------------------------------
 * 缺页自检：按需分页 / 写时复制 / 栈增长 / 换入 + 拒绝路径
 * ------------------------------------------------------------------------ */
u32 vmm_fault_selftest(void)
{
    vmm_mm_t *kmm, *ma = NULL, *mb = NULL;
    u32 rc = 0;
    u32 base, phys_a, phys_b;
    u32 f_before, r_before;
    u32 stack_addr;
    u32 val;

    kmm = vmm_kernel_mm();
    if (!kmm) return 1;

    ma = vmm_mm_create();
    if (!ma) return 2;
    mb = vmm_mm_create();
    if (!mb) { rc = 3; goto out; }

    vmm_dbg_clear();

    /* ---- 用例 1：按需分页 —— 未映射但落在区域内，读操作应经 #PF 自动补页 ---- */
    base = vmm_mmap(ma, T_VADDR_B, 0x4000u, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    if (base != T_VADDR_B) { rc = 4; goto out; }
    if (vmm_pte_present(ma, T_VADDR_B)) { rc = 5; goto out; }

    if (vmm_mm_switch(ma) != VMM_OK) { rc = 6; goto out; }
    f_before = vmm_fault_count();
    val = *(volatile u32 *)T_VADDR_B;               /* 触发真实缺页 */
    if (val != 0u) { rc = 7; goto out; }            /* 匿名页按需清零 */
    if (vmm_fault_count() != f_before + 1u) { rc = 8; goto out; }
    if (!vmm_pte_present(ma, T_VADDR_B)) { rc = 9; goto out; }

    /* ---- 用例 2：写入按需页 ---- */
    *(volatile u32 *)(T_VADDR_B + 0x100u) = 0xC0FFEE11u;
    if (*(volatile u32 *)(T_VADDR_B + 0x100u) != 0xC0FFEE11u) { rc = 10; goto out; }

    /* ---- 用例 3：缺页日志记录了「按需分配」与正确的故障地址 ---- */
    {
        u32 i, found = 0;
        for (i = 0; i < vmm_fault_log_count(); i++) {
            const vmm_fault_rec_t *rec = vmm_fault_log_at(i);
            if (rec && rec->vaddr == T_VADDR_B &&
                rec->action == VMM_FA_DEMAND && rec->result == VMM_OK) {
                found = 1;
            }
        }
        if (!found) { rc = 11; goto out; }
    }

    /* ---- 用例 4：栈自动增长 —— 触碰栈区下方一页 ---- */
    if (ma->stack_vma == 0xFFFFFFFFu) { rc = 12; goto out; }
    stack_addr = ma->stack_start - PTE_SIZE;
    if (stack_addr < USER_MIN_MAP_ADDR) { rc = 13; goto out; }
    if (vmm_vma_find(ma, stack_addr)) { rc = 14; goto out; }   /* 增长前不属任何区域 */
    {
        u32 g_before = ma->grow_count;
        *(volatile u32 *)stack_addr = 0x5A5A5A5Au;             /* 触发 #PF → 增长 */
        if (*(volatile u32 *)stack_addr != 0x5A5A5A5Au) { rc = 15; goto out; }
        if (ma->grow_count != g_before + 1u) { rc = 16; goto out; }
        if (!vmm_vma_find(ma, stack_addr)) { rc = 17; goto out; }
    }

    /* ---- 用例 5：栈不得越过保护间隙无限扩张 ---- */
    {
        u32 far_addr = ma->stack_start - VMM_STACK_GAP - PTE_SIZE;
        u32 r = vmm_fault_resolve(ma, far_addr, PF_PRESENT | PF_WRITE, 0);
        if (r == VMM_OK) { rc = 18; goto out; }        /* 必须被拒绝 */
    }

    /* ---- 用例 6：写时复制 —— 同一物理页共享给两个地址空间 ---- */
    phys_a = vmm_translate(ma, T_VADDR_B);
    if (!phys_a) { rc = 19; goto out; }
    if (vmm_mmap(mb, T_VADDR_C, 0x1000u, PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, 0, 0) != T_VADDR_C) {
        rc = 20; goto out;
    }
    if (vmm_cow_share(ma, T_VADDR_B, mb, T_VADDR_C) != VMM_OK) { rc = 21; goto out; }
    if (vmm_rmap_count_for(phys_a) < 2u) { rc = 22; goto out; }
    if (vmm_pte_writable(ma, T_VADDR_B)) { rc = 23; goto out; }   /* 共享后转只读 */
    if (!vmm_pte_cow(ma, T_VADDR_B))     { rc = 24; goto out; }

    /* 在 mb 中写入 → 真实 #PF → 复制 → 双方各自独立 */
    if (vmm_mm_switch(mb) != VMM_OK) { rc = 25; goto out; }
    {
        u32 c_before = mb->cow_count;
        *(volatile u32 *)T_VADDR_C = 0xBAD0BAD0u;
        if (*(volatile u32 *)T_VADDR_C != 0xBAD0BAD0u) { rc = 26; goto out; }
        if (mb->cow_count != c_before + 1u) { rc = 27; goto out; }
    }
    phys_b = vmm_translate(mb, T_VADDR_C);
    if (!phys_b) { rc = 28; goto out; }
    if (phys_b == phys_a) { rc = 29; goto out; }              /* 已复制到新物理页 */
    vmm_dbg[0] = phys_a;
    vmm_dbg[1] = *(volatile u32 *)(phys_a + 0x100u);
    vmm_dbg[2] = phys_b;
    vmm_dbg[3] = vmm_pte_get(ma, T_VADDR_B);
    vmm_dbg[4] = vmm_pte_get(mb, T_VADDR_C);
    vmm_dbg[5] = pmm_refcount(phys_a);
    vmm_dbg[6] = pmm_refcount(phys_b);
    vmm_dbg[7] = vmm_rmap_count_for(phys_a);
    /* 数据写在 T_VADDR_B + 0x100，核对原页必须带同一偏移。
     * 原实现漏了 +0x100，在任何正确实现下都必然失败 —— 是断言写错，
     * 不是写时复制有问题（真机现场：val@+0x100 = 0xC0FFEE11，双方引用计数均为 1）。 */
    if (*(volatile u32 *)(phys_a + 0x100u) != 0xC0FFEE11u) { rc = 30; goto out; }

    /* ---- 用例 7：页换出与换入 ---- */
    if (vmm_mm_switch(ma) != VMM_OK) { rc = 31; goto out; }
    {
        u32 s_before = vmm_swap_used_slots();
        if (vmm_swap_out(ma, T_VADDR_B) != VMM_OK) { rc = 32; goto out; }
        if (!vmm_pte_swapped(ma, T_VADDR_B)) { rc = 33; goto out; }
        if (vmm_translate(ma, T_VADDR_B) != 0u) { rc = 34; goto out; }
        if (vmm_swap_used_slots() != s_before + 1u) { rc = 35; goto out; }

        /* 访问已换出的页 → #PF → 换入 → 内容必须与换出前一致 */
        val = *(volatile u32 *)(T_VADDR_B + 0x100u);
        vmm_dbg[0] = val;
        vmm_dbg[1] = vmm_swap_used_slots();
        vmm_dbg[2] = vmm_pte_get(ma, T_VADDR_B);
        vmm_dbg[3] = vmm_translate(ma, T_VADDR_B);
        if (val != 0xC0FFEE11u) { rc = 36; goto out; }
        if (vmm_pte_swapped(ma, T_VADDR_B)) { rc = 37; goto out; }
        if (vmm_swap_used_slots() != s_before) { rc = 38; goto out; }
    }

    /* ---- 用例 8：拒绝路径（直接调用解析器，不在自检中真的打进停机） ---- */
    r_before = vmm_fault_refused_count();

    /* 8.1 空洞访问：既无区域覆盖，也不在栈间隙内 */
    if (vmm_fault_resolve(ma, 0x30000000u, PF_PRESENT | PF_WRITE, 0) == VMM_OK) {
        rc = 39; goto out;
    }
    /* 8.2 用户态访问内核地址 */
    if (vmm_fault_resolve(ma, KERNEL_SPACE_BASE, PF_USER | PF_PRESENT, 0) != VMM_EPERM) {
        rc = 40; goto out;
    }
    /* 8.3 保留位被置位：页表项损坏，不可补页修复 */
    if (vmm_fault_resolve(ma, T_VADDR_B, PF_RSVD | PF_PRESENT, 0) != VMM_ERESVD) {
        rc = 41; goto out;
    }
    /* 8.4 写只读且非 COW 的页：真实权限违例 */
    if (vmm_mmap(ma, T_VADDR_C + 0x100000u, 0x1000u, PROT_READ,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, 0, 0) !=
        (T_VADDR_C + 0x100000u)) {
        rc = 42; goto out;
    }
    if (vmm_populate(ma, T_VADDR_C + 0x100000u, 0x1000u) != VMM_OK) { rc = 43; goto out; }
    if (vmm_fault_resolve(ma, T_VADDR_C + 0x100000u, PF_PRESENT | PF_WRITE, 0) != VMM_EPERM) {
        rc = 44; goto out;
    }
    /* 8.5 内核态访问内核区却缺页：内核映射缺失，属真实缺陷 */
    if (vmm_fault_resolve(ma, 0xF0000000u, PF_PRESENT, 0) != VMM_EFAULT) {
        rc = 45; goto out;
    }
    if (vmm_fault_refused_count() <= r_before) { rc = 46; goto out; }

out:
    if (vmm_current_mm() != kmm) vmm_mm_switch(kmm);
    if (ma) vmm_mm_destroy(ma);
    if (mb) vmm_mm_destroy(mb);
    if (vmm_current_mm() != kmm) vmm_mm_switch(kmm);
    return rc;
}

/* --------------------------------------------------------------------------
 * 扩展自检：反向映射、页缓存回写、保护位、回收、地址空间销毁
 * ------------------------------------------------------------------------ */
u32 vmm_ext_selftest(void)
{
    vmm_mm_t *kmm, *mm = NULL;
    u32 rc = 0;
    u32 base, phys;
    u32 free_before, free_after;

    kmm = vmm_kernel_mm();
    if (!kmm) return 1;

    mm = vmm_mm_create();
    if (!mm) return 2;

    vmm_dbg_clear();

    /* ---- 用例 1：匿名映射 + 填充 + 反向映射计数 ---- */
    base = vmm_mmap(mm, T_VADDR_A, 0x3000u, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, 0, 0);
    if (base != T_VADDR_A) { rc = 3; goto out; }
    if (mm->total_vm != 3u) { rc = 4; goto out; }

    phys = vmm_translate(mm, T_VADDR_A);
    if (!phys) { rc = 5; goto out; }
    if (vmm_rmap_count_for(phys) != 1u) { rc = 6; goto out; }
    if (vmm_rmap_total_for_phys(phys) != 1u) { rc = 7; goto out; }

    /* 反向映射必须能反查到具体的地址空间与虚拟地址 */
    {
        u32 i, found = 0;
        for (i = 0; i < VMM_MAX_RMAP; i++) {
            u32 rp, rid, rv;
            if (vmm_rmap_get(i, &rp, &rid, &rv) != VMM_OK) continue;
            if (rp == phys && rid == mm->id && rv == T_VADDR_A) found = 1;
        }
        if (!found) { rc = 8; goto out; }
    }

    /* ---- 用例 2：mprotect 真实改权限 ---- */
    if (vmm_mprotect(mm, T_VADDR_A, 0x3000u, PROT_READ) != VMM_OK) { rc = 9; goto out; }
    if (vmm_pte_writable(mm, T_VADDR_A)) { rc = 10; goto out; }
    if (vmm_pte_writable(mm, T_VADDR_A + 0x2000u)) { rc = 11; goto out; }
    if (vmm_mprotect(mm, T_VADDR_A, 0x3000u, PROT_READ | PROT_WRITE) != VMM_OK) { rc = 12; goto out; }
    if (!vmm_pte_writable(mm, T_VADDR_A)) { rc = 13; goto out; }

    /* ---- 用例 3：访问位 / 脏位扫描 ---- */
    if (vmm_mm_switch(mm) != VMM_OK) { rc = 14; goto out; }
    *(volatile u32 *)T_VADDR_A = 0x11223344u;
    {
        u32 acc = 0, dty = 0;
        if (vmm_scan_ad(mm, T_VADDR_A, 0x3000u, &acc, &dty) != VMM_OK) { rc = 15; goto out; }
        if (acc == 0) { rc = 16; goto out; }
        if (dty == 0) { rc = 17; goto out; }
    }
    if (vmm_mm_switch(kmm) != VMM_OK) { rc = 18; goto out; }

    /* ---- 用例 4：文件页与页缓存回写（真实写盘 → 丢弃 → 重读一致） ---- */
    if (vmm_pgcache_selftest() != 0) { rc = 19; goto out; }

    /* ---- 用例 5：文件映射按需从页缓存取页 ---- */
    {
        u32 fb = 0x30000000u;
        const vmm_backing_t *bk = vmm_backing_at(1);
        if (!bk) { rc = 20; goto out; }
        if (vmm_mmap(mm, fb, PTE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_FIXED | MAP_FILE | MAP_PRIVATE, 1, 0) != fb) {
            rc = 21; goto out;
        }
        if (vmm_populate(mm, fb, PTE_SIZE) != VMM_OK) { rc = 22; goto out; }
        if (vmm_translate(mm, fb) == 0u) { rc = 23; goto out; }
        if (mm->file_vm == 0u) { rc = 24; goto out; }
    }

    /* ---- 用例 6：回收 —— shrinker 已注册且能真正回收出页 ---- */
    free_before = pmm_free_page_count();
    {
        u32 freed = vmm_reclaim(4);
        free_after = pmm_free_page_count();
        if (vmm_shrinker_calls() == 0) { rc = 25; goto out; }
        if (freed == 0 && free_after <= free_before) { rc = 26; goto out; }
    }

    /* ---- 用例 7：销毁地址空间必须把页与页表全部还回 PMM ---- */
    free_before = pmm_free_page_count();
    if (vmm_mm_destroy(mm) != VMM_OK) { rc = 27; goto out; }
    mm = NULL;
    free_after = pmm_free_page_count();
    if (free_after <= free_before) { rc = 28; goto out; }

    /* ---- 用例 8：内核地址空间不可被销毁 ---- */
    if (vmm_mm_destroy(kmm) == VMM_OK) { rc = 29; goto out; }

    /* ---- 用例 9：统计口径自洽 ---- */
    {
        vmm_stats_t st;
        vmm_stats(&st);
        if (st.enabled != 1u) { rc = 30; goto out; }
        if (st.kernel_pgd_shared == 0u) { rc = 31; goto out; }
        if (st.fault_total == 0u) { rc = 32; goto out; }
        if (st.fault_resolved == 0u) { rc = 33; goto out; }
        if (st.swap_out_total == 0u || st.swap_in_total == 0u) { rc = 34; goto out; }
        if (st.cow_total == 0u) { rc = 35; goto out; }
        if (st.grow_total == 0u) { rc = 36; goto out; }
    }

    /* ---- 用例 10-13：05 册第二轮（并发锁 / 换页调度器 / 栈增长上限） ---- */
    {
        vmm_mm_t *m2 = vmm_mm_create();
        if (!m2) { rc = 37; goto out; }

        /* 用例 10：全局锁必须被真实使用（管理入口均走临界区） */
        if (vmm_lock_calls() == 0) { rc = 38; goto out; }

        /* 用例 11：换页调度器 —— 时钟候选选择 + 页面置换 + 策略切换 + 统计 */
        if (vmm_mmap(m2, 0x50000000u, PTE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, 0, 0)
            != 0x50000000u) { rc = 39; goto out; }
        {
            u32 cand = 0, dirty = 0;
            if (vmm_swap_select_clock(m2, &cand, &dirty) != VMM_OK) { rc = 40; goto out; }
            if (vmm_page_replace(m2, cand) != VMM_OK) { rc = 41; goto out; }
            if (vmm_swap_clock_reclaimed() == 0) { rc = 42; goto out; }
            if (vmm_page_fault_rate_x1000() > 1000u) { rc = 43; goto out; }
            vmm_swap_policy_set(VMM_SWAP_POLICY_FIFO);
            if (vmm_swap_policy() != VMM_SWAP_POLICY_FIFO) { rc = 44; goto out; }
            vmm_swap_policy_set(VMM_SWAP_POLICY_CLOCK);
            if (vmm_swap_policy() != VMM_SWAP_POLICY_CLOCK) { rc = 45; goto out; }
        }

        /* 用例 12：栈增长次数上限 —— grow_count 置顶后必须拒绝继续增长 */
        if (m2->stack_vma != 0xFFFFFFFFu) {
            m2->grow_count = VMM_STACK_GROW_MAX;
            if (vmm_expand_stack(m2, m2->stack_start - 0x1000u) != VMM_ELIMIT) {
                rc = 46; goto out;
            }
            m2->grow_count = 0;
        }

        if (vmm_mm_destroy(m2) != VMM_OK) { rc = 47; goto out; }
    }

out:
    if (mm) {
        if (vmm_current_mm() == mm) vmm_mm_switch(kmm);
        vmm_mm_destroy(mm);
    }
    if (vmm_current_mm() != kmm) vmm_mm_switch(kmm);
    return rc;
}
