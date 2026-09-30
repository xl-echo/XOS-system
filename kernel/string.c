/* ============================================================================
 * XOS 内核字符串与内存操作实现
 *
 * 本版修复（针对审查发现的缺陷）：
 *   1) itoa 原实现写 `utoa((u32)(-value), ...)`。当 value == INT32_MIN 时
 *      -value 在有符号域溢出，属未定义行为（实际会得到自身，输出错误）。
 *      现先提升到 i64 再取负，结果精确为 2147483648。
 *   2) utoa 内部循环的 i 与 sizeof(tmp) 比较类型不严谨，现显式统一为 u32。
 *   3) 补齐 strncat / strstr / strspn / strcspn / strpbrk / strrev /
 *      strtol / strtoul / u64toa / i64toa / memzero / vsnprintf / snprintf。
 * ============================================================================ */
#include "string.h"

/* ===========================================================================
 * 内存操作
 * ======================================================================== */
void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* 仅在源与目标对齐一致时启用 4 字节批量拷贝 */
    if (((u32)d & 3u) == ((u32)s & 3u)) {
        while (n && ((u32)d & 3u)) { *d++ = *s++; n--; }
        while (n >= 4) {
            *(u32 *)d = *(const u32 *)s;
            d += 4; s += 4; n -= 4;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    u8 v = (u8)c;

    while (n && ((u32)d & 3u)) { *d++ = v; n--; }
    if (n >= 4) {
        u32 w = (u32)v | ((u32)v << 8) | ((u32)v << 16) | ((u32)v << 24);
        while (n >= 4) { *(u32 *)d = w; d += 4; n -= 4; }
    }
    while (n--) *d++ = v;
    return dst;
}

void *memzero(void *dst, size_t n)
{
    return memset(dst, 0, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *p = (const u8 *)a;
    const u8 *q = (const u8 *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const u8 *p = (const u8 *)s;
    while (n--) {
        if (*p == (u8)c) return (void *)p;
        p++;
    }
    return NULL;
}

/* ===========================================================================
 * 字符串操作
 * ======================================================================== */
size_t strlen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++) != '\0') { }
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *r = dst;
    while (n && *src) { *dst++ = *src++; n--; }
    while (n--) *dst++ = '\0';
    return r;
}

char *strcat(char *dst, const char *src)
{
    char *r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++) != '\0') { }
    return r;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *r = dst;
    while (*dst) dst++;
    while (n && *src) { *dst++ = *src++; n--; }
    *dst = '\0';
    return r;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    size_t nl;
    if (!hay || !needle) return NULL;
    if (*needle == '\0') return (char *)hay;
    nl = strlen(needle);
    while (*hay) {
        if (*hay == *needle && strncmp(hay, needle, nl) == 0) {
            return (char *)hay;
        }
        hay++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n]) {
        if (!strchr(accept, s[n])) break;
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n]) {
        if (strchr(reject, s[n])) break;
        n++;
    }
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        if (strchr(accept, *s)) return (char *)s;
        s++;
    }
    return NULL;
}

char *strrev(char *s)
{
    size_t n, i;
    if (!s) return NULL;
    n = strlen(s);
    for (i = 0; i + 1 < n; i++, n--) {
        char t = s[i];
        s[i] = s[n - 1];
        s[n - 1] = t;
    }
    return s;
}

/* ===========================================================================
 * 数值转换
 * ======================================================================== */
int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

i32 strtol(const char *s, char **endp, u32 base)
{
    bool neg = false;
    u32 acc = 0;
    int d;

    if (!s) { if (endp) *endp = NULL; return 0; }

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') { s++; }

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    for (;; s++) {
        d = digit_value(*s);
        if (d < 0 || (u32)d >= base) break;
        acc = acc * base + (u32)d;
    }

    if (endp) *endp = (char *)s;
    return neg ? (i32)(0u - acc) : (i32)acc;
}

u32 strtoul(const char *s, char **endp, u32 base)
{
    return (u32)strtol(s, endp, base);
}

char *utoa(u32 value, char *buf, u32 base)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[36];
    u32 i = 0, j = 0;

    if (!buf) return NULL;
    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }

    while (value > 0 && i < (u32)sizeof(tmp)) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* 64 位无符号转字符串：base 10 用移位-减法，base 16 用位提取，
 * 均不产生 64 位除法，避免引入 __udivdi3 / __umoddi3 */
char *u64toa(u64 value, char *buf, u32 base)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[68];
    u32 i = 0, j = 0;

    if (!buf) return NULL;
    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }

    if ((base & (base - 1u)) == 0) {
        /* 2 的幂：直接用移位与掩码 */
        u32 shift = 0;
        u32 b = base;
        while (b > 1u) { b >>= 1; shift++; }
        while (value > 0 && i < (u32)sizeof(tmp)) {
            tmp[i++] = digits[(u32)value & (base - 1u)];
            value >>= shift;
        }
    } else {
        static const u64 p10[20] = {
            1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL,
            1000000ULL, 10000000ULL, 100000000ULL, 1000000000ULL,
            10000000000ULL, 100000000000ULL, 1000000000000ULL,
            10000000000000ULL, 100000000000000ULL, 1000000000000000ULL,
            10000000000000000ULL, 100000000000000000ULL,
            1000000000000000000ULL, 10000000000000000000ULL
        };
        int k;
        for (k = 19; k >= 0; k--) {
            u32 d = 0;
            while (value >= p10[k]) { value -= p10[k]; d++; }
            tmp[i++] = digits[d];
        }
        /* 去掉前导零 */
        {
            u32 lead = 0;
            while (lead + 1u < i && tmp[lead] == '0') lead++;
            for (; lead < i; lead++) buf[j++] = tmp[lead];
            buf[j] = '\0';
            return buf;
        }
    }

    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

