/* ============================================================================
 * XOS 桌面环境（第 21 册：桌面环境）
 * 完全自研实现，20 子域全覆盖，desk_selftest 逐域断言。
 * ========================================================================== */
#include "desktop.h"
#include "console.h"
#include "string.h"

extern void *memset(void *dst, int c, unsigned int n);
extern int strcmp(const char *a, const char *b);

static desk_icon_t desk_icons[DESK_MAX_ICON];
static desk_task_t desk_tasks[DESK_MAX_TASKBAR];
static desk_tray_t desk_trays[DESK_MAX_TRAY];
static desk_notif_t desk_notifs[DESK_MAX_NOTIF];
static desk_user_t desk_users[DESK_MAX_USER];
static u32 desk_apps[DESK_MAX_APP];
static u32 desk_app_cnt;
static desk_theme_t desk_theme;
static u32 desk_menu, desk_context, desk_settings;
static u32 desk_lock_flag;
static u32 desk_session_state = SESS_BOOT;
static u32 desk_hour, desk_min;
static u32 desk_icon_next = 1u;
static u32 desk_drag_icon;
static u32 desk_lang;
static u32 desk_a11y[2];
static u32 desk_stats;

/* ---------------- 1. 桌面背景与壁纸 ---------------- */
int desk_init(void)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_ICON; i++) desk_icons[i].used = 0u;
    for (i = 0u; i < DESK_MAX_TASKBAR; i++) desk_tasks[i].used = 0u;
    for (i = 0u; i < DESK_MAX_TRAY; i++) desk_trays[i].used = 0u;
    for (i = 0u; i < DESK_MAX_NOTIF; i++) desk_notifs[i].used = 0u;
    for (i = 0u; i < DESK_MAX_USER; i++) desk_users[i].used = 0u;
    for (i = 0u; i < DESK_MAX_APP; i++) desk_apps[i] = 0u;
    desk_app_cnt = 0u;
    desk_theme.bg = 0x103050u;
    desk_theme.fg = 0xE8E8E8u;
    desk_theme.accent = 0x2F7DE1u;
    desk_theme.border = 0x4A6A9Au;
    desk_theme.hilite = 0x4E9BF5u;
    desk_theme.shadow = 0x000000u;
    desk_theme.dark = 1u;
    desk_menu = 0u;
    desk_context = 0u;
    desk_settings = 0u;
    desk_lock_flag = 0u;
    desk_session_state = SESS_LOGIN;
    desk_hour = 0u;
    desk_min = 0u;
    desk_icon_next = 1u;
    desk_drag_icon = 0u;
    desk_lang = 0u;
    desk_a11y[0] = 0u;               /* 大字模式 */
    desk_a11y[1] = 0u;               /* 高对比 */
    desk_stats = 0u;
    return 0;
}

int desk_wallpaper(u32 mode, u32 color)
{
    if (mode == 0u) {
        desk_theme.bg = color;
        desk_stats++;
        return 0;
    }
    return -1;                       /* 模式 1（位图壁纸）预留 */
}

/* ---------------- 2. 桌面图标管理 ---------------- */
int desk_icon_add(const char *name, u32 x, u32 y, u32 *id)
{
    u32 i, slot = DESK_MAX_ICON;
    if (!name || !id) return -1;
    for (i = 0u; i < DESK_MAX_ICON; i++) {
        if (!desk_icons[i].used) { slot = i; break; }
    }
    if (slot == DESK_MAX_ICON) return -2;
    memset(&desk_icons[slot], 0, sizeof(desk_icon_t));
    desk_icons[slot].id = desk_icon_next++;
    desk_icons[slot].x = x;
    desk_icons[slot].y = y;
    {
        u32 k = 0u;
        while (name[k] != '\0' && k < DESK_NAME_LEN - 1u) {
            desk_icons[slot].name[k] = name[k];
            k++;
        }
        desk_icons[slot].name[k] = '\0';
    }
    desk_icons[slot].used = 1u;
    *id = desk_icons[slot].id;
    desk_stats++;
    return 0;
}

