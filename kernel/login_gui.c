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
#include "xos_loginbg.h"

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

/* 登录界面重设计（方案A·极光科技）：描边文本外部声明（desk_gui.c 实现） */
extern void dg_text_ol(u32 x, u32 y, const char *s, u32 fg, u32 ol, u32 bg);

/* 阶梯圆角矩形（45° 像素圆角） */
static void lg_rrect(u32 x, u32 y, u32 w, u32 h, u32 r, u32 color)
{
    u32 i;
    if (w < 2u * r || h < 2u * r) { dg_fill(x, y, w, h, color); return; }
    dg_fill(x, y + r, w, h - 2u * r, color);
    dg_fill(x + r, y, w - 2u * r, r, color);
    dg_fill(x + r, y + h - r, w - 2u * r, r, color);
    for (i = 0u; i < r; i++) {
        u32 len = r - i;
        dg_fill(x + i, y + i, len, 1u, color);              /* 左上 */
        dg_fill(x + w - r + i, y + i, len, 1u, color);      /* 右上 */
        dg_fill(x + i, y + h - r + i, len, 1u, color);      /* 左下 */
        dg_fill(x + w - r + i, y + h - r + i, len, 1u, color); /* 右下 */
    }
}

/* 垂直渐变矩形 */
static void lg_grad_v(u32 x, u32 y, u32 w, u32 h, u32 c1, u32 c2)
{
    u32 i;
    u32 r1 = (c1 >> 16u) & 0xFFu, g1 = (c1 >> 8u) & 0xFFu, b1 = c1 & 0xFFu;
    u32 r2 = (c2 >> 16u) & 0xFFu, g2 = (c2 >> 8u) & 0xFFu, b2 = c2 & 0xFFu;
    for (i = 0u; i < h; i++) {
        u32 k = (i * 255u) / (h ? h : 1u);
        dg_fill(x, y + i, w, 1u,
                dg_rgb((r1 * (255u - k) + r2 * k) / 255u,
                       (g1 * (255u - k) + g2 * k) / 255u,
                       (b1 * (255u - k) + b2 * k) / 255u));
    }
}

/* 发光大字：多遍偏移光晕 + 本体（白字亮蓝描边） */
static void lg_glow_text(u32 x, u32 y, const char *s, u32 fg, u32 glow, u32 bg, u32 scale)
{
    u32 fs = g_font_scale;
    g_font_scale = scale;
    dg_text_ol(x - 2u, y, s, glow, glow, bg);
    dg_text_ol(x + 2u, y, s, glow, glow, bg);
    dg_text_ol(x, y - 2u, s, glow, glow, bg);
    dg_text_ol(x, y + 2u, s, glow, glow, bg);
    dg_text_ol(x, y, s, fg, glow, bg);
    g_font_scale = fs;
}

/* 输入框：深底+聚焦亮蓝框+文字/掩码+闪烁光标 */
static void lg_input(u32 x, u32 y, u32 w, u32 h, u32 focus,
                     const char *buf, u32 cap, u32 mask)
{
    u32 i;
    char ch[2];
    u32 text_c = dg_rgb(0xE8, 0xE8, 0xE8);
    u32 boxbg = dg_rgb(0x08, 0x0E, 0x18);
    dg_fill(x, y, w, h, boxbg);
    dg_rect(x, y, w, h, focus ? dg_rgb(0x38, 0x8F, 0xFF) : dg_rgb(0x2A, 0x4A, 0x78));
    if (focus) {
        dg_fill(x + 1u, y + 1u, w - 2u, 2u, dg_rgb(0x6A, 0xB0, 0xFF));
        dg_fill(x + 1u, y + h - 3u, w - 2u, 2u, dg_rgb(0x18, 0x40, 0x78));
    }
    for (i = 0u; i < cap; i++) {
        if (buf[i] == 0) break;
        if (mask) {
            dg_fill(x + 6u + i * 9u, y + h / 2u - 5u, 5u, 10u, text_c);
        } else {
            ch[0] = buf[i]; ch[1] = 0;
            dg_text(x + 6u + i * 9u, y + (h - 8u) / 2u, ch, text_c, boxbg);
        }
    }
    if (focus && ((pit_tick_count() / 100u) & 1u)) {
        dg_fill(x + 6u + i * 9u, y + 6u, 2u, h - 12u, dg_rgb(0x8A, 0xC8, 0xFF));
    }
}

