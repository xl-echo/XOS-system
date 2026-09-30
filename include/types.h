/* ============================================================================
 * XOS 基础类型定义
 * 功能点：内核基础数据类型、编译期断言、常用宏
 * ============================================================================ */
#ifndef __XOS_TYPES_H__
#define __XOS_TYPES_H__

typedef unsigned char       u8;
typedef signed char         i8;
typedef unsigned short      u16;
typedef signed short        i16;
typedef unsigned int        u32;
typedef signed int          i32;
typedef unsigned long long  u64;
typedef signed long long    i64;

typedef u32                 size_t;
typedef i32                 ssize_t;
typedef u32                 uintptr_t;

typedef int                 bool;
#define true  1
#define false 0

#ifndef NULL
#define NULL ((void *)0)
#endif

#define PACKED      __attribute__((packed))
#define UNUSED(x)   ((void)(x))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))

#define COMPILE_ASSERT(cond, msg) typedef char __ca_##msg[(cond) ? 1 : -1]

/* 位操作 */
static inline void set_bit(u32 *map, u32 bit)
{
    map[bit >> 5] |= (1u << (bit & 31));
}

static inline void clear_bit(u32 *map, u32 bit)
{
    map[bit >> 5] &= ~(1u << (bit & 31));
}

static inline u32 test_bit(const u32 *map, u32 bit)
{
    return (map[bit >> 5] >> (bit & 31)) & 1u;
}

#endif /* __XOS_TYPES_H__ */
