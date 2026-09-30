/* ============================================================================
 * XOS VGA 文本控制台实现
 *
 * 本版修复（针对审查发现的缺陷）：
 *   1) con_printf 的 %l 分支原先按 u32/i32 取参，而调用方传入的是 u64，
 *      导致可变参数栈错位（后续所有参数全部读错）。现按 u64 正确取参，
 *      并用「移位-减法」实现 64 位十进制，不依赖 libgcc 的 __udivdi3。
 *   2) con_put_hex 未限制 digits 上界，digits>8 时移位量 >=32 属未定义行为。
 *   3) 原实现在每个字符后都做 4 次端口 I/O 同步光标（80 字符 = 320 次），
 *      现改为脏标记 + 行边界/接口出口批量刷新。
 *   4) '\b' 在行首时原实现直接失效，现回退到上一行行尾。
 *   5) con_scroll 清行原使用当前前景色，滚屏后会残留上一次输出颜色，
 *      现统一使用 default_attr。
 *   6) 新增 %X、宽度、零填充、%lu/%ld/%llu/%lld 支持。
 * ============================================================================ */
#include "console.h"
#include "string.h"
#include "stdarg.h"

static u16 *const vga = (u16 *)VGA_MEM_BASE;
static u32 cur_row = 0;
static u32 cur_col = 0;
static u8  cur_attr = VGA_ATTR_DEFAULT;
static u8  default_attr = VGA_ATTR_DEFAULT;
static bool cursor_dirty = false;

/* --------------------------------------------------------------------------
 * 端口 I/O
 * ------------------------------------------------------------------------ */
static inline void outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* --------------------------------------------------------------------------
 * 串口（COM1）调试输出
 * 内核控制台走双路：VGA 负责屏幕呈现，串口负责完整日志取证。
 * 115200 8N1，与 Stage2 早期输出共用同一通道 —— 从 MBR 到内核停机，
 * 全程日志在串口日志里连续无缺失（屏幕只有 25 行，滚屏会冲掉中间现场）。
 * ------------------------------------------------------------------------ */
#define SERIAL_COM1 0x3F8u

static inline void serial_putc(char c)
{
    u32 i;
    for (i = 0; i < 10000u; i++) {          /* 等待发送保持寄存器空（LSR.THRE） */
        if (inb(SERIAL_COM1 + 5) & 0x20u) break;
    }
    outb(SERIAL_COM1, (u8)c);
}

static void serial_puts_echo(char c)
{
    if (c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    } else {
        serial_putc(c);
    }
}

static void hw_write_cursor(u16 pos)
{
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

void con_update_hw_cursor(void)
{
    hw_write_cursor((u16)(cur_row * VGA_WIDTH + cur_col));
    cursor_dirty = false;
}

void con_flush(void)
{
    if (!cursor_dirty) return;
    hw_write_cursor((u16)(cur_row * VGA_WIDTH + cur_col));
    cursor_dirty = false;
}

void con_set_cursor(u32 row, u32 col)
{
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col >= VGA_WIDTH)  col = VGA_WIDTH - 1;
    cur_row = row;
    cur_col = col;
    con_update_hw_cursor();
}

u32 con_get_row(void) { return cur_row; }
u32 con_get_col(void) { return cur_col; }

void con_set_color(u8 fg, u8 bg)
{
    cur_attr = (u8)((bg << 4) | (fg & 0x0F));
}

u8 con_get_attr(void) { return cur_attr; }

void con_set_default_attr(u8 attr) { default_attr = attr; }

/* --------------------------------------------------------------------------
 * 清屏
 * ------------------------------------------------------------------------ */
void con_clear(void)
{
    u32 i;
    for (i = 0; i < (u32)(VGA_WIDTH * VGA_HEIGHT); i++) {
        vga[i] = (u16)(((u16)default_attr << 8) | ' ');
    }
    cur_row = 0;
    cur_col = 0;
    con_update_hw_cursor();
}

void con_init(void)
{
    cur_attr = VGA_ATTR_DEFAULT;
    default_attr = VGA_ATTR_DEFAULT;
    con_clear();
}

/* --------------------------------------------------------------------------
 * 滚动：整屏上移一行，末行用默认属性填充
 * ------------------------------------------------------------------------ */