/* 全屏渐变背景 + 极光光晕（仅进入时绘制一次） */
static void lg_draw_bg(void)
{
    u32 x, y;
    int rx = (int)(DG_W / 2u);
    int ry = (int)((DG_H * 22u) / 100u);
    int cxd = (int)(DG_W / 2u);
    int cyd = (int)((DG_H * 26u) / 100u);
    int px2 = (int)(DG_W * 72u / 100u), py2 = (int)(DG_H * 16u / 100u);
    int rx2 = (int)(DG_W * 16u / 100u), ry2 = (int)(DG_H * 12u / 100u);
    long long R2 = (long long)rx * rx * ry * ry;
    long long R22 = (long long)rx2 * rx2 * ry2 * ry2;
    for (y = 0u; y < DG_H; y++) {
        u32 kk = (y * 255u) / (DG_H ? DG_H : 1u);
        u32 br = (0x10u * (255u - kk) + 0x01u * kk) / 255u;
        u32 bg = (0x20u * (255u - kk) + 0x02u * kk) / 255u;
        u32 bb = (0x42u * (255u - kk) + 0x06u * kk) / 255u;
        dg_fill(0u, y, DG_W, 1u, dg_rgb(br, bg, bb));
    }
    for (y = 0u; y < DG_H; y++) {
        for (x = 0u; x < DG_W; x++) {
            u32 c = dg_fb[y * DG_W + x];
            u32 cr = (c >> 16u) & 0xFFu, cg = (c >> 8u) & 0xFFu, cb = c & 0xFFu;
            int dx = (int)x - cxd, dy = (int)y - cyd;
            long long v = (long long)rx * rx * dy * dy + (long long)ry * ry * dx * dx;
            if (v < R2) {
                int k = (int)((R2 - v) / (R2 / 256LL + 1LL));
                if (k > 255) k = 255;
                cr = (cr * (255u - (u32)k) + 0x22u * (u32)k) / 255u;
                cg = (cg * (255u - (u32)k) + 0x62u * (u32)k) / 255u;
                cb = (cb * (255u - (u32)k) + 0xA8u * (u32)k) / 255u;
            }
            dx = (int)x - px2; dy = (int)y - py2;
            v = (long long)rx2 * rx2 * dy * dy + (long long)ry2 * ry2 * dx * dx;
            if (v < R22) {
                int k = (int)((R22 - v) / (R22 / 256LL + 1LL));
                if (k > 255) k = 255;
                cr = (cr * (255u - (u32)k) + 0x3Au * (u32)k) / 255u;
                cg = (cg * (255u - (u32)k) + 0x20u * (u32)k) / 255u;
                cb = (cb * (255u - (u32)k) + 0x6Au * (u32)k) / 255u;
            }
            dg_fb[y * DG_W + x] = (cr << 16u) | (cg << 8u) | cb;
        }
    }
}

