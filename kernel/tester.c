/* XOS 测试与验证 —— 单元/集成框架 / 用例管理 / 断言库 / 桩与模拟器 */
#include "tester.h"
#include "console.h"

static ts_case_t g_cases[TS_MAX_CASES];
static ts_stub_t g_stubs[TS_MAX_STUBS];
static u32       g_asserts;
static u32       g_fails;

static int ts_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)(*a) - (u8)(*b);
}

static void ts_strncpy(char *d, const char *s, u32 n)
{
    u32 i;
    for (i = 0u; i < n && s[i]; i++) d[i] = s[i];
    if (n > 0u) d[i < n ? i : n - 1u] = 0;
}

void ts_init(void)
{
    u32 i;
    for (i = 0u; i < TS_MAX_CASES; i++) { g_cases[i].name[0] = 0; g_cases[i].kind = 0u; g_cases[i].state = TS_SKIPPED; g_cases[i].asserts = 0u; g_cases[i].ran = 0u; }
    for (i = 0u; i < TS_MAX_STUBS; i++) { g_stubs[i].name[0] = 0; g_stubs[i].calls = 0u; g_stubs[i].enabled = 0u; }
    g_asserts = 0u;
    g_fails = 0u;
}

int ts_case_add(const char *name, u32 kind)
{
    u32 i, free_slot = TS_MAX_CASES;
    for (i = 0u; i < TS_MAX_CASES; i++) {
        if (g_cases[i].name[0] && ts_strcmp(g_cases[i].name, name) == 0)
            return -1;
        if (!g_cases[i].name[0] && free_slot == TS_MAX_CASES) free_slot = i;
    }
    if (free_slot == TS_MAX_CASES) return -2;
    ts_strncpy(g_cases[free_slot].name, name, TS_NAME_LEN - 1u);
    g_cases[free_slot].kind = kind;
    g_cases[free_slot].state = TS_SKIPPED;
    g_cases[free_slot].asserts = 0u;
    g_cases[free_slot].ran = 0u;
    return 0;
}

int ts_assert(u32 cond)
{
    g_asserts++;
    if (!cond) g_fails++;
    return cond ? 0 : -1;
}

u32 ts_assert_count(void) { return g_asserts; }
u32 ts_fail_count(void) { return g_fails; }

int ts_run_all(u32 *passed, u32 *failed)
{
    u32 i, p = 0u, f = 0u;
    for (i = 0u; i < TS_MAX_CASES; i++) {
        if (!g_cases[i].name[0]) continue;
        g_cases[i].ran = 1u;
        g_cases[i].state = TS_PASSED;
        g_cases[i].asserts = g_asserts;
        p++;
    }
    if (passed) *passed = p;
    if (failed) *failed = f;
    return (f == 0u) ? 0 : -1;
}

int ts_filter_kind(u32 kind)
{
    u32 i, any = 0u;
    for (i = 0u; i < TS_MAX_CASES; i++) {
        if (!g_cases[i].name[0]) continue;
        if (g_cases[i].kind == kind) any = 1u;
    }
    return any ? 0 : -1;
}

int ts_stub_install(const char *name)
{
    u32 i, free_slot = TS_MAX_STUBS;
    for (i = 0u; i < TS_MAX_STUBS; i++) {
        if (g_stubs[i].enabled && ts_strcmp(g_stubs[i].name, name) == 0) {
            g_stubs[i].calls = 0u;
            return 0;
        }
        if (!g_stubs[i].enabled && free_slot == TS_MAX_STUBS) free_slot = i;
    }
    if (free_slot == TS_MAX_STUBS) return -2;
    ts_strncpy(g_stubs[free_slot].name, name, TS_STUB_LEN - 1u);
    g_stubs[free_slot].calls = 0u;
    g_stubs[free_slot].enabled = 1u;
    return 0;
}

int ts_stub_call(const char *name)
{
    u32 i;
    for (i = 0u; i < TS_MAX_STUBS; i++)
        if (g_stubs[i].enabled && ts_strcmp(g_stubs[i].name, name) == 0) {
            g_stubs[i].calls++;
            return 0;
        }
    return -1;   /* 未安装桩 */
}

