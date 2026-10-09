/* XOS 内核链接脚本：加载地址 0x10000，平坦二进制输出
 * 导出 __bss_start / __bss_end 供启动代码清零 .bss
 */
OUTPUT_FORMAT(pei-i386)
ENTRY(_kmain)
SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rdata)
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    /* .bss 固定定位到 0x200000：避开 0xA0000-0xC0000 VGA/ROM 区（此前
     * .bss 从 0x98000 增长会跨入显存区，memset 写显存导致启动卡死）。
     * pmm 保留 [0x10000, 0x300000) 覆盖 .text/.rodata/.data/.bss 全区。
     * flat 镜像不含 .bss（NOBITS），stage2 按 __bss_start/__bss_end 清零。 */
    . = 0x200000;
    PROVIDE(__bss_start = .);
    PROVIDE(___bss_start = .);
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    PROVIDE(__bss_end = .);
    PROVIDE(___bss_end = .);
    PROVIDE(__kernel_end = .);
    PROVIDE(___kernel_end = .);

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.reloc)
        *(.idata)
        *(.debug*)
        *(.drectve)
    }
}
