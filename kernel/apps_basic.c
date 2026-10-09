/* ============================================================================
 * XOS 基础应用集（第 43/44/48/49/72 册配套应用）
 *   calc      — 交互式计算器（表达式解析：+ - * / % 括号、ans 记忆）
 *   clock     — 实时时钟显示（联动桌面时钟；setclock 可设置）
 *   sysinfo   — 系统信息面板（内核/内存/文件/网络/运行时间汇总）
 *   df        — 磁盘与内存统计（分页/已用/最大空闲）
 *   ps        — 应用生态进程表（ELF 程序描述符）
 *   netinfo   — 网络接口信息（探测结果 / MAC / 收发统计）
 *   about     — 版本与版权信息
 * 全部自研、文本模式直接可用；经 shell 命令表接入。
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/string.h"
#include "../include/fs.h"
#include "../include/pmm.h"
#include "../include/keyboard.h"
#include "../include/task.h"
#include "../include/net.h"
#include "../include/app.h"
#include "../include/desktop.h"
#include "../include/irq.h"
#include "../include/shell.h"
#include "../include/rtc.h"

#define APPS_MAX_LINE 96u

void cmd_date(u32 argc, char (*argv)[SH_MAX_CMD]);

i64 g_calc_last;

/* ---------------- calc：表达式求值器（递归下降） ---------------- */
static const char *g_expr;
static u32 g_pos;
static int g_calc_err;

static i64 calc_expr(void);

static void calc_skip(void)
{
    while (g_expr[g_pos] == ' ' || g_expr[g_pos] == '\t') g_pos++;
}

static i64 calc_factor(void)
{
    i64 v;
    calc_skip();
    if (g_expr[g_pos] == '(') {
        g_pos++;
        v = calc_expr();
        calc_skip();
        if (g_expr[g_pos] == ')') g_pos++;
        else g_calc_err = 1;
        return v;
    }
    if (g_expr[g_pos] == '-') {
        g_pos++;
        return -calc_factor();
    }
    if (g_expr[g_pos] == '+') {
        g_pos++;
        return calc_factor();
    }
    if (g_expr[g_pos] == 'a' || g_expr[g_pos] == 'A') {   /* ans */
        g_pos++;
        return g_calc_last;
    }
    if (g_expr[g_pos] >= '0' && g_expr[g_pos] <= '9') {
        v = 0;
        while (g_expr[g_pos] >= '0' && g_expr[g_pos] <= '9') {
            v = v * 10 + (g_expr[g_pos] - '0');
            g_pos++;
        }
        return v;
    }
    g_calc_err = 1;
    return 0;
}

static i64 calc_term(void)
{
    i64 v = calc_factor();
    calc_skip();
    while (g_expr[g_pos] == '*' || g_expr[g_pos] == '/' || g_expr[g_pos] == '%') {
        char op = g_expr[g_pos++];
        i64 r = calc_factor();
        if (op == '*') v *= r;
        else if (op == '/') {
            if (r == 0) { g_calc_err = 1; return 0; }
            v /= r;
        } else {
            if (r == 0) { g_calc_err = 1; return 0; }
            v %= r;
        }
        calc_skip();
    }
    return v;
}

static i64 calc_expr(void)
{
    i64 v = calc_term();
    calc_skip();
    while (g_expr[g_pos] == '+' || g_expr[g_pos] == '-') {
        char op = g_expr[g_pos++];
        i64 r = calc_term();
        if (op == '+') v += r; else v -= r;
        calc_skip();
    }
    return v;
}

static void app_calc_once(const char *line)
{
    i64 r;
    g_expr = line;
    g_pos = 0;
    g_calc_err = 0;
    r = calc_expr();
    calc_skip();
    if (g_calc_err || g_expr[g_pos] != 0) {
        con_puts("calc: syntax error\n");
        return;
    }
    g_calc_last = r;
    if (r < 0) { con_putc('-'); r = -r; }
    con_puts("  = ");
    con_put_dec64((u64)r);
    con_puts("\n");
}

