/* XOS MBR 链接脚本：输出严格 512 字节引导扇区，加载地址 0x7C00 */
OUTPUT_FORMAT(pei-i386)
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;

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
