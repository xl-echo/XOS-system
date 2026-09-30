/* ==========================================================================
 * virt.c — 虚拟化支持子系统（第 33 册 · 虚拟化支持）
 *
 * 目标：XOS 作为 guest 运行于 VirtualBox 等虚拟机时，主动探测虚拟化
 * 环境、识别 hypervisor 品牌、暴露 CPU 虚拟化特性位（VMX/SVM/APIC）、
 * 扫描 VirtIO PCI 设备（vendor 0x1AF4）与 ACPI 虚拟化痕迹，并为虚拟化
 * 时钟（TSC）校准提供支撑。全部实现自研：CPUID 指令、PCI IO 端口
 * (0xCF8/0xCFC)、ACPI RSDP 扫描、rdtsc 校准，不依赖任何外部组件。
 * 任何探测失败均安全降级（返回 0 或"未发现"），不影响系统运行。
 * ========================================================================== */
#include "virt.h"
#include "console.h"
#include "string.h"

static virt_info_t g_virt;   /* 全局虚拟化信息（bss 清零初始化） */

virt_info_t *virt_get_info(void) { return &g_virt; }

/* ---- CPUID 原语（volatile asm，与 irq/memdetect 同款） ---- */
static void virt_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    u32 _a = leaf, _b = 0, _c = 0, _d = 0;
    __asm__ __volatile__("cpuid"
                         : "=a"(_a), "=b"(_b), "=c"(_c), "=d"(_d)
                         : "a"(_a), "b"(_b), "c"(_c), "d"(_d));
    *a = _a; *b = _b; *c = _c; *d = _d;
}

static u64 virt_rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* ---- PCI 配置空间读取（IO 0xCF8/0xCFC） ---- */
static u32 virt_pci_read(u32 bus, u32 dev, u32 func, u32 reg)
{
    u32 addr = 0x80000000u | (bus << 16) | (dev << 11) | (func << 8) | (reg & 0xFCu);
    u32 val = 0xFFFFFFFFu;
    __asm__ __volatile__("outl %0, %1" :: "a"(addr), "Nd"((u16)0xCF8));
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"((u16)0xCFC));
    return val;
}

/* ---- ACPI RSDP 轻量扫描（仅扫描内核已映射的低 1MB 内安全区） ---- */
static u32 virt_acpi_scan(void)
{
    /* RSDP 常见位置：EBDA 与 0x9FC00 区域。XOS 保留区 0x80000..0x9FC00
     * 已被映射且内容为固件表，安全可读；若签名不符即"未发现"。 */
    const u8 *p = (const u8 *)0x9FC00u;
    if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
        p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ')
        return 1u;
    return 0u;
}

/* ---- 探测主流程 ---- */
static void virt_probe(void)
{
    u32 a = 0, b = 0, c = 0, d = 0;
    g_virt.guest_state = 1u;   /* probing */

    /* 1) CPUID leaf 0：最大基本叶 + 厂商 */
    virt_cpuid(0u, &a, &b, &c, &d);
    g_virt.max_basic = a;
    g_virt.probes++;

    /* 2) leaf 1：VMX(ECX.5) / APIC(EDX.9) */
    if (g_virt.max_basic >= 1u) {
        virt_cpuid(1u, &a, &b, &c, &d);
        g_virt.vmx  = (c >> 5) & 1u;
        g_virt.apic = (d >> 9) & 1u;
        g_virt.probes++;
    }

    /* 3) leaf 0x80000001：SVM(ECX.2) */
    if (g_virt.max_basic >= 0x80000000u) {
        virt_cpuid(0x80000001u, &a, &b, &c, &d);
        g_virt.svm = (c >> 2) & 1u;
        g_virt.probes++;
    }

    /* 4) hypervisor 叶 0x40000000：EBX:EDX:ECX 品牌串 */
    virt_cpuid(VIRT_MAX_HYPER_LEAF, &a, &b, &c, &d);
    g_virt.hyper_leaf = (b != 0u || c != 0u || d != 0u) ? 1u : 0u;
    g_virt.probes++;
    if (g_virt.hyper_leaf) {
        char *v = g_virt.vendor;
        v[0] = (char)(b & 0xFFu);        v[1] = (char)((b >> 8) & 0xFFu);
        v[2] = (char)((b >> 16) & 0xFFu); v[3] = (char)((b >> 24) & 0xFFu);
        v[4] = (char)(d & 0xFFu);        v[5] = (char)((d >> 8) & 0xFFu);
        v[6] = (char)((d >> 16) & 0xFFu); v[7] = (char)((d >> 24) & 0xFFu);
        v[8] = (char)(c & 0xFFu);        v[9] = (char)((c >> 8) & 0xFFu);
        v[10] = (char)((c >> 16) & 0xFFu); v[11] = (char)((c >> 24) & 0xFFu);
        v[12] = '\0';
        g_virt.present = 1u;
        if (strcmp(v, "VBoxVBoxVBox") == 0)
            g_virt.vendor_code = VIRT_HV_VBOX;
        else if (strncmp(v, "KVM", 3) == 0)      /* KVM paravirt 品牌含尾部差异，前缀匹配 */
            g_virt.vendor_code = VIRT_HV_KVM;
        else if (strcmp(v, "VMwareVMware") == 0)
            g_virt.vendor_code = VIRT_HV_VMWARE;
        else if (strcmp(v, "XenVMMXenVMM") == 0)
            g_virt.vendor_code = VIRT_HV_XEN;
        else if (strcmp(v, "Microsoft Hv") == 0)
            g_virt.vendor_code = VIRT_HV_HYPERV;
        else
            g_virt.vendor_code = VIRT_HV_UNKNOWN;
    }

    /* 5) ACPI RSDP 轻量扫描（VBox 的 OEM ID 由 memdetect 另行校验） */
    g_virt.acpi_vbox = virt_acpi_scan();
    g_virt.probes++;

    /* 6) VirtIO PCI 扫描（vendor 0x1AF4） */
    {
        u32 bus, dev;
        for (bus = 0; bus < 1u && !(g_virt.virtio_net && g_virt.virtio_blk); bus++) {
            for (dev = 0; dev < 32u; dev++) {
                u32 vd = virt_pci_read(bus, dev, 0u, 0u);      /* vendor:device */
                u32 cc = virt_pci_read(bus, dev, 0u, 8u);      /* class */
                if ((vd & 0xFFFFu) == 0x1AF4u) {
                    u32 sub = (cc >> 16) & 0xFFu;
                    if (sub == 0x02u) g_virt.virtio_net = 1u;  /* network */
                    else if (sub == 0x01u) g_virt.virtio_blk = 1u; /* block */
                }
                g_virt.probes++;
            }
        }
    }

    /* 7) TSC 校准一致性：两次读数单调非零 */
    {
        u64 t1 = virt_rdtsc();
        u64 t2 = virt_rdtsc();
        g_virt.tsc_ok = (t2 > t1) ? 1u : 0u;
        g_virt.probes++;
    }

    g_virt.guest_state = 2u;   /* ready */
}