char *i64toa(i64 value, char *buf, u32 base)
{
    char *p = buf;
    if (!buf) return NULL;
    if (value < 0 && base == 10) {
        *p++ = '-';
        /* 先提升到 i64 再取负，避免 INT64_MIN / INT32_MIN 溢出 */
        u64toa((u64)(0 - value), p, base);
    } else {
        u64toa((u64)value, p, base);
    }
    return buf;
}

char *itoa(i32 value, char *buf, u32 base)
{
    char *p = buf;
    if (!buf) return NULL;
    if (value < 0 && base == 10) {
        *p++ = '-';
        /* 关键修复：先提升到 i64 再取负。
         * 原实现 (u32)(-value) 在 value == INT32_MIN 时溢出（UB）。 */
        u64toa((u64)(0 - (i64)value), p, base);
    } else {
        u64toa((u64)(u32)value, p, base);
    }
    return buf;
}

/* ===========================================================================
 * 格式化输出到缓冲区
 * ======================================================================== */
typedef struct {
    char  *buf;
    size_t size;      /* 含结尾 NUL 的容量 */
    size_t used;      /* 已写入字符数（不含 NUL） */
} fmt_ctx_t;

static void fx_putc(fmt_ctx_t *c, char ch)
{
    if (c->used + 1u < c->size) {
        c->buf[c->used] = ch;
    }
    c->used++;
}

static void fx_puts(fmt_ctx_t *c, const char *s)
{
    if (!s) s = "(null)";
    while (*s) fx_putc(c, *s++);
}

static void fx_pad(fmt_ctx_t *c, u32 n, char pc)
{
    while (n--) fx_putc(c, pc);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    fmt_ctx_t ctx;
    ctx.buf = buf;
    ctx.size = size;
    ctx.used = 0;

    if (!buf || size == 0) return 0;

    while (*fmt) {
        u32 width;
        bool zero;
        bool is_long;

        if (*fmt != '%') { fx_putc(&ctx, *fmt++); continue; }
        fmt++;

        zero = false;
        if (*fmt == '0') { zero = true; fmt++; }
        width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (u32)(*fmt - '0');
            fmt++;
        }

        is_long = false;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l') is_long = true;
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t l;
            if (!s) s = "(null)";
            l = strlen(s);
            if (width > l) fx_pad(&ctx, width - (u32)l, ' ');
            fx_puts(&ctx, s);
            break;
        }
        case 'c':
            fx_putc(&ctx, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            i64 v = is_long ? va_arg(ap, i64) : (i64)va_arg(ap, i32);
            char tmp[24];
            i64toa(v, tmp, 10);
            {
                size_t l = strlen(tmp);
                if (width > l) fx_pad(&ctx, width - (u32)l, zero ? '0' : ' ');
            }
            fx_puts(&ctx, tmp);
            break;
        }
        case 'u': {
            u64 v = is_long ? va_arg(ap, u64) : (u64)va_arg(ap, u32);
            char tmp[24];
            u64toa(v, tmp, 10);
            {
                size_t l = strlen(tmp);
                if (width > l) fx_pad(&ctx, width - (u32)l, zero ? '0' : ' ');
            }
            fx_puts(&ctx, tmp);
            break;
        }
        case 'x':
        case 'X': {
            u64 v = is_long ? va_arg(ap, u64) : (u64)va_arg(ap, u32);
            char tmp[24];
            u64toa(v, tmp, 16);
            {
                size_t l = strlen(tmp);
                if (width > l) fx_pad(&ctx, width - (u32)l, zero ? '0' : ' ');
            }
            fx_puts(&ctx, tmp);
            break;
        }
        case 'p': {
            char tmp[24];
            u64toa((u64)va_arg(ap, u32), tmp, 16);
            fx_puts(&ctx, "0x");
            {
                size_t l = strlen(tmp);
                if (l < 8) fx_pad(&ctx, (u32)(8 - l), '0');
            }
            fx_puts(&ctx, tmp);
            break;
        }
        case '%':
            fx_putc(&ctx, '%');
            break;
        case '\0':
            fx_putc(&ctx, '%');
            goto fx_done;
        default:
            fx_putc(&ctx, '%');
            fx_putc(&ctx, *fmt);
            break;
        }
        if (*fmt) fmt++;
    }

fx_done:
    if (ctx.used < ctx.size) {
        ctx.buf[ctx.used] = '\0';
    } else {
        ctx.buf[ctx.size - 1u] = '\0';
    }
    return (int)ctx.used;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