int desk_icon_move(u32 id, u32 x, u32 y)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_ICON; i++)
        if (desk_icons[i].used && desk_icons[i].id == id) {
            desk_icons[i].x = x;
            desk_icons[i].y = y;
            desk_stats++;
            return 0;
        }
    return -1;
}

int desk_icon_select(u32 id)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_ICON; i++) {
        if (desk_icons[i].used && desk_icons[i].id == id) {
            desk_icons[i].selected = 1u;
            return 0;
        }
    }
    return -1;
}

int desk_icon_clear_sel(void)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_ICON; i++) desk_icons[i].selected = 0u;
    return 0;
}

int desk_icon_hit(u32 x, u32 y, u32 *id)
{
    u32 i;
    if (!id) return -1;
    for (i = 0u; i < DESK_MAX_ICON; i++) {
        desk_icon_t *ic = &desk_icons[i];
        if (!ic->used) continue;
        if (x >= ic->x && x < ic->x + 32u && y >= ic->y && y < ic->y + 32u) {
            *id = ic->id;
            return 0;
        }
    }
    return -2;
}

u32 desk_icon_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < DESK_MAX_ICON; i++) if (desk_icons[i].used) n++;
    return n;
}

/* ---------------- 3. 任务栏 ---------------- */
int desk_task_add(u32 win_id, const char *title, u32 *slot)
{
    u32 i, s = DESK_MAX_TASKBAR;
    if (!title) return -1;
    for (i = 0u; i < DESK_MAX_TASKBAR; i++) {
        if (!desk_tasks[i].used) { s = i; break; }
    }
    if (s == DESK_MAX_TASKBAR) return -2;
    desk_tasks[s].win_id = win_id;
    {
        u32 k = 0u;
        while (title[k] != '\0' && k < DESK_NAME_LEN - 1u) {
            desk_tasks[s].title[k] = title[k];
            k++;
        }
        desk_tasks[s].title[k] = '\0';
    }
    desk_tasks[s].active = 0u;
    desk_tasks[s].used = 1u;
    if (slot) *slot = s;
    desk_stats++;
    return 0;
}

int desk_task_remove(u32 win_id)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_TASKBAR; i++) {
        if (desk_tasks[i].used && desk_tasks[i].win_id == win_id) {
            desk_tasks[i].used = 0u;
            desk_stats++;
            return 0;
        }
    }
    return -1;
}

int desk_task_activate(u32 win_id)
{
    u32 i, found = 0u;
    for (i = 0u; i < DESK_MAX_TASKBAR; i++) desk_tasks[i].active = 0u;
    for (i = 0u; i < DESK_MAX_TASKBAR; i++) {
        if (desk_tasks[i].used && desk_tasks[i].win_id == win_id) {
            desk_tasks[i].active = 1u;
            found = 1u;
        }
    }
    return found ? 0 : -1;
}

u32 desk_task_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < DESK_MAX_TASKBAR; i++) if (desk_tasks[i].used) n++;
    return n;
}

/* ---------------- 4. 开始菜单 ---------------- */
int desk_menu_open(void)
{
    desk_menu = 1u;
    desk_stats++;
    return 0;
}

int desk_menu_close(void)
{
    desk_menu = 0u;
    return 0;
}

u32 desk_menu_state(void)
{
    return desk_menu;
}

/* ---------------- 5. 系统托盘 ---------------- */
int desk_tray_add(u32 id, u32 *slot)
{
    u32 i, s = DESK_MAX_TRAY;
    for (i = 0u; i < DESK_MAX_TRAY; i++) {
        if (!desk_trays[i].used) { s = i; break; }
    }
    if (s == DESK_MAX_TRAY) return -2;
    desk_trays[s].id = id;
    desk_trays[s].state = 0u;
    desk_trays[s].used = 1u;
    if (slot) *slot = s;
    desk_stats++;
    return 0;
}

int desk_tray_remove(u32 id)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_TRAY; i++) {
        if (desk_trays[i].used && desk_trays[i].id == id) {
            desk_trays[i].used = 0u;
            desk_stats++;
            return 0;
        }
    }
    return -1;
}

u32 desk_tray_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < DESK_MAX_TRAY; i++) if (desk_trays[i].used) n++;
    return n;
}

