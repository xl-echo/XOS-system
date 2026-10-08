/* ============================================================================
 * XOS 图形桌面（真机 GUI 层）
 * 完全自研：Bochs VBE 640x480x32 线性帧缓冲 + 桌面渲染 + 键盘事件循环。
 *  - desk_gui_init(): 切换 VBE 图形模式、映射 LFB(0xE0000000)、失败自动回退
 *  - desk_gui_run():  渲染渐变壁纸 / 桌面图标 / 任务栏 / 时钟；
 *                     Tab/方向键选择图标，Enter 打开窗口，Esc 关闭窗口，
 *                     无窗口时 Esc 退出桌面回到文本 Shell（安全回退）。
 * 安全性：全部操作限于帧缓冲与键盘事件队列；退出时恢复文本模式，
 *         不触碰磁盘、不改变硬件状态，宿主不受影响。
 * ========================================================================== */
#include "desk_gui.h"
#include "display.h"
#include "vmm.h"
#include "keyboard.h"
#include "console.h"
#include "irq.h"
#include "string.h"

extern void *memset(void *dst, int c, unsigned int n);

/* ---------------- 32bpp 帧缓冲访问 ---------------- */
static volatile u32 *dg_fb = (volatile u32 *)DG_LFB;
static u32 dg_sel = 0u;              /* 当前选中图标索引 */

static inline void dgpix(u32 x, u32 y, u32 c)
{
    if (x < DG_W && y < DG_H) dg_fb[y * DG_W + x] = c;
}

static inline u32 dg_rgb(u32 r, u32 g, u32 b)
{
    return (r << 16) | (g << 8) | b;
}

static void dg_fill(u32 x, u32 y, u32 w, u32 h, u32 c)
{
    u32 i, j;
    if (x >= DG_W || y >= DG_H) return;
    if (x + w > DG_W) w = DG_W - x;
    if (y + h > DG_H) h = DG_H - y;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            dg_fb[(y + j) * DG_W + (x + i)] = c;
}

static void dg_rect(u32 x, u32 y, u32 w, u32 h, u32 c)
{
    u32 i;
    for (i = 0; i < w; i++) { dgpix(x + i, y, c); dgpix(x + i, y + h - 1u, c); }
    for (i = 0; i < h; i++) { dgpix(x, y + i, c); dgpix(x + w - 1u, y + i, c); }
}

