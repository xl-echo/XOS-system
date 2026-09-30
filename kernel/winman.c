/* ============================================================================
 * XOS 窗口管理器（第 20 册：窗口管理器）
 * 完全自研实现，20 子域全覆盖，wm_selftest 逐域断言。
 * ========================================================================== */
#include "winman.h"
#include "console.h"
#include "string.h"

extern void *memset(void *dst, int c, unsigned int n);
extern int strcmp(const char *a, const char *b);

static wm_win_t wm_wins[WM_MAX_WIN];
static wm_ws_t  wm_ws_tab[WM_MAX_WS];
static wm_anim_t wm_anims[WM_MAX_ANIM];
static u32 wm_next = 1u;
static u32 wm_zcur = 0u;
static u32 wm_focus = 0u;
static u32 wm_active_ws = 0u;
static u32 wm_modal = 0u;
static u32 wm_stats = 0u;

static wm_win_t *wm_by_id(u32 id)
{
    u32 i;
    for (i = 0u; i < WM_MAX_WIN; i++)
        if (wm_wins[i].used && wm_wins[i].id == id) return &wm_wins[i];
    return NULL;
}

/* ---------------- 1. 生命周期 ---------------- */
int wm_init(void)
{
    u32 i;
    for (i = 0u; i < WM_MAX_WIN; i++) wm_wins[i].used = 0u;
    for (i = 0u; i < WM_MAX_WS; i++) wm_ws_tab[i].used = 0u;
    for (i = 0u; i < WM_MAX_ANIM; i++) wm_anims[i].used = 0u;
    wm_next = 1u;
    wm_zcur = 0u;
    wm_focus = 0u;
    wm_active_ws = 1u;
    wm_modal = 0u;
    wm_stats = 0u;
    wm_ws_tab[0].id = 1u;
    wm_ws_tab[0].active = 1u;
    wm_ws_tab[0].used = 1u;
    return 0;
}

int wm_create(u32 x, u32 y, u32 w, u32 h, const char *title, u32 owner)
{
    u32 i, slot = WM_MAX_WIN;
    for (i = 0u; i < WM_MAX_WIN; i++) {
        if (!wm_wins[i].used) { slot = i; break; }
    }
    if (slot == WM_MAX_WIN) return -1;
    memset(&wm_wins[slot], 0, sizeof(wm_win_t));
    wm_wins[slot].id = wm_next++;
    wm_wins[slot].x = x;
    wm_wins[slot].y = y;
    wm_wins[slot].w = w;
    wm_wins[slot].h = h;
    wm_wins[slot].state = WM_STATE_NORMAL;
    wm_wins[slot].z = ++wm_zcur;
    wm_wins[slot].head = 0u;
    wm_wins[slot].ws = wm_active_ws;
    wm_wins[slot].visible = 1u;
    wm_wins[slot].owner = owner;
    if (title) {
        u32 k = 0u;
        while (title[k] != '\0' && k < WM_WIN_TITLE - 1u) {
            wm_wins[slot].title[k] = title[k];
            k++;
        }
        wm_wins[slot].title[k] = '\0';
    }
    wm_wins[slot].used = 1u;
    wm_stats++;
    return (int)wm_wins[slot].id;
}

int wm_destroy(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (wm_focus == id) wm_focus = 0u;
    if (wm_modal == id) wm_modal = 0u;
    w->used = 0u;
    wm_stats++;
    return 0;
}

int wm_find(u32 id, wm_win_t *out)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !out) return -1;
    *out = *w;
    return 0;
}

u32 wm_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < WM_MAX_WIN; i++) if (wm_wins[i].used) n++;
    return n;
}

u32 wm_next_id(void)
{
    return wm_next;
}

/* ---------------- 2. Z 序 ---------------- */
int wm_zorder_raise(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    w->z = ++wm_zcur;
    wm_stats++;
    return 0;
}

