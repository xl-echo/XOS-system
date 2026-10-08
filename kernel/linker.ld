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

    /* 大 .bss（位图/索引/堆等）不得落在 0xA0000-0xBFFFF VGA/BIOS 保留区：
     * 原布局 .bss 终点 0xA0520 已越界，static 变量写 VGA 区读回垃圾。
     * 将 .bss 整体移至 1MB 之上的常规 RAM（0x100000 起），
     * 由 pmm 保留 [0x100000, 0x140000)；flat 镜像不含 .bss（NOBITS），
     * stage2 按 __bss_start/__bss_end 清零。 */
    . = 0x100000;
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