void cmd_calc(u32 argc, char (*argv)[SH_MAX_CMD])
{
    char line[APPS_MAX_LINE];
    u32 n;
    if (argc > 1) {
        /* 一次性计算：拼接参数 */
        line[0] = 0;
        for (n = 1; n < argc && n < 8; n++) {
            if (n > 1) strcat(line, " ");
            strncat(line, argv[n], APPS_MAX_LINE - strlen(line) - 1);
        }
        app_calc_once(line);
        return;
    }
    con_puts("XOS calc  (表达式支持 + - * / % ( )，a=上次结果；q 退出)\n");
    for (;;) {
        kbd_event_t ev;
        char ln[APPS_MAX_LINE];
        u32 len = 0;
        con_puts("calc> ");
        con_flush();
        ln[0] = 0;
        for (;;) {
            if (kbd_read_event(&ev) != 0) { kbd_poll(); __asm__ __volatile__("hlt"); continue; }
            if (ev.type == EV_CHAR || (ev.type == EV_KEY_DOWN && ev.ch != 0)) {
                char c = (char)ev.ch;
                if (c == '\n' || c == '\r') { con_puts("\n"); break; }
                if (c == '\b' || c == 127) { if (len) { len--; con_puts("\b \b"); } }
                else if (c >= 32 && c < 127 && len < APPS_MAX_LINE - 1) {
                    ln[len++] = c; con_putc(c);
                }
                con_flush();
            } else if (ev.type == EV_KEY_DOWN && ev.key == KEY_ENTER) { con_puts("\n"); break; }
        }
        ln[len] = 0;
        if (strcmp(ln, "q") == 0 || strcmp(ln, "quit") == 0) break;
        if (ln[0]) app_calc_once(ln);
    }
}

/* ---------------- clock / setclock / date（真实 CMOS RTC） ---------------- */
void cmd_clock(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 i;
    char buf[32];
    rtc_time_t t;
    con_puts("XOS clock  真实 CMOS RTC；按 q/Esc 退出；date 可查看完整日期\n");
    for (i = 0; i < 10; i++) {
        kbd_event_t ev;
        if (rtc_read_all(&t) == 0) {
            rtc_format(buf, sizeof(buf), &t);
            con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
            con_puts("\r  现在: ");
            con_puts(buf);
            con_puts("   (tick=");
            con_put_dec(pit_tick_count());
            con_puts(")   ");
            con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
            con_flush();
        }
        if (kbd_read_event(&ev) == 0) {
            if ((ev.type == EV_CHAR && (ev.ch == 'q' || ev.ch == 'Q')) ||
                (ev.type == EV_KEY_DOWN && ev.key == KEY_ESC))
                break;
        }
        msleep(1000);
    }
    con_puts("\n");
}

/* date：显示或设置完整日期时间（date / date YYYY-MM-DD HH:MM[:SS]） */
void cmd_date(u32 argc, char (*argv)[SH_MAX_CMD])
{
    rtc_time_t t;
    char buf[32];
    if (argc < 2) {
        if (rtc_read_all(&t) != 0) { con_puts("date: RTC 不可用\n"); return; }
        rtc_format(buf, sizeof(buf), &t);
        con_puts(buf);
        con_puts("   (ts=");
        con_put_dec64((u64)rtc_to_timestamp(&t));
        con_puts(")\n");
        return;
    }
    /* date YYYY-MM-DD HH:MM[:SS] */
    if (argc >= 3) {
        u32 y, m, d, h, mi, s = 0u;
        y = (u32)atoi(argv[1]);
        m = (u32)atoi(argv[2]);
        d = (u32)atoi(argv[3]);
        h = (u32)atoi(argv[4]);
        mi = (u32)atoi(argv[5]);
        if (argc >= 7) s = (u32)atoi(argv[6]);
        if (y < 2000u || y > 2099u || m < 1u || m > 12u || d < 1u ||
            d > rtc_days_in_month(y, m) || h > 23u || mi > 59u || s > 59u) {
            con_puts("date: 参数非法 (YYYY MM DD HH MM [SS])\n");
            return;
        }
        t.year = y; t.mon = m; t.day = d; t.hour = h; t.min = mi; t.sec = s;
        t.dow = (u32)((rtc_days_from_civil(y, m, d) + 4u) % 7u);   /* 1970-01-01=周四 */
        if (rtc_set_time(&t) != 0) { con_puts("date: 设置失败\n"); return; }
        con_puts("date: 已设置 -> ");
        rtc_format(buf, sizeof(buf), &t);
        con_puts(buf);
        con_puts("\n");
    } else {
        con_puts("usage: date           显示当前日期时间\n");
        con_puts("       date <YYYY> <MM> <DD> <HH> <MM> [<SS>]   设置时间\n");
    }
}

void cmd_setclock(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 h, m;
    if (argc < 3) { con_puts("usage: setclock <hour> <minute>\n"); return; }
    h = (u32)atoi(argv[1]);
    m = (u32)atoi(argv[2]);
    if (h > 23 || m > 59) { con_puts("setclock: 0-23 时, 0-59 分\n"); return; }
    if (desk_clock_set(h, m) != 0) con_puts("setclock: failed\n");
    else { con_puts("setclock: ok -> "); con_put_dec(h); con_putc(':'); con_put_dec(m); con_puts("\n"); }
}