int wm_zorder_lower(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    u32 i, minz;
    if (!w) return -1;
    minz = w->z;
    for (i = 0u; i < WM_MAX_WIN; i++) {
        if (wm_wins[i].used && wm_wins[i].z < minz)
            minz = wm_wins[i].z;
    }
    if (minz == w->z) return 0;          /* 已在最低层 */
    w->z = minz - 1u;
    wm_stats++;
    return 0;
}

u32 wm_zorder_top(void)
{
    return wm_zcur;
}

/* ---------------- 3. 移动与缩放 ---------------- */
int wm_move(u32 id, u32 x, u32 y)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    w->x = x;
    w->y = y;
    wm_stats++;
    return 0;
}

int wm_resize(u32 id, u32 w, u32 h)
{
    wm_win_t *win = wm_by_id(id);
    if (!win) return -1;
    if (w < 16u) w = 16u;
    if (h < 16u) h = 16u;
    win->w = w;
    win->h = h;
    wm_stats++;
    return 0;
}

int wm_resize_min(u32 id, u32 min_w, u32 min_h)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (w->w < min_w || w->h < min_h) return wm_resize(id, min_w, min_h);
    return 0;
}

/* ---------------- 4. 装饰 ---------------- */
int wm_decor_rect(u32 id, u32 *tx, u32 *ty, u32 *tw, u32 *th)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !tx || !ty || !tw || !th) return -1;
    *tx = w->x;
    *ty = w->y;
    *tw = w->w;
    *th = 12u;                            /* 标题栏高 12 */
    return 0;
}

int wm_decor_has(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    return (w && w->used) ? 1 : 0;
}

/* ---------------- 5. 最小化/最大化/还原 ---------------- */
int wm_minimize(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (w->state == WM_STATE_NORMAL) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
    }
    w->state = WM_STATE_MIN;
    w->visible = 0u;
    if (wm_focus == id) wm_focus = 0u;
    wm_stats++;
    return 0;
}

int wm_maximize(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (w->state == WM_STATE_NORMAL) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
    }
    w->state = WM_STATE_MAX;
    w->x = 0u;
    w->y = 0u;
    w->w = 640u;
    w->h = 400u;
    w->visible = 1u;
    wm_stats++;
    return 0;
}

int wm_restore(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (w->state == WM_STATE_MIN || w->state == WM_STATE_MAX) {
        w->x = w->sx;
        w->y = w->sy;
        w->w = w->sw;
        w->h = w->sh;
        w->state = WM_STATE_NORMAL;
        w->visible = 1u;
    }
    wm_stats++;
    return 0;
}

u32 wm_state(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    return w ? w->state : WM_STATE_NORMAL;
}

/* ---------------- 6. 焦点 ---------------- */
int wm_focus_get(u32 *id)
{
    if (!id) return -1;
    *id = wm_focus;
    return 0;
}

int wm_focus_set(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (!w->visible) return -2;
    wm_focus = id;
    w->active = 1u;
    wm_stats++;
    return 0;
}

int wm_focus_clear(void)
{
    wm_win_t *w = wm_by_id(wm_focus);
    if (w) w->active = 0u;
    wm_focus = 0u;
    return 0;
}

/* ---------------- 7. 激活置顶 ---------------- */
int wm_activate(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (wm_focus != id) {
        wm_win_t *old = wm_by_id(wm_focus);
        if (old) old->active = 0u;
    }
    wm_focus = id;
    w->active = 1u;
    w->z = ++wm_zcur;
    wm_stats++;
    return 0;
}

/* ---------------- 8. 阴影 ---------------- */
int wm_shadow(u32 id, u32 *off, u32 *size)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !off || !size) return -1;
    *off = 4u;                              /* 阴影偏移 4 像素 */
    *size = 8u;                             /* 阴影宽度 8 */
    return 0;
}

