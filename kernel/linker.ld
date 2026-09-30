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

    . = ALIGN(16);
    /* 同时导出带 PE 下划线前缀的别名：汇编侧用 __bss_start，
     * C 侧（i386-pe 目标）引用会变成 ___bss_start，两者都要可见。 */
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
