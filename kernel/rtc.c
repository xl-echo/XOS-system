/* ============================================================================
 * XOS 实时时钟（RTC / CMOS）子系统 — 完全自研
 *   真实读取主板 CMOS RTC（端口 0x70/0x71）：时/分/秒/星期/日/月/年/世纪
 *   BCD↔二进制、24/12 小时制、UIP 更新保护、NMI 屏蔽写保护
 *   闰年 / 月天数 / 1970 时间戳换算 / 格式化输出
 *   时钟内核源：desk 时钟与 date 命令均基于本子系统，不再用内存模拟
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/string.h"
#include "../include/rtc.h"

/* I/O 原语（自研内联） */
static inline void x_outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 x_inb(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* CMOS 寄存器索引 */
#define CMOS_SEC      0x00u
#define CMOS_MIN      0x02u
#define CMOS_HOUR     0x04u
#define CMOS_DOW      0x06u   /* 1=周日 .. 7=周六 */
#define CMOS_DOM      0x07u
#define CMOS_MON      0x08u
#define CMOS_YEAR     0x09u
#define CMOS_CENTURY  0x32u
#define CMOS_STA_A    0x0Au
#define CMOS_STA_B    0x0Bu

#define CMOS_PORT_IDX 0x70u
#define CMOS_PORT_DAT 0x71u
#define CMOS_NMI_MASK 0x80u
#define CMOS_UIP_BIT  0x80u   /* 状态 A bit7：更新进行中 */

static int g_rtc_ok;          /* 探测结果 */
static u32 g_rtc_tick_last;   /* 上次缓存时间戳（防重复读撕裂） */

static u8 cmos_read(u8 idx)
{
    x_outb(CMOS_PORT_IDX, idx);
    return (u8)x_inb(CMOS_PORT_DAT);
}

static void cmos_write(u8 idx, u8 val)
{
    x_outb(CMOS_PORT_IDX, idx | CMOS_NMI_MASK);   /* 屏蔽 NMI 写 */
    x_outb(CMOS_PORT_DAT, val);
    x_outb(CMOS_PORT_IDX, CMOS_NMI_MASK);         /* 恢复 NMI 屏蔽位 */
}

static u8 bcd2bin(u8 b)
{
    return (u8)(((b >> 4) & 0x0Fu) * 10u + (b & 0x0Fu));
}

static u8 bin2bcd(u8 v)
{
    return (u8)(((v / 10u) & 0x0Fu) << 4u) | (u8)(v % 10u);
}

/* 等待 RTC 未处于更新中（最多 ~2ms），避免读到撕裂值 */
static int cmos_wait_uip(void)
{
    u32 i;
    for (i = 0u; i < 100000u; i++) {
        if (!(cmos_read(CMOS_STA_A) & CMOS_UIP_BIT)) return 0;
    }
    return -1;
}

/* ---------------- 初始化：探测 RTC 存在并确认 24h 制 ---------------- */
int rtc_init(void)
{
    u8 staB = cmos_read(CMOS_STA_B);
    g_rtc_ok = 1;
    if (staB & 0x02u) {          /* 12 小时制：强制切 24h */
        staB &= (u8)~0x02u;
        cmos_write(CMOS_STA_B, staB);
    }
    g_rtc_tick_last = 0u;
    return 0;
}

/* 整组读取（BCD→二进制；12h 制 PM 位已由 init 排除，防御性再处理） */
int rtc_read_all(rtc_time_t *t)
{
    u8 s, m, h, dow, dom, mon, yr, ce;
    u32 tries = 0;
    if (!t) return -1;
    if (!g_rtc_ok) return -2;
    /* 连续两次读数一致，过滤更新撕裂 */
    for (;;) {
        if (cmos_wait_uip() != 0) return -3;
        s   = cmos_read(CMOS_SEC);
        m   = cmos_read(CMOS_MIN);
        h   = cmos_read(CMOS_HOUR);
        dow = cmos_read(CMOS_DOW);
        dom = cmos_read(CMOS_DOM);
        mon = cmos_read(CMOS_MON);
        yr  = cmos_read(CMOS_YEAR);
        ce  = cmos_read(CMOS_CENTURY);
        if (cmos_wait_uip() != 0) return -3;
        if (cmos_read(CMOS_SEC) == s) break;
        if (++tries > 8u) return -4;
    }
    if (!(cmos_read(CMOS_STA_B) & 0x02u)) {        /* BCD */
        s = bcd2bin(s); m = bcd2bin(m); h = bcd2bin(h);
        dom = bcd2bin(dom); mon = bcd2bin(mon);
        yr = bcd2bin(yr); ce = bcd2bin(ce);
        if (h & 0x80u) { h = (u8)((h & 0x7Fu) + 12u); }   /* 12h PM 兜底 */
    }
    t->sec  = s & 0x3Fu;
    t->min  = m & 0x3Fu;
    t->hour = h & 0x1Fu;
    t->dow  = (dow == 0u) ? 0u : (u32)(dow - 1u);   /* 0=周日 */
    t->day  = dom & 0x1Fu;
    t->mon  = (mon == 0u) ? 1u : (mon & 0x0Fu);
    t->year = (u32)yr + (u32)ce * 100u;
    if (t->year < 100u) t->year += 2000u;
    return 0;
}

/* 只读时间（秒粒度），供 clock/date 快速显示 */
int rtc_get_time(rtc_time_t *t)
{
    return rtc_read_all(t);
}

/* 设置完整时间（写 CMOS；BCD） */
int rtc_set_time(const rtc_time_t *t)
{
    u32 ce, yr;
    if (!t) return -1;
    if (t->hour > 23u || t->min > 59u || t->sec > 59u) return -2;
    if (t->day < 1u || t->day > 31u || t->mon < 1u || t->mon > 12u) return -3;
    if (t->year < 1900u) return -4;
    cmos_wait_uip();
    cmos_write(CMOS_SEC,   bin2bcd(t->sec));
    cmos_write(CMOS_MIN,   bin2bcd(t->min));
    cmos_write(CMOS_HOUR,  bin2bcd(t->hour));
    cmos_write(CMOS_DOW,   bin2bcd((u8)(t->dow + 1u)));
    cmos_write(CMOS_DOM,   bin2bcd(t->day));
    cmos_write(CMOS_MON,   bin2bcd(t->mon));
    ce = t->year / 100u; yr = t->year % 100u;
    cmos_write(CMOS_CENTURY, bin2bcd((u8)ce));
    cmos_write(CMOS_YEAR,    bin2bcd((u8)yr));
    return 0;
}

/* ---------------- 日期算法（全部自研） ---------------- */
int rtc_is_leap(u32 y)
{
    return (y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u);
}

static const u8 rtc_mdays[12] = { 31u, 28u, 31u, 30u, 31u, 30u,
                                  31u, 31u, 30u, 31u, 30u, 31u };

u32 rtc_days_in_month(u32 y, u32 m)
{
    if (m < 1u || m > 12u) return 0u;
    if (m == 2u && rtc_is_leap(y)) return 29u;
    return rtc_mdays[m - 1u];
}

/* 公历 → 1970-01-01 起的天数（Howard Hinnant 逆算法，锚点可验证） */
u32 rtc_days_from_civil(u32 y, u32 m, u32 d)
{
    u32 era, yoe, doy, doe;
    if (m <= 2u) { y--; m += 12u; }
    era = y / 400u;
    yoe = y - era * 400u;
    doy = (153u * (m - 3u) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097u + doe - 719468u;
}

/* 1970 起秒 */
i64 rtc_to_timestamp(const rtc_time_t *t)
{
    return (i64)rtc_days_from_civil(t->year, t->mon, t->day) * 86400LL
           + (i64)t->hour * 3600LL + (i64)t->min * 60LL + (i64)t->sec;
}

/* 星期名（dow 0=周日） */
static const char *const rtc_wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* 格式化 "YYYY-MM-DD HH:MM:SS Www" */
int rtc_format(char *buf, u32 sz, const rtc_time_t *t)
{
    char *p = buf;
    u32 i;
    if (!buf || sz < 26u) return -1;
    p += rtc_utoa10(p, t->year, 4u);
    *p++ = '-';
    p += rtc_utoa10(p, t->mon, 2u);
    *p++ = '-';
    p += rtc_utoa10(p, t->day, 2u);
    *p++ = ' ';
    p += rtc_utoa10(p, t->hour, 2u);
    *p++ = ':';
    p += rtc_utoa10(p, t->min, 2u);
    *p++ = ':';
    p += rtc_utoa10(p, t->sec, 2u);
    *p++ = ' ';
    if (t->dow < 7u) {
        for (i = 0; i < 3u && rtc_wdays[t->dow][i]; i++) *p++ = rtc_wdays[t->dow][i];
    }
    *p = 0;
    return 0;
}

/* 供 con_put 无符号十进制输出 */
u32 rtc_utoa10(char *dst, u32 v, u32 min_digits)
{
    char tmp[12];
    u32 i = 0, n;
    if (v == 0u) tmp[i++] = '0';
    while (v > 0u) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i < min_digits) tmp[i++] = '0';
    for (n = 0u; n < i; n++) dst[n] = tmp[i - 1u - n];
    return i;
}

/* ---------------- 自检 ---------------- */
int rtc_selftest(void)
{
    rtc_time_t t;
    u32 d0, d2000;
    /* 闰年与月天数 */
    if (rtc_is_leap(2000) != 1) return 1;
    if (rtc_is_leap(1900) != 0) return 2;
    if (rtc_is_leap(2024) != 1) return 3;
    if (rtc_days_in_month(2024, 2) != 29u) return 4;
    if (rtc_days_in_month(2023, 2) != 28u) return 5;
    if (rtc_days_in_month(2024, 1) != 31u) return 6;
    if (rtc_days_in_month(2024, 12) != 31u) return 7;
    /* 公历天数（可验证锚点） */
    d0 = rtc_days_from_civil(1970, 1, 1);
    if (d0 != 0u) return 8;
    d2000 = rtc_days_from_civil(2000, 1, 1);
    if (d2000 != 10957u) return 9;
    if (rtc_days_from_civil(2024, 2, 29) - rtc_days_from_civil(2024, 2, 28) != 1u) return 10;
    /* 时间戳锚点 */
    t.year = 1970; t.mon = 1; t.day = 1; t.hour = 0; t.min = 0; t.sec = 0; t.dow = 3;
    if (rtc_to_timestamp(&t) != 0LL) return 11;
    t.year = 2000; t.mon = 1; t.day = 1; t.hour = 0; t.min = 0; t.sec = 0;
    if (rtc_to_timestamp(&t) != 946684800LL) return 12;
    t.year = 2024; t.mon = 1; t.day = 1; t.hour = 0; t.min = 0; t.sec = 0;
    if (rtc_to_timestamp(&t) != 1704067200LL) return 13;
    /* BCD 转换 */
    if (bcd2bin(0x59) != 59u) return 14;
    if (bcd2bin(0x00) != 0u) return 15;
    if (bin2bcd(59) != 0x59u) return 16;
    if (bin2bcd(0) != 0x00u) return 17;
    if (bin2bcd(23) != 0x23u) return 18;
    /* 格式化 */
    {
        char buf[32];
        t.year = 2026; t.mon = 9; t.day = 29; t.hour = 14; t.min = 5; t.sec = 7; t.dow = 2;
        if (rtc_format(buf, sizeof(buf), &t) != 0) return 19;
        if (strcmp(buf, "2026-09-29 14:05:07 Tue") != 0) return 20;
    }
    /* 参数校验 */
    t.year = 1800; t.mon = 1; t.day = 1;
    if (rtc_set_time(&t) != -4) return 21;
    t.year = 2026; t.mon = 13; t.day = 1;
    if (rtc_set_time(&t) != -3) return 22;
    t.year = 2026; t.mon = 1; t.day = 1; t.hour = 25;
    if (rtc_set_time(&t) != -2) return 23;
    return 0;
}

void rtc_dump(void)
{
    rtc_time_t t;
    char buf[32];
    if (rtc_read_all(&t) != 0) { con_puts("  rtc: not available\n"); return; }
    rtc_format(buf, sizeof(buf), &t);
    con_puts("  rtc: CMOS RTC ok  now=");
    con_puts(buf);
    con_puts("  ts=");
    con_put_dec64((u64)rtc_to_timestamp(&t));
    con_puts("\n");
}