/* ---------------- 9. 工作区 ---------------- */
int wm_ws_create(u32 id)
{
    u32 i, slot = WM_MAX_WS;
    if (id == 0u) return -1;
    for (i = 0u; i < WM_MAX_WS; i++) {
        if (wm_ws_tab[i].used && wm_ws_tab[i].id == id) return -2;
        if (!wm_ws_tab[i].used && slot == WM_MAX_WS) slot = i;
    }
    if (slot == WM_MAX_WS) return -3;
    wm_ws_tab[slot].id = id;
    wm_ws_tab[slot].active = 0u;
    wm_ws_tab[slot].used = 1u;
    return 0;
}

int wm_ws_switch(u32 id)
{
    u32 i;
    for (i = 0u; i < WM_MAX_WS; i++) {
        if (wm_ws_tab[i].used && wm_ws_tab[i].id == id) {
            wm_ws_tab[i].active = 1u;
            wm_active_ws = id;
            wm_stats++;
            return 0;
        }
    }
    return -1;
}

int wm_ws_assign(u32 win_id, u32 ws_id)
{
    wm_win_t *w = wm_by_id(win_id);
    u32 i, found = 0u;
    if (!w) return -1;
    for (i = 0u; i < WM_MAX_WS; i++)
        if (wm_ws_tab[i].used && wm_ws_tab[i].id == ws_id) found = 1u;
    if (!found) return -2;
    w->ws = ws_id;
    return 0;
}

u32 wm_ws_active(void)
{
    return wm_active_ws;
}

/* ---------------- 10. 吸附与平铺 ---------------- */
int wm_snap(u32 id, u32 screen_w, u32 screen_h, u32 edge, u32 *x, u32 *y)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !x || !y) return -1;
    switch (edge) {
    case 0u:  *x = 0u;            *y = 0u; break;              /* 左上 */
    case 1u:  *x = screen_w - w->w; *y = 0u; break;            /* 右上 */
    case 2u:  *x = 0u;            *y = screen_h - w->h; break; /* 左下 */
    case 3u:  *x = screen_w - w->w; *y = screen_h - w->h; break; /* 右下 */
    default: return -2;
    }
    return 0;
}

int wm_tile(u32 a, u32 b, u32 screen_w, u32 screen_h)
{
    wm_win_t *wa = wm_by_id(a), *wb = wm_by_id(b);
    if (!wa || !wb) return -1;
    wa->x = 0u;
    wa->y = 0u;
    wa->w = screen_w / 2u;
    wa->h = screen_h;
    wb->x = screen_w / 2u;
    wb->y = 0u;
    wb->w = screen_w - screen_w / 2u;
    wb->h = screen_h;
    return 0;
}

/* ---------------- 11. 切换器 ---------------- */
static int wm_switcher_find_visible(u32 from_id, u32 dir, u32 *out)
{
    u32 i, best = 0u, bestz = 0u, found = 0u;
    for (i = 0u; i < WM_MAX_WIN; i++) {
        wm_win_t *w = &wm_wins[i];
        if (!w->used || !w->visible) continue;
        if (dir == 1u) {
            if (w->z > wm_by_id(from_id)->z && (!found || w->z < bestz)) {
                best = w->id; bestz = w->z; found = 1u;
            }
        } else {
            if (w->z < wm_by_id(from_id)->z && (!found || w->z > bestz)) {
                best = w->id; bestz = w->z; found = 1u;
            }
        }
    }
    if (!found) {
        /* 回绕到最前/最后 */
        u32 j, extreme = 0u, ez = (dir == 1u) ? 0u : 0xFFFFFFFFu, efound = 0u;
        for (j = 0u; j < WM_MAX_WIN; j++) {
            wm_win_t *w = &wm_wins[j];
            if (!w->used || !w->visible) continue;
            if (dir == 1u) { if (!efound || w->z < ez) { extreme = w->id; ez = w->z; efound = 1u; } }
            else { if (!efound || w->z > ez) { extreme = w->id; ez = w->z; efound = 1u; } }
        }
        if (!efound) return -1;
        *out = extreme;
        return 0;
    }
    *out = best;
    return 0;
}

