/* ============================================================================
 * XOS 控件库（第 22 册：控件库）
 * 完全自研：按钮/复选框/单选框/文本框/列表表格/下拉组合/进度条滑块/
 * 滚动条滚动区域/菜单菜单栏/工具栏状态栏/对话框消息框/标签分组框/
 * 树形控件/选项卡分页/日期时间选择器/富文本代码编辑/图表控件/
 * 布局对齐/主题皮肤/事件消息/测试自动化。
 * ========================================================================== */
#ifndef XOS_WIDGET_H
#define XOS_WIDGET_H

#include "types.h"
#include "gui.h"

#define WDG_MAX_WIDGET   24u
#define WDG_MAX_ITEMS    8u
#define WDG_MAX_EVENTS   8u
#define WDG_MAX_TEXT     16u
#define WDG_MAX_CHILDREN 8u

/* 控件类型 */
#define WDG_BUTTON    1u
#define WDG_CHECK     2u
#define WDG_RADIO     3u
#define WDG_EDIT      4u
#define WDG_LIST      5u
#define WDG_COMBO     6u
#define WDG_PROGRESS  7u
#define WDG_SLIDER    8u
#define WDG_SCROLL    9u
#define WDG_MENU      10u
#define WDG_TOOLBAR   11u
#define WDG_STATUSBAR 12u
#define WDG_DIALOG    13u
#define WDG_LABEL     14u
#define WDG_GROUP     15u
#define WDG_TREE      16u
#define WDG_TABS      17u
#define WDG_DATE      18u
#define WDG_RICH      19u
#define WDG_CHART     20u

/* 控件状态 */
#define WDG_S_NORMAL  0u
#define WDG_S_HOVER   1u
#define WDG_S_PRESSED 2u
#define WDG_S_FOCUS   4u
#define WDG_S_DISABLED 8u
#define WDG_S_CHECKED 16u
#define WDG_S_EXPAND  32u

/* 事件类型 */
#define WDG_EV_CLICK   1u
#define WDG_EV_CHANGE  2u
#define WDG_EV_FOCUS   3u
#define WDG_EV_BLUR    4u

typedef struct {
    u32 type;
    u32 x, y, w, h;
    u32 state;
    u32 parent;
    u32 id;
    u32 value;                 /* 选中索引 / 进度 / 滑杆 / 行号等 */
    char text[WDG_MAX_TEXT];
    u32 items[WDG_MAX_ITEMS];
    u32 item_cnt;
    u32 children[WDG_MAX_CHILDREN];
    u32 child_cnt;
    u32 used;
} wdg_t;

typedef struct {
    u32 type;
    u32 wid;
    u32 param;
    u32 used;
} wdg_ev_t;

/* ---------------- API ---------------- */
/* 1. 按钮 */
int wdg_init(void);
int wdg_button_create(u32 x, u32 y, u32 w, u32 h, const char *text, u32 *id);
int wdg_button_click(u32 id);
int wdg_button_state(u32 id, u32 *state);

/* 2. 复选框与单选框 */
int wdg_check_create(u32 x, u32 y, const char *text, u32 *id);
int wdg_check_toggle(u32 id);
int wdg_check_checked(u32 id, u32 *on);
int wdg_radio_group(u32 first, u32 count);
int wdg_radio_select(u32 id);
u32 wdg_radio_selected(u32 group_first);

/* 3. 文本框与编辑框 */
int wdg_edit_create(u32 x, u32 y, u32 w, const char *text, u32 *id);
int wdg_edit_set(u32 id, const char *text);
int wdg_edit_get(u32 id, char *out, u32 max);
int wdg_edit_caret(u32 id, u32 pos);

/* 4. 列表与表格 */
int wdg_list_create(u32 x, u32 y, u32 w, u32 h, u32 *id);
int wdg_list_add(u32 id, const char *item);
int wdg_list_select(u32 id, u32 index);
int wdg_list_get(u32 id, u32 index, char *out, u32 max);
u32 wdg_list_count(u32 id);

/* 5. 下拉框与组合框 */
int wdg_combo_create(u32 x, u32 y, u32 w, u32 *id);
int wdg_combo_add(u32 id, const char *opt);
int wdg_combo_select(u32 id, u32 index);
int wdg_combo_get(u32 id, u32 *index);

