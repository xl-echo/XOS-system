#ifndef XOS_DBG_H
#define XOS_DBG_H

#include "types.h"

#define DBG_LOG_ENTRIES   32u
#define DBG_LOG_LINE_LEN  40u
#define DBG_LEVEL_MASK    0xFFu
#define DBG_LEVEL_DEBUG   1u
#define DBG_LEVEL_INFO    2u
#define DBG_LEVEL_WARN    3u
#define DBG_LEVEL_ERROR   4u
#define DBG_MAX_SYMS      16u
#define DBG_SYM_LEN       20u
#define DBG_MAX_BP        8u
#define DBG_BP_LEN        16u

/* 日志磁盘持久化（自研）：关机/重启前把日志缓冲写入磁盘末尾固定区，
 * 下次启动恢复并可在 dmesg 中查看——Linux dmesg 持久化语义的自研实现 */
#define DBG_PLOG_MAGIC    0x584C4F47u   /* "XLOG" */
#define DBG_PLOG_VERSION  1u
#define DBG_PLOG_SECTS    16u           /* 8KB 持久化区 */
#define DBG_PLOG_MAX      DBG_LOG_ENTRIES /* 持久化条目上限 = 环形缓冲深度 */

typedef struct dbg_plog_ent {
    u32   ticks;
    u32   level;
    char  line[DBG_LOG_LINE_LEN];
} dbg_plog_ent_t;

typedef struct dbg_plog {
    u32   magic;
    u32   version;
    u32   seq;              /* 落盘序号（递增） */
    u32   count;            /* 条目数 */
    u32   ticks;            /* 落盘时 tick */
    dbg_plog_ent_t ent[DBG_PLOG_MAX];
} dbg_plog_t;

void dbg_init(void);
int  dbg_com_write(const u8 *buf, u32 len);   /* 串口调试输出（COM1 语义） */
u32  dbg_com_sent(void);
int  dbg_log_add(u32 level, const char *line);
u32  dbg_log_count(void);
int  dbg_log_dump(char *out, u32 max);        /* 最近一条 */
u32  dbg_level_filter(u32 level);             /* 0=通过 1=被过滤 */
int  dbg_sym_add(const char *name, u32 addr);
int  dbg_sym_lookup(const char *name, u32 *addr);
int  dbg_backtrace(const u32 *fp_chain, u32 depth, u32 *out, u32 max); /* 栈回溯 */
int  dbg_bp_set(const char *name);
int  dbg_bp_hit(const char *name);            /* kgdb 断点命中 */
u32  dbg_bp_count(void);
int  dbg_selftest(void);

/* 日志持久化（V2） */
int  dbg_log_persist(void);                   /* 落盘：序列化当前缓冲 → 磁盘末尾区 */
int  dbg_log_restore(void);                   /* 恢复：启动读回上次落盘日志（打印+缓存） */
void dbg_log_show(void);                      /* dmesg：级别名+时间戳，先恢复后当前 */
int  dbg_selftest_plog(void);                 /* 持久化序列化往返自检 */

#endif
