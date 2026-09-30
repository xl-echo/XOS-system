#ifndef XOS_TESTER_H
#define XOS_TESTER_H

#include "types.h"

#define TS_MAX_CASES    8u
#define TS_MAX_SUITES   4u
#define TS_NAME_LEN     16u
#define TS_MAX_STUBS    8u
#define TS_STUB_LEN     16u

/* 用例状态 */
#define TS_PASSED       0u
#define TS_FAILED       1u
#define TS_SKIPPED      2u

typedef struct {
    char  name[TS_NAME_LEN];
    u32   kind;            /* 0=单元 1=集成 */
    u32   state;           /* 结果 */
    u32   asserts;
    u32   ran;
} ts_case_t;

typedef struct {
    char  name[TS_STUB_LEN];
    u32   calls;           /* 桩调用次数 */
    u32   enabled;
} ts_stub_t;

void ts_init(void);
int  ts_case_add(const char *name, u32 kind);
int  ts_run_all(u32 *passed, u32 *failed);
int  ts_assert(u32 cond);              /* 断言与校验 */
u32  ts_assert_count(void);
u32  ts_fail_count(void);
int  ts_filter_kind(u32 kind);         /* 按类型筛选运行 */
int  ts_stub_install(const char *name);
int  ts_stub_call(const char *name);   /* 模拟调用 */
u32  ts_stub_calls(const char *name);
int  ts_selftest(void);

#endif
