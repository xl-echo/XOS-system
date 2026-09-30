/* ============================================================================
 * XOS 窗口管理器（第 20 册：窗口管理器）
 * 完全自研：窗口生命周期/Z序/移动缩放/装饰/最小化最大化还原/焦点/
 * 激活置顶/阴影特效/工作区虚拟桌面/吸附平铺/切换器/多显示器路由/
 * 输入事件命中路由/模态对话框/窗口动画/状态持久化/权限安全/
 * 合成器对接/统计诊断/自检。
 * ========================================================================== */
#ifndef XOS_WINMAN_H
#define XOS_WINMAN_H

#include "types.h"
#include "gui.h"

#define WM_MAX_WIN      16u
#define WM_MAX_WS       4u
#define WM_MAX_ANIM     8u
#define WM_WIN_TITLE    16u

/* 窗口状态标志 */
#define WM_STATE_NORMAL   0u
#define WM_STATE_MIN      1u
#define WM_STATE_MAX      2u

typedef struct {
    u32 id;
    u32 x, y, w, h;
    u32 sx, sy, sw, sh;        /* 还原用保存几何 */
    u32 state;                 /* WM_STATE_* */
    u32 z;
    u32 head;                  /* 显示器头 */
    u32 ws;                    /* 工作区 */
    u32 visible;
    u32 active;
    u32 modal;                 /* 是否模态 */
    u32 owner;                 /* 属主（权限） */
    u32 flags;
    char title[WM_WIN_TITLE];
    u32 used;
} wm_win_t;

typedef struct {
    u32 id;
    u32 active;
    u32 used;
} wm_ws_t;

typedef struct {
    u32 win_id;
    u32 fx, fy, fw, fh;        /* 动画起点 */
    u32 tx, ty, tw, th;        /* 动画终点 */
    u32 frame, total;
    u32 used;
} wm_anim_t;

/* ---------------- API ---------------- */
/* 1. 窗口生命周期 */
int wm_init(void);
int wm_create(u32 x, u32 y, u32 w, u32 h, const char *title, u32 owner);
int wm_destroy(u32 id);
int wm_find(u32 id, wm_win_t *out);
u32 wm_count(void);
u32 wm_next_id(void);

/* 2. Z 序 */
int wm_zorder_raise(u32 id);
int wm_zorder_lower(u32 id);
u32 wm_zorder_top(void);

/* 3. 移动与缩放 */
int wm_move(u32 id, u32 x, u32 y);
int wm_resize(u32 id, u32 w, u32 h);
int wm_resize_min(u32 id, u32 min_w, u32 min_h);

/* 4. 装饰（标题栏） */
int wm_decor_rect(u32 id, u32 *tx, u32 *ty, u32 *tw, u32 *th);
int wm_decor_has(u32 id);

/* 5. 最小化/最大化/还原 */
int wm_minimize(u32 id);
int wm_maximize(u32 id);
int wm_restore(u32 id);
u32 wm_state(u32 id);

/* 6. 焦点 */
int wm_focus_get(u32 *id);
int wm_focus_set(u32 id);
int wm_focus_clear(void);

/* 7. 激活与置顶 */
int wm_activate(u32 id);

/* 8. 阴影特效 */
int wm_shadow(u32 id, u32 *off, u32 *size);

/* 9. 工作区/虚拟桌面 */
int wm_ws_create(u32 id);
int wm_ws_switch(u32 id);
int wm_ws_assign(u32 win_id, u32 ws_id);
u32 wm_ws_active(void);

/* 10. 吸附与平铺 */
int wm_snap(u32 id, u32 screen_w, u32 screen_h, u32 edge, u32 *x, u32 *y);
int wm_tile(u32 a, u32 b, u32 screen_w, u32 screen_h);

/* 11. 切换器 */
int wm_switcher_next(u32 *id);
int wm_switcher_prev(u32 *id);

/* 12. 多显示器 */
int wm_head_assign(u32 id, u32 head);
int wm_head_get(u32 id, u32 *head);

/* 13. 输入事件路由 */
int wm_hit_test(u32 x, u32 y, u32 *id);
int wm_hit_decor(u32 x, u32 y, u32 *id);

/* 14. 模态对话框 */
int wm_modal_push(u32 id);
int wm_modal_pop(void);
u32 wm_modal_active(void);

/* 15. 窗口动画 */
int wm_anim_start(u32 id, u32 tx, u32 ty, u32 tw, u32 th, u32 frames);
int wm_anim_step(void);
u32 wm_anim_count(void);

/* 16. 状态持久化 */
int wm_snapshot(u32 id, wm_win_t *out);
int wm_restore_geom(u32 id, const wm_win_t *in);

/* 17. 权限与安全 */
int wm_can_operate(u32 id, u32 caller_owner);
int wm_set_owner(u32 id, u32 owner);

/* 18. 合成器对接 */
int wm_compositor_commit(void);

/* 19. 统计与诊断 */
u32  wm_stats_ops(void);
void wm_dump(void);

/* 20. 自检 */
int wm_selftest(void);

#endif /* XOS_WINMAN_H */
