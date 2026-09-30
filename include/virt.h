#ifndef XOS_VIRT_H
#define XOS_VIRT_H

#include "types.h"

/* ---- 虚拟化支持子系统（第 33 册）----
 * 目标：作为 guest 运行于 VirtualBox 等虚拟机时，主动探测虚拟化环境、
 * 识别 hypervisor 品牌、暴露 CPU 虚拟化特性（VMX/SVM 位）、探测 VirtIO
 * 设备与 ACPI 虚拟化痕迹，并为虚拟化时钟/TSC 校准提供支撑。
 * 全部逻辑自研（CPUID 指令 + 内存/IO 探测），不依赖任何外部组件。
 */

#define VIRT_MAX_HYPER_LEAF  0x40000000u
#define VIRT_VENDOR_LEN      13u

typedef struct {
    u32  present;          /* 1 = 检测到 hypervisor 环境 */
    u32  max_basic;        /* CPUID leaf 0 返回的最大基本叶 */
    u32  vmx;              /* CPUID.1:ECX[5] VMX 支持位 */
    u32  svm;              /* CPUID.80000001h:ECX[2] SVM 支持位 */
    u32  apic;             /* CPUID.1:EDX[9] APIC 位 */
    u32  hyper_leaf;       /* 0x40000000 叶是否可用 */
    u32  vendor_code;      /* hypervisor 品牌枚举 */
    char vendor[VIRT_VENDOR_LEN]; /* 品牌串（"VBoxVBoxVBox" 等） */
    u32  acpi_vbox;        /* ACPI OEM ID = "VBOX " 检测 */
    u32  virtio_net;       /* PCI vendor 1AF4 网卡探测 */
    u32  virtio_blk;       /* PCI vendor 1AF4 块设备探测 */
    u32  tsc_ok;           /* TSC 可用且校准一致 */
    u32  guest_state;      /* 状态机：0=raw 1=probe 2=ready */
    u32  probes;           /* 探测计数 */
    u32  selftest_ok;      /* 自检通过数 */
} virt_info_t;

/* 品牌枚举 */
#define VIRT_HV_NONE   0u
#define VIRT_HV_VBOX   1u
#define VIRT_HV_KVM    2u
#define VIRT_HV_VMWARE 3u
#define VIRT_HV_XEN    4u
#define VIRT_HV_HYPERV 5u
#define VIRT_HV_UNKNOWN 6u

void        virt_init(void);
void        virt_dump(void);
u32         virt_selftest(void);
virt_info_t *virt_get_info(void);

#endif
