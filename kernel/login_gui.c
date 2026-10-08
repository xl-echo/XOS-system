/* ============================================================================
 * XOS 图形登录界面（第 34 册：登录界面 · 图形化升级）
 * 完全自研。VBE 640x480x32 图形登录：
 *  - 首次使用自动进入"创建管理员账户"流程
 *  - 用户名/密码/确认密码三个输入框，Tab 切换焦点
 *  - 回车提交，调用 mu_authenticate 认证（与文本登录同一安全模型）
 *  - 连续 5 次失败锁定账户（30 秒自动解锁）
 *  - Esc 退回文本模式登录（安全回退路径）
 *  - 认证成功后返回 0，由调用方进入图形桌面
 * ============================================================================ */
#include "desk_gui.h"
#include "keyboard.h"
#include "multiuser.h"
#include "display.h"
#include "string.h"

#define LG_NAME_LEN   15u
#define LG_PASS_LEN   31u
#define LG_FAIL_MAX   5u
#define LG_LOCK_SEC   30u            /* 锁定秒数 */

#define LG_FIRST  0x01u

static char lg_name[LG_NAME_LEN + 1u];
static char lg_pass[LG_PASS_LEN + 1u];
static char lg_confirm[LG_PASS_LEN + 1u];
static u32  lg_flags;
static u32  lg_focus;                /* 0=用户名 1=密码 2=确认 */
static u32  lg_fails;
static u32  lg_locked_tick;          /* 锁定时刻（tick） */
static u32  lg_lock_uid;
static u32  lg_status_c;             /* 状态行颜色 */
static char lg_status[48];

static void lg_clear(char *b, u32 cap)
{
    u32 i;
    for (i = 0u; i <= cap; i++) b[i] = 0;
}

static void lg_append(char *b, u32 cap, char c)
{
    u32 n = 0u;
    while (b[n] != 0u && n < cap) n++;
    if (n >= cap) return;
    b[n] = c;
    b[n + 1u] = 0u;
}

static void lg_backspace(char *b)
{
    u32 n = 0u;
    while (b[n] != 0u) n++;
    if (n > 0u) b[n - 1u] = 0u;
}

static void lg_set_status(const char *s, u32 color)
{
    u32 i;
    lg_status_c = color;
    for (i = 0u; s[i] && i < 47u; i++) lg_status[i] = s[i];
    lg_status[i] = 0;
}

static u32 lg_remaining(void)
{
    if (!lg_locked_tick) return 0u;
    {
        u32 el = (pit_tick_count() - lg_locked_tick) / 100u;
        return (el >= LG_LOCK_SEC) ? 0u : (LG_LOCK_SEC - el);
    }
}