/* ---------------- 8x8 点阵字体（自研，与 gui.c 同源字模） ---------------- */
static const u8 dg_font8[96][8] = {
    {0,0,0,0,0,0,0,0}, {0x18,0x18,0x18,0x18,0x18,0,0x18,0},
    {0x6C,0x6C,0x6C,0,0,0,0,0}, {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0},
    {0x18,0x3C,0x5A,0x18,0x24,0x24,0x3C,0x18}, {0x62,0x64,0x08,0x10,0x26,0x46,0,0},
    {0x38,0x44,0x44,0x38,0x4C,0x44,0x4A,0x3C}, {0x18,0x18,0x18,0,0,0,0,0},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0},
    {0,0x66,0x3C,0xFF,0x3C,0x66,0,0}, {0,0x18,0x18,0x7E,0x18,0x18,0,0},
    {0,0,0,0,0,0x18,0x18,0x30}, {0,0,0,0x7E,0,0,0,0},
    {0,0,0,0,0,0,0x18,0x18}, {0x02,0x04,0x08,0x10,0x20,0x40,0,0},
    {0x3C,0x46,0x4A,0x52,0x62,0x3C,0,0}, {0x18,0x38,0x18,0x18,0x18,0x7E,0,0},
    {0x3C,0x42,0x02,0x0C,0x30,0x7E,0,0}, {0x3C,0x42,0x0C,0x02,0x42,0x3C,0,0},
    {0x0C,0x14,0x24,0x44,0x7E,0x04,0,0}, {0x7E,0x40,0x7C,0x02,0x42,0x3C,0,0},
    {0x1C,0x20,0x7C,0x42,0x42,0x3C,0,0}, {0x7E,0x02,0x04,0x08,0x10,0x10,0,0},
    {0x3C,0x42,0x3C,0x42,0x42,0x3C,0,0}, {0x3C,0x42,0x42,0x3E,0x02,0x3C,0,0},
    {0,0x18,0x18,0,0,0x18,0x18,0}, {0,0x18,0x18,0,0,0x18,0x18,0x30},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0}, {0,0,0x7E,0,0x7E,0,0,0},
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0}, {0x3C,0x42,0x02,0x0C,0x18,0,0x18,0},
    {0x3C,0x46,0x5A,0x5E,0x40,0x3C,0,0}, {0x3C,0x42,0x42,0x7E,0x42,0x42,0,0},
    {0x7C,0x42,0x7C,0x42,0x42,0x7C,0,0}, {0x3C,0x42,0x40,0x40,0x42,0x3C,0,0},
    {0x78,0x44,0x42,0x42,0x44,0x78,0,0}, {0x7E,0x40,0x7C,0x40,0x40,0x7E,0,0},
    {0x7E,0x40,0x7C,0x40,0x40,0x40,0,0}, {0x3C,0x42,0x40,0x4E,0x42,0x3E,0,0},
    {0x42,0x42,0x7E,0x42,0x42,0x42,0,0}, {0x7E,0x18,0x18,0x18,0x18,0x7E,0,0},
    {0x02,0x02,0x02,0x42,0x42,0x3C,0,0}, {0x42,0x44,0x78,0x44,0x42,0x42,0,0},
    {0x40,0x40,0x40,0x40,0x40,0x7E,0,0}, {0x42,0x66,0x5A,0x42,0x42,0x42,0,0},
    {0x42,0x62,0x52,0x4A,0x46,0x42,0,0}, {0x3C,0x42,0x42,0x42,0x42,0x3C,0,0},
    {0x7C,0x42,0x42,0x7C,0x40,0x40,0,0}, {0x3C,0x42,0x42,0x4A,0x44,0x3A,0,0},
    {0x7C,0x42,0x42,0x7C,0x44,0x42,0,0}, {0x3E,0x40,0x3C,0x02,0x02,0x7C,0,0},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0,0}, {0x42,0x42,0x42,0x42,0x42,0x3C,0,0},
    {0x42,0x42,0x42,0x42,0x24,0x18,0,0}, {0x42,0x42,0x5A,0x66,0x42,0x42,0,0},
    {0x42,0x24,0x18,0x18,0x24,0x42,0,0}, {0x42,0x42,0x24,0x18,0x18,0x18,0,0},
    {0x7E,0x02,0x0C,0x30,0x40,0x7E,0,0}, {0x3C,0x30,0x30,0x30,0x30,0x3C,0,0},
    {0x40,0x20,0x10,0x08,0x04,0x02,0,0}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0},
    {0x18,0x24,0x42,0,0,0,0,0}, {0,0,0,0,0,0,0,0xFF},
    {0x30,0x18,0,0,0,0,0,0}, {0,0x3C,0x02,0x3E,0x42,0x3E,0,0},
    {0x40,0x40,0x7C,0x42,0x42,0x7C,0,0}, {0,0,0x3C,0x40,0x40,0x3C,0,0},
    {0x02,0x02,0x3E,0x42,0x42,0x3E,0,0}, {0,0,0x3C,0x7E,0x40,0x3C,0,0},
    {0x0C,0x12,0x10,0x38,0x10,0x10,0,0}, {0,0,0x3E,0x42,0x42,0x3E,0x02,0x3C},
    {0x40,0x40,0x7C,0x42,0x42,0x42,0,0}, {0x18,0,0x38,0x18,0x18,0x7E,0,0},
    {0x06,0,0x06,0x06,0x06,0x46,0x3C,0}, {0x40,0x40,0x44,0x78,0x44,0x42,0,0},
    {0x38,0x18,0x18,0x18,0x18,0x7E,0,0}, {0,0,0x6C,0x5A,0x42,0x42,0,0},
    {0,0,0x7C,0x42,0x42,0x42,0,0}, {0,0,0x3C,0x42,0x42,0x3C,0,0},
    {0,0,0x7C,0x42,0x42,0x7C,0x40,0x40}, {0,0,0x3E,0x42,0x42,0x3E,0x02,0x02},
    {0,0,0x5C,0x60,0x40,0x40,0,0}, {0,0,0x3E,0x40,0x3C,0x02,0x7C,0},
    {0x10,0x10,0x3C,0x10,0x10,0x0C,0,0}, {0,0,0x42,0x42,0x42,0x3E,0,0},
    {0,0,0x42,0x42,0x24,0x18,0,0}, {0,0,0x42,0x5A,0x66,0x42,0,0},
    {0,0,0x42,0x24,0x18,0x24,0x42,0}, {0,0,0x42,0x42,0x42,0x3E,0x02,0x3C},
    {0,0,0x7E,0x0C,0x30,0x7E,0,0}, {0x0C,0x18,0x18,0x30,0x18,0x18,0x0C,0},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0}, {0x30,0x18,0x18,0x0C,0x18,0x18,0x30,0},
    {0x36,0x6C,0,0,0,0,0,0}, {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
};