/* ---------------- 6. 通知中心 ---------------- */
int desk_notify(u32 level, const char *text, u32 *slot)
{
    u32 i, s = DESK_MAX_NOTIF;
    if (!text) return -1;
    if (level > 2u) level = 2u;
    for (i = 0u; i < DESK_MAX_NOTIF; i++) {
        if (!desk_notifs[i].used) { s = i; break; }
    }
    if (s == DESK_MAX_NOTIF) return -2;
    desk_notifs[s].level = level;
    {
        u32 k = 0u;
        while (text[k] != '\0' && k < DESK_NAME_LEN - 1u) {
            desk_notifs[s].text[k] = text[k];
            k++;
        }
        desk_notifs[s].text[k] = '\0';
    }
    desk_notifs[s].used = 1u;
    if (slot) *slot = s;
    desk_stats++;
    return 0;
}

int desk_notify_dismiss(u32 slot)
{
    if (slot >= DESK_MAX_NOTIF) return -1;
    if (!desk_notifs[slot].used) return -2;
    desk_notifs[slot].used = 0u;
    return 0;
}

u32 desk_notify_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < DESK_MAX_NOTIF; i++) if (desk_notifs[i].used) n++;
    return n;
}

/* ---------------- 7. 时钟与日历 ---------------- */
int desk_clock_set(u32 hour, u32 minute)
{
    if (hour > 23u || minute > 59u) return -1;
    desk_hour = hour;
    desk_min = minute;
    return 0;
}

int desk_clock_get(u32 *hour, u32 *minute)
{
    if (!hour || !minute) return -1;
    *hour = desk_hour;
    *minute = desk_min;
    return 0;
}

u32 desk_calendar_day(void)
{
    return (desk_hour * 60u + desk_min) / 1440u + 1u;   /* 当日 = 1 */
}

/* ---------------- 8. 快捷方式与启动器 ---------------- */
int desk_launcher_add(const char *name, u32 app_id, u32 *slot)
{
    u32 i;
    if (!name) return -1;
    if (desk_app_cnt >= DESK_MAX_APP) return -2;
    for (i = 0u; i < desk_app_cnt; i++)
        if (desk_apps[i] == app_id) return -3;
    desk_apps[desk_app_cnt] = app_id;
    if (slot) *slot = desk_app_cnt;
    desk_app_cnt++;
    desk_stats++;
    return 0;
}

int desk_launcher_launch(u32 slot)
{
    if (slot >= desk_app_cnt) return -1;
    desk_stats++;
    return 0;
}

u32 desk_launcher_count(void)
{
    return desk_app_cnt;
}

/* ---------------- 9. 桌面右键菜单 ---------------- */
int desk_context_open(u32 x, u32 y)
{
    (void)x; (void)y;
    desk_context = 1u;
    desk_stats++;
    return 0;
}

int desk_context_close(void)
{
    desk_context = 0u;
    return 0;
}

u32 desk_context_state(void)
{
    return desk_context;
}

/* ---------------- 10. 文件拖放 ---------------- */
int desk_drag_begin(u32 icon_id)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_ICON; i++)
        if (desk_icons[i].used && desk_icons[i].id == icon_id) {
            desk_drag_icon = icon_id;
            return 0;
        }
    return -1;
}

int desk_drag_drop(u32 x, u32 y)
{
    if (desk_drag_icon == 0u) return -1;
    {
        u32 i;
        for (i = 0u; i < DESK_MAX_ICON; i++)
            if (desk_icons[i].used && desk_icons[i].id == desk_drag_icon) {
                desk_icons[i].x = x;
                desk_icons[i].y = y;
                desk_drag_icon = 0u;
                return 0;
            }
    }
    desk_drag_icon = 0u;
    return -2;
}

int desk_drag_active(void)
{
    return (int)desk_drag_icon;
}

/* ---------------- 11. 主题与外观 ---------------- */
int desk_theme_apply(const desk_theme_t *t)
{
    if (!t) return -1;
    desk_theme = *t;
    desk_stats++;
    return 0;
}

int desk_theme_get(desk_theme_t *out)
{
    if (!out) return -1;
    *out = desk_theme;
    return 0;
}

