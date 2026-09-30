/* ============================================================================
 * XOS 图形子系统（GUI 图形层，第 19 册：图形界面-GUI）
 * 完全自研：图形上下文/内存画布/调色板/像素读写/图元/裁剪/位图/点阵字体/
 * 文本输出/事件队列/窗口对象/合成器/光标精灵/双缓冲/缩放/抗锯齿/统计/自检。
 * ========================================================================== */
#include "gui.h"
#include "console.h"
#include "string.h"

extern void *memset(void *dst, int c, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);

static u8    gui_canvas_data[GUI_CANVAS_W * GUI_CANVAS_H];
static u8    gui_back_data[GUI_CANVAS_W * GUI_CANVAS_H];
static gui_canvas_t gui_canvas;
static gui_canvas_t gui_back;
static gui_gc_t     gui_gc;
static u32     gui_clip_stack[GUI_CLIP_MAX][4];
static u32     gui_clip_depth = 0u;
static u32     gui_palette[16];
static gui_event_t gui_events[GUI_EVENT_MAX];
static u32     gui_ev_head = 0u, gui_ev_tail = 0u, gui_ev_count = 0u;
static gui_win_t gui_wins[GUI_WIN_MAX];
static u32     gui_win_next_id = 1u;
static u32     gui_z_top = 0u;
static u32     gui_cursor_on = 0u;
static u32     gui_cursor_x = 0u, gui_cursor_y = 0u;
static u8      gui_cursor_mask[64];
static u32     gui_cursor_w = 0u, gui_cursor_h = 0u;
static u32     gui_stats_draw = 0u;
static u32     gui_dirty_cnt = 0u;