void con_scroll(void)
{
    u32 i;
    for (i = 0; i < (u32)(VGA_WIDTH * (VGA_HEIGHT - 1)); i++) {
        vga[i] = vga[i + VGA_WIDTH];
    }
    for (i = (u32)(VGA_WIDTH * (VGA_HEIGHT - 1));
         i < (u32)(VGA_WIDTH * VGA_HEIGHT); i++) {
        vga[i] = (u16)(((u16)default_attr << 8) | ' ');
    }
    cur_row = VGA_HEIGHT - 1;
}

/* --------------------------------------------------------------------------
 * 输出单个字符
 * ------------------------------------------------------------------------ */
void con_putc(char c)
{
    serial_puts_echo(c);   /* 串口镜像：完整启动日志取证 */

    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\t') {
        cur_col = (cur_col + 4) & ~3u;
        if (cur_col >= VGA_WIDTH) { cur_col = 0; cur_row++; }
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
        } else if (cur_row > 0) {
            cur_row--;
            cur_col = VGA_WIDTH - 1;
        }
        vga[cur_row * VGA_WIDTH + cur_col] =
            (u16)(((u16)cur_attr << 8) | ' ');
    } else if (c == '\0') {
        /* 忽略，避免把 NUL 写进显存 */
        return;
    } else {
        vga[cur_row * VGA_WIDTH + cur_col] =
            (u16)(((u16)cur_attr << 8) | (u8)c);
        cur_col++;
        if (cur_col >= VGA_WIDTH) { cur_col = 0; cur_row++; }
    }

    if (cur_row >= VGA_HEIGHT) {
        con_scroll();
    }

    cursor_dirty = true;
    /* 行边界是天然刷新点，避免每个字符都做端口 I/O */
    if (c == '\n' || c == '\r') {
        con_flush();
    }
}

void con_puts(const char *s)
{
    if (!s) return;
    while (*s) con_putc(*s++);
    con_flush();
}

/* --------------------------------------------------------------------------
 * 数值输出
 * ------------------------------------------------------------------------ */
void con_put_hex(u32 v, u32 digits)
{
    static const char hex[] = "0123456789ABCDEF";
    i32 i;
    if (digits > 8) digits = 8;          /* 防止移位量 >= 32 的未定义行为 */
    for (i = (i32)digits - 1; i >= 0; i--) {
        con_putc(hex[(v >> ((u32)i * 4u)) & 0xFu]);
    }
}

void con_put_hex32(u32 v) { con_puts("0x"); con_put_hex(v, 8); con_flush(); }
void con_put_hex16(u16 v) { con_puts("0x"); con_put_hex(v, 4); con_flush(); }
void con_put_hex8(u8 v)   { con_puts("0x"); con_put_hex(v, 2); con_flush(); }

void con_put_hex64(u64 v)
{
    con_puts("0x");
    con_put_hex((u32)(v >> 32), 8);
    con_put_hex((u32)(v & 0xFFFFFFFFu), 8);
    con_flush();
}

void con_put_dec(u32 v)
{
    char buf[12];
    utoa(v, buf, 10);
    con_puts(buf);
    con_flush();
}

/* 64 位十进制：用「移位-减法」逐位求商，避免 64 位除法引入 __udivdi3 */
void con_put_dec64(u64 v)
{
    static const u64 p10[20] = {
        1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL,
        1000000ULL, 10000000ULL, 100000000ULL, 1000000000ULL,
        10000000000ULL, 100000000000ULL, 1000000000000ULL,
        10000000000000ULL, 100000000000000ULL, 1000000000000000ULL,
        10000000000000000ULL, 100000000000000000ULL,
        1000000000000000000ULL, 10000000000000000000ULL
    };
    int i;
    bool started = false;

    for (i = 19; i >= 0; i--) {
        u32 d = 0;
        while (v >= p10[i]) { v -= p10[i]; d++; }
        if (d != 0 || started || i == 0) {
            con_putc((char)('0' + (char)d));
            started = true;
        }
    }
    con_flush();
}

/* --------------------------------------------------------------------------
 * printf 内部辅助
 * ------------------------------------------------------------------------ */
static void put_hex_digits(char *out, u32 v, u32 digits)
{
    static const char hex[] = "0123456789ABCDEF";
    u32 i;
    if (digits > 8) digits = 8;
    for (i = 0; i < digits; i++) {
        out[i] = hex[(v >> ((digits - 1u - i) * 4u)) & 0xFu];
    }
    out[digits] = '\0';
}

