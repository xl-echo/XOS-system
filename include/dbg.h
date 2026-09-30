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

#endif