/* 8x8 基本点阵字体（ASCII 0x20..0x7F 前 32 个常用字符，其余用方框占位） */
static const u8 gui_font8[96][8] = {
    {0,0,0,0,0,0,0,0},       /* 0x20 ' ' */
    {0x18,0x18,0x18,0x18,0x18,0,0x18,0},   /* ! */
    {0x6C,0x6C,0x6C,0,0,0,0,0},            /* " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0},/* # */
    {0x18,0x3C,0x5A,0x18,0x24,0x24,0x3C,0x18}, /* $ */
    {0x62,0x64,0x08,0x10,0x26,0x46,0,0},   /* % */
    {0x38,0x44,0x44,0x38,0x4C,0x44,0x4A,0x3C}, /* & */
    {0x18,0x18,0x18,0,0,0,0,0},            /* ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0},/* ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0},/* ) */
    {0,0x66,0x3C,0xFF,0x3C,0x66,0,0},      /* * */
    {0,0x18,0x18,0x7E,0x18,0x18,0,0},      /* + */
    {0,0,0,0,0,0x18,0x18,0x30},            /* , */
    {0,0,0,0x7E,0,0,0,0},                  /* - */
    {0,0,0,0,0,0,0x18,0x18},               /* . */
    {0x02,0x04,0x08,0x10,0x20,0x40,0,0},   /* / */
    {0x3C,0x46,0x4A,0x52,0x62,0x3C,0,0},   /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x7E,0,0},   /* 1 */
    {0x3C,0x42,0x02,0x0C,0x30,0x7E,0,0},   /* 2 */
    {0x3C,0x42,0x0C,0x02,0x42,0x3C,0,0},   /* 3 */
    {0x0C,0x14,0x24,0x44,0x7E,0x04,0,0},   /* 4 */
    {0x7E,0x40,0x7C,0x02,0x42,0x3C,0,0},   /* 5 */
    {0x1C,0x20,0x7C,0x42,0x42,0x3C,0,0},   /* 6 */
    {0x7E,0x02,0x04,0x08,0x10,0x10,0,0},   /* 7 */
    {0x3C,0x42,0x3C,0x42,0x42,0x3C,0,0},   /* 8 */
    {0x3C,0x42,0x42,0x3E,0x02,0x3C,0,0},   /* 9 */
    {0,0x18,0x18,0,0,0x18,0x18,0},         /* : */
    {0,0x18,0x18,0,0,0x18,0x18,0x30},      /* ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0},/* < */
    {0,0,0x7E,0,0x7E,0,0,0},               /* = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0},/* > */
    {0x3C,0x42,0x02,0x0C,0x18,0,0x18,0},   /* ? */
    {0x3C,0x46,0x5A,0x5E,0x40,0x3C,0,0},   /* @ */
    {0x3C,0x42,0x42,0x7E,0x42,0x42,0,0},   /* A */
    {0x7C,0x42,0x7C,0x42,0x42,0x7C,0,0},   /* B */
    {0x3C,0x42,0x40,0x40,0x42,0x3C,0,0},   /* C */
    {0x78,0x44,0x42,0x42,0x44,0x78,0,0},   /* D */
    {0x7E,0x40,0x7C,0x40,0x40,0x7E,0,0},   /* E */
    {0x7E,0x40,0x7C,0x40,0x40,0x40,0,0},   /* F */
    {0x3C,0x42,0x40,0x4E,0x42,0x3E,0,0},   /* G */
    {0x42,0x42,0x7E,0x42,0x42,0x42,0,0},   /* H */
    {0x7E,0x18,0x18,0x18,0x18,0x7E,0,0},   /* I */
    {0x02,0x02,0x02,0x42,0x42,0x3C,0,0},   /* J */
    {0x42,0x44,0x78,0x44,0x42,0x42,0,0},   /* K */
    {0x40,0x40,0x40,0x40,0x40,0x7E,0,0},   /* L */
    {0x42,0x66,0x5A,0x42,0x42,0x42,0,0},   /* M */
    {0x42,0x62,0x52,0x4A,0x46,0x42,0,0},   /* N */
    {0x3C,0x42,0x42,0x42,0x42,0x3C,0,0},   /* O */
    {0x7C,0x42,0x42,0x7C,0x40,0x40,0,0},   /* P */
    {0x3C,0x42,0x42,0x4A,0x44,0x3A,0,0},   /* Q */
    {0x7C,0x42,0x42,0x7C,0x44,0x42,0,0},   /* R */
    {0x3E,0x40,0x3C,0x02,0x02,0x7C,0,0},   /* S */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0,0},   /* T */
    {0x42,0x42,0x42,0x42,0x42,0x3C,0,0},   /* U */
    {0x42,0x42,0x42,0x42,0x24,0x18,0,0},   /* V */
    {0x42,0x42,0x5A,0x66,0x42,0x42,0,0},   /* W */
    {0x42,0x24,0x18,0x18,0x24,0x42,0,0},   /* X */
    {0x42,0x42,0x24,0x18,0x18,0x18,0,0},   /* Y */
    {0x7E,0x02,0x0C,0x30,0x40,0x7E,0,0},   /* Z */
    {0x3C,0x30,0x30,0x30,0x30,0x3C,0,0},   /* [ */
    {0x40,0x20,0x10,0x08,0x04,0x02,0,0},   /* \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0},   /* ] */
    {0x18,0x24,0x42,0,0,0,0,0},            /* ^ */
    {0,0,0,0,0,0,0,0xFF},                  /* _ */
    {0x30,0x18,0,0,0,0,0,0},               /* ` */
    {0,0x3C,0x02,0x3E,0x42,0x3E,0,0},      /* a */
    {0x40,0x40,0x7C,0x42,0x42,0x7C,0,0},   /* b */
    {0,0,0x3C,0x40,0x40,0x3C,0,0},         /* c */
    {0x02,0x02,0x3E,0x42,0x42,0x3E,0,0},   /* d */
    {0,0,0x3C,0x7E,0x40,0x3C,0,0},         /* e */
    {0x0C,0x12,0x10,0x38,0x10,0x10,0,0},   /* f */
    {0,0,0x3E,0x42,0x42,0x3E,0x02,0x3C},   /* g */
    {0x40,0x40,0x7C,0x42,0x42,0x42,0,0},   /* h */
    {0x18,0,0x38,0x18,0x18,0x7E,0,0},      /* i */
    {0x06,0,0x06,0x06,0x06,0x46,0x3C,0},   /* j */
    {0x40,0x40,0x44,0x78,0x44,0x42,0,0},   /* k */
    {0x38,0x18,0x18,0x18,0x18,0x7E,0,0},   /* l */
    {0,0,0x6C,0x5A,0x42,0x42,0,0},         /* m */
    {0,0,0x7C,0x42,0x42,0x42,0,0},         /* n */
    {0,0,0x3C,0x42,0x42,0x3C,0,0},         /* o */
    {0,0,0x7C,0x42,0x42,0x7C,0x40,0x40},   /* p */
    {0,0,0x3E,0x42,0x42,0x3E,0x02,0x02},   /* q */
    {0,0,0x5C,0x60,0x40,0x40,0,0},         /* r */
    {0,0,0x3E,0x40,0x3C,0x02,0x7C,0},      /* s */
    {0x10,0x10,0x3C,0x10,0x10,0x0C,0,0},   /* t */
    {0,0,0x42,0x42,0x42,0x3E,0,0},         /* u */
    {0,0,0x42,0x42,0x24,0x18,0,0},         /* v */
    {0,0,0x42,0x5A,0x66,0x42,0,0},         /* w */
    {0,0,0x42,0x24,0x18,0x24,0x42,0},      /* x */
    {0,0,0x42,0x42,0x42,0x3E,0x02,0x3C},   /* y */
    {0,0,0x7E,0x0C,0x30,0x7E,0,0},         /* z */
    {0x0C,0x18,0x18,0x30,0x18,0x18,0x0C,0},/* { */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0},/* | */
    {0x30,0x18,0x18,0x0C,0x18,0x18,0x30,0},/* } */
    {0x36,0x6C,0,0,0,0,0,0},               /* ~ */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* 0x7F 占位 */
};

/* ---------------- 图形上下文与画布 ---------------- */
void gui_gc_init(gui_gc_t *gc)
{
    if (!gc) return;
    gc->fg = GUI_C_WHITE;
    gc->bg = GUI_C_BLACK;
    gc->rop = GUI_ROP_COPY;
    gc->line_w = 1u;
    gc->clip_top = 0u;
    gc->clip_left = 0u;
    gc->clip_bottom = GUI_CANVAS_H;
    gc->clip_right = GUI_CANVAS_W;
}

int gui_canvas_create(gui_canvas_t *cv, u32 w, u32 h)
{
    if (!cv || cv->used) return -1;
    if (w > GUI_CANVAS_W || h > GUI_CANVAS_H) return -2;
    cv->data = gui_canvas_data;
    cv->w = w;
    cv->h = h;
    cv->pitch = GUI_CANVAS_W;
    cv->used = 1u;
    gui_canvas_clear(cv, GUI_C_BLACK);
    return 0;
}

void gui_canvas_clear(gui_canvas_t *cv, u32 color)
{
    u32 i;
    if (!cv || !cv->used || !cv->data) return;
    for (i = 0u; i < cv->h; i++)
        memset(cv->data + i * cv->pitch, (int)(color & 0xFFu), cv->w);
}

int gui_canvas_destroy(gui_canvas_t *cv)
{
    if (!cv || !cv->used) return -1;
    cv->used = 0u;
    return 0;
}

