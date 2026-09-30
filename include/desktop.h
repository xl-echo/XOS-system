/* ============================================================================
 * XOS 桌面环境（第 21 册：桌面环境）
 * 完全自研：桌面背景/壁纸、图标管理、任务栏、开始菜单、系统托盘、
 * 通知中心、时钟日历、快捷方式启动器、桌面右键菜单、文件拖放、
 * 主题外观、桌面设置面板、屏幕锁定屏保、登录欢迎界面、会话管理、
 * 桌面搜索、多语言本地化、无障碍支持、统计诊断、自检。
 * ========================================================================== */
#ifndef XOS_DESKTOP_H
#define XOS_DESKTOP_H

#include "types.h"

#define DESK_MAX_ICON    8u
#define DESK_MAX_TASKBAR 8u
#define DESK_MAX_TRAY    4u
#define DESK_MAX_NOTIF   4u
#define DESK_MAX_APP     6u
#define DESK_MAX_USER    2u
#define DESK_NAME_LEN    16u

/* 会话状态机 */
#define SESS_BOOT    0u
#define SESS_LOGIN   1u
#define SESS_DESKTOP 2u
#define SESS_LOCKED  3u
#define SESS_SHUTDOWN 4u

typedef struct {
    u32 id;
    u32 x, y;                      /* 网格位置 */
    char name[DESK_NAME_LEN];
    u32 selected;
    u32 used;
} desk_icon_t;

typedef struct {
    u32 win_id;
    char title[DESK_NAME_LEN];
    u32 active;
    u32 used;
} desk_task_t;

typedef struct {
    u32 id;
    u32 state;
    u32 used;
} desk_tray_t;

typedef struct {
    u32 level;                     /* 0 信息 / 1 警告 / 2 错误 */
    char text[DESK_NAME_LEN];
    u32 used;
} desk_notif_t;

typedef struct {
    u32 bg, fg, accent, border, hilite, shadow;   /* 主题配色 RGB 值 */
    u32 dark;
} desk_theme_t;

typedef struct {
    char user[DESK_NAME_LEN];
    u32 active;
    u32 used;
} desk_user_t;

/* ---------------- API ---------------- */
/* 1. 桌面背景与壁纸 */
int desk_init(void);
int desk_wallpaper(u32 mode, u32 color);

/* 2. 桌面图标管理 */
int desk_icon_add(const char *name, u32 x, u32 y, u32 *id);
int desk_icon_move(u32 id, u32 x, u32 y);
int desk_icon_select(u32 id);
int desk_icon_clear_sel(void);
int desk_icon_hit(u32 x, u32 y, u32 *id);
u32 desk_icon_count(void);

/* 3. 任务栏 */
int desk_task_add(u32 win_id, const char *title, u32 *slot);
int desk_task_remove(u32 win_id);
int desk_task_activate(u32 win_id);
u32 desk_task_count(void);

/* 4. 开始菜单 */
int desk_menu_open(void);
int desk_menu_close(void);
u32 desk_menu_state(void);

/* 5. 系统托盘 */
int desk_tray_add(u32 id, u32 *slot);
int desk_tray_remove(u32 id);
u32 desk_tray_count(void);

/* 6. 通知中心 */
int desk_notify(u32 level, const char *text, u32 *slot);
int desk_notify_dismiss(u32 slot);
u32 desk_notify_count(void);

/* 7. 时钟与日历 */
int desk_clock_set(u32 hour, u32 minute);
int desk_clock_get(u32 *hour, u32 *minute);
u32 desk_calendar_day(void);

/* 8. 快捷方式与启动器 */
int desk_launcher_add(const char *name, u32 app_id, u32 *slot);
int desk_launcher_launch(u32 slot);
u32 desk_launcher_count(void);

/* 9. 桌面右键菜单 */
int desk_context_open(u32 x, u32 y);
int desk_context_close(void);
u32 desk_context_state(void);

/* 10. 文件拖放 */
int desk_drag_begin(u32 icon_id);
int desk_drag_drop(u32 x, u32 y);
int desk_drag_active(void);

/* 11. 主题与外观 */
int desk_theme_apply(const desk_theme_t *t);
int desk_theme_get(desk_theme_t *out);
int desk_theme_dark(u32 on);

/* 12. 桌面设置面板 */
int desk_settings_open(void);
int desk_settings_close(void);
u32 desk_settings_state(void);

/* 13. 屏幕锁定与屏保 */
int desk_lock(void);
int desk_unlock(void);
u32 desk_locked(void);

/* 14. 登录与欢迎界面 */
int desk_login(const char *user);
int desk_logout(void);
u32 desk_session(void);

/* 15. 会话管理 */
int desk_user_add(const char *name, u32 *slot);
int desk_user_switch(const char *name);
u32 desk_user_count(void);

/* 16. 桌面搜索 */
int desk_search(const char *kw, u32 *match_id);

/* 17. 多语言与本地化 */
int desk_lang_set(u32 lang);
u32 desk_lang_get(void);

/* 18. 无障碍支持 */
int desk_a11y_set(u32 feature, u32 on);
u32 desk_a11y_get(u32 feature);

/* 19. 桌面统计与诊断 */
u32  desk_stats_ops(void);
void desk_dump(void);

/* 20. 桌面环境测试 */
int desk_selftest(void);

#endif /* XOS_DESKTOP_H */
