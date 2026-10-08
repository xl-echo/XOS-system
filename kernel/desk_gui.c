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

/* ---------------- 窗口管理器（多窗口：创建/关闭/移动/最小化/切换/Z序） ---------------- */
#define DG_WIN_MAX   4u
typedef struct {
    u32  open;
    u32  icon;                 /* 关联图标 */
    u32  x, y, w, h;           /* 窗口位置与尺寸 */
    u32  min;                  /* 最小化（缩到任务栏） */
} dg_win_t;

static dg_win_t dg_wins[DG_WIN_MAX];
static u32  dg_nwin  = 0u;     /* 打开窗口数 */
static u32  dg_focus = 0u;     /* 焦点窗口索引 */
static u32  dg_move_mode = 0u; /* 移动模式 */

/* 开始菜单 / 右键菜单 状态 */
#define DG_MENU_NONE   0u
#define DG_MENU_START  1u
#define DG_MENU_RIGHT  2u
static u32  dg_menu    = DG_MENU_NONE;
static u32  dg_menusel = 0u;

static const char *dg_apps[DG_ICON_N] = {
    "文件管理器", "文本编辑器", "计算器", "图形终端",
    "浏览器", "设置中心", "音乐播放器", "游戏中心",
};

/* 窗口内容（按图标不同） */
static void dg_win_content(const dg_win_t *w)
{
    u32 body = dg_rgb(0xEE,0xEE,0xEE);
    u32 fg   = dg_rgb(0x18,0x18,0x18);
    u32 blue = dg_rgb(0x1E,0x6F,0xD0);
    u32 gray = dg_rgb(0x60,0x60,0x60);
    u32 bx = w->x + 16u, by = w->y + 44u;
    switch (w->icon) {
    case 0u: /* 文件管理器 */
        dg_text(bx, by, "/root", blue, body);
        dg_text(bx, by + 22u, "  [D] docs.txt", fg, body);
        dg_text(bx, by + 44u, "  [F] note.txt", fg, body);
        dg_text(bx, by + 66u, "  [S] system.dat", fg, body);
        dg_text(bx, by + 110u, "按 M 移动窗口  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    case 1u: /* 文本编辑器 */
        dg_text(bx, by, "note.txt - 文本编辑器", fg, body);
        dg_text(bx, by + 24u, "XOS 图形桌面已启动。", fg, body);
        dg_text(bx, by + 48u, "按 M 移动窗口  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    case 2u: /* 计算器 */
        dg_fill(bx + 4u, by, 120u, 20u, dg_rgb(0x2A,0x3A,0x4A));
        dg_text(bx + 10u, by + 5u, "0", dg_rgb(0xF0,0xF0,0xF0), dg_rgb(0x2A,0x3A,0x4A));
        dg_fill(bx + 4u, by + 28u, 24u, 18u, dg_rgb(0xC8,0xCC,0xD0));
        dg_fill(bx + 34u, by + 28u, 24u, 18u, dg_rgb(0xC8,0xCC,0xD0));
        dg_fill(bx + 64u, by + 28u, 24u, 18u, dg_rgb(0xC8,0xCC,0xD0));
        dg_text(bx + 40u, by + 90u, "M 移动  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    case 3u: /* 图形终端 */
        dg_fill(bx - 8u, by - 8u, w->w - 16u, 100u, dg_rgb(0x10,0x10,0x18));
        dg_text(bx + 4u, by, "XOS # ", dg_rgb(0x30,0xC0,0x50), dg_rgb(0x10,0x10,0x18));
        dg_text(bx + 4u, by + 22u, "图形终端已连接桌面。", dg_rgb(0xE0,0xE0,0xE0), dg_rgb(0x10,0x10,0x18));
        dg_text(bx, by + 130u, "M 移动  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    case 4u: /* 浏览器 */
        dg_text(bx, by, "XOS 浏览器 - 首页", fg, body);
        dg_text(bx, by + 24u, "正在加载 https://xos.local ...", blue, body);
        dg_text(bx, by + 48u, "M 移动  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    case 5u: /* 设置 */
        dg_text(bx, by, "设置中心", fg, body);
        dg_text(bx, by + 22u, "  [1] 显示与分辨率", fg, body);
        dg_text(bx, by + 44u, "  [2] 声音", fg, body);
        dg_text(bx, by + 66u, "  [3] 网络", fg, body);
        dg_text(bx, by + 88u, "  [4] 账户", fg, body);
        dg_text(bx, by + 130u, "M 移动  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    case 6u: /* 音乐 */
        dg_text(bx, by, "音乐播放器", fg, body);
        dg_fill(bx + 4u, by + 28u, 120u, 4u, dg_rgb(0x50,0xC8,0x70));
        dg_text(bx + 4u, by + 52u, "▶ XOS 主题曲", blue, body);
        dg_text(bx, by + 110u, "M 移动  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    default: /* 游戏 */
        dg_text(bx, by, "游戏中心", fg, body);
        dg_text(bx, by + 22u, "  [1] 贪吃蛇", fg, body);
        dg_text(bx, by + 44u, "  [2] 2048", fg, body);
        dg_text(bx, by + 110u, "M 移动  N 最小化  W 切换  Esc 关闭", gray, body);
        break;
    }
}

static void dg_window_draw(const dg_win_t *w)
{
    u32 title = dg_rgb(0x1E,0x3A,0x5F);
    u32 body  = dg_rgb(0xEE,0xEE,0xEE);
    const char *name = dg_icons[w->icon].name;
    dg_fill(w->x, w->y, w->w, w->h, body);
    dg_fill(w->x, w->y, w->w, 26u, title);
    dg_text(w->x + 10u, w->y + 9u, name, dg_rgb(0xF0,0xF0,0xF0), title);
    dg_rect(w->x, w->y, w->w, w->h, dg_rgb(0x0A,0x14,0x22));
    /* 关闭按钮 X */
    dg_fill(w->x + w->w - 24u, w->y + 5u, 18u, 16u, dg_rgb(0xC0,0x30,0x30));
    dg_char(w->x + w->w - 20u, w->y + 9u, 'X', dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0xC0,0x30,0x30));
    /* 最小化按钮 _ */
    dg_fill(w->x + w->w - 48u, w->y + 5u, 18u, 16u, dg_rgb(0x3A,0x5A,0x8A));
    dg_fill(w->x + w->w - 43u, w->y + 16u, 8u, 2u, dg_rgb(0xF0,0xF0,0xF0));
    /* 移动模式边框提示 */
    if (dg_move_mode && w == &dg_wins[dg_focus])
        dg_rect(w->x - 2u, w->y - 2u, w->w + 4u, w->h + 4u, dg_rgb(0x30,0xC0,0x50));
    dg_win_content(w);
}

/* ---------------- 桌面渲染 ---------------- */
static void dg_render(void)
{
    u32 i, x, y, k;
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

    /* 2) 图标：两行四列（窗口打开时仍可见，除被窗口遮挡外） */
    for (i = 0; i < DG_ICON_N; i++) {
        u32 col = i % 4u;
        u32 row = i / 4u;
        x = 40u + col * 140u;
        y = 50u + row * 130u;
        dg_icon_pattern(x, y, dg_icons[i].icon);
        if (i == dg_sel && dg_nwin == 0u)
            dg_rect(x - 3u, y - 3u, 38u, 38u, sel_c);
        dg_text(x + 2u, y + 36u, dg_icons[i].name, txt, dg_rgb(0x0E,0x22,0x40));
    }

    /* 3) 窗口：非最小化按 Z 序绘制（焦点最后=最上） */
    for (k = 0u; k < DG_WIN_MAX; k++) {
        i = (dg_focus + 1u + k) % DG_WIN_MAX;      /* 从焦点后开始，保证焦点最后画 */
        if (dg_wins[i].open && !dg_wins[i].min)
            dg_window_draw(&dg_wins[i]);
    }

    /* 4) 任务栏 */
    dg_fill(0u, DG_H - DG_TASKBAR, DG_W, DG_TASKBAR, bar);
    dg_fill(0u, DG_H - DG_TASKBAR, DG_W, 2u, dg_rgb(0x2E,0x3A,0x4E));
    /* 开始按钮（S 打开开始菜单） */
    dg_fill(6u, DG_H - DG_TASKBAR + 5u, 56u, 20u, dg_rgb(0x2F,0x7D,0xE1));
    dg_text(14u, DG_H - DG_TASKBAR + 11u, "XOS", dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0x2F,0x7D,0xE1));
    /* 状态提示 */
    dg_text(90u, DG_H - DG_TASKBAR + 11u,
            "Tab 选择  S 开始  R 菜单  Enter 打开", dg_rgb(0xA0,0xB0,0xC8), bar);
    /* 最小化窗口的恢复按钮（点击概念：按对应数字键恢复） */
    if (dg_nwin > 0u) {
        u32 rbx = 300u;
        for (i = 0u; i < DG_WIN_MAX; i++) {
            if (dg_wins[i].open) {
                dg_fill(rbx, DG_H - DG_TASKBAR + 6u, 34u, 18u,
                        dg_wins[i].min ? dg_rgb(0x2A,0x4A,0x6A) : dg_rgb(0x2E,0x3E,0x52));
                tbuf[0] = (char)('1' + i);
                tbuf[1] = 0;
                dg_text(rbx + 13u, DG_H - DG_TASKBAR + 12u, tbuf,
                        dg_wins[i].min ? dg_rgb(0x80,0xC0,0xF0) : dg_rgb(0xE0,0xE0,0xE0), bar);
                rbx += 40u;
            }
        }
    }
    /* 系统托盘：网络 + 音量 + 电池 + 通知角标 */
    dg_fill(DG_W - 150u, DG_H - DG_TASKBAR + 9u, 10u, 10u, dg_rgb(0x30,0xC0,0x50));   /* 网络绿点 */
    dg_fill(DG_W - 128u, DG_H - DG_TASKBAR + 8u, 12u, 12u, dg_rgb(0xE0,0xB0,0x40));   /* 音量 */
    dg_fill(DG_W - 128u, DG_H - DG_TASKBAR + 10u, 2u, 8u, dg_rgb(0xE0,0xB0,0x40));
    dg_rect(DG_W - 108u, DG_H - DG_TASKBAR + 8u, 14u, 12u, dg_rgb(0x80,0xE0,0x90));    /* 电池 */
    dg_fill(DG_W - 108u, DG_H - DG_TASKBAR + 8u, 10u, 12u, dg_rgb(0x20,0x50,0x30));
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

    /* 5) 开始菜单浮层 */
    if (dg_menu == DG_MENU_START) {
        u32 mx = 6u, my = DG_H - DG_TASKBAR - DG_ICON_N * 20u - 16u;
        u32 mw = 190u, mh = DG_ICON_N * 20u + 16u;
        dg_fill(mx, my, mw, mh, dg_rgb(0x22,0x2A,0x38));
        dg_rect(mx, my, mw, mh, dg_rgb(0x4A,0x5A,0x78));
        dg_text(mx + 8u, my + 4u, "XOS 应用程序", dg_rgb(0x80,0xC0,0xF0), dg_rgb(0x22,0x2A,0x38));
        for (i = 0u; i < DG_ICON_N; i++) {
            u32 iy = my + 20u + i * 20u;
            if (i == dg_menusel) dg_fill(mx + 4u, iy, mw - 8u, 18u, dg_rgb(0x2F,0x7D,0xE1));
            dg_text(mx + 10u, iy + 5u, dg_apps[i],
                    i == dg_menusel ? dg_rgb(0xFF,0xFF,0xFF) : dg_rgb(0xD0,0xD8,0xE0),
                    i == dg_menusel ? dg_rgb(0x2F,0x7D,0xE1) : dg_rgb(0x22,0x2A,0x38));
        }
        dg_text(mx + 8u, my + mh - 12u, "↑↓ 选择  Enter 启动  Esc 关闭",
                dg_rgb(0x90,0xA0,0xB8), dg_rgb(0x22,0x2A,0x38));
    }

    /* 6) 右键菜单浮层 */
    if (dg_menu == DG_MENU_RIGHT) {
        static const char *ritems[3] = { "打开", "属性", "关闭窗口" };
        u32 rx = 40u + (dg_sel % 4u) * 140u + 30u;
        u32 ry = 50u + (dg_sel / 4u) * 130u + 10u;
        dg_fill(rx, ry, 150u, 66u, dg_rgb(0x22,0x2A,0x38));
        dg_rect(rx, ry, 150u, 66u, dg_rgb(0x4A,0x5A,0x78));
        for (i = 0u; i < 3u; i++) {
            u32 iy = ry + 6u + i * 20u;
            if (i == dg_menusel) dg_fill(rx + 4u, iy, 142u, 18u, dg_rgb(0x2F,0x7D,0xE1));
            dg_text(rx + 10u, iy + 5u, ritems[i],
                    i == dg_menusel ? dg_rgb(0xFF,0xFF,0xFF) : dg_rgb(0xD0,0xD8,0xE0),
                    i == dg_menusel ? dg_rgb(0x2F,0x7D,0xE1) : dg_rgb(0x22,0x2A,0x38));
        }
    }

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

/* 打开一个窗口（找到空闲槽，位置级联偏移） */
static void dg_win_open(u32 icon)
{
    u32 i, slot = DG_WIN_MAX;
    for (i = 0u; i < DG_WIN_MAX; i++)
        if (!dg_wins[i].open) { slot = i; break; }
    if (slot == DG_WIN_MAX) return;             /* 窗口数已满 */
    dg_wins[slot].open = 1u;
    dg_wins[slot].icon = icon;
    dg_wins[slot].x = 100u + (slot % 3u) * 36u;
    dg_wins[slot].y = 60u + (slot % 3u) * 30u;
    dg_wins[slot].w = 400u;
    dg_wins[slot].h = 260u;
    dg_wins[slot].min = 0u;
    dg_focus = slot;
    dg_nwin++;
    dg_move_mode = 0u;
}

/* 关闭焦点窗口，焦点跳到最近打开的窗口 */
static void dg_win_close(void)
{
    u32 i;
    dg_wins[dg_focus].open = 0u;
    dg_wins[dg_focus].min = 0u;
    if (dg_nwin > 0u) dg_nwin--;
    for (i = 0u; i < DG_WIN_MAX; i++) {
        u32 idx = (dg_focus + DG_WIN_MAX - 1u - i) % DG_WIN_MAX;
        if (dg_wins[idx].open) { dg_focus = idx; return; }
    }
    dg_focus = 0u;
}

void desk_gui_run(void)
{
    kbd_event_t ev, tmp;
    if (desk_gui_init() != 0) return;              /* 图形不可用：安全回退文本 Shell */
    while (kbd_read_event(&tmp) == 0) { }           /* 清空残留键盘事件 */

    dg_nwin = 0u;
    dg_focus = 0u;
    dg_menu = DG_MENU_NONE;
    dg_menusel = 0u;
    dg_move_mode = 0u;
    dg_sel = 0u;
    dg_render();

    for (;;) {
        u32 key;
        kbd_poll();
        if (kbd_read_event(&ev) != 0) {
            __asm__ __volatile__("hlt");
            continue;
        }
        if (ev.type != EV_KEY_DOWN) continue;
        key = ev.key;

        /* --- 开始菜单 --- */
        if (dg_menu == DG_MENU_START) {
            if (key == KEY_UP || key == KEY_LEFT) {
                dg_menusel = (dg_menusel + DG_ICON_N - 1u) % DG_ICON_N;
            } else if (key == KEY_DOWN || key == KEY_RIGHT) {
                dg_menusel = (dg_menusel + 1u) % DG_ICON_N;
            } else if (key == KEY_ENTER) {
                dg_win_open(dg_menusel);
                dg_menu = DG_MENU_NONE;
            } else if (key == KEY_ESC) {
                dg_menu = DG_MENU_NONE;
            }
            dg_render();
            continue;
        }

        /* --- 右键菜单 --- */
        if (dg_menu == DG_MENU_RIGHT) {
            if (key == KEY_UP) {
                dg_menusel = (dg_menusel + 2u) % 3u;
            } else if (key == KEY_DOWN) {
                dg_menusel = (dg_menusel + 1u) % 3u;
            } else if (key == KEY_ENTER) {
                if (dg_menusel == 0u) dg_win_open(dg_sel);
                else if (dg_menusel == 1u) dg_win_open(5u);     /* 属性→设置窗口 */
                else if (dg_nwin > 0u) dg_win_close();
                dg_menu = DG_MENU_NONE;
            } else if (key == KEY_ESC) {
                dg_menu = DG_MENU_NONE;
            }
            dg_render();
            continue;
        }

        /* --- 有窗口：窗口操作 --- */
        if (dg_nwin > 0u) {
            if (dg_move_mode) {
                if (key == KEY_LEFT) {
                    if (dg_wins[dg_focus].x > 4u) dg_wins[dg_focus].x -= 10u;
                } else if (key == KEY_RIGHT) {
                    if (dg_wins[dg_focus].x + dg_wins[dg_focus].w + 4u < DG_W)
                        dg_wins[dg_focus].x += 10u;
                } else if (key == KEY_UP) {
                    if (dg_wins[dg_focus].y > 4u) dg_wins[dg_focus].y -= 10u;
                } else if (key == KEY_DOWN) {
                    if (dg_wins[dg_focus].y + dg_wins[dg_focus].h + 4u < DG_H - DG_TASKBAR)
                        dg_wins[dg_focus].y += 10u;
                } else if (key == KEY_ENTER || key == KEY_ESC || key == KEY_M) {
                    dg_move_mode = 0u;                       /* 结束移动 */
                }
                dg_render();
                continue;
            }
            if (key == KEY_M) {
                dg_move_mode = 1u;                           /* 进入移动模式 */
            } else if (key == KEY_N) {
                dg_wins[dg_focus].min = dg_wins[dg_focus].min ? 0u : 1u;  /* 最小化/恢复 */
            } else if (key == KEY_W || key == KEY_TAB) {
                u32 i, next = dg_focus;
                for (i = 0u; i < DG_WIN_MAX; i++) {          /* 切换焦点（跳过最小化） */
                    next = (next + 1u) % DG_WIN_MAX;
                    if (dg_wins[next].open) break;
                }
                if (dg_wins[next].open) dg_focus = next;
            } else if (key >= KEY_1 && key <= KEY_4) {
                u32 wi = key - KEY_1;                        /* 恢复对应窗口 */
                if (dg_wins[wi].open) {
                    dg_wins[wi].min = 0u;
                    dg_focus = wi;
                }
            } else if (key == KEY_ESC) {
                dg_win_close();
            } else if (key == KEY_S) {
                dg_menu = DG_MENU_START;
                dg_menusel = 0u;
            } else if (key == KEY_R) {
                dg_menu = DG_MENU_RIGHT;
                dg_menusel = 0u;
            }
            dg_render();
            continue;
        }

        /* --- 无窗口：桌面图标导航 --- */
        if (key == KEY_TAB || key == KEY_RIGHT || key == KEY_DOWN) {
            dg_sel = (dg_sel + 1u) % DG_ICON_N;
        } else if (key == KEY_LEFT || key == KEY_UP) {
            dg_sel = (dg_sel + DG_ICON_N - 1u) % DG_ICON_N;
        } else if (key == KEY_ENTER) {
            dg_win_open(dg_sel);
        } else if (key == KEY_S) {
            dg_menu = DG_MENU_START;
            dg_menusel = 0u;
        } else if (key == KEY_R) {
            dg_menu = DG_MENU_RIGHT;
            dg_menusel = 0u;
        } else if (key == KEY_ESC) {
            break;                                         /* 退出桌面 → 文本 Shell */
        }
        dg_render();
    }

    /* 安全退出：恢复文本模式并清屏（不影响硬件/磁盘） */
    display_set_mode(0);
    con_clear();
    con_set_cursor(0u, 0u);
    con_puts("  XOS 桌面已退出，返回文本终端。输入 'desktop' 重新进入桌面。\n");
    con_flush();
}