static void lg_draw(void)
{
    u32 first = (lg_flags & LG_FIRST) ? 1u : 0u;
    u32 x, yy;
    u32 fg = dg_rgb(0xE8, 0xE8, 0xE8);
    u32 dim = dg_rgb(0x9A, 0xC8, 0xF0);
    u32 dim2 = dg_rgb(0x70, 0x88, 0xA8);
    u32 panel = dg_rgb(0x0E, 0x16, 0x28);
    u32 panelb = dg_rgb(0x3A, 0x5A, 0x8A);
    u32 ox = (DG_W > 460u) ? (DG_W - 460u) / 2u : 8u;
    u32 oy = (DG_H > 420u) ? (DG_H - 420u) / 2u : 6u;

    /* 圆角面板 + 边框 + 顶部亮条 */
    lg_rrect(ox, oy, 460u, 420u, 12u, panel);
    dg_rect(ox, oy, 460u, 420u, panelb);
    dg_fill(ox + 12u, oy, 460u - 24u, 3u, dg_rgb(0x4A, 0x7A, 0xB8));

    /* 发光 XOS 标志 + 系统名 + 标题 */
    lg_glow_text(ox + 200u, oy + 16u, "XOS", dg_rgb(0xFF, 0xFF, 0xFF),
                 dg_rgb(0x38, 0x8F, 0xFF), panel, 2u);
    dg_text(ox + 196u, oy + 50u, "XOS SYSTEM", dim2, panel);
    lg_glow_text(ox + 170u, oy + 70u, first ? "First Use - Create Admin" : "User Login",
                 fg, dg_rgb(0x2A, 0x6A, 0xB8), panel, 1u);

    /* 输入框组 */
    yy = oy + 106u;
    dg_text(ox + 70u, yy - 6u, "Username", dim, panel);
    lg_input(ox + 70u, yy, 320u, 30u, lg_focus == 0u, lg_name, LG_NAME_LEN, 0u);
    yy += 62u;
    dg_text(ox + 70u, yy - 6u, "Password", dim, panel);
    lg_input(ox + 70u, yy, 320u, 30u, lg_focus == 1u, lg_pass, LG_PASS_LEN, 1u);
    if (first) {
        yy += 62u;
        dg_text(ox + 70u, yy - 6u, "Confirm", dim, panel);
        lg_input(ox + 70u, yy, 320u, 30u, lg_focus == 2u, lg_confirm, LG_PASS_LEN, 1u);
    }

    /* 登录按钮（像素立体感：主色+上下高光） */
    yy += 62u;
    lg_grad_v(ox + 160u, yy, 140u, 36u, dg_rgb(0x2E, 0x7B, 0xE8), dg_rgb(0x18, 0x48, 0xA8));
    dg_fill(ox + 160u, yy, 140u, 2u, dg_rgb(0x6A, 0xA8, 0xF0));
    dg_fill(ox + 160u, yy + 34u, 140u, 2u, dg_rgb(0x0E, 0x2C, 0x6A));
    dg_rect(ox + 160u, yy, 140u, 36u, dg_rgb(0x5A, 0x90, 0xD8));
    dg_text(ox + 160u + 50u, yy + 10u, "Login", dg_rgb(0xFF, 0xFF, 0xFF), dg_rgb(0x20, 0x5A, 0xC8));

    /* 状态行 */
    yy += 42u;
    dg_text(ox + 70u, yy, lg_status, lg_status_c, panel);

    /* 操作提示 */
    yy += 24u;
    if (first) {
        dg_text(ox + 70u, yy, "[Tab] switch  [Enter] login  [BkSp] del  [Esc] text-mode",
                dim2, panel);
        yy += 16u;
        dg_text(ox + 70u, yy, "XOS v1.0 self-built - 5 fails locks account",
                dg_rgb(0x50, 0x68, 0x88), panel);
    } else {
        dg_text(ox + 70u, yy, "[Tab] switch  [Enter] login  [BkSp] del", dim2, panel);
        yy += 16u;
        dg_text(ox + 70u, yy, "[Esc] text-mode login", dim2, panel);
        yy += 16u;
        dg_text(ox + 70u, yy, "XOS v1.0 self-built - 5 fails locks account",
                dg_rgb(0x50, 0x68, 0x88), panel);
    }
    (void)x;
}

/* 提交认证；0=成功（已建会话） */
static int lg_submit(void)
{
    u32 uid = 0u;
    char token[24];
    if (lg_remaining() > 0u) {
        lg_set_status("Account locked, please wait...", dg_rgb(0xFF, 0x60, 0x60));
        return 1;
    }
    if (lg_name[0] == 0) { lg_set_status("Enter username", dg_rgb(0xFF, 0x60, 0x60)); return 1; }
    if (lg_pass[0] == 0) { lg_set_status("Enter password", dg_rgb(0xFF, 0x60, 0x60)); return 1; }
    if (lg_flags & LG_FIRST) {
        if (strcmp(lg_pass, lg_confirm) != 0) {
            lg_set_status("Passwords do not match", dg_rgb(0xFF, 0x60, 0x60));
            lg_clear(lg_pass, LG_PASS_LEN);
            lg_clear(lg_confirm, LG_PASS_LEN);
            return 1;
        }
        if (mu_user_add(lg_name, lg_pass, 0u, &uid) != 0) {
            lg_set_status("Create admin failed (name exists)", dg_rgb(0xFF, 0x60, 0x60));
            lg_clear(lg_name, LG_NAME_LEN);
            lg_clear(lg_pass, LG_PASS_LEN);
            lg_clear(lg_confirm, LG_PASS_LEN);
            return 1;
        }
        mu_session_open(uid, token, sizeof(token));
        dg_welcome = 1u;                 /* 首次使用欢迎向导 */
        lg_set_status("Admin created, entering system...", dg_rgb(0x60, 0xE0, 0x80));
        return 0;
    }
    if (mu_authenticate(lg_name, lg_pass, &uid) == 0) {
        lg_fails = 0u;
        mu_session_open(uid, token, sizeof(token));
        lg_set_status("Login OK, entering system...", dg_rgb(0x60, 0xE0, 0x80));
        return 0;
    }
    lg_fails++;
    if (lg_fails >= LG_FAIL_MAX) {
        mu_user_lock(uid, 1u);
        lg_lock_uid = uid;
        lg_locked_tick = pit_tick_count();
        lg_set_status("5 fails - locked (unlock 30s)", dg_rgb(0xFF, 0x60, 0x60));
    } else {
        char nb[48];
        u32 i = 0u;
        const char *s = "Wrong user or password, left ";
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
    lg_draw_bg();                             /* 渐变+极光背景（一次性） */
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