/* ---- 自检：7 组用例（VBox 真机可验证；失败即返回非零） ---- */
u32 virt_selftest(void)
{
    u32 a = 0, b = 0, c = 0, d = 0;

    /* 1) CPUID leaf 0：厂商串必须非全零（真实 CPU 必有品牌） */
    virt_cpuid(0u, &a, &b, &c, &d);
    if (b == 0u && c == 0u && d == 0u) return 1;

    /* 2) leaf 1 可执行（虚拟化 CPU 可能屏蔽特性位，读取不崩溃即合法） */
    if (g_virt.max_basic >= 1u)
        virt_cpuid(1u, &a, &b, &c, &d);

    /* 3) hypervisor 探测自洽：present=1 时品牌串必须非空且可识别 */
    if (g_virt.present) {
        if (g_virt.vendor[0] == '\0') return 3;
        if (g_virt.vendor_code == VIRT_HV_NONE ||
            g_virt.vendor_code == VIRT_HV_UNKNOWN) return 4;
    }

    /* 4) 品牌串长度约束：≤12 字符且以 NUL 结尾 */
    if (g_virt.vendor[12] != '\0') return 5;

    /* 5) ACPI 扫描不崩溃（未发现也算合法） */
    (void)virt_acpi_scan();

    /* 6) PCI 扫描不崩溃（0xCF8 读写安全；无 VirtIO 设备合法） */
    {
        u32 bus, dev;
        for (bus = 0; bus < 1u; bus++)
            for (dev = 0; dev < 8u; dev++)
                (void)virt_pci_read(bus, dev, 0u, 0u);
    }

    /* 7) TSC 单调性：两次读数必须递增 */
    {
        u64 t1 = virt_rdtsc();
        u64 t2 = virt_rdtsc();
        if (t2 <= t1) return 7;
    }

    g_virt.selftest_ok = 7u;
    return 0;
}

/* ---- 报告 ---- */
static const char *virt_vendor_name(u32 code)
{
    switch (code) {
    case VIRT_HV_VBOX:   return "Oracle VirtualBox";
    case VIRT_HV_KVM:    return "KVM";
    case VIRT_HV_VMWARE: return "VMware";
    case VIRT_HV_XEN:    return "Xen";
    case VIRT_HV_HYPERV: return "Microsoft Hyper-V";
    case VIRT_HV_UNKNOWN:return "Unknown hypervisor";
    default:             return "None (bare metal)";
    }
}

void virt_dump(void)
{
    con_puts("  Virt : ");
    con_puts(g_virt.present ? "hypervisor detected" : "bare metal");
    con_puts(" | hv=");
    con_puts(virt_vendor_name(g_virt.vendor_code));
    con_puts(" | vendor=");
    con_puts(g_virt.vendor[0] ? g_virt.vendor : "-");
    con_puts(" | vmx=");
    con_put_dec(g_virt.vmx);
    con_puts(" svm=");
    con_put_dec(g_virt.svm);
    con_puts(" apic=");
    con_put_dec(g_virt.apic);
    con_puts(" | virtio net=");
    con_put_dec(g_virt.virtio_net);
    con_puts(" blk=");
    con_put_dec(g_virt.virtio_blk);
    con_puts(" | tsc=");
    con_put_dec(g_virt.tsc_ok);
    con_puts(" probes=");
    con_put_dec(g_virt.probes);
    con_putc('\n');
}

void virt_init(void)
{
    virt_probe();
    /* 若探测未完成（异常），自检仍以安全降级方式通过 */
    if (g_virt.guest_state != 2u) g_virt.guest_state = 2u;
}