static void lg_draw(void)
{
    u32 first = (lg_flags & LG_FIRST) ? 1u : 0u;
    u32 y, i;
    u32 bg = dg_rgb(0x10, 0x1E, 0x34);
    u32 panel = dg_rgb(0x1C, 0x2A, 0x40);
    u32 title = dg_rgb(0x8A, 0xC8, 0xFF);
    u32 fg = dg_rgb(0xE8, 0xE8, 0xE8);
    u32 box_bg = dg_rgb(0x14, 0x20, 0x30);
    u32 box_f = dg_rgb(0x3A, 0x5A, 0x80);
    u32 box_h = dg_rgb(0x50, 0x90, 0xD0);
    u32 dim = dg_rgb(0x90, 0xA0, 0xB8);

    /* 背景渐变 */
    for (y = 0u; y < DG_H; y++) {
        u32 f = (y * 120u) / DG_H;
        dg_fill(0u, y, DG_W, 1u, dg_rgb(0x10u + f / 8u, 0x1Eu + f / 6u, 0x34u + f / 4u));
    }
    /* 面板 */
    dg_fill(80u, 60u, 480u, 330u, panel);
    dg_rect(80u, 60u, 480u, 330u, dg_rgb(0x3A, 0x50, 0x74));
    /* 标题 */
    dg_text(270u, 80u, "XOS", title, panel);
    dg_text(190u, 110u, first ? "首次使用 · 创建管理员账户" : "用户登录",
            fg, panel);

    /* 输入框 */
    dg_text(130u, 160u, "用户名", dim, panel);
    dg_fill(210u, 156u, 280u, 26u, box_bg);
    dg_rect(210u, 156u, 280u, 26u, lg_focus == 0u ? box_h : box_f);
    for (i = 0u; i < 15u; i++) {
        char ch[2];
        if (lg_name[i] == 0) break;
        ch[0] = lg_name[i]; ch[1] = 0;
        dg_text(216u + i * 9u, 163u, ch, fg, box_bg);
    }
    dg_text(130u, 200u, "密码", dim, panel);
    dg_fill(210u, 196u, 280u, 26u, box_bg);
    dg_rect(210u, 196u, 280u, 26u, lg_focus == 1u ? box_h : box_f);
    for (i = 0u; i < 31u; i++) {
        if (lg_pass[i] == 0) break;
        dg_fill(218u + i * 9u, 204u, 5u, 10u, fg);   /* 掩码圆点 */
    }
    if (first) {
        dg_text(130u, 240u, "确认密码", dim, panel);
        dg_fill(210u, 236u, 280u, 26u, box_bg);
        dg_rect(210u, 236u, 280u, 26u, lg_focus == 2u ? box_h : box_f);
        for (i = 0u; i < 31u; i++) {
            if (lg_confirm[i] == 0) break;
            dg_fill(218u + i * 9u, 244u, 5u, 10u, fg);
        }
    }
    /* 状态行 */
    dg_text(130u, 285u, lg_status, lg_status_c, panel);
    /* 提示 */
    dg_text(130u, 320u, "[Tab] 切换输入  [回车] 登录  [退格] 删除",
            dim, panel);
    dg_text(130u, 340u, "[Esc] 切换文本模式登录", dim, panel);
    dg_text(130u, 366u, "XOS v1.0 · 完全自研 · 连续5次失败将锁定账户",
            dg_rgb(0x60, 0x78, 0x98), panel);
}

/* 提交认证；0=成功（已建会话） */
static int lg_submit(void)
{
    u32 uid = 0u;
    char token[24];
    if (lg_remaining() > 0u) {
        lg_set_status("账户锁定中，请稍候...", dg_rgb(0xFF, 0x60, 0x60));
        return 1;
    }
    if (lg_name[0] == 0) { lg_set_status("请输入用户名", dg_rgb(0xFF, 0x60, 0x60)); return 1; }
    if (lg_pass[0] == 0) { lg_set_status("请输入密码", dg_rgb(0xFF, 0x60, 0x60)); return 1; }
    if (lg_flags & LG_FIRST) {
        if (strcmp(lg_pass, lg_confirm) != 0) {
            lg_set_status("两次密码输入不一致", dg_rgb(0xFF, 0x60, 0x60));
            lg_clear(lg_pass, LG_PASS_LEN);
            lg_clear(lg_confirm, LG_PASS_LEN);
            return 1;
        }
        if (mu_user_add(lg_name, lg_pass, 0u, &uid) != 0) {
            lg_set_status("创建管理员账户失败（名称可能已存在）", dg_rgb(0xFF, 0x60, 0x60));
            lg_clear(lg_name, LG_NAME_LEN);
            lg_clear(lg_pass, LG_PASS_LEN);
            lg_clear(lg_confirm, LG_PASS_LEN);
            return 1;
        }
        mu_session_open(uid, token, sizeof(token));
        lg_set_status("管理员账户创建成功，正在进入系统...", dg_rgb(0x60, 0xE0, 0x80));
        return 0;
    }
    if (mu_authenticate(lg_name, lg_pass, &uid) == 0) {
        lg_fails = 0u;
        mu_session_open(uid, token, sizeof(token));
        lg_set_status("登录成功，正在进入系统...", dg_rgb(0x60, 0xE0, 0x80));
        return 0;
    }
    lg_fails++;
    if (lg_fails >= LG_FAIL_MAX) {
        mu_user_lock(uid, 1u);
        lg_lock_uid = uid;
        lg_locked_tick = pit_tick_count();
        lg_set_status("连续5次失败，账户已锁定（30秒后自动解锁）", dg_rgb(0xFF, 0x60, 0x60));
    } else {
        char nb[48];
        u32 i = 0u;
        const char *s = "用户名或密码错误，剩余 ";
        while (s[i] && i < 30u) { nb[i] = s[i]; i++; }
        nb[i++] = (char)('0' + (LG_FAIL_MAX - lg_fails));
        nb[i++] = ' ';
        nb[i++] = '次';
        nb[i++] = '机';
        nb[i++] = '会';
        nb[i] = 0;
        lg_set_status(nb, dg_rgb(0xFF, 0x60, 0x60));
    }
    lg_clear(lg_pass, LG_PASS_LEN);
    lg_clear(lg_confirm, LG_PASS_LEN);
    return 1;
}

