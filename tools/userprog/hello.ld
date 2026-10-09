/* XOS 用户程序链接脚本：加载到用户空间基址 0x08000000 */
ENTRY(_start)

SECTIONS
{
    . = 0x08000000;

    .text : {
        *(.text)
        *(.rodata)
    }
    . = ALIGN(4);
    .data : {
        *(.data)
    }
    . = ALIGN(4);
    .bss : {
        *(.bss)
        *(COMMON)
    }
    . = ALIGN(4096);
    _end = .;
}