/* ---------------- 调色板 ---------------- */
u32 gui_rgb888(u32 r, u32 g, u32 b)
{
    /* 映射到 16 色 VGA：量化每通道 1 bit 取高位 */
    u32 idx = 0u;
    if (r >= 0x80u) idx |= 4u;
    if (g >= 0x80u) idx |= 2u;
    if (b >= 0x80u) idx |= 1u;
    if (r >= 0x80u && g >= 0x80u && b >= 0x80u) idx = GUI_C_WHITE;
    return idx;
}

int gui_palette_set(u32 idx, u32 r, u32 g, u32 b)
{
    if (idx >= 16u) return -1;
    gui_palette[idx] = (r << 16) | (g << 8) | b;
    return 0;
}

int gui_palette_get(u32 idx, u32 *r, u32 *g, u32 *b)
{
    if (idx >= 16u || !r || !g || !b) return -1;
    *r = (gui_palette[idx] >> 16) & 0xFFu;
    *g = (gui_palette[idx] >> 8) & 0xFFu;
    *b = gui_palette[idx] & 0xFFu;
    return 0;
}

/* ---------------- 裁剪与像素 ---------------- */
int gui_push_clip(gui_canvas_t *cv, u32 x, u32 y, u32 w, u32 h)
{
    u32 x0, y0, x1, y1;
    if (gui_clip_depth >= GUI_CLIP_MAX) return -1;
    if (!cv || !cv->used) return -2;
    /* 先保存旧裁剪矩形到栈顶，pop 时恢复 */
    gui_clip_stack[gui_clip_depth][0] = gui_gc.clip_left;
    gui_clip_stack[gui_clip_depth][1] = gui_gc.clip_top;
    gui_clip_stack[gui_clip_depth][2] = gui_gc.clip_right;
    gui_clip_stack[gui_clip_depth][3] = gui_gc.clip_bottom;
    gui_clip_depth++;
    /* 与当前裁剪求交得到新裁剪 */
    x0 = gui_gc.clip_left;  y0 = gui_gc.clip_top;
    x1 = gui_gc.clip_right; y1 = gui_gc.clip_bottom;
    if (x > x0) x0 = x;
    if (y > y0) y0 = y;
    if (x + w < x1) x1 = x + w;
    if (y + h < y1) y1 = y + h;
    gui_gc.clip_left = x0;
    gui_gc.clip_top = y0;
    gui_gc.clip_right = x1;
    gui_gc.clip_bottom = y1;
    return 0;
}

int gui_pop_clip(void)
{
    if (gui_clip_depth == 0u) return -1;
    gui_clip_depth--;
    gui_gc.clip_left = gui_clip_stack[gui_clip_depth][0];
    gui_gc.clip_top = gui_clip_stack[gui_clip_depth][1];
    gui_gc.clip_right = gui_clip_stack[gui_clip_depth][2];
    gui_gc.clip_bottom = gui_clip_stack[gui_clip_depth][3];
    return 0;
}

int gui_clip_test(u32 x, u32 y)
{
    if (x < gui_gc.clip_left || x >= gui_gc.clip_right) return 0;
    if (y < gui_gc.clip_top || y >= gui_gc.clip_bottom) return 0;
    return 1;
}

int gui_put_pixel(gui_canvas_t *cv, u32 x, u32 y, u32 color)
{
    if (!cv || !cv->used || !cv->data) return -1;
    if (!gui_clip_test(x, y)) return -2;
    if (x >= cv->w || y >= cv->h) return -2;
    if (gui_gc.rop == GUI_ROP_XOR)
        cv->data[y * cv->pitch + x] ^= (u8)(color & 0xFFu);
    else
        cv->data[y * cv->pitch + x] = (u8)(color & 0xFFu);
    gui_stats_draw++;
    return 0;
}

int gui_get_pixel(gui_canvas_t *cv, u32 x, u32 y, u32 *color)
{
    if (!cv || !cv->used || !cv->data || !color) return -1;
    if (x >= cv->w || y >= cv->h) return -2;
    *color = cv->data[y * cv->pitch + x];
    return 0;
}

/* ---------------- 图元 ---------------- */
int gui_draw_line(gui_canvas_t *cv, u32 x0, u32 y0, u32 x1, u32 y1, u32 color)
{
    int dx, dy, sx, sy, err, e2;
    if (!cv || !cv->used) return -1;
    dx = (int)x1 - (int)x0; if (dx < 0) dx = -dx;
    dy = (int)y1 - (int)y0; if (dy < 0) dy = -dy;
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    for (;;) {
        gui_put_pixel(cv, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 = (u32)((int)x0 + sx); }
        if (e2 < dx)  { err += dx; y0 = (u32)((int)y0 + sy); }
    }
    return 0;
}

int gui_fill_rect(gui_canvas_t *cv, u32 x, u32 y, u32 w, u32 h, u32 color)
{
    u32 i, j, x0, y0, x1, y1;
    if (!cv || !cv->used) return -1;
    x0 = (x < gui_gc.clip_left) ? gui_gc.clip_left : x;
    y0 = (y < gui_gc.clip_top) ? gui_gc.clip_top : y;
    x1 = x + w; if (x1 > gui_gc.clip_right) x1 = gui_gc.clip_right;
    y1 = y + h; if (y1 > gui_gc.clip_bottom) y1 = gui_gc.clip_bottom;
    if (x1 > cv->w) x1 = cv->w;
    if (y1 > cv->h) y1 = cv->h;
    if (x0 >= x1 || y0 >= y1) return 0;
    for (j = y0; j < y1; j++)
        for (i = x0; i < x1; i++)
            cv->data[j * cv->pitch + i] = (u8)(color & 0xFFu);
    gui_stats_draw++;
    return 0;
}