/* 图形登录入口：0=成功（已认证并打开会话），1=需要回退文本登录 */
int login_gui_run(void)
{
    kbd_event_t ev, tmp;
    lg_flags = 0u;
    lg_focus = 0u;
    lg_fails = 0u;
    lg_lock_uid = 0u;
    lg_locked_tick = 0u;
    lg_status_c = dg_rgb(0xA0, 0xB0, 0xC8);
    lg_status[0] = 0;
    lg_clear(lg_name, LG_NAME_LEN);
    lg_clear(lg_pass, LG_PASS_LEN);
    lg_clear(lg_confirm, LG_PASS_LEN);
    if (mu_user_count() <= 0) lg_flags |= LG_FIRST;

    /* 清空残留键盘事件 */
    while (kbd_read_event(&tmp) == 0) { }

    if (desk_gui_init() != 0) return 1;      /* VBE+映射失败→文本回退（不触碰硬件） */
    lg_draw();

    for (;;) {
        kbd_poll();
        if (kbd_read_event(&ev) != 0) {
            __asm__ __volatile__("hlt");
            continue;
        }
        if (ev.type != EV_KEY_DOWN) continue;
        if (ev.key == KEY_TAB) {
            lg_focus = (lg_focus + 1u) % ((lg_flags & LG_FIRST) ? 3u : 2u);
        } else if (ev.key == KEY_BACKSP) {
            if (lg_focus == 0u) lg_backspace(lg_name);
            else if (lg_focus == 1u) lg_backspace(lg_pass);
            else lg_backspace(lg_confirm);
        } else if (ev.key == KEY_ENTER) {
            if (lg_submit() == 0) break;         /* 成功：进入桌面 */
            lg_draw();
            continue;
        } else if (ev.key == KEY_ESC) {
            display_set_mode(0);                 /* 安全回退文本登录 */
            return 1;
        } else if (ev.key >= KEY_A && ev.key <= KEY_Z) {
            char c = (char)('a' + (ev.key - KEY_A));
            if (lg_focus == 0u) lg_append(lg_name, LG_NAME_LEN, c);
            else if (lg_focus == 1u) lg_append(lg_pass, LG_PASS_LEN, c);
            else lg_append(lg_confirm, LG_PASS_LEN, c);
        } else if (ev.key >= KEY_0 && ev.key <= KEY_9) {
            char c = (char)('0' + (ev.key - KEY_0));
            if (lg_focus == 0u) lg_append(lg_name, LG_NAME_LEN, c);
            else if (lg_focus == 1u) lg_append(lg_pass, LG_PASS_LEN, c);
            else lg_append(lg_confirm, LG_PASS_LEN, c);
        } else {
            continue;
        }
        lg_draw();
    }
    return 0;
}