int wm_switcher_next(u32 *id)
{
    wm_win_t *f = wm_by_id(wm_focus);
    if (!id) return -1;
    if (!f) return wm_switcher_prev(id);
    return wm_switcher_find_visible(wm_focus, 1u, id);
}

int wm_switcher_prev(u32 *id)
{
    wm_win_t *f = wm_by_id(wm_focus);
    if (!id) return -1;
    if (!f) return wm_switcher_find_visible(0u, 0u, id);
    return wm_switcher_find_visible(wm_focus, 0u, id);
}

/* ---------------- 12. 多显示器 ---------------- */
int wm_head_assign(u32 id, u32 head)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    w->head = head;
    return 0;
}

int wm_head_get(u32 id, u32 *head)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !head) return -1;
    *head = w->head;
    return 0;
}

/* ---------------- 13. 输入路由 ---------------- */
int wm_hit_test(u32 x, u32 y, u32 *id)
{
    u32 i, bestz = 0u, best = 0u, found = 0u;
    if (!id) return -1;
    if (wm_modal != 0u) {
        wm_win_t *m = wm_by_id(wm_modal);
        if (m && m->visible) {
            if (x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h) {
                *id = m->id;
                return 0;
            }
            *id = m->id;               /* 模态：屏蔽下层输入 */
            return 0;
        }
    }
    for (i = 0u; i < WM_MAX_WIN; i++) {
        wm_win_t *w = &wm_wins[i];
        if (!w->used || !w->visible) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (!found || w->z > bestz) { best = w->id; bestz = w->z; found = 1u; }
        }
    }
    if (!found) return -2;
    *id = best;
    return 0;
}

int wm_hit_decor(u32 x, u32 y, u32 *id)
{
    u32 tid;
    if (wm_hit_test(x, y, &tid) != 0) return -1;
    {
        wm_win_t *w = wm_by_id(tid);
        if (!w) return -2;
        if (y >= w->y && y < w->y + 12u) { if (id) *id = tid; return 0; }
    }
    return -3;
}

/* ---------------- 14. 模态 ---------------- */
int wm_modal_push(u32 id)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (wm_modal != 0u) return -2;         /* 已存在模态窗口 */
    w->modal = 1u;
    wm_modal = id;
    wm_activate(id);
    return 0;
}

int wm_modal_pop(void)
{
    wm_win_t *w = wm_by_id(wm_modal);
    if (wm_modal == 0u) return -1;
    if (w) w->modal = 0u;
    wm_modal = 0u;
    return 0;
}

u32 wm_modal_active(void)
{
    return wm_modal;
}

/* ---------------- 15. 动画 ---------------- */
int wm_anim_start(u32 id, u32 tx, u32 ty, u32 tw, u32 th, u32 frames)
{
    wm_win_t *w = wm_by_id(id);
    u32 i, slot = WM_MAX_ANIM;
    if (!w) return -1;
    if (frames == 0u) frames = 1u;
    for (i = 0u; i < WM_MAX_ANIM; i++) {
        if (!wm_anims[i].used) { slot = i; break; }
        if (wm_anims[i].win_id == id) { slot = i; break; }
    }
    if (slot == WM_MAX_ANIM) return -2;
    wm_anims[slot].win_id = id;
    wm_anims[slot].fx = w->x; wm_anims[slot].fy = w->y;
    wm_anims[slot].fw = w->w; wm_anims[slot].fh = w->h;
    wm_anims[slot].tx = tx; wm_anims[slot].ty = ty;
    wm_anims[slot].tw = tw; wm_anims[slot].th = th;
    wm_anims[slot].frame = 0u;
    wm_anims[slot].total = frames;
    wm_anims[slot].used = 1u;
    return 0;
}