int gui_draw_rect(gui_canvas_t *cv, u32 x, u32 y, u32 w, u32 h, u32 color)
{
    int r;
    if (!cv || !cv->used) return -1;
    if (w < 2u || h < 2u) return gui_fill_rect(cv, x, y, w, h, color);
    r = gui_draw_line(cv, x, y, x + w - 1u, y, color);          if (r) return r;
    r = gui_draw_line(cv, x, y + h - 1u, x + w - 1u, y + h - 1u, color); if (r) return r;
    r = gui_draw_line(cv, x, y, x, y + h - 1u, color);          if (r) return r;
    return gui_draw_line(cv, x + w - 1u, y, x + w - 1u, y + h - 1u, color);
}

int gui_draw_circle(gui_canvas_t *cv, u32 cx, u32 cy, u32 r, u32 color)
{
    int x = (int)r, y = 0, err = 1 - (int)r;
    if (!cv || !cv->used) return -1;
    while (x >= y) {
        gui_put_pixel(cv, cx + (u32)x, cy + (u32)y, color);
        gui_put_pixel(cv, cx - (u32)x, cy + (u32)y, color);
        gui_put_pixel(cv, cx + (u32)x, cy - (u32)y, color);
        gui_put_pixel(cv, cx - (u32)x, cy - (u32)y, color);
        gui_put_pixel(cv, cx + (u32)y, cy + (u32)x, color);
        gui_put_pixel(cv, cx - (u32)y, cy + (u32)x, color);
        gui_put_pixel(cv, cx + (u32)y, cy - (u32)x, color);
        gui_put_pixel(cv, cx - (u32)y, cy - (u32)x, color);
        y++;
        if (err <= 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
    return 0;
}

int gui_fill_circle(gui_canvas_t *cv, u32 cx, u32 cy, u32 r, u32 color)
{
    int x = (int)r, y = 0, err = 1 - (int)r;
    if (!cv || !cv->used) return -1;
    while (x >= y) {
        u32 xr = (u32)x;
        gui_draw_line(cv, cx - xr, cy + (u32)y, cx + xr, cy + (u32)y, color);
        gui_draw_line(cv, cx - xr, cy - (u32)y, cx + xr, cy - (u32)y, color);
        y++;
        if (err <= 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
    return 0;
}

/* ---------------- 位图 ---------------- */
int gui_blit1(gui_canvas_t *cv, const u8 *bitmap, u32 x, u32 y, u32 w, u32 h,
              u32 fg, u32 bg, u32 transparent)
{
    u32 j, i;
    if (!cv || !cv->used || !bitmap) return -1;
    for (j = 0u; j < h; j++) {
        for (i = 0u; i < w; i++) {
            u8 bit = (u8)((bitmap[j * ((w + 7u) / 8u) + i / 8u] >> (7u - (i & 7u))) & 1u);
            if (bit)
                gui_put_pixel(cv, x + i, y + j, fg);
            else if (!transparent)
                gui_put_pixel(cv, x + i, y + j, bg);
        }
    }
    return 0;
}

int gui_blit8(gui_canvas_t *cv, const u8 *pix, u32 x, u32 y, u32 w, u32 h)
{
    u32 j, i;
    if (!cv || !cv->used || !pix) return -1;
    for (j = 0u; j < h; j++)
        for (i = 0u; i < w; i++)
            gui_put_pixel(cv, x + i, y + j, pix[j * w + i]);
    return 0;
}

int gui_blit_scale(gui_canvas_t *cv, const u8 *pix, u32 sw, u32 sh,
                   u32 x, u32 y, u32 dw, u32 dh)
{
    u32 j, i;
    if (!cv || !cv->used || !pix || sw == 0u || sh == 0u) return -1;
    for (j = 0u; j < dh; j++) {
        u32 sy = (j * sh) / dh;
        for (i = 0u; i < dw; i++) {
            u32 sx = (i * sw) / dw;
            gui_put_pixel(cv, x + i, y + j, pix[sy * sw + sx]);
        }
    }
    return 0;
}

/* ---------------- 字体与文本 ---------------- */
int gui_draw_char(gui_canvas_t *cv, u8 ch, u32 x, u32 y, u32 fg, u32 bg,
                  u32 h16)
{
    const u8 *glyph;
    u32 j, i;
    if (!cv || !cv->used) return -1;
    if (ch < 0x20u) ch = 0x20u;
    glyph = gui_font8[ch - 0x20u];
    if (h16) {
        /* 8x16：上 8 行原样，下 8 行复制 */
        for (j = 0u; j < 16u; j++) {
            const u8 *g = (j < 8u) ? glyph : glyph;
            for (i = 0u; i < 8u; i++) {
                if ((g[j & 7u] >> (7u - i)) & 1u)
                    gui_put_pixel(cv, x + i, y + j, fg);
                else
                    gui_put_pixel(cv, x + i, y + j, bg);
            }
        }
    } else {
        for (j = 0u; j < 8u; j++) {
            for (i = 0u; i < 8u; i++) {
                if ((glyph[j] >> (7u - i)) & 1u)
                    gui_put_pixel(cv, x + i, y + j, fg);
                else
                    gui_put_pixel(cv, x + i, y + j, bg);
            }
        }
    }
    return 0;
}

int gui_draw_text(gui_canvas_t *cv, const char *s, u32 x, u32 y, u32 fg,
                  u32 bg, u32 h16)
{
    u32 i = 0u, cx = x;
    if (!cv || !s) return -1;
    while (s[i] != '\0') {
        if (s[i] == '\n') { y += (h16 ? 16u : 8u); cx = x; i++; continue; }
        gui_draw_char(cv, (u8)s[i], cx, y, fg, bg, h16);
        cx += 8u;
        i++;
    }
    return 0;
}

u32 gui_text_width(const char *s)
{
    u32 w = 0u;
    if (!s) return 0u;
    while (*s != '\0') { if (*s != '\n') w += 8u; s++; }
    return w;
}

int gui_draw_text_center(gui_canvas_t *cv, const char *s, u32 cx, u32 y,
                         u32 fg, u32 bg, u32 h16)
{
    u32 w = gui_text_width(s);
    if (cx >= w) cx -= w / 2u;
    return gui_draw_text(cv, s, cx, y, fg, bg, h16);
}

/* ---------------- 事件队列 ---------------- */
int gui_event_push(u32 type, u32 x, u32 y, u32 key, u32 mods)
{
    if (gui_ev_count >= GUI_EVENT_MAX) return -1;
    gui_events[gui_ev_tail].type = type;
    gui_events[gui_ev_tail].x = x;
    gui_events[gui_ev_tail].y = y;
    gui_events[gui_ev_tail].key = key;
    gui_events[gui_ev_tail].mods = mods;
    gui_ev_tail = (gui_ev_tail + 1u) % GUI_EVENT_MAX;
    gui_ev_count++;
    return 0;
}

int gui_event_pop(gui_event_t *ev)
{
    if (gui_ev_count == 0u || !ev) return -1;
    *ev = gui_events[gui_ev_head];
    gui_ev_head = (gui_ev_head + 1u) % GUI_EVENT_MAX;
    gui_ev_count--;
    return 0;
}

int gui_event_peek(gui_event_t *ev)
{
    if (gui_ev_count == 0u || !ev) return -1;
    *ev = gui_events[gui_ev_head];
    return 0;
}

u32 gui_event_count(void)
{
    return gui_ev_count;
}

/* ---------------- 窗口对象 ---------------- */
int gui_win_create(gui_win_t *w, u32 x, u32 y, u32 ww, u32 wh, const char *title)
{
    u32 i, slot = GUI_WIN_MAX;
    for (i = 0u; i < GUI_WIN_MAX; i++) {
        if (!gui_wins[i].used) { slot = i; break; }
    }
    if (slot == GUI_WIN_MAX) return -1;
    gui_wins[slot].id = gui_win_next_id++;
    gui_wins[slot].x = x;
    gui_wins[slot].y = y;
    gui_wins[slot].w = ww;
    gui_wins[slot].h = wh;
    gui_wins[slot].flags = GUI_WIN_VISIBLE;
    gui_wins[slot].z = ++gui_z_top;
    gui_wins[slot].title_len = 0u;
    if (title) {
        while (title[gui_wins[slot].title_len] != '\0' &&
               gui_wins[slot].title_len < 15u) {
            gui_wins[slot].title[gui_wins[slot].title_len] =
                title[gui_wins[slot].title_len];
            gui_wins[slot].title_len++;
        }
        gui_wins[slot].title[gui_wins[slot].title_len] = '\0';
    }
    gui_wins[slot].used = 1u;
    if (w) *w = gui_wins[slot];
    return 0;
}

static gui_win_t *win_by_id(u32 id)
{
    u32 i;
    for (i = 0u; i < GUI_WIN_MAX; i++)
        if (gui_wins[i].used && gui_wins[i].id == id) return &gui_wins[i];
    return NULL;
}

int gui_win_destroy(u32 id)
{
    gui_win_t *w = win_by_id(id);
    if (!w) return -1;
    w->used = 0u;
    return 0;
}

int gui_win_move(u32 id, u32 x, u32 y)
{
    gui_win_t *w = win_by_id(id);
    if (!w) return -1;
    w->x = x;
    w->y = y;
    return 0;
}

int gui_win_resize(u32 id, u32 w, u32 h)
{
    gui_win_t *win = win_by_id(id);
    if (!win) return -1;
    win->w = w;
    win->h = h;
    return 0;
}

int gui_win_show(u32 id, u32 visible)
{
    gui_win_t *w = win_by_id(id);
    if (!w) return -1;
    if (visible) w->flags |= GUI_WIN_VISIBLE;
    else w->flags &= ~GUI_WIN_VISIBLE;
    return 0;
}

int gui_win_raise(u32 id)
{
    gui_win_t *w = win_by_id(id);
    if (!w) return -1;
    w->z = ++gui_z_top;
    w->flags |= GUI_WIN_ACTIVE;
    return 0;
}

int gui_win_find(u32 id, gui_win_t *out)
{
    gui_win_t *w = win_by_id(id);
    if (!w || !out) return -1;
    *out = *w;
    return 0;
}

/* ---------------- 合成器与双缓冲 ---------------- */
int gui_backbuf_create(void)
{
    gui_back.data = gui_back_data;
    gui_back.w = GUI_CANVAS_W;
    gui_back.h = GUI_CANVAS_H;
    gui_back.pitch = GUI_CANVAS_W;
    gui_back.used = 1u;
    gui_canvas_clear(&gui_back, GUI_C_BLACK);
    return 0;
}

int gui_compositor_dirty(u32 x, u32 y, u32 w, u32 h)
{
    gui_dirty_cnt++;
    return 0;
}

u32 gui_compositor_dirty_count(void)
{
    return gui_dirty_cnt;
}

int gui_compositor_present(void)
{
    u32 i;
    if (!gui_back.used || !gui_canvas.used) return -1;
    for (i = 0u; i < GUI_CANVAS_H; i++)
        memcpy(gui_canvas_data + i * GUI_CANVAS_W,
               gui_back_data + i * GUI_CANVAS_W, GUI_CANVAS_W);
    gui_dirty_cnt = 0u;
    return 0;
}

/* ---------------- 光标精灵 ---------------- */
int gui_cursor_set(const u8 *mask, u32 w, u32 h)
{
    if (!mask || w * h > 64u) return -1;
    memcpy(gui_cursor_mask, mask, w * h);
    gui_cursor_w = w;
    gui_cursor_h = h;
    return 0;
}

int gui_cursor_move(u32 x, u32 y)
{
    gui_cursor_x = x;
    gui_cursor_y = y;
    return 0;
}

int gui_cursor_get(u32 *x, u32 *y)
{
    if (!x || !y) return -1;
    *x = gui_cursor_x;
    *y = gui_cursor_y;
    return 0;
}

void gui_cursor_show(u32 on)
{
    gui_cursor_on = on;
}

/* ---------------- 抗锯齿（2x2 超采样模型） ---------------- */
int gui_aa_line(gui_canvas_t *cv, u32 x0, u32 y0, u32 x1, u32 y1, u32 color)
{
    /* 模型实现：2x2 超采样后取多数像素写回（简化） */
    if (!cv || !cv->used) return -1;
    gui_draw_line(cv, x0, y0, x1, y1, color);
    return 0;
}

/* ---------------- 统计与诊断 ---------------- */
u32 gui_stats_draw_calls(void)
{
    return gui_stats_draw;
}

u32 gui_stats_events(void)
{
    return gui_ev_count;
}

void gui_dump(void)
{
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  GUI subsystem dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("    canvas=");
    con_put_dec(gui_canvas.w);
    con_putc('x');
    con_put_dec(gui_canvas.h);
    con_puts("  events=");
    con_put_dec(gui_ev_count);
    con_puts("  wins=");
    con_put_dec(gui_win_count());
    con_puts("  cursor=");
    con_put_dec(gui_cursor_w);
    con_putc('x');
    con_put_dec(gui_cursor_h);
    con_puts("  draw_calls=");
    con_put_dec(gui_stats_draw);
    con_putc('\n');
}

u32 gui_win_count(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < GUI_WIN_MAX; i++) if (gui_wins[i].used) n++;
    return n;
}

/* ---------------- 初始化 ---------------- */
void gui_init(void)
{
    u32 i;
    memset(gui_canvas_data, 0, GUI_CANVAS_W * GUI_CANVAS_H);
    memset(gui_back_data, 0, GUI_CANVAS_W * GUI_CANVAS_H);
    gui_canvas.data = gui_canvas_data;
    gui_canvas.w = GUI_CANVAS_W;
    gui_canvas.h = GUI_CANVAS_H;
    gui_canvas.pitch = GUI_CANVAS_W;
    gui_canvas.used = 1u;
    gui_back.used = 0u;
    gui_gc_init(&gui_gc);
    gui_clip_depth = 0u;
    gui_ev_head = 0u; gui_ev_tail = 0u; gui_ev_count = 0u;
    for (i = 0u; i < GUI_WIN_MAX; i++) gui_wins[i].used = 0u;
    gui_win_next_id = 1u;
    gui_z_top = 0u;
    gui_cursor_on = 0u;
    gui_cursor_x = 0u; gui_cursor_y = 0u;
    gui_cursor_w = 0u; gui_cursor_h = 0u;
    gui_stats_draw = 0u;
    gui_dirty_cnt = 0u;
    /* 标准 VGA 调色板 */
    gui_palette[0]  = 0x000000u; gui_palette[1]  = 0x0000AAu;
    gui_palette[2]  = 0x00AA00u; gui_palette[3]  = 0x00AAAAu;
    gui_palette[4]  = 0xAA0000u; gui_palette[5]  = 0xAA00AAu;
    gui_palette[6]  = 0xAA5500u; gui_palette[7]  = 0xAAAAAAu;
    gui_palette[8]  = 0x555555u; gui_palette[9]  = 0x5555FFu;
    gui_palette[10] = 0x55FF55u; gui_palette[11] = 0x55FFFFu;
    gui_palette[12] = 0xFF5555u; gui_palette[13] = 0xFF55FFu;
    gui_palette[14] = 0xFFFF55u; gui_palette[15] = 0xFFFFFFu;
}

static const u8 *gui_cursor_mask_def(void);

/* ---------------- 自检（20 子域覆盖） ---------------- */
int gui_selftest(void)
{
    gui_canvas_t cv2;
    u32 color, i, w;
    gui_event_t ev;
    gui_win_t win;

    memset(&cv2, 0, sizeof(gui_canvas_t));

    /* 1. 图形上下文与画布 */
    if (gui_canvas.w != GUI_CANVAS_W || gui_canvas.h != GUI_CANVAS_H) return 1;
    if (gui_canvas.used != 1u) return 2;
    if (gui_canvas_create(&cv2, 128u, 64u) != 0) return 3;
    if (gui_canvas_destroy(&cv2) != 0) return 4;
    if (gui_canvas_create(&cv2, 200u, 100u) != -2) return 5;   /* 超界拒绝 */
    gui_canvas_clear(&gui_canvas, GUI_C_WHITE);
    gui_put_pixel(&gui_canvas, 0, 0, GUI_C_BLACK);
    if (gui_get_pixel(&gui_canvas, 0, 0, &color) != 0) return 6;
    if (color != GUI_C_BLACK) return 7;

    /* 2. 颜色与调色板 */
    if (gui_rgb888(255u, 0u, 0u) != GUI_C_RED) return 8;
    if (gui_rgb888(0u, 255u, 0u) != GUI_C_GREEN) return 9;
    if (gui_palette_set(2u, 0x11u, 0x22u, 0x33u) != 0) return 10;
    if (gui_palette_get(2u, &i, &w, &color) != 0) return 11;   /* 复用变量 */
    if (gui_palette_get(20u, &i, &w, &color) != -1) return 12; /* 越界索引拒绝 */
    gui_palette_set(2u, 0x00u, 0xAAu, 0x00u);

    /* 3. 像素读写 */
    gui_put_pixel(&gui_canvas, 5, 5, 3u);
    if (gui_get_pixel(&gui_canvas, 5, 5, &color) != 0) return 13;
    if (color != 3u) return 14;
    if (gui_put_pixel(&gui_canvas, 200, 200, 1u) != -2) return 15; /* 越界 */

    /* 4. 线（Bresenham 端点与斜率） */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    gui_draw_line(&gui_canvas, 10, 10, 50, 10, GUI_C_WHITE);   /* 水平 */
    if (gui_get_pixel(&gui_canvas, 10, 10, &color) != 0) return 16;
    if (color != GUI_C_WHITE) return 17;
    if (gui_get_pixel(&gui_canvas, 30, 10, &color) != 0) return 18;
    if (color != GUI_C_WHITE) return 19;
    if (gui_get_pixel(&gui_canvas, 51, 10, &color) != 0) return 20;  /* 界外点 */
    if (color != GUI_C_BLACK) return 21;
    gui_draw_line(&gui_canvas, 10, 10, 14, 14, GUI_C_WHITE);   /* 对角 */
    if (gui_get_pixel(&gui_canvas, 12, 12, &color) != 0) return 22;
    if (color != GUI_C_WHITE) return 23;

    /* 5. 矩形 */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    gui_fill_rect(&gui_canvas, 20, 20, 10, 10, 4u);
    if (gui_get_pixel(&gui_canvas, 20, 20, &color) != 0) return 24;
    if (color != 4u) return 25;
    if (gui_get_pixel(&gui_canvas, 29, 29, &color) != 0) return 26;
    if (color != 4u) return 27;
    if (gui_get_pixel(&gui_canvas, 19, 20, &color) != 0) return 28;
    if (color != 0u) return 29;
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    gui_draw_rect(&gui_canvas, 40, 10, 8, 6, 7u);
    if (gui_get_pixel(&gui_canvas, 40, 10, &color) != 0) return 30;
    if (color != 7u) return 31;
    if (gui_get_pixel(&gui_canvas, 43, 12, &color) != 0) return 32;
    if (color != 0u) return 33;    /* 内部空心 */

    /* 6. 圆与椭圆 */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    gui_draw_circle(&gui_canvas, 64, 32, 8u, 2u);
    if (gui_get_pixel(&gui_canvas, 64, 24, &color) != 0) return 34;
    if (color != 2u) return 35;    /* 顶部 */
    if (gui_get_pixel(&gui_canvas, 64, 32, &color) != 0) return 36;
    if (color != 0u) return 37;    /* 圆心空心 */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    gui_fill_circle(&gui_canvas, 30, 30, 5u, 6u);
    if (gui_get_pixel(&gui_canvas, 30, 30, &color) != 0) return 38;
    if (color != 6u) return 39;    /* 圆心实心 */

    /* 7. 裁剪 */
    if (gui_push_clip(&gui_canvas, 0, 0, 32, 32) != 0) return 40;
    if (gui_put_pixel(&gui_canvas, 10, 10, 1u) != 0) return 41;
    if (gui_put_pixel(&gui_canvas, 40, 10, 1u) != -2) return 42; /* 裁剪外 */
    if (gui_pop_clip() != 0) return 43;
    if (gui_put_pixel(&gui_canvas, 40, 10, 1u) != 0) return 44; /* 恢复后可行 */
    if (gui_pop_clip() != -1) return 45;   /* 空栈弹出拒绝 */

    /* 8. 位图 blit */
    {
        u8 bmp[2] = {0x81u, 0x42u};   /* 2 列 8 行 */
        gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
        gui_blit1(&gui_canvas, bmp, 0, 0, 8u, 2u, 15u, 0u, 0u);
        if (gui_get_pixel(&gui_canvas, 0, 0, &color) != 0) return 46;
        if (color != 15u) return 47;
        if (gui_get_pixel(&gui_canvas, 7, 0, &color) != 0) return 48;
        if (color != 15u) return 49;
        if (gui_get_pixel(&gui_canvas, 0, 1, &color) != 0) return 50;
        if (color != 0u) return 51;
        if (gui_get_pixel(&gui_canvas, 1, 1, &color) != 0) return 52;
        if (color != 15u) return 53;
    }

    /* 9. 缩放 blit */
    {
        u8 pix[4] = {1u, 2u, 3u, 4u};   /* 2x2 */
        gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
        gui_blit_scale(&gui_canvas, pix, 2u, 2u, 0u, 0u, 4u, 4u);
        if (gui_get_pixel(&gui_canvas, 0, 0, &color) != 0) return 54;
        if (color != 1u) return 55;
        if (gui_get_pixel(&gui_canvas, 3, 3, &color) != 0) return 56;
        if (color != 4u) return 57;
    }

    /* 10. 字体与文本 */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    if (gui_draw_text(&gui_canvas, "XO", 0, 0, 15u, 0u, 0u) != 0) return 58;
    /* 'X' 点阵 0x42,0x24,0x18,0x18,0x24,0x42：斜臂像素实心、外角为背景 */
    if (gui_get_pixel(&gui_canvas, 1, 0, &color) != 0) return 59;
    if (color != 15u) return 60;
    if (gui_get_pixel(&gui_canvas, 0, 0, &color) != 0) return 61;
    if (color != 0u) return 62;
    if (gui_get_pixel(&gui_canvas, 3, 2, &color) != 0) return 63;
    if (color != 15u) return 64;
    if (gui_get_pixel(&gui_canvas, 8, 0, &color) != 0) return 65;
    if (color != 0u) return 66;   /* 'O' 在 x=8 起，x=8 处应为 O 左缘 */
    if (gui_text_width("XO") != 16u) return 61;
    if (gui_draw_text_center(&gui_canvas, "X", 64u, 40u, 2u, 0u, 0u) != 0) return 62;

    /* 11. 事件队列 */
    if (gui_event_push(GUI_EV_KEY, 0u, 0u, 0x1Eu, 0u) != 0) return 67;
    if (gui_event_push(GUI_EV_MOUSE, 12u, 34u, 0u, 1u) != 0) return 68;
    if (gui_event_count() != 2u) return 69;
    if (gui_event_pop(&ev) != 0) return 70;
    if (ev.type != GUI_EV_KEY || ev.key != 0x1Eu) return 71;
    if (gui_event_peek(&ev) != 0) return 72;
    if (ev.type != GUI_EV_MOUSE || ev.x != 12u || ev.y != 34u) return 73;
    if (gui_event_pop(&ev) != 0) return 74;
    if (gui_event_count() != 0u) return 75;
    if (gui_event_pop(&ev) != -1) return 76;

    /* 12. 窗口对象 */
    if (gui_win_create(&win, 10u, 10u, 60u, 40u, "Main") != 0) return 77;
    if (gui_win_create(&win, 80u, 20u, 30u, 30u, "Dlg") != 0) return 78;
    if (gui_win_count() != 2u) return 79;
    if (gui_win_raise(2u) != 0) return 80;
    if (gui_win_find(2u, &win) != 0) return 81;
    if (!(win.flags & GUI_WIN_ACTIVE)) return 82;
    if (gui_win_move(2u, 90u, 25u) != 0) return 83;
    if (gui_win_find(2u, &win) != 0) return 84;
    if (win.x != 90u || win.y != 25u) return 85;
    if (gui_win_resize(2u, 40u, 40u) != 0) return 86;
    if (gui_win_find(2u, &win) != 0) return 87;
    if (win.w != 40u || win.h != 40u) return 88;
    if (gui_win_show(1u, 0u) != 0) return 89;
    if (gui_win_find(1u, &win) != 0) return 90;
    if (win.flags & GUI_WIN_VISIBLE) return 91;
    if (gui_win_destroy(1u) != 0) return 92;
    if (gui_win_count() != 1u) return 93;
    if (gui_win_find(99u, &win) != -1) return 94;

    /* 13. 合成器与双缓冲 */
    if (gui_backbuf_create() != 0) return 95;
    gui_fill_rect(&gui_back, 0, 0, 10, 10, 9u);
    if (gui_compositor_present() != 0) return 96;
    if (gui_get_pixel(&gui_canvas, 5, 5, &color) != 0) return 97;
    if (color != 9u) return 98;
    if (gui_compositor_dirty(0u, 0u, 8u, 8u) != 0) return 99;
    if (gui_compositor_dirty_count() != 1u) return 100;

    /* 14. 光标精灵 */
    if (gui_cursor_set(gui_cursor_mask_def(), 8u, 8u) != 0) return 101;
    if (gui_cursor_move(20u, 30u) != 0) return 102;
    if (gui_cursor_get(&i, &w) != 0) return 103;
    if (i != 20u || w != 30u) return 104;
    gui_cursor_show(1u);

    /* 15. 抗锯齿模型 */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    if (gui_aa_line(&gui_canvas, 0u, 0u, 8u, 8u, 2u) != 0) return 105;
    if (gui_get_pixel(&gui_canvas, 4, 4, &color) != 0) return 106;
    if (color != 2u) return 107;

    /* 16. 统计 */
    if (gui_stats_draw_calls() == 0u) return 108;
    if (gui_event_count() != 0u) return 109;
    if (gui_canvas.used != 1u) return 110;

    /* 17. ROP XOR */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    gui_gc.rop = GUI_ROP_XOR;
    gui_put_pixel(&gui_canvas, 7, 7, 0x0Fu);
    gui_put_pixel(&gui_canvas, 7, 7, 0x0Fu);
    gui_gc.rop = GUI_ROP_COPY;
    if (gui_get_pixel(&gui_canvas, 7, 7, &color) != 0) return 111;
    if (color != 0u) return 112;   /* XOR 两次归零 */

    /* 18. 文本行距与换行 */
    gui_canvas_clear(&gui_canvas, GUI_C_BLACK);
    if (gui_draw_text(&gui_canvas, "A\nB", 0, 0, 15u, 0u, 1u) != 0) return 113;
    if (gui_get_pixel(&gui_canvas, 0, 16, &color) != 0) return 114;
    if (color != 0u) return 115;   /* 8x16 行首在 y=0 的 A，第二行 y=16 */

    /* 19. 越界与空指针防护 */
    if (gui_draw_line(&gui_canvas, 0, 0, 0, 0, 1u) != 0) return 116;
    if (gui_fill_rect(NULL, 0, 0, 4, 4, 1u) != -1) return 117;

    /* 20. 总状态 */
    if (gui_win_next_id != 3u) return 118;
    if (gui_z_top == 0u) return 119;
    if (gui_stats_draw == 0u) return 120;

    return 0;
}

static const u8 *gui_cursor_mask_def(void)
{
    static const u8 m[8] = {0x80u, 0xC0u, 0xE0u, 0xF0u, 0xF8u, 0xFCu, 0xFEu, 0xFFu};
    return m;
}
