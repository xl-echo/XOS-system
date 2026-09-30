/* XOS Stage2 链接脚本：加载地址 0x7E00 */
OUTPUT_FORMAT(pei-i386)
ENTRY(_start)
SECTIONS
{
    . = 0x7E00;

    .text : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.reloc)
        *(.idata)
        *(.debug*)
        *(.drectve)
        *(.bss)
        *(COMMON)
    }
}