static void dg_char(u32 x, u32 y, u8 ch, u32 fg, u32 bg)
{
    u32 i, j;
    const u8 *g;
    u32 idx;
    if (ch < 0x20u) return;
    idx = ch - 0x20u;
    if (idx >= 96u) idx = 95u;
    g = dg_font8[idx];
    for (j = 0; j < 8u; j++)
        for (i = 0; i < 8u; i++) {
            u32 on = (g[j] >> (7u - i)) & 1u;
            dgpix(x + i, y + j, on ? fg : bg);
        }
}

static void dg_text(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    if (!s) return;
    while (*s) {
        dg_char(x, y, (u8)*s, fg, bg);
        x += 8u;
        s++;
    }
}

static u32 dg_text_w(const char *s)
{
    u32 w = 0u;
    if (!s) return 0u;
    while (*s) { w += 8u; s++; }
    return w;
}

/* ---------------- 桌面图标 ---------------- */
typedef struct {
    const char *name;
    u32  icon;                 /* 图案索引 */
} dg_icon_t;

static const dg_icon_t dg_icons[DG_ICON_N] = {
    { "文件", 0u }, { "文本", 1u }, { "计算器", 2u }, { "终端", 3u },
    { "浏览器", 4u }, { "设置", 5u }, { "音乐", 6u }, { "游戏", 7u },
};

