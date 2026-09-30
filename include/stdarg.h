/* ============================================================================
 * XOS 变参支持（freestanding 环境，基于编译器内建）
 * ============================================================================ */
#ifndef __XOS_STDARG_H__
#define __XOS_STDARG_H__

typedef __builtin_va_list va_list;

#define va_start(v, l)  __builtin_va_start(v, l)
#define va_arg(v, t)    __builtin_va_arg(v, t)
#define va_end(v)       __builtin_va_end(v)
#define va_copy(d, s)   __builtin_va_copy(d, s)

#endif /* __XOS_STDARG_H__ */