/* 按宽度输出十六进制，去掉前导零但保留至少一位 */
static void emit_hex(u32 v, u32 width, bool zero, bool prefix)
{
    char full[9];
    u32 start = 8, need, total, pad;

    put_hex_digits(full, v, 8);

    if (v == 0) {
        start = 7;
    } else {
        u32 i;
        for (i = 0; i < 8; i++) {
            if (full[i] != '0') { start = i; break; }
        }
    }

    need  = 8u - start;
    total = need + (prefix ? 2u : 0u);
    pad   = (width > total) ? (width - total) : 0u;

    if (prefix) con_puts("0x");
    while (pad--) con_putc(zero ? '0' : ' ');
    con_puts(full + start);
}

static void emit_dec(u64 v, bool neg, u32 width, bool zero)
{
    char buf[24];
    u32 len, pad;
    char *p = buf;

    if (neg) *p++ = '-';

    /* 用移位-减法生成十进制串 */
    {
        static const u64 p10[20] = {
            1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL,
            1000000ULL, 10000000ULL, 100000000ULL, 1000000000ULL,
            10000000000ULL, 100000000000ULL, 1000000000000ULL,
            10000000000000ULL, 100000000000000ULL, 1000000000000000ULL,
            10000000000000000ULL, 100000000000000000ULL,
            1000000000000000000ULL, 10000000000000000000ULL
        };
        int i;
        bool started = false;
        for (i = 19; i >= 0; i--) {
            u32 d = 0;
            while (v >= p10[i]) { v -= p10[i]; d++; }
            if (d != 0 || started || i == 0) {
                *p++ = (char)('0' + (char)d);
                started = true;
            }
        }
    }
    *p = '\0';

    len = strlen(buf);
    pad = (width > len) ? (width - len) : 0u;
    if (zero && pad) {
        /* 零填充时负号在最前 */
        if (buf[0] == '-') {
            con_putc('-');
            while (pad--) con_putc('0');
            con_puts(buf + 1);
            return;
        }
        while (pad--) con_putc('0');
        con_puts(buf);
        return;
    }
    while (pad--) con_putc(' ');
    con_puts(buf);
}

static void emit_str(const char *s, u32 width)
{
    u32 len, pad;
    if (!s) s = "(null)";
    len = strlen(s);
    pad = (width > len) ? (width - len) : 0u;
    while (pad--) con_putc(' ');
    con_puts(s);
}

/* --------------------------------------------------------------------------
 * printf 子集
 *   支持：%s %c %d %i %u %x %X %p %%  以及宽度、零填充、l/ll 长度修饰
 * ------------------------------------------------------------------------ */
void con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        u32 width;
        bool zero;
        bool is_long;

        if (*fmt != '%') {
            con_putc(*fmt++);
            continue;
        }
        fmt++;

        /* ---- 解析标志与宽度 ---- */
        zero = false;
        if (*fmt == '0') { zero = true; fmt++; }
        width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (u32)(*fmt - '0');
            fmt++;
        }

        /* ---- 解析长度修饰符 ---- */
        is_long = false;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l') is_long = true;
            fmt++;
        }

        /* ---- 转换字符 ---- */
        switch (*fmt) {
        case 's':
            emit_str(va_arg(ap, const char *), width);
            break;
        case 'c':
            con_putc((char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            i64 v = is_long ? va_arg(ap, i64) : (i64)va_arg(ap, i32);
            if (v < 0) {
                emit_dec((u64)(-v), true, width, zero);
            } else {
                emit_dec((u64)v, false, width, zero);
            }
            break;
        }
        case 'u': {
            u64 v = is_long ? va_arg(ap, u64) : (u64)va_arg(ap, u32);
            emit_dec(v, false, width, zero);
            break;
        }
        case 'x':
        case 'X': {
            u64 v = is_long ? va_arg(ap, u64) : (u64)va_arg(ap, u32);
            if (is_long) {
                emit_hex((u32)(v >> 32), 8, true, false);
                emit_hex((u32)(v & 0xFFFFFFFFu), 8, true, false);
            } else {
                emit_hex((u32)v, width, zero, false);
            }
            break;
        }
        case 'p':
            emit_hex((u32)va_arg(ap, u32), 8, true, true);
            break;
        case '%':
            con_putc('%');
            break;
        case '\0':
            con_putc('%');
            goto done;
        default:
            con_putc('%');
            con_putc(*fmt);
            break;
        }
        if (*fmt) fmt++;
    }
done:
    va_end(ap);
    con_flush();
}
