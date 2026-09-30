/* ============================================================================
 * XOS VGA 文本控制台
 * 功能点：VGA 文本模式初始化、字符输出、滚动、光标控制、颜色、
 *         数值格式化输出（十六进制/十进制，含 64 位）
 * ============================================================================ */
#ifndef __XOS_CONSOLE_H__
#define __XOS_CONSOLE_H__

#include "types.h"

#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define VGA_MEM_BASE    0xB8000u

/* VGA 颜色：前景色 */
#define VGA_BLACK       0
#define VGA_BLUE        1
#define VGA_GREEN       2
#define VGA_CYAN        3
#define VGA_RED         4
#define VGA_MAGENTA     5
#define VGA_BROWN       6
#define VGA_LIGHTGRAY   7
#define VGA_DARKGRAY    8
#define VGA_LIGHTBLUE   9
#define VGA_LIGHTGREEN  10
#define VGA_LIGHTCYAN   11
#define VGA_LIGHTRED    12
#define VGA_LIGHTMAGENTA 13
#define VGA_YELLOW      14
#define VGA_WHITE       15

/* 默认属性：黑底浅灰字 */
#define VGA_ATTR_DEFAULT 0x07u

void con_init(void);
void con_clear(void);
void con_set_color(u8 fg, u8 bg);
u8   con_get_attr(void);
void con_set_default_attr(u8 attr);

void con_putc(char c);
void con_puts(const char *s);
void con_scroll(void);

/* 数值输出 */
void con_put_hex(u32 v, u32 digits);
void con_put_hex32(u32 v);
void con_put_hex16(u16 v);
void con_put_hex8(u8 v);
void con_put_hex64(u64 v);
void con_put_dec(u32 v);
void con_put_dec64(u64 v);

/* 光标 */
void con_set_cursor(u32 row, u32 col);
void con_update_hw_cursor(void);     /* 立即写硬件光标 */
void con_flush(void);                /* 若位置有变动则写硬件光标 */
u32  con_get_row(void);
u32  con_get_col(void);

void con_printf(const char *fmt, ...);

#endif /* __XOS_CONSOLE_H__ */
