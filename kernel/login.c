/* ============================================================================
 * XOS 登录界面（第 34 册：登录界面）
 * 完全自研。文本模式图形化登录：
 *  - 首次使用自动进入"创建管理员账户"流程
 *  - 用户名/密码/确认密码三个输入域，Tab 切换焦点
 *  - 回车提交，调用 mu_authenticate 认证
 *  - 连续 5 次失败锁定账户（自动倒计时解锁）
 *  - 认证成功后创建会话，返回 0 由调用方进入桌面/Shell
 * ============================================================================ */
#include "login.h"
#include "console.h"
#include "keyboard.h"
#include "multiuser.h"
#include "string.h"

#define LOGIN_NAME_LEN   15u
#define LOGIN_PASS_LEN   31u
#define LOGIN_FAIL_MAX   5u
#define LOGIN_LOCK_LOOPS 3000u      /* 近似倒计时（循环次数，约 30s） */

#define LBL_COL   12u
#define BOX_COL   26u
#define BOX_W     36u

static char g_name[LOGIN_NAME_LEN + 1u];
static char g_pass[LOGIN_PASS_LEN + 1u];
static char g_confirm[LOGIN_PASS_LEN + 1u];
static u32  g_flags;                /* 位0=首次模式 */
static u32  g_focus;                /* 0=用户名 1=密码 2=确认 */
static u32  g_fails;
static u32  g_lock_uid;
static u32  g_lock_loops;
static u32  g_poll_fb;

#define LG_FIRST  0x01u

static void status_line(const char *s, u32 color)
{
    con_set_cursor(10u, 0u);
    con_set_color(color, VGA_BLACK);
    con_puts("          状态: ");
    con_puts(s);
    con_puts("                                                     \n");
    con_flush();
}

static void draw_field(u32 row, const char *label, const char *buf, u32 masked, u32 focus)
{
    u32 i;
    char line[BOX_W + 1u];
    con_set_cursor(row, 0u);
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("           ");
    con_puts(label);
    con_puts(": ");
    for (i = 0u; i < BOX_W; i++) line[i] = ' ';
    line[BOX_W] = 0;
    for (i = 0u; buf[i] != 0 && i < BOX_W; i++) line[i] = masked ? '*' : buf[i];
    con_set_color(focus ? VGA_BLACK : VGA_WHITE, focus ? VGA_LIGHTCYAN : VGA_BLACK);
    con_puts("[");
    con_puts(line);
    con_puts("]");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("\n");
}

static void draw_all(void)
{
    u32 first = (g_flags & LG_FIRST) ? 1u : 0u;
    con_set_cursor(0u, 0u);
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  +--------------------------------------------------------------+\n");
    con_puts(first
        ? "  |                XOS 首次使用 · 创建管理员账户                 |\n"
        : "  |                       XOS 用户登录                          |\n");
    con_puts("  +--------------------------------------------------------------+\n");
    con_puts("  |                                                              |\n");
    draw_field(4u, "用户名", g_name, 0u, g_focus == 0u);
    draw_field(5u, "密码",   g_pass, 1u, g_focus == 1u);
    draw_field(6u, "确认密码", g_confirm, 1u, g_focus == 2u);
    con_set_cursor(8u, 0u);
    con_set_color(VGA_DARKGRAY, VGA_BLACK);
    con_puts(first
        ? "  |       [Tab] 切换输入    [回车] 创建并登录   [退格] 删除      |\n"
        : "  |       [Tab] 切换输入    [回车] 登录       [退格] 删除        |\n");
    con_set_cursor(9u, 0u);
    con_puts("  |                                                              |\n");
    con_set_cursor(12u, 0u);
    con_puts("  +--------------------------------------------------------------+\n");
    con_puts("  |  XOS v1.0 · 完全自研操作系统 · 连续5次失败将锁定账户          |\n");
    con_puts("  +--------------------------------------------------------------+\n");
    con_flush();
}

static void clear_buf(char *b, u32 cap)
{
    u32 i;
    for (i = 0u; i <= cap; i++) b[i] = 0;
}

static void do_append(char *b, u32 cap, char c)
{
    u32 n = 0u;
    while (b[n] != 0u && n < cap) n++;
    if (n >= cap) return;
    b[n] = c;
    b[n + 1u] = 0u;
}

static void do_backspace(char *b)
{
    u32 n = 0u;
    while (b[n] != 0u) n++;
    if (n > 0u) b[n - 1u] = 0u;
}