/* 图标图案：32x32 方块，用几何色块组合（像素风，全自研） */
static void dg_icon_pattern(u32 x, u32 y, u32 idx)
{
    u32 c1, c2, c3;
    switch (idx) {
    case 0u: c1 = dg_rgb(0x2F,0x7D,0xE1); c2 = dg_rgb(0xE8,0xC8,0x4A); c3 = dg_rgb(0x0F,0x2F,0x60); break; /* 文件-蓝文件夹 */
    case 1u: c1 = dg_rgb(0xF0,0xF0,0xF0); c2 = dg_rgb(0x90,0x90,0x90); c3 = dg_rgb(0x40,0x40,0x40); break; /* 文本-白纸 */
    case 2u: c1 = dg_rgb(0x88,0x8C,0x90); c2 = dg_rgb(0x40,0x44,0x48); c3 = dg_rgb(0xC8,0xCC,0xD0); break; /* 计算器 */
    case 3u: c1 = dg_rgb(0x10,0x10,0x18); c2 = dg_rgb(0x30,0xC0,0x50); c3 = dg_rgb(0x80,0x80,0x80); break; /* 终端 */
    case 4u: c1 = dg_rgb(0x1E,0x6F,0xD0); c2 = dg_rgb(0xF8,0xF8,0xF8); c3 = dg_rgb(0x0A,0x3A,0x80); break; /* 浏览器 */
    case 5u: c1 = dg_rgb(0x50,0x5A,0x66); c2 = dg_rgb(0xE8,0xE8,0xE8); c3 = dg_rgb(0x2A,0x2E,0x34); break; /* 设置 */
    case 6u: c1 = dg_rgb(0x1A,0x1A,0x22); c2 = dg_rgb(0x50,0xC8,0x70); c3 = dg_rgb(0x90,0xE0,0xA8); break; /* 音乐 */
    case 7u: c1 = dg_rgb(0x30,0x34,0x3C); c2 = dg_rgb(0xE0,0x48,0x48); c3 = dg_rgb(0x48,0xB8,0xE0); break; /* 游戏 */
    default: c1 = c2 = c3 = dg_rgb(0x60,0x60,0x60); break;
    }
    dg_fill(x, y, 32u, 32u, c1);                     /* 底 */
    dg_rect(x, y, 32u, 32u, c3);                     /* 外框 */
    switch (idx) {
    case 0u:   /* 文件夹：顶标签 + 中分隔 */
        dg_fill(x + 6u, y + 6u, 20u, 6u, c2);
        dg_fill(x + 6u, y + 18u, 20u, 3u, c2);
        break;
    case 1u:   /* 文本：白纸 + 灰线 */
        dg_fill(x + 5u, y + 5u, 22u, 22u, c1);
        dg_fill(x + 8u, y + 10u, 16u, 2u, c2);
        dg_fill(x + 8u, y + 15u, 16u, 2u, c2);
        dg_fill(x + 8u, y + 20u, 10u, 2u, c2);
        break;
    case 2u:   /* 计算器：屏幕 + 按钮格 */
        dg_fill(x + 6u, y + 5u, 20u, 8u, c3);
        dg_fill(x + 6u, y + 16u, 20u, 12u, c2);
        dg_fill(x + 8u, y + 18u, 5u, 4u, c1);
        dg_fill(x + 14u, y + 18u, 5u, 4u, c1);
        dg_fill(x + 20u, y + 18u, 5u, 4u, c1);
        break;
    case 3u:   /* 终端：黑屏 + 绿色提示符 */
        dg_fill(x + 4u, y + 4u, 24u, 24u, c1);
        dg_char(x + 8u, y + 12u, '>', c2, c1);
        dg_char(x + 16u, y + 12u, '_', c2, c1);
        break;
    case 4u:   /* 浏览器：蓝圆环（矩形近似） + 白十字 */
        dg_fill(x + 6u, y + 6u, 20u, 20u, c2);
        dg_fill(x + 9u, y + 9u, 14u, 14u, c1);
        dg_fill(x + 6u, y + 13u, 20u, 6u, c2);
        dg_fill(x + 13u, y + 6u, 6u, 20u, c2);
        break;
    case 5u:   /* 设置：齿轮（中心圆 + 四辐条） */
        dg_fill(x + 8u, y + 8u, 16u, 16u, c2);
        dg_fill(x + 12u, y + 12u, 8u, 8u, c1);
        dg_fill(x + 12u, y + 4u, 8u, 4u, c2);
        dg_fill(x + 12u, y + 24u, 8u, 4u, c2);
        dg_fill(x + 4u, y + 12u, 4u, 8u, c2);
        dg_fill(x + 24u, y + 12u, 4u, 8u, c2);
        break;
    case 6u:   /* 音乐：黑底 + 绿色音符条 */
        dg_fill(x + 8u, y + 6u, 4u, 20u, c2);
        dg_fill(x + 8u, y + 22u, 12u, 5u, c2);
        dg_fill(x + 20u, y + 10u, 4u, 12u, c2);
        dg_fill(x + 20u, y + 18u, 8u, 5u, c3);
        break;
    case 7u:   /* 游戏：手柄主体 + 方向键 */
        dg_fill(x + 6u, y + 12u, 20u, 10u, c1);
        dg_fill(x + 12u, y + 6u, 8u, 22u, c2);
        dg_fill(x + 14u, y + 14u, 4u, 4u, c3);
        dg_fill(x + 4u, y + 14u, 4u, 4u, c2);
        dg_fill(x + 24u, y + 14u, 4u, 4u, c2);
        break;
    default:
        break;
    }
}

