/* ============================================================================
 * XOS 内核字符串与内存操作库
 * ============================================================================ */
#ifndef __XOS_STRING_H__
#define __XOS_STRING_H__

#include "types.h"
#include "stdarg.h"

/* ---- 内存操作 ---- */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memzero(void *dst, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* ---- 字符串操作 ---- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strrev(char *s);

/* ---- 数值转换 ---- */
int    atoi(const char *s);
i32    strtol(const char *s, char **endp, u32 base);
u32    strtoul(const char *s, char **endp, u32 base);

char  *utoa(u32 value, char *buf, u32 base);
char  *itoa(i32 value, char *buf, u32 base);
char  *u64toa(u64 value, char *buf, u32 base);
char  *i64toa(i64 value, char *buf, u32 base);

/* ---- 格式化 ---- */
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int    snprintf(char *buf, size_t size, const char *fmt, ...);

#endif /* __XOS_STRING_H__ */