static int submit(void)
{
    u32 uid = 0u;
    int r;
    char token[24];
    if (g_lock_loops > 0u) {
        status_line("账户锁定中，请稍候...", VGA_LIGHTRED);
        return 1;
    }
    if (g_name[0] == 0) { status_line("请输入用户名", VGA_LIGHTRED); return 1; }
    if (g_pass[0] == 0) { status_line("请输入密码", VGA_LIGHTRED); return 1; }
    if (g_flags & LG_FIRST) {
        if (strcmp(g_pass, g_confirm) != 0) {
            status_line("两次密码输入不一致", VGA_LIGHTRED);
            clear_buf(g_pass, LOGIN_PASS_LEN);
            clear_buf(g_confirm, LOGIN_PASS_LEN);
            return 1;
        }
        r = mu_user_add(g_name, g_pass, 0u, &uid);
        if (r != 0) {
            status_line("创建管理员账户失败（名称可能已存在）", VGA_LIGHTRED);
            clear_buf(g_name, LOGIN_NAME_LEN);
            clear_buf(g_pass, LOGIN_PASS_LEN);
            clear_buf(g_confirm, LOGIN_PASS_LEN);
            return 1;
        }
        mu_session_open(uid, token, sizeof(token));
        status_line("管理员账户创建成功，正在进入系统...", VGA_LIGHTGREEN);
        return 0;
    }
    r = mu_authenticate(g_name, g_pass, &uid);
    if (r == 0) {
        g_fails = 0u;
        mu_session_open(uid, token, sizeof(token));
        status_line("登录成功，正在进入系统...", VGA_LIGHTGREEN);
        return 0;
    }
    if (r == -3) {
        status_line("账户已锁定，请联系管理员", VGA_LIGHTRED);
        return 1;
    }
    g_fails++;
    if (g_fails >= LOGIN_FAIL_MAX) {
        mu_user_lock(uid, 1u);
        g_lock_uid = uid;
        g_lock_loops = LOGIN_LOCK_LOOPS;
        status_line("连续5次失败，账户已锁定（自动解锁中）", VGA_LIGHTRED);
    } else {
        status_line("用户名或密码错误，剩余 N 次机会", VGA_LIGHTRED);
        con_set_cursor(10u, 0u);
        con_set_color(VGA_LIGHTRED, VGA_BLACK);
        con_puts("          状态: 用户名或密码错误，剩余 ");
        con_put_dec(LOGIN_FAIL_MAX - g_fails);
        con_puts(" 次机会\n");
    }
    clear_buf(g_pass, LOGIN_PASS_LEN);
    clear_buf(g_confirm, LOGIN_PASS_LEN);
    return 1;
}

int login_run(void)
{
    kbd_event_t ev, tmp;
    g_flags = 0u;
    g_focus = 0u;
    g_fails = 0u;
    g_lock_uid = 0u;
    g_lock_loops = 0u;
    clear_buf(g_name, LOGIN_NAME_LEN);
    clear_buf(g_pass, LOGIN_PASS_LEN);
    clear_buf(g_confirm, LOGIN_PASS_LEN);
    if (mu_user_count() <= 0) g_flags |= LG_FIRST;

    /* 清空自检阶段可能残留的键盘事件，保证焦点/缓冲从干净状态开始 */
    while (kbd_read_event(&tmp) == 0) { }

    con_clear();
    draw_all();

    for (;;) {
        kbd_poll();  /* 与 IRQ1 中断形成双读兜底：中断注入漏读的字节由轮询补上 */
        if (kbd_read_event(&ev) == 0) {   /* 0 = 有事件 */
            u32 max_focus = (g_flags & LG_FIRST) ? 2u : 1u;
            if (g_lock_loops > 0u) {
                /* 锁定期间忽略输入，仅等倒计时 */
            } else if (ev.type == EV_CHAR && ev.ch >= 32u && ev.ch <= 126u) {
                if (g_focus == 0u) do_append(g_name, LOGIN_NAME_LEN, (char)ev.ch);
                else if (g_focus == 1u) do_append(g_pass, LOGIN_PASS_LEN, (char)ev.ch);
                else do_append(g_confirm, LOGIN_PASS_LEN, (char)ev.ch);
                draw_all();
            } else if (ev.type == EV_KEY_DOWN) {
                if (ev.key == KEY_TAB) {
                    g_focus = (g_focus + 1u) % (max_focus + 1u);
                    draw_all();
                } else if (ev.key == KEY_BACKSP) {
                    if (g_focus == 0u) do_backspace(g_name);
                    else if (g_focus == 1u) do_backspace(g_pass);
                    else do_backspace(g_confirm);
                    draw_all();
                } else if (ev.key == KEY_ENTER) {
                    if (submit() == 0) return 0;
                    draw_all();
                }
            }
            continue;   /* 快速消费事件，不落 hlt，避免注入风暴积压 */
        }
        /* 仅由 IRQ1 键盘中断驱动事件队列；此处不轮询 PS/2，避免竞争乱序 */
        if (g_lock_loops > 0u) {
            g_lock_loops--;
            if (g_lock_loops == 0u && g_lock_uid != 0u) {
                mu_user_lock(g_lock_uid, 0u);
                g_lock_uid = 0u;
                g_fails = 0u;
                status_line("账户已解锁，请重新登录", VGA_LIGHTGREEN);
            }
        }
        __asm__ __volatile__("hlt");
    }
}