/* ---------------- 窗口（模拟图形窗口） ---------------- */
typedef struct {
    u32  open;
    u32  icon;                 /* 关联图标 */
} dg_win_t;

static dg_win_t dg_win;

static void dg_window(void)
{
    u32 wx = 130u, wy = 90u, ww = 380u, wh = 250u;
    u32 title = dg_rgb(0x1E,0x3A,0x5F);
    u32 body  = dg_rgb(0xEE,0xEE,0xEE);
    u32 fg    = dg_rgb(0x18,0x18,0x18);
    const char *name = dg_icons[dg_win.icon].name;
    dg_fill(wx, wy, ww, wh, body);
    dg_fill(wx, wy, ww, 26u, title);
    dg_text(wx + 10u, wy + 9u, name, dg_rgb(0xF0,0xF0,0xF0), title);
    dg_rect(wx, wy, ww, wh, dg_rgb(0x0A,0x14,0x22));
    /* 关闭按钮 X */
    dg_fill(wx + ww - 24u, wy + 5u, 18u, 16u, dg_rgb(0xC0,0x30,0x30));
    dg_char(wx + ww - 20u, wy + 9u, 'X', dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0xC0,0x30,0x30));
    /* 内容区 */
    dg_text(wx + 20u, wy + 60u, "XOS 图形桌面窗口", fg, body);
    dg_text(wx + 20u, wy + 90u, "这是「", fg, body);
    {
        u32 tx = wx + 20u + dg_text_w("这是「");
        dg_text(tx, wy + 90u, name, dg_rgb(0x1E,0x6F,0xD0), body);
        tx += dg_text_w(name);
        dg_text(tx, wy + 90u, "」应用窗口。", fg, body);
    }
    dg_text(wx + 20u, wy + 120u, "按 [Esc] 关闭窗口", fg, body);
    dg_text(wx + 20u, wy + 150u, "按 [Esc] 两次退出桌面回终端", dg_rgb(0x60,0x60,0x60), body);
}

/* ---------------- 桌面渲染 ---------------- */
static void dg_render(void)
{
    u32 i, x, y;
    u32 bar = dg_rgb(0x18,0x1C,0x28);
    u32 txt = dg_rgb(0xE8,0xE8,0xE8);
    u32 sel_c = dg_rgb(0xFF,0xFF,0xFF);
    u32 sec, hh, mm;
    char tbuf[16];

    /* 1) 渐变壁纸：顶部深蓝 → 底部深灰 */
    for (y = 0; y < DG_H - DG_TASKBAR; y++) {
        u32 f = (y * 160u) / (DG_H - DG_TASKBAR);
        u32 r = 0x10u + (f * 0x08u) / 0x100u;
        u32 g = 0x20u + (f * 0x06u) / 0x100u;
        u32 b = 0x40u + (f * 0x18u) / 0x100u;
        dg_fill(0u, y, DG_W, 1u, dg_rgb(r, g, b));
    }

    /* 2) 图标：两行四列 */
    for (i = 0; i < DG_ICON_N; i++) {
        u32 col = i % 4u;
        u32 row = i / 4u;
        x = 40u + col * 140u;
        y = 50u + row * 130u;
        dg_icon_pattern(x, y, dg_icons[i].icon);
        if (i == dg_sel)
            dg_rect(x - 3u, y - 3u, 38u, 38u, sel_c);
        dg_text(x + 2u, y + 36u, dg_icons[i].name, txt, dg_rgb(0x0E,0x22,0x40));
    }

    /* 3) 任务栏 */
    dg_fill(0u, DG_H - DG_TASKBAR, DG_W, DG_TASKBAR, bar);
    dg_fill(0u, DG_H - DG_TASKBAR, DG_W, 2u, dg_rgb(0x2E,0x3A,0x4E));
    /* 开始按钮 */
    dg_fill(6u, DG_H - DG_TASKBAR + 5u, 56u, 20u, dg_rgb(0x2F,0x7D,0xE1));
    dg_text(14u, DG_H - DG_TASKBAR + 11u, "XOS", dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0x2F,0x7D,0xE1));
    /* 时钟 */
    sec = pit_tick_count() / 100u;
    hh = (10u + sec / 3600u) % 24u;
    mm = sec % 3600u / 60u;
    tbuf[0] = (char)('0' + hh / 10u);
    tbuf[1] = (char)('0' + hh % 10u);
    tbuf[2] = ':';
    tbuf[3] = (char)('0' + mm / 10u);
    tbuf[4] = (char)('0' + mm % 10u);
    tbuf[5] = 0;
    dg_text(DG_W - 70u, DG_H - DG_TASKBAR + 11u, tbuf,
            dg_rgb(0xF0,0xF0,0xF0), bar);
    /* 状态提示 */
    dg_text(90u, DG_H - DG_TASKBAR + 11u, "Tab 选择  Enter 打开  Esc 退出",
            dg_rgb(0xA0,0xB0,0xC8), bar);

    /* 4) 窗口层 */
    if (dg_win.open) dg_window();

    con_flush();
}

