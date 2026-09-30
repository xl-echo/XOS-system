/* ============================================================================
 * XOS 图形子系统（GUI 图形层，第 19 册：图形界面-GUI）
 * 完全自研：图形上下文/内存画布/调色板/像素读写/点线矩形圆椭圆/裁剪栈/
 * 位图与透明blit/点阵字体与文本输出/事件环形队列/窗口对象与Z序/
 * 合成器与脏矩形/软件光标精灵/双缓冲后备/缩放/抗锯齿/统计诊断/自检。
 * 不依赖任何外部 GUI/图形库；以内存画布为确定性测试底座。
 * ========================================================================== */
#ifndef XOS_GUI_H
#define XOS_GUI_H

#include "types.h"

#define GUI_CANVAS_W     128u
#define GUI_CANVAS_H     64u
#define GUI_EVENT_MAX    64u          /* 事件环形队列容量 */
#define GUI_WIN_MAX      16u          /* 窗口表容量 */
#define GUI_CLIP_MAX     8u           /* 裁剪矩形栈深度 */
#define GUI_FONT_W       8u
#define GUI_FONT_H       8u
#define GUI_FONT_W16     8u
#define GUI_FONT_H16     16u

/* 16 色 VGA 调色板索引 */
#define GUI_C_BLACK       0u
#define GUI_C_BLUE        1u
#define GUI_C_GREEN       2u
#define GUI_C_CYAN        3u
#define GUI_C_RED         4u
#define GUI_C_MAGENTA     5u
#define GUI_C_BROWN       6u
#define GUI_C_LGRAY       7u
#define GUI_C_DGRAY       8u
#define GUI_C_LBLUE       9u
#define GUI_C_LGREEN     10u
#define GUI_C_LCYAN      11u
#define GUI_C_LRED       12u
#define GUI_C_LMAGENTA   13u
#define GUI_C_YELLOW     14u
#define GUI_C_WHITE      15u

/* ROP */
#define GUI_ROP_COPY      0u
#define GUI_ROP_XOR       1u

/* 事件类型 */
#define GUI_EV_KEY        1u
#define GUI_EV_MOUSE      2u
#define GUI_EV_BUTTON     3u
#define GUI_EV_MOTION     4u

/* 窗口标志 */
#define GUI_WIN_VISIBLE   0x01u
#define GUI_WIN_ACTIVE    0x02u

typedef struct {
    u32 fg, bg;            /* 前景/背景调色板索引 */
    u32 rop;
    u32 line_w;            /* 线宽 */
    u32 clip_top;          /* 裁剪矩形 */
    u32 clip_left;
    u32 clip_bottom;       /* 开区间 */
    u32 clip_right;
} gui_gc_t;

typedef struct {
    u8  *data;
    u32 w, h, pitch;
    u32 used;
} gui_canvas_t;

typedef struct {
    u32 type;
    u32 x, y;              /* 鼠标坐标 / 键扫描码 */
    u32 key;
    u32 mods;
} gui_event_t;

typedef struct {
    u32 id;
    u32 x, y, w, h;
    u32 flags;
    u32 z;                 /* Z 序（大=靠前） */
    u32 title_len;
    char title[16];
    u32 used;
} gui_win_t;

/* ---------------- API ---------------- */
/* 图形上下文与画布 */
void gui_gc_init(gui_gc_t *gc);
int  gui_canvas_create(gui_canvas_t *cv, u32 w, u32 h);
void gui_canvas_clear(gui_canvas_t *cv, u32 color);
int  gui_canvas_destroy(gui_canvas_t *cv);

/* 颜色与调色板 */
u32  gui_rgb888(u32 r, u32 g, u32 b);          /* → 调色板索引 */
int  gui_palette_set(u32 idx, u32 r, u32 g, u32 b);
int  gui_palette_get(u32 idx, u32 *r, u32 *g, u32 *b);

/* 像素读写 */
int gui_put_pixel(gui_canvas_t *cv, u32 x, u32 y, u32 color);
int gui_get_pixel(gui_canvas_t *cv, u32 x, u32 y, u32 *color);

/* 图元 */
int gui_draw_line(gui_canvas_t *cv, u32 x0, u32 y0, u32 x1, u32 y1, u32 color);
int gui_fill_rect(gui_canvas_t *cv, u32 x, u32 y, u32 w, u32 h, u32 color);
int gui_draw_rect(gui_canvas_t *cv, u32 x, u32 y, u32 w, u32 h, u32 color);
int gui_draw_circle(gui_canvas_t *cv, u32 cx, u32 cy, u32 r, u32 color);
int gui_fill_circle(gui_canvas_t *cv, u32 cx, u32 cy, u32 r, u32 color);

/* 裁剪 */
int gui_push_clip(gui_canvas_t *cv, u32 x, u32 y, u32 w, u32 h);
int gui_pop_clip(void);
int gui_clip_test(u32 x, u32 y);

/* 位图 */
int gui_blit1(gui_canvas_t *cv, const u8 *bitmap, u32 x, u32 y, u32 w, u32 h,
              u32 fg, u32 bg, u32 transparent);
int gui_blit8(gui_canvas_t *cv, const u8 *pix, u32 x, u32 y, u32 w, u32 h);
int gui_blit_scale(gui_canvas_t *cv, const u8 *pix, u32 sw, u32 sh,
                   u32 x, u32 y, u32 dw, u32 dh);

/* 字体与文本 */
int  gui_draw_char(gui_canvas_t *cv, u8 ch, u32 x, u32 y, u32 fg, u32 bg,
                   u32 h16);
int  gui_draw_text(gui_canvas_t *cv, const char *s, u32 x, u32 y, u32 fg,
                   u32 bg, u32 h16);
u32  gui_text_width(const char *s);
int  gui_draw_text_center(gui_canvas_t *cv, const char *s, u32 cx, u32 y,
                          u32 fg, u32 bg, u32 h16);

/* 事件队列 */
int  gui_event_push(u32 type, u32 x, u32 y, u32 key, u32 mods);
int  gui_event_pop(gui_event_t *ev);
int  gui_event_peek(gui_event_t *ev);
u32  gui_event_count(void);

/* 窗口对象 */
int gui_win_create(gui_win_t *w, u32 x, u32 y, u32 ww, u32 wh, const char *title);
int gui_win_destroy(u32 id);
int gui_win_move(u32 id, u32 x, u32 y);
int gui_win_resize(u32 id, u32 w, u32 h);
int gui_win_show(u32 id, u32 visible);
int gui_win_raise(u32 id);                 /* 置顶（提升 Z 序） */
int gui_win_find(u32 id, gui_win_t *out);
u32 gui_win_count(void);

/* 合成器与双缓冲 */
int  gui_backbuf_create(void);
int  gui_compositor_present(void);         /* 后备 → 主画布 */
int  gui_compositor_dirty(u32 x, u32 y, u32 w, u32 h);
u32  gui_compositor_dirty_count(void);

/* 光标精灵 */
int gui_cursor_set(const u8 *mask, u32 w, u32 h);   /* 1bpp 掩码 */
int gui_cursor_move(u32 x, u32 y);
int gui_cursor_get(u32 *x, u32 *y);
void gui_cursor_show(u32 on);

/* 缩放与抗锯齿 */
int gui_aa_line(gui_canvas_t *cv, u32 x0, u32 y0, u32 x1, u32 y1, u32 color);

/* 统计与诊断 */
u32  gui_stats_draw_calls(void);
u32  gui_stats_events(void);
void gui_dump(void);

/* 初始化和自检 */
void gui_init(void);
int  gui_selftest(void);

#endif /* XOS_GUI_H */