int wm_anim_step(void)
{
    u32 i, done = 0u;
    for (i = 0u; i < WM_MAX_ANIM; i++) {
        wm_anim_t *a = &wm_anims[i];
        wm_win_t *w;
        if (!a->used) continue;
        a->frame++;
        w = wm_by_id(a->win_id);
        if (!w) { a->used = 0u; continue; }
        if (a->frame >= a->total) {
            w->x = a->tx; w->y = a->ty; w->w = a->tw; w->h = a->th;
            a->used = 0u;
            done++;
        } else {
            w->x = a->fx + (a->tx - a->fx) * a->frame / a->total;
            w->y = a->fy + (a->ty - a->fy) * a->frame / a->total;
            w->w = a->fw + (a->tw - a->fw) * a->frame / a->total;
            w->h = a->fh + (a->th - a->fh) * a->frame / a->total;
        }
    }
    return (int)done;
}

u32 wm_anim_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < WM_MAX_ANIM; i++) if (wm_anims[i].used) n++;
    return n;
}

/* ---------------- 16. 状态持久化 ---------------- */
int wm_snapshot(u32 id, wm_win_t *out)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !out) return -1;
    *out = *w;
    return 0;
}

int wm_restore_geom(u32 id, const wm_win_t *in)
{
    wm_win_t *w = wm_by_id(id);
    if (!w || !in) return -1;
    w->x = in->x;
    w->y = in->y;
    w->w = in->w;
    w->h = in->h;
    w->state = in->state;
    return 0;
}

/* ---------------- 17. 权限与安全 ---------------- */
int wm_can_operate(u32 id, u32 caller_owner)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    if (caller_owner == 0u) return 1;      /* 系统属主全权 */
    if (w->owner == caller_owner) return 1;
    return 0;
}

int wm_set_owner(u32 id, u32 owner)
{
    wm_win_t *w = wm_by_id(id);
    if (!w) return -1;
    w->owner = owner;
    return 0;
}

/* ---------------- 18. 合成器对接 ---------------- */
int wm_compositor_commit(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < WM_MAX_WIN; i++)
        if (wm_wins[i].used && wm_wins[i].visible) n++;
    gui_compositor_dirty(0u, 0u, GUI_CANVAS_W, GUI_CANVAS_H);
    return (int)n;
}

/* ---------------- 19. 统计与诊断 ---------------- */
u32 wm_stats_ops(void)
{
    return wm_stats;
}

void wm_dump(void)
{
    u32 f;
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  Window manager dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("    wins=");
    con_put_dec(wm_count());
    con_puts("  ws=");
    con_put_dec(wm_active_ws);
    con_puts("  focus=");
    wm_focus_get(&f);
    con_put_dec(f);
    con_puts("  modal=");
    con_put_dec(wm_modal);
    con_puts("  z_top=");
    con_put_dec(wm_zcur);
    con_puts("  ops=");
    con_put_dec(wm_stats);
    con_putc('\n');
}