/* ---------------- sysinfo / df / ps / netinfo / about ---------------- */
void cmd_sysinfo(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 pages, used, reserved;
    char name[FS_NAME_MAX];
    u32 n, fcnt = 0;
    pages = pmm_total_pages(); used = pmm_used_pages(); reserved = pmm_reserved_pages();
    con_puts("========== XOS 系统信息 ==========\n");
    con_puts("  内核       : XOS v1.0 自研 x86 操作系统\n");
    con_puts("  当前时间   : ");
    {
        char buf[32];
        rtc_time_t t;
        if (rtc_read_all(&t) == 0) { rtc_format(buf, sizeof(buf), &t); con_puts(buf); }
        else con_puts("(RTC 不可用)");
    }
    con_puts("\n");
    con_puts("  运行时间   : ");
    con_put_dec(pit_tick_count() / 100u);
    con_puts(" 秒\n");
    con_puts("  物理内存   : 总 ");
    con_put_dec(pages * 4u / 1024u);
    con_puts(" MB, 已用 ");
    con_put_dec(used * 4u / 1024u);
    con_puts(" MB, 保留 ");
    con_put_dec(reserved * 4u / 1024u);
    con_puts(" MB\n");
    con_puts("  文件系统   : 根目录条目: ");
    for (n = 0; n < 200; n++) {
        if (fs_readdir("/", n, name) != 0 || name[0] == 0) break;
        fcnt++;
    }
    con_put_dec(fcnt);
    con_puts(" 个\n");
    con_puts("  桌面时钟   : ");
    {
        u32 h, m; desk_clock_get(&h, &m);
        con_put_dec(h); con_putc(':'); con_put_dec(m);
    }
    con_puts("\n");
    con_puts("  网络接口   : 探测 ");
    {
        u8 mac[6]; int i;
        if (net_get_mac(0, mac) == 0) {
            con_puts("存在 MAC=");
            for (i = 0; i < 6; i++) { if (i) con_putc(':'); con_put_hex8(mac[i]); }
            con_puts("\n");
        } else con_puts("未发现\n");
    }
    con_puts("  应用程序   : ELF 加载器就绪\n");
    con_puts("==================================\n");
}

void cmd_df(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 pages, used, reserved;
    u32 largest;
    pages = pmm_total_pages(); used = pmm_used_pages();
    reserved = pmm_reserved_pages(); largest = pmm_largest_free_run();
    con_puts("--- 内存统计 (4KB/页) ---\n");
    con_puts("  总页数     : "); con_put_dec(pages); con_puts("\n");
    con_puts("  已用       : "); con_put_dec(used); con_puts("\n");
    con_puts("  保留       : "); con_put_dec(reserved); con_puts("\n");
    con_puts("  空闲       : "); con_put_dec(pages - used - reserved); con_puts("\n");
    con_puts("  最大连续   : "); con_put_dec(largest); con_puts(" 页\n");
    con_puts("  可用率     : ");
    if (pages) con_put_dec((pages - used - reserved) * 100u / pages);
    con_puts("%\n");
}

void cmd_ps(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("--- XOS 应用进程表 ---\n");
    app_dump();
    con_puts("  调度心跳   : ");
    con_put_dec(pit_tick_count());
    con_puts(" ticks\n");
}

void cmd_netinfo(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u8 mac[6];
    u32 i;
    con_puts("--- 网络接口信息 ---\n");
    if (net_get_mac(0, mac) == 0) {
        con_puts("  接口 0 MAC: ");
        for (i = 0; i < 6; i++) { if (i) con_putc(':'); con_put_hex8(mac[i]); }
        con_puts("\n");
        con_puts("  接口 0 已打开\n");
    } else {
        con_puts("  未检测到可用网卡（VBox 下 82540EM 需 PCI 探测）\n");
    }
}

void cmd_about(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("XOS v1.0 — 完全自研 x86 操作系统\n");
    con_puts("  内核: 自研 C + 汇编（引导/分页/调度/FS/GUI/网络/音频）\n");
    con_puts("  协议: 不依赖任何外部内核/闭源/收费方案\n");
    con_puts("  目标: 无系统电脑直接安装，可于虚拟机内运行\n");
    con_puts("  命令: 输入 help 查看全部命令\n");
}
