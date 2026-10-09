/* ============================================================================
 * XOS 实时时钟（RTC / CMOS）接口
 * ========================================================================== */
#ifndef XOS_RTC_H
#define XOS_RTC_H

#include "types.h"

typedef struct rtc_time {
    u32 year;      /* 完整年份，如 2026 */
    u32 mon;       /* 1-12 */
    u32 day;       /* 1-31 */
    u32 hour;      /* 0-23 */
    u32 min;       /* 0-59 */
    u32 sec;       /* 0-59 */
    u32 dow;       /* 0=周日 .. 6=周六 */
} rtc_time_t;

/* 初始化：探测 RTC，强制 24h 制 */
int rtc_init(void);

/* 整组读取（BCD→二进制，UIP 保护，撕裂过滤） */
int rtc_read_all(rtc_time_t *t);
int rtc_get_time(rtc_time_t *t);

/* 设置完整时间（写 CMOS，BCD） */
int rtc_set_time(const rtc_time_t *t);

/* 日期算法 */
int  rtc_is_leap(u32 y);
u32  rtc_days_in_month(u32 y, u32 m);
u32  rtc_days_from_civil(u32 y, u32 m, u32 d);
i64  rtc_to_timestamp(const rtc_time_t *t);

/* 格式化 "YYYY-MM-DD HH:MM:SS Www" */
int rtc_format(char *buf, u32 sz, const rtc_time_t *t);

/* 无符号十进制写（供内部格式化复用） */
u32 rtc_utoa10(char *dst, u32 v, u32 min_digits);

/* 自检与信息 */
int  rtc_selftest(void);
void rtc_dump(void);

#endif /* XOS_RTC_H */