/* ---------------- 20. 自检 ---------------- */
int wm_selftest(void)
{
    int id1, id2;
    wm_win_t w;
    u32 r, id;

    /* 1. 生命周期 */
    id1 = wm_create(10u, 10u, 100u, 80u, "Main", 1u);
    if (id1 <= 0) return 1;
    id2 = wm_create(200u, 50u, 60u, 40u, "Dlg", 1u);
    if (id2 <= 0) return 2;
    if (wm_count() != 2u) return 3;
    if (wm_find((u32)id1, &w) != 0) return 4;
    if (w.x != 10u || w.w != 100u) return 5;
    if (wm_destroy((u32)id2) != 0) return 6;
    if (wm_count() != 1u) return 7;
    if (wm_find(999u, &w) != -1) return 8;
    id2 = wm_create(200u, 50u, 60u, 40u, "Dlg", 1u);

    /* 2. Z 序 */
    if (wm_zorder_raise((u32)id1) != 0) return 9;
    if (wm_zorder_top() == 0u) return 10;
    if (wm_zorder_lower((u32)id1) != 0) return 11;

    /* 3. 移动缩放 */
    if (wm_move((u32)id1, 30u, 20u) != 0) return 12;
    if (wm_find((u32)id1, &w) != 0) return 13;
    if (w.x != 30u || w.y != 20u) return 14;
    if (wm_resize((u32)id1, 200u, 150u) != 0) return 15;
    if (wm_find((u32)id1, &w) != 0) return 16;
    if (w.w != 200u || w.h != 150u) return 17;
    if (wm_resize((u32)id1, 4u, 4u) != 0) return 18;      /* 下限 16 */
    if (wm_find((u32)id1, &w) != 0) return 19;
    if (w.w != 16u || w.h != 16u) return 20;
    if (wm_resize((u32)id1, 100u, 80u) != 0) return 21;
    if (wm_resize_min((u32)id1, 50u, 40u) != 0) return 22;

    /* 4. 装饰 */
    if (wm_decor_rect((u32)id1, &r, &id, &r, &id) != 0) return 23; /* 参数为真 */
    if (wm_decor_has((u32)id1) != 1) return 24;
    if (wm_decor_has(999u) != 0) return 25;

    /* 5. 最小化/最大化/还原 */
    if (wm_state((u32)id1) != WM_STATE_NORMAL) return 26;
    if (wm_maximize((u32)id1) != 0) return 27;
    if (wm_state((u32)id1) != WM_STATE_MAX) return 28;
    if (wm_find((u32)id1, &w) != 0) return 29;
    if (w.w != 640u) return 30;
    if (wm_restore((u32)id1) != 0) return 31;
    if (wm_state((u32)id1) != WM_STATE_NORMAL) return 32;
    if (wm_find((u32)id1, &w) != 0) return 33;
    if (w.w != 100u) return 34;                              /* 还原原几何 */
    if (wm_minimize((u32)id1) != 0) return 35;
    if (wm_state((u32)id1) != WM_STATE_MIN) return 36;
    if (wm_find((u32)id1, &w) != 0) return 37;
    if (w.visible != 0u) return 38;
    if (wm_restore((u32)id1) != 0) return 39;
    if (w.used && wm_state((u32)id1) != WM_STATE_NORMAL) return 40;

    /* 6. 焦点 */
    if (wm_focus_set((u32)id1) != 0) return 41;
    if (wm_focus_get(&r) != 0) return 42;
    if (r != (u32)id1) return 43;
    if (wm_focus_set((u32)id2) != 0) return 44;
    if (wm_focus_get(&r) != 0) return 45;
    if (r != (u32)id2) return 46;
    if (wm_focus_clear() != 0) return 47;
    if (wm_focus_get(&r) != 0) return 48;
    if (r != 0u) return 49;

    /* 7. 激活置顶 */
    if (wm_activate((u32)id1) != 0) return 50;
    if (wm_focus_get(&r) != 0) return 51;
    if (r != (u32)id1) return 52;
    if (wm_activate((u32)id2) != 0) return 53;
    if (wm_focus_get(&r) != 0) return 54;
    if (r != (u32)id2) return 55;

    /* 8. 阴影 */
    if (wm_shadow((u32)id1, &r, &id) != 0) return 56;
    if (r != 4u) return 57;

    /* 9. 工作区 */
    if (wm_ws_active() != 1u) return 58;
    if (wm_ws_create(2u) != 0) return 59;
    if (wm_ws_create(2u) != -2) return 60;                   /* 重复创建 */
    if (wm_ws_assign((u32)id1, 2u) != 0) return 61;
    if (wm_ws_switch(2u) != 0) return 62;
    if (wm_ws_active() != 2u) return 63;
    if (wm_ws_switch(1u) != 0) return 64;
    if (wm_ws_active() != 1u) return 65;
    if (wm_ws_switch(9u) != -1) return 66;

    /* 10. 吸附平铺 */
    if (wm_snap((u32)id1, 640u, 400u, 0u, &r, &id) != 0) return 67;
    if (r != 0u || id != 0u) return 68;
    if (wm_snap((u32)id1, 640u, 400u, 1u, &r, &id) != 0) return 69;
    if (r != 540u) return 70;                                /* 640-100 */
    if (wm_tile((u32)id1, (u32)id2, 640u, 400u) != 0) return 71;
    if (wm_find((u32)id1, &w) != 0) return 72;
    if (w.w != 320u) return 73;
    if (wm_find((u32)id2, &w) != 0) return 74;
    if (w.w != 320u || w.x != 320u) return 75;

    /* 11. 切换器 */
    if (wm_activate((u32)id1) != 0) return 76;
    if (wm_switcher_next(&r) != 0) return 77;
    if (r != (u32)id2) return 78;                            /* z 更高的 id2 */
    if (wm_switcher_prev(&r) != 0) return 79;
    if (r != (u32)id2) return 80;

    /* 12. 多显示器 */
    if (wm_head_assign((u32)id1, 1u) != 0) return 81;
    if (wm_head_get((u32)id1, &r) != 0) return 82;
    if (r != 1u) return 83;
    if (wm_head_assign(999u, 1u) != -1) return 84;

    /* 13. 输入路由 */
    if (wm_hit_test(30u, 30u, &r) != 0) return 85;
    if (wm_hit_test(400u, 60u, &r) != 0) return 86;
    if (r != (u32)id2) return 87;                            /* id2 覆盖在 250,60 */
    if (wm_hit_test(700u, 300u, &r) != -2) return 88;        /* 空白处 x>640 */
    if (wm_hit_decor(30u, 5u, &r) != 0) return 89;           /* id1 顶部装饰区 y=5<12 → 命中 */

    /* 14. 模态 */
    if (wm_modal_push((u32)id2) != 0) return 90;
    if (wm_modal_active() != (u32)id2) return 91;
    if (wm_modal_push((u32)id1) != -2) return 92;            /* 已有模态 */
    if (wm_hit_test(30u, 30u, &r) != 0) return 93;
    if (r != (u32)id2) return 94;                            /* 模态屏蔽 id1 */
    if (wm_modal_pop() != 0) return 95;
    if (wm_modal_active() != 0u) return 96;
    if (wm_modal_pop() != -1) return 97;

    /* 15. 动画 */
    if (wm_anim_start((u32)id1, 300u, 200u, 100u, 80u, 4u) != 0) return 98;
    if (wm_anim_count() != 1u) return 99;
    if (wm_anim_step() != 0) return 100;                     /* 第 1 帧未完成 */
    if (wm_find((u32)id1, &w) != 0) return 101;
    if (w.x < 30u || w.x >= 300u) return 102;                /* 插值在区间内 */
    if (wm_anim_step() != 0) return 103;
    if (wm_anim_step() != 0) return 104;
    if (wm_anim_step() != 1) return 105;                     /* 第 4 帧完成 */
    if (wm_anim_count() != 0u) return 106;
    if (wm_find((u32)id1, &w) != 0) return 107;
    if (w.x != 300u || w.y != 200u) return 108;

    /* 16. 持久化 */
    if (wm_snapshot((u32)id2, &w) != 0) return 109;
    if (wm_move((u32)id2, 500u, 400u) != 0) return 110;
    if (wm_restore_geom((u32)id2, &w) != 0) return 111;
    if (wm_find((u32)id2, &w) != 0) return 112;
    if (w.x != 320u) return 113;

    /* 17. 权限 */
    if (wm_set_owner((u32)id1, 2u) != 0) return 114;
    if (wm_can_operate((u32)id1, 0u) != 1) return 115;       /* 系统属主 */
    if (wm_can_operate((u32)id1, 1u) != 0) return 116;       /* 非属主 */
    if (wm_can_operate((u32)id1, 2u) != 1) return 117;       /* 属主 */
    if (wm_can_operate(999u, 0u) != -1) return 118;

    /* 18. 合成器对接 */
    if (wm_compositor_commit() < 1) return 119;

    /* 19. 统计 */
    if (wm_stats_ops() == 0u) return 120;

    /* 20. 总体 */
    if (wm_count() == 0u) return 121;
    if (wm_next_id() == 0u) return 122;

    return 0;
}