/* 6. 进度条与滑块 */
int wdg_progress_create(u32 x, u32 y, u32 w, u32 *id);
int wdg_progress_set(u32 id, u32 pct);
int wdg_progress_get(u32 id, u32 *pct);
int wdg_slider_create(u32 x, u32 y, u32 w, u32 *id);
int wdg_slider_set(u32 id, u32 pos);
int wdg_slider_get(u32 id, u32 *pos);

/* 7. 滚动条与滚动区域 */
int wdg_scroll_create(u32 x, u32 y, u32 len, u32 *id);
int wdg_scroll_set(u32 id, u32 pos, u32 range);
int wdg_scroll_get(u32 id, u32 *pos);

/* 8. 菜单与菜单栏 */
int wdg_menu_create(u32 x, u32 y, u32 *id);
int wdg_menu_add(u32 id, const char *item, u32 *slot);
int wdg_menu_click(u32 id, u32 slot);

/* 9. 工具栏与状态栏 */
int wdg_toolbar_create(u32 x, u32 y, u32 *id);
int wdg_toolbar_add(u32 id, const char *btn, u32 *slot);
int wdg_statusbar_create(u32 x, u32 y, u32 w, u32 *id);
int wdg_status_set(u32 id, const char *text);

/* 10. 对话框与消息框 */
int wdg_dialog_create(u32 x, u32 y, u32 w, u32 h, const char *title, u32 *id);
int wdg_message_box(const char *text, u32 *id);
int wdg_dialog_close(u32 id);

/* 11. 标签与分组框 */
int wdg_label_create(u32 x, u32 y, const char *text, u32 *id);
int wdg_label_set(u32 id, const char *text);
int wdg_group_create(u32 x, u32 y, u32 w, u32 h, const char *title, u32 *id);
int wdg_group_add(u32 id, u32 child);

/* 12. 树形控件 */
int wdg_tree_create(u32 x, u32 y, u32 w, u32 h, u32 *id);
int wdg_tree_add(u32 id, const char *node, u32 parent, u32 *slot);
int wdg_tree_expand(u32 id, u32 slot);
int wdg_tree_select(u32 id, u32 slot);
int wdg_tree_collapsed(u32 id, u32 slot, u32 *on);

/* 13. 选项卡与分页 */
int wdg_tabs_create(u32 x, u32 y, u32 w, u32 *id);
int wdg_tabs_add(u32 id, const char *tab, u32 *slot);
int wdg_tabs_switch(u32 id, u32 slot);
int wdg_tabs_get(u32 id, u32 *slot);

/* 14. 富文本与代码编辑 */
int wdg_rich_create(u32 x, u32 y, u32 w, u32 h, u32 *id);
int wdg_rich_append(u32 id, const char *line);
int wdg_rich_get(u32 id, u32 line, char *out, u32 max);
u32 wdg_rich_lines(u32 id);

/* 14b. 日期与时间选择器 */
int wdg_date_create(u32 x, u32 y, u32 w, u32 *id);
int wdg_date_set(u32 id, u32 y, u32 m, u32 d);
int wdg_date_get(u32 id, u32 *y, u32 *m, u32 *d);

/* 15. 图表控件 */
int wdg_chart_create(u32 x, u32 y, u32 w, u32 h, u32 *id);
int wdg_chart_set(u32 id, u32 index, u32 value);
int wdg_chart_get(u32 id, u32 index, u32 *value);
u32 wdg_chart_count(u32 id);

/* 16. 控件布局与对齐 */
int wdg_layout_anchor(u32 id, u32 parent_w, u32 parent_h, u32 anchor, u32 margin);
int wdg_layout_align_row(u32 first, u32 count, u32 gap);

/* 17. 控件主题与皮肤 */
int wdg_skin_apply(u32 bg, u32 fg, u32 accent);
int wdg_skin_get(u32 *bg, u32 *fg, u32 *accent);

/* 18. 控件事件与消息 */
int wdg_event_push(u32 type, u32 wid, u32 param);
int wdg_event_next(u32 *type, u32 *wid, u32 *param);
u32 wdg_event_count(void);
int wdg_event_clear(void);

/* 19. 控件测试与自动化 */
u32  wdg_stats(void);
void wdg_dump(void);
int  wdg_selftest(void);

#endif /* XOS_WIDGET_H */