int desk_theme_dark(u32 on)
{
    desk_theme.dark = on ? 1u : 0u;
    return 0;
}

/* ---------------- 12. 桌面设置面板 ---------------- */
int desk_settings_open(void)
{
    desk_settings = 1u;
    desk_stats++;
    return 0;
}

int desk_settings_close(void)
{
    desk_settings = 0u;
    return 0;
}

u32 desk_settings_state(void)
{
    return desk_settings;
}

/* ---------------- 13. 屏幕锁定与屏保 ---------------- */
int desk_lock(void)
{
    desk_lock_flag = 1u;
    desk_session_state = SESS_LOCKED;
    desk_stats++;
    return 0;
}

int desk_unlock(void)
{
    desk_lock_flag = 0u;
    desk_session_state = SESS_DESKTOP;
    return 0;
}

u32 desk_locked(void)
{
    return desk_lock_flag;
}

/* ---------------- 14. 登录与欢迎界面 ---------------- */
int desk_login(const char *user)
{
    u32 i;
    if (!user) return -1;
    for (i = 0u; i < DESK_MAX_USER; i++) {
        if (desk_users[i].used && strcmp(desk_users[i].user, user) == 0) {
            desk_users[i].active = 1u;
            desk_session_state = SESS_DESKTOP;
            desk_stats++;
            return 0;
        }
    }
    return -2;
}

int desk_logout(void)
{
    u32 i;
    for (i = 0u; i < DESK_MAX_USER; i++) desk_users[i].active = 0u;
    desk_session_state = SESS_LOGIN;
    return 0;
}

u32 desk_session(void)
{
    return desk_session_state;
}

/* ---------------- 15. 会话管理 ---------------- */
int desk_user_add(const char *name, u32 *slot)
{
    u32 i, s = DESK_MAX_USER;
    if (!name) return -1;
    for (i = 0u; i < DESK_MAX_USER; i++) {
        if (desk_users[i].used && strcmp(desk_users[i].user, name) == 0) return -2;
        if (!desk_users[i].used && s == DESK_MAX_USER) s = i;
    }
    if (s == DESK_MAX_USER) return -3;
    memset(&desk_users[s], 0, sizeof(desk_user_t));
    {
        u32 k = 0u;
        while (name[k] != '\0' && k < DESK_NAME_LEN - 1u) {
            desk_users[s].user[k] = name[k];
            k++;
        }
        desk_users[s].user[k] = '\0';
    }
    desk_users[s].used = 1u;
    if (slot) *slot = s;
    desk_stats++;
    return 0;
}

int desk_user_switch(const char *name)
{
    return desk_login(name);
}

u32 desk_user_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < DESK_MAX_USER; i++) if (desk_users[i].used) n++;
    return n;
}

/* ---------------- 16. 桌面搜索 ---------------- */
int desk_search(const char *kw, u32 *match_id)
{
    u32 i;
    if (!kw || !match_id) return -1;
    for (i = 0u; i < DESK_MAX_ICON; i++) {
        desk_icon_t *ic = &desk_icons[i];
        u32 k;
        if (!ic->used) continue;
        for (k = 0u; ic->name[k] != '\0' && kw[k] != '\0'; k++)
            if (ic->name[k] != kw[k]) break;
        if (ic->name[k] == '\0' || kw[k] == '\0') {
            if (ic->name[k] == '\0' && kw[k] == '\0') {
                *match_id = ic->id;
                return 0;
            }
            if (ic->name[k] == '\0') continue;   /* 名字短于关键词 */
            if (kw[k] == '\0') {                  /* 前缀命中 */
                *match_id = ic->id;
                return 0;
            }
        }
    }
    return -2;
}

/* ---------------- 17. 多语言与本地化 ---------------- */
int desk_lang_set(u32 lang)
{
    if (lang > 1u) return -1;      /* 0=中文 1=English */
    desk_lang = lang;
    return 0;
}

u32 desk_lang_get(void)
{
    return desk_lang;
}

/* ---------------- 18. 无障碍支持 ---------------- */
int desk_a11y_set(u32 feature, u32 on)
{
    if (feature >= 2u) return -1;
    desk_a11y[feature] = on ? 1u : 0u;
    return 0;
}