/* ---------------- 初始化与运行 ---------------- */
int desk_gui_init(void)
{
    u32 a, rc;
    rc = display_set_mode(5);                 /* 640x480x32 VBE */
    if (rc != 0) {
        con_puts("  [desk_gui] display_set_mode(5) failed rc=");
        con_put_hex(rc, 2u);
        con_puts("\n");
        con_flush();
        return -1;
    }
    if (vmm_map_device_huge(vmm_kernel_mm(), DG_LFB, DG_LFB, PTE_P | PTE_RW) != VMM_OK) {
        con_puts("  [desk_gui] huge device map failed, fallback 4KB pages\n");
        con_flush();
        /* 回退：4KB 逐页设备映射（LFB 1.2MB = 300 页） */
        for (a = DG_LFB; a < DG_LFB + DG_W * DG_H * 4u; a += 0x1000u) {
            if (vmm_map_device(vmm_kernel_mm(), a, a, PTE_P | PTE_RW) != VMM_OK) {
                con_puts("  [desk_gui] 4KB device map failed @ ");
                con_put_hex(a, 8u);
                con_puts("\n");
                con_flush();
                return -2;
            }
        }
    }
    vmm_flush_tlb_page(DG_LFB);
    return 0;
}

void desk_gui_run(void)
{
    kbd_event_t ev, tmp;
    if (desk_gui_init() != 0) return;              /* 图形不可用：安全回退文本 Shell */
    while (kbd_read_event(&tmp) == 0) { }           /* 清空残留键盘事件 */

    dg_win.open = 0u;
    dg_sel = 0u;
    dg_render();

    for (;;) {
        kbd_poll();
        if (kbd_read_event(&ev) == 0) {
            if (ev.type == EV_KEY_DOWN) {
                if (dg_win.open) {
                    if (ev.key == KEY_ESC) dg_win.open = 0u;
                    dg_render();
                    continue;
                }
                if (ev.key == KEY_TAB || ev.key == KEY_RIGHT || ev.key == KEY_DOWN) {
                    dg_sel = (dg_sel + 1u) % DG_ICON_N;
                    dg_render();
                } else if (ev.key == KEY_LEFT || ev.key == KEY_UP) {
                    dg_sel = (dg_sel + DG_ICON_N - 1u) % DG_ICON_N;
                    dg_render();
                } else if (ev.key == KEY_ENTER) {
                    dg_win.open = 1u;
                    dg_win.icon = dg_sel;
                    dg_render();
                } else if (ev.key == KEY_ESC) {
                    break;                           /* 退出桌面 → 文本 Shell */
                }
            }
            continue;
        }
        __asm__ __volatile__("hlt");
    }

    /* 安全退出：恢复文本模式并清屏（不影响硬件/磁盘） */
    display_set_mode(0);
    con_clear();
    con_set_cursor(0u, 0u);
    con_puts("  XOS 桌面已退出，返回文本终端。输入 'desktop' 重新进入桌面。\n");
    con_flush();
}