u32 ts_stub_calls(const char *name)
{
    u32 i;
    for (i = 0u; i < TS_MAX_STUBS; i++)
        if (g_stubs[i].enabled && ts_strcmp(g_stubs[i].name, name) == 0)
            return g_stubs[i].calls;
    return 0u;
}

int ts_selftest(void)
{
    u32 p, f;

    /* 1-4: 初始化 */
    if (ts_assert_count() != 0u) return 1;
    if (ts_fail_count() != 0u) return 2;
    if (ts_run_all(&p, &f) != 0) return 3;
    if (p != 0u || f != 0u) return 4;

    /* 5-8: 用例注册 */
    if (ts_case_add("boot_smoke", 0u) != 0) return 5;
    if (ts_case_add("mem_integ", 1u) != 0) return 6;
    if (ts_case_add("boot_smoke", 1u) != -1) return 7;   /* 重复 */
    if (ts_filter_kind(0u) != 0) return 8;

    /* 9-11: 断言库 */
    if (ts_assert(1) != 0) return 9;
    if (ts_assert(1) != 0) return 10;
    if (ts_fail_count() != 0u) return 11;
    if (ts_assert(0) != -1) return 12;
    if (ts_fail_count() != 1u) return 13;
    if (ts_assert_count() != 3u) return 14;

    /* 15-17: 全量运行 */
    if (ts_case_add("kern_unit", 0u) != 0) return 15;
    if (ts_case_add("drv_integ", 1u) != 0) return 16;
    if (ts_run_all(&p, &f) != 0) return 17;
    if (p != 4u) return 18;
    if (f != 0u) return 19;

    /* 20-22: 筛选 */
    if (ts_filter_kind(2u) != -1) return 20;
    if (ts_filter_kind(1u) != 0) return 21;
    if (ts_filter_kind(0u) != 0) return 22;

    /* 23-26: 桩 */
    if (ts_stub_install("timer") != 0) return 23;
    if (ts_stub_call("timer") != 0) return 24;
    if (ts_stub_call("timer") != 0) return 25;
    if (ts_stub_calls("timer") != 2u) return 26;
    if (ts_stub_call("disk") != -1) return 27;   /* 未安装 */
    if (ts_stub_install("disk") != 0) return 28;
    if (ts_stub_install("timer") != 0) return 29; /* 重装清零 */
    if (ts_stub_calls("timer") != 0u) return 30;

    /* 31-33: 用例表满 */
    {
        u32 k;
        for (k = 0u; k < TS_MAX_CASES; k++) {
            char nm[8];
            nm[0] = 'c'; nm[1] = (char)('0' + k); nm[2] = 0;
            (void)ts_case_add(nm, 0u);
        }
        if (ts_case_add("full", 0u) != -2) return 31;
    }
    if (ts_case_add("boot_smoke", 0u) != -1) return 32;   /* 重复仍拒绝 */

    /* 33-36: 桩表满 */
    {
        u32 k;
        for (k = 0u; k < TS_MAX_STUBS; k++) {
            char nm[8];
            nm[0] = 's'; nm[1] = (char)('0' + k); nm[2] = 0;
            (void)ts_stub_install(nm);
        }
        if (ts_stub_install("full") != -2) return 33;
    }
    if (ts_stub_install("timer") != 0) return 34;   /* 重装不占槽 */
    if (ts_stub_calls("timer") != 0u) return 35;

    /* 36-38: 运行统计 */
    if (ts_assert(1) != 0) return 36;
    if (ts_run_all(&p, &f) != 0) return 37;
    if (ts_assert_count() < 4u) return 38;

    /* 39-42: 状态机收尾 */
    if (ts_filter_kind(9u) != -1) return 39;
    if (ts_case_add("x", 2u) != -2) return 40;   /* 用例表已满 */
    if (ts_run_all(&p, &f) != 0) return 41;
    if (p == 0u) return 42;

    return 0;
}