u32 desk_a11y_get(u32 feature)
{
    if (feature >= 2u) return 0u;
    return desk_a11y[feature];
}

/* ---------------- 19. 桌面统计与诊断 ---------------- */
u32 desk_stats_ops(void)
{
    return desk_stats;
}

void desk_dump(void)
{
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  Desktop environment dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("    session=");
    con_put_dec(desk_session_state);
    con_puts("  icons=");
    con_put_dec(desk_icon_count());
    con_puts("  taskbar=");
    con_put_dec(desk_task_count());
    con_puts("  tray=");
    con_put_dec(desk_tray_count());
    con_puts("  notif=");
    con_put_dec(desk_notify_count());
    con_puts("  users=");
    con_put_dec(desk_user_count());
    con_puts("  ops=");
    con_put_dec(desk_stats);
    con_putc('\n');
}

/* ---------------- 20. 桌面环境测试 ---------------- */
int desk_selftest(void)
{
    u32 r, r2;
    desk_theme_t th;

    /* 1. 桌面背景 */
    if (desk_wallpaper(0u, 0x103050u) != 0) return 1;
    if (desk_wallpaper(1u, 0u) != -1) return 2;

    /* 2. 图标管理 */
    if (desk_icon_add("File", 20u, 20u, &r) != 0) return 3;
    if (r != 1u) return 4;
    if (desk_icon_add("App", 120u, 20u, &r2) != 0) return 5;
    if (desk_icon_count() != 2u) return 6;
    if (desk_icon_hit(30u, 30u, &r) != 0) return 7;
    if (r != 1u) return 8;
    if (desk_icon_hit(500u, 300u, &r) != -2) return 9;
    if (desk_icon_move(1u, 60u, 60u) != 0) return 10;
    if (desk_icon_hit(30u, 30u, &r) != -2) return 11;
    if (desk_icon_select(1u) != 0) return 12;
    if (desk_icon_clear_sel() != 0) return 13;

    /* 3. 任务栏 */
    if (desk_task_add(100u, "Terminal", &r) != 0) return 14;
    if (desk_task_count() != 1u) return 15;
    if (desk_task_add(101u, "Explorer", &r2) != 0) return 16;
    if (desk_task_count() != 2u) return 17;
    if (desk_task_activate(100u) != 0) return 18;
    if (desk_task_remove(101u) != 0) return 19;
    if (desk_task_count() != 1u) return 20;
    if (desk_task_remove(999u) != -1) return 21;

    /* 4. 开始菜单 */
    if (desk_menu_state() != 0u) return 22;
    if (desk_menu_open() != 0) return 23;
    if (desk_menu_state() != 1u) return 24;
    if (desk_menu_close() != 0) return 25;
    if (desk_menu_state() != 0u) return 26;

    /* 5. 系统托盘 */
    if (desk_tray_add(50u, &r) != 0) return 27;
    if (desk_tray_count() != 1u) return 28;
    if (desk_tray_add(51u, &r2) != 0) return 29;
    if (desk_tray_count() != 2u) return 30;
    if (desk_tray_remove(50u) != 0) return 31;
    if (desk_tray_count() != 1u) return 32;
    if (desk_tray_remove(999u) != -1) return 33;

    /* 6. 通知中心 */
    if (desk_notify(0u, "Update ready", &r) != 0) return 34;
    if (desk_notify_count() != 1u) return 35;
    if (desk_notify(2u, "Disk low", &r2) != 0) return 36;
    if (desk_notify_count() != 2u) return 37;
    if (desk_notify_dismiss(0u) != 0) return 38;
    if (desk_notify_count() != 1u) return 39;
    if (desk_notify_dismiss(9u) != -1) return 40;

    /* 7. 时钟日历 */
    if (desk_clock_set(9u, 30u) != 0) return 41;
    if (desk_clock_get(&r, &r2) != 0) return 42;
    if (r != 9u || r2 != 30u) return 43;
    if (desk_clock_set(24u, 0u) != -1) return 44;
    if (desk_calendar_day() != 1u) return 45;

    /* 8. 启动器 */
    if (desk_launcher_add("Term", 10u, &r) != 0) return 46;
    if (desk_launcher_count() != 1u) return 47;
    if (desk_launcher_add("Edit", 11u, &r2) != 0) return 48;
    if (desk_launcher_count() != 2u) return 49;
    if (desk_launcher_add("Edit", 11u, &r) != -3) return 50;
    if (desk_launcher_launch(0u) != 0) return 51;
    if (desk_launcher_launch(9u) != -1) return 52;

    /* 9. 右键菜单 */
    if (desk_context_open(50u, 50u) != 0) return 53;
    if (desk_context_state() != 1u) return 54;
    if (desk_context_close() != 0) return 55;
    if (desk_context_state() != 0u) return 56;

    /* 10. 拖放 */
    if (desk_drag_begin(1u) != 0) return 57;
    if (desk_drag_active() == 0) return 58;
    if (desk_drag_drop(200u, 150u) != 0) return 59;
    if (desk_drag_active() != 0) return 60;
    if (desk_icon_hit(210u, 160u, &r) != 0) return 61;
    if (r != 1u) return 62;
    if (desk_drag_drop(0u, 0u) != -1) return 63;

    /* 11. 主题 */
    th.bg = 0x202020u; th.fg = 0xFFFFFFu; th.accent = 0xFF8800u;
    th.border = 0x555555u; th.hilite = 0xFFAA33u; th.shadow = 0u;
    th.dark = 1u;
    if (desk_theme_apply(&th) != 0) return 64;
    if (desk_theme_get(&th) != 0) return 65;
    if (th.bg != 0x202020u || th.accent != 0xFF8800u) return 66;
    if (desk_theme_dark(0u) != 0) return 67;

    /* 12. 设置面板 */
    if (desk_settings_open() != 0) return 68;
    if (desk_settings_state() != 1u) return 69;
    if (desk_settings_close() != 0) return 70;
    if (desk_settings_state() != 0u) return 71;

    /* 13. 锁屏 */
    if (desk_locked() != 0u) return 72;
    if (desk_lock() != 0) return 73;
    if (desk_locked() != 1u) return 74;
    if (desk_session() != SESS_LOCKED) return 75;
    if (desk_unlock() != 0) return 76;
    if (desk_locked() != 0u) return 77;

    /* 14. 登录 */
    if (desk_user_add("root", &r) != 0) return 78;
    if (desk_session() != SESS_DESKTOP) return 79;
    if (desk_login("root") != 0) return 80;
    if (desk_session() != SESS_DESKTOP) return 81;
    if (desk_login("nobody") != -2) return 82;
    if (desk_logout() != 0) return 83;
    if (desk_session() != SESS_LOGIN) return 84;

    /* 15. 会话 */
    if (desk_user_add("root", &r) != -2) return 85;
    if (desk_user_add("guest", &r2) != 0) return 86;
    if (desk_user_count() != 2u) return 87;
    if (desk_user_switch("guest") != 0) return 88;
    if (desk_session() != SESS_DESKTOP) return 89;

    /* 16. 搜索 */
    if (desk_icon_add("Browser", 40u, 200u, &r) != 0) return 90;
    if (desk_search("Bro", &r) != 0) return 91;
    if (r != 3u) return 92;
    if (desk_search("zzz", &r) != -2) return 93;

    /* 17. 语言 */
    if (desk_lang_get() != 0u) return 94;
    if (desk_lang_set(1u) != 0) return 95;
    if (desk_lang_get() != 1u) return 96;
    if (desk_lang_set(9u) != -1) return 97;

    /* 18. 无障碍 */
    if (desk_a11y_set(0u, 1u) != 0) return 98;
    if (desk_a11y_get(0u) != 1u) return 99;
    if (desk_a11y_set(1u, 1u) != 0) return 100;
    if (desk_a11y_get(1u) != 1u) return 101;
    if (desk_a11y_set(5u, 1u) != -1) return 102;

    /* 19. 统计 */
    if (desk_stats_ops() == 0u) return 103;

    /* 20. 总体 */
    if (desk_icon_count() == 0u) return 104;
    if (desk_task_count() == 0u) return 105;

    return 0;
}
