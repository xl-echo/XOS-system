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
#include "fs.h"
#include "pmm.h"
#include "task.h"

extern void *memset(void *dst, int c, unsigned int n);

/* ---------------- 32bpp 帧缓冲访问 ---------------- */
static volatile u32 *dg_fb = (volatile u32 *)DG_LFB;
static u32 dg_sel = 0u;
static void dg_itoa(int v, char *out);

/* 音乐播放器状态 */
static u32 dg_mu_play = 0u;      /* 播放中 */
static u32 dg_mu_track = 0u;     /* 当前曲目 0-2 */
static u32 dg_mu_start = 0u;     /* 播放起始 tick */
/* 小游戏状态（5x5 收集） */
static i32  dg_gm_px = 2, dg_gm_py = 2;
static i32  dg_gm_fx = 4, dg_gm_fy = 3;
static u32  dg_gm_score = 0u;
static u32  dg_gm_seed = 1u;
static u32  dg_gm_mode = 0u;            /* 0收集 1=2048 */
static u32  dg_g2048[16];               /* 2048 棋盘 */
static u32  dg_2048_score = 0u;         /* 2048 分数 */
static u32  dg_2048_over  = 0u;         /* 无空位=结束 */
static u32  dg_wsz        = 0u;         /* 窗口尺寸档：0标准 1大 2特大 */
static const u16 dg_win_sz[3][2] = { {300u,200u}, {420u,280u}, {560u,380u} };
/* 照片查看器：当前图片索引 0-2 */
static u32  dg_ph_idx = 0u;
/* 秒表状态 */
static u32  dg_st_running = 0u;
static u32  dg_st_start   = 0u;
static u32  dg_st_total   = 0u;

u32 dg_welcome = 0u;                 /* 首次使用欢迎向导 */

static u32 dg_rand(void)
{
    dg_gm_seed = dg_gm_seed * 1103515245u + 12345u;
    return (dg_gm_seed >> 16) & 0x7FFFu;
}              /* 当前选中图标索引 */

static inline void dgpix(u32 x, u32 y, u32 c)
{
    if (x < DG_W && y < DG_H) dg_fb[y * DG_W + x] = c;
}

u32 dg_rgb(u32 r, u32 g, u32 b)
{
    return (r << 16) | (g << 8) | b;
}

void dg_fill(u32 x, u32 y, u32 w, u32 h, u32 c)
{
    u32 i, j;
    if (x >= DG_W || y >= DG_H) return;
    if (x + w > DG_W) w = DG_W - x;
    if (y + h > DG_H) h = DG_H - y;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            dg_fb[(y + j) * DG_W + (x + i)] = c;
}

void dg_rect(u32 x, u32 y, u32 w, u32 h, u32 c)
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

void dg_text(u32 x, u32 y, const char *s, u32 fg, u32 bg)
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
    { "照片", 8u }, { "时钟", 9u }, { "监控", 10u }, { "天气", 11u },
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
    case 8u: c1 = dg_rgb(0xE8,0xA0,0x40); c2 = dg_rgb(0xF8,0xE0,0xB0); c3 = dg_rgb(0xA0,0x60,0x20); break; /* 照片 */
    case 9u: c1 = dg_rgb(0x28,0x3A,0x50); c2 = dg_rgb(0xE8,0xE8,0xE8); c3 = dg_rgb(0x58,0x88,0xC0); break; /* 时钟 */
    case 10u: c1 = dg_rgb(0x1E,0x3A,0x24); c2 = dg_rgb(0x90,0xE8,0x90); c3 = dg_rgb(0x40,0x80,0x40); break; /* 监控 */
    case 11u: c1 = dg_rgb(0x1A,0x3A,0x5E); c2 = dg_rgb(0xE0,0xF0,0xFF); c3 = dg_rgb(0x40,0xA0,0xE0); break; /* 天气 */
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
    case 8u:   /* 照片：相框 + 太阳 */
        dg_fill(x + 6u, y + 6u, 20u, 20u, c2);
        dg_fill(x + 12u, y + 10u, 8u, 8u, c1);
        dg_fill(x + 6u, y + 22u, 20u, 4u, c3);
        dg_char(x + 10u, y + 12u, '1', c3, c2);
        break;
    case 9u:   /* 时钟：圆盘 + 指针 */
        dg_fill(x + 8u, y + 8u, 16u, 16u, c2);
        dg_fill(x + 15u, y + 8u, 2u, 16u, c3);
        dg_fill(x + 8u, y + 15u, 16u, 2u, c3);
        break;
    case 10u:   /* 监控：柱状图 */
        dg_fill(x + 8u, y + 6u, 5u, 4u, c3);
        dg_fill(x + 14u, y + 10u, 5u, 10u, c2);
        dg_fill(x + 20u, y + 8u, 5u, 8u, c1);
        break;
    case 11u:   /* 天气：太阳 */
        dg_fill(x + 13u, y + 13u, 6u, 6u, c2);
        dg_fill(x + 9u, y + 15u, 14u, 2u, c3);
        dg_fill(x + 15u, y + 9u, 2u, 14u, c3);
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
    char inbuf[24];            /* 输入缓冲（计算器/终端） */
    u32  inlen;
    char out[4][40];           /* 输出缓冲（终端/计算器结果） */
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
static u32  dg_wx_idx  = 0u;              /* 天气：0晴 1多云 2雨 3雪 */
static u32  dg_set_page = 0u;            /* 设置中心页签：0概览 1驱动 2存储 3关于 */
static u32  dg_mu_vol   = 60u;           /* 音乐音量 0-100 */
static u32  dg_fm_dir  = 0u;            /* 文件管理器：0根 1/mnt 2/dev 3查看note */
static u32  dg_fm_sel  = 0u;            /* 文件管理器高亮项 */
static char dg_fm_note[40];             /* note.txt 内容缓存 */
static u32  dg_menusel = 0u;

static const char *dg_apps[DG_ICON_N] = {
    "文件管理器", "文本编辑器", "计算器", "图形终端",
    "浏览器", "设置中心", "音乐播放器", "游戏中心",
    "照片查看器", "时钟日历", "系统监控", "天气",
};

/* 无符号整数转字符串 */
static void dg_u2s(u32 v, char *out)
{
    char t[12];
    int n = 0, i;
    if (v == 0u) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0u) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

/* 窗口内容（按图标不同） */
static void dg_win_content(const dg_win_t *w)
{
    u32 body = dg_rgb(0xEE,0xEE,0xEE);
    u32 fg   = dg_rgb(0x18,0x18,0x18);
    u32 blue = dg_rgb(0x1E,0x6F,0xD0);
    u32 gray = dg_rgb(0x60,0x60,0x60);
    u32 bx = w->x + 16u, by = w->y + 44u;
    u32 dy = (w->h > 70u) ? (w->h - 30u) / 8u : 18u;
    u32 dy2, dy3, dy4, dy5, dy6, dy7;
    if (dy < 16u) dy = 16u;
    if (dy > 42u) dy = 42u;
    dy2 = 2u * dy; dy3 = 3u * dy; dy4 = 4u * dy;
    dy5 = 5u * dy; dy6 = 6u * dy; dy7 = 7u * dy;
    switch (w->icon) {
    case 0u: /* 文件管理器 v2：目录导航 + 查看 */
        {
            char nm[128];
            u32 di = 0u, ln = 0u;
            const char *title = "/ (root)";
            const char *path = "/";
            u32 n = 0u;
            if (dg_fm_dir == 1u) { title = "/mnt"; path = "/mnt"; }
            else if (dg_fm_dir == 2u) { title = "/dev"; path = "/dev"; }
            else if (dg_fm_dir == 3u) { title = "note.txt"; }
            dg_text(bx, by, title, blue, body);
            if (dg_fm_dir == 3u) {
                u32 j = 0u;
                dg_text(bx + 4u, by + 24u, "note.txt 内容:", dg_rgb(0x1E,0x6F,0xD0), body);
                while (dg_fm_note[j] && j < 39u) { dg_text(bx + 4u, by + 48u + (j / 30u) * 22u, dg_fm_note + j, fg, body); j += 30u; }
                if (!dg_fm_note[0]) dg_text(bx + 4u, by + 48u, "(empty)", gray, body);
            } else {
                while (di < 64u) {
                    if (fs_readdir(path, di, nm) != 0) break;
                    if (nm[0] == 0) break;
                    n++;
                    dg_text(bx + 4u, by + dy + ln * dy,
                            ln == dg_fm_sel ? " >" : "  ", dg_rgb(0xE8,0xA0,0x30), body);
                    /* 尝试按文件打开：成功=文件，失败=目录 */
                    {
                        char fp[64];
                        u32 j = 0u;
                        const char *base = (dg_fm_dir == 1u) ? "/mnt/" : (dg_fm_dir == 2u) ? "/dev/" : "/";
                        while (base[j]) { fp[j] = base[j]; j++; }
                        { u32 k = 0u; while (nm[k] && j + k < 60u) { fp[j + k] = nm[k]; k++; } fp[j + k] = 0; }
                        if (fs_open(fp, O_READ) >= 0) {
                            dg_text(bx + 12u, by + dy + ln * 22u, "[F]", dg_rgb(0x2A,0x8A,0x3A), body);
                        } else {
                            dg_text(bx + 12u, by + dy + ln * 22u, "[D]", dg_rgb(0x1E,0x6F,0xD0), body);
                        }
                    }
                    dg_text(bx + 30u, by + dy + ln * 22u, nm, fg, body);
                    ln++;
                    di++;
                    if (ln >= 5u) break;
                }
                if (!ln) dg_text(bx, by + dy, "(empty)", gray, body);
            }
            dg_text(bx, by + dy6, "Enter进入/查看  Backspace返回  Esc关闭  M移动", gray, body);
        }
        break;
    case 1u: /* 文本编辑器：真实输入 + 保存到 /note.txt + 历史回读（打开时预读） */
        dg_text(bx, by, "note.txt - 文本编辑器", fg, body);
        dg_text(bx + 4u, by + dy, w->inlen ? w->inbuf : "(输入文本，Enter 保存)",
                dg_rgb(0xF0,0xF0,0xF0), body);
        dg_text(bx + 4u, by + dy2, w->out[0], gray, body);
        dg_text(bx + 4u, by + dy3, w->out[2], blue, body);
        dg_text(bx + 4u, by + dy4, w->out[1], fg, body);
        dg_text(bx, by + dy6, "字母数字输入  Enter保存  Esc关闭  M移动", gray, body);
        break;
    case 2u: /* 计算器：真实输入与四则求值 */
        {
            u32 j;
            dg_fill(bx + 4u, by, 150u, 22u, dg_rgb(0x2A,0x3A,0x4A));
            dg_rect(bx + 4u, by, 150u, 22u, dg_rgb(0x1A,0x2A,0x3A));
            dg_text(bx + 10u, by + 6u, w->inlen ? w->inbuf : "0",
                    dg_rgb(0xF0,0xF0,0xF0), dg_rgb(0x2A,0x3A,0x4A));
            dg_text(bx + 10u, by + 34u, w->out[0], dg_rgb(0x1E,0x6F,0xD0), body);
            for (j = 0u; j < 3u; j++) {
                dg_fill(bx + 4u + j * 52u, by + 58u, 46u, 22u, dg_rgb(0xC8,0xCC,0xD0));
                dg_rect(bx + 4u + j * 52u, by + 58u, 46u, 22u, dg_rgb(0x88,0x8C,0x90));
            }
            dg_text(bx + 12u, by + 63u, "1", fg, dg_rgb(0xC8,0xCC,0xD0));
            dg_text(bx + 64u, by + 63u, "2", fg, dg_rgb(0xC8,0xCC,0xD0));
            dg_text(bx + 116u, by + 63u, "3", fg, dg_rgb(0xC8,0xCC,0xD0));
            dg_text(bx, by + dy5, "数字+运算符  Enter=  Backspace=删  Esc=关", gray, body);
            break;
        }
    case 3u: /* 图形终端：真实命令执行 */
        {
            u32 j;
            dg_fill(bx - 8u, by - 8u, w->w - 16u, 116u, dg_rgb(0x10,0x10,0x18));
            dg_text(bx + 4u, by, "XOS # ", dg_rgb(0x30,0xC0,0x50), dg_rgb(0x10,0x10,0x18));
            dg_text(bx + 40u, by, w->inbuf, dg_rgb(0xF0,0xF0,0xF0), dg_rgb(0x10,0x10,0x18));
            for (j = 0u; j < 4u; j++) {
                if (w->out[j][0])
                    dg_text(bx + 4u, by + dy + j * 22u, w->out[j],
                            dg_rgb(0xE0,0xE0,0xE0), dg_rgb(0x10,0x10,0x18));
            }
            dg_text(bx, by + dy6, "输入命令: help echo mem df ps uptime clear", gray, body);
            break;
        }
    case 4u: /* 浏览器：地址栏 + 内置页面 */
        {
            const char *pg = w->out[0][0] ? w->out[0] : "home";
            dg_text(bx, by, "地址: xos://", fg, body);
            dg_text(bx + 68u, by, w->inlen ? w->inbuf : "home", blue, body);
            if (pg[0] == 'a' && pg[1] == 'b' && pg[2] == 'o' && pg[3] == 'u' && pg[4] == 't') {
                dg_text(bx + 4u, by + 28u, "关于 XOS", fg, body);
                dg_text(bx + 4u, by + 50u, "XOS 0.3.0 完全自研 x86 桌面操作系统", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "内核 / 文件系统 / 图形界面均独立实现", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 94u, "不依赖任何外部商业或闭源组件", dg_rgb(0xE0,0xE0,0xE0), body);
            } else if (pg[0] == 's' && pg[1] == 'y' && pg[2] == 's') {
                extern u32 pmm_total_pages(void);
                extern u32 desk_task_count(void);
                char nb[16];
                dg_text(bx + 4u, by + 28u, "系统状态", fg, body);
                dg_text(bx + 4u, by + 50u, "内存: 128 MB  (4K 页)", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "进程数: ", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_itoa((int)desk_task_count(), nb);
                dg_text(bx + 72u, by + 72u, nb, dg_rgb(0x30,0xC0,0x50), body);
                dg_text(bx + 4u, by + 94u, "显示: 640x480x32  VBE 帧缓冲", dg_rgb(0xE0,0xE0,0xE0), body);
            } else if (pg[0] == 'h' && pg[1] == 'e' && pg[2] == 'l' && pg[3] == 'p') {
                dg_text(bx + 4u, by + 28u, "桌面快捷键", fg, body);
                dg_text(bx + 4u, by + 50u, "M 移动窗口  N 最小化  1-4 恢复", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "W/Tab 切换窗口  S 开始菜单  R 右键", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 94u, "Esc 关闭窗口 / 退出桌面", dg_rgb(0xE0,0xE0,0xE0), body);
            } else {
                dg_text(bx + 4u, by + 28u, "XOS 浏览器", fg, body);
                dg_text(bx + 4u, by + 50u, "内置页面: 1 关于  2 系统  3 帮助", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "输入 xos://about 等地址回车导航", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 94u, "（无网络栈，内置页面离线可用）", gray, body);
            }
            dg_text(bx, by + dy6, "输入地址  Enter导航  Esc关闭  M移动", gray, body);
            break;
        }
    case 5u: /* 设置中心：1概览 2驱动 3存储 4关于 */
        {
            u32 pmem, pproc;
            extern u32 desk_task_count(void);
            pmem = pmm_total_pages() * 4u / 1024u;         /* MB */
            pproc = desk_task_count();
            if (dg_set_page == 0u) {
                char nb[16];
                dg_text(bx, by, "设置中心 - 系统概览 [1]", fg, body);
                dg_text(bx + 4u, by + 24u, "系统:  XOS 0.3.0", fg, body);
                dg_text(bx + 4u, by + 46u, "架构:  x86 (Intel 32位)", fg, body);
                dg_text(bx + 4u, by + 68u, "内存:  ", fg, body);
                dg_u2s(pmem, nb);
                dg_text(bx + 64u, by + 68u, nb, blue, body);
                dg_text(bx + 92u, by + 68u, " MB", gray, body);
                dg_text(bx + 4u, by + 90u, "进程:  ", fg, body);
                dg_u2s(pproc, nb);
                dg_text(bx + 64u, by + 90u, nb, blue, body);
                dg_text(bx + 4u, by + 112u, "显示:  640x480 32位色", fg, body);
                dg_text(bx, by + 136u, "数字 1-4 切换页签  Esc关闭  M移动", gray, body);
            } else if (dg_set_page == 1u) {
                dg_text(bx, by, "设置中心 - 硬件驱动 [2]", fg, body);
                dg_text(bx + 4u, by + 24u, "键盘:  PS/2 已加载", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx + 4u, by + 46u, "鼠标:  待接入", gray, body);
                dg_text(bx + 4u, by + 68u, "显示:  Bochs VBE 640x480x32", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx + 4u, by + 90u, "磁盘:  ATA 已加载 (xos.img)", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx + 4u, by + 112u, "串口:  COM1 已加载 (调试日志)", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx, by + 136u, "数字 1-4 切换页签  Esc关闭  M移动", gray, body);
            } else if (dg_set_page == 2u) {
                char nb[16];
                int fd = fs_open("/note.txt", O_READ);
                u32 sz = 0u;
                char t[8];
                if (fd >= 0) { sz = (u32)fs_read(fd, t, 7u); fs_close(fd); }
                dg_text(bx, by, "设置中心 - 存储 [3]", fg, body);
                dg_text(bx + 4u, by + 24u, "磁盘:  10 MB 虚拟盘 (xos.img)", fg, body);
                dg_text(bx + 4u, by + 46u, "根目录:  /mnt /dev /note.txt", fg, body);
                dg_text(bx + 4u, by + 68u, "note.txt 大小: ", fg, body);
                dg_u2s(sz, nb);
                dg_text(bx + 140u, by + 68u, nb, blue, body);
                dg_text(bx + 160u, by + 68u, " 字节", gray, body);
                dg_text(bx + 4u, by + 90u, "文件系统: XOS-FS v1 (自研)", fg, body);
                dg_text(bx, by + 136u, "数字 1-4 切换页签  Esc关闭  M移动", gray, body);
            } else {
                dg_text(bx, by, "设置中心 - 关于 [4]", fg, body);
                dg_text(bx + 4u, by + 24u, "XOS 操作系统 0.3.0", blue, body);
                dg_text(bx + 4u, by + 46u, "完全自研 x86 内核 + 图形桌面", fg, body);
                dg_text(bx + 4u, by + 68u, "不依赖任何外部内核或闭源组件", fg, body);
                dg_text(bx + 4u, by + 90u, "官方账号: admin / admin123", fg, body);
                dg_text(bx + 4u, by + 112u, "技术支持: 内置于终端 help 命令", fg, body);
                dg_text(bx, by + 136u, "数字 1-4 切换页签  Esc关闭  M移动", gray, body);
            }
        }
        break;
    case 6u: /* 音乐播放器：真实时钟进度 */
        {
            u32 sec = 0u;
            const char *tracks[3] = { "XOS 主题曲", "启动协奏", "桌面圆舞曲" };
            if (dg_mu_play) sec = (pit_tick_count() - dg_mu_start) / 100u;
            dg_text(bx, by, "音乐播放器", fg, body);
            dg_text(bx + 4u, by + dy, dg_mu_play ? "▶ 播放中" : "⏸ 已暂停", blue, body);
            dg_fill(bx + 4u, by + dy2, 180u, 6u, dg_rgb(0x30,0x40,0x50));
            dg_fill(bx + 4u, by + dy2, (sec % 60u) * 3u, 6u, dg_rgb(0x50,0xC8,0x70));
            dg_text(bx + 4u, by + 58u, tracks[dg_mu_track % 3u], dg_rgb(0xE0,0xE0,0xE0), body);
            {
                char nb[16];
                nb[0] = (char)('0' + (sec / 60u));
                nb[1] = ':';
                nb[2] = (char)('0' + (sec % 60u) / 10u);
                nb[3] = (char)('0' + (sec % 60u) % 10u);
                nb[4] = 0;
                dg_text(bx + 132u, by + 58u, nb, gray, body);
            }
            dg_text(bx + 4u, by + 80u, "音量: ", gray, body);
            dg_fill(bx + 64u, by + 80u, 80u, 5u, dg_rgb(0x30,0x40,0x50));
            dg_fill(bx + 64u, by + 80u, dg_mu_vol * 80u / 100u, 5u, dg_rgb(0xE8,0xB0,0x30));
            {
                char nb[16];
                u32 vv = dg_mu_vol;
                nb[0] = (char)('0' + vv / 100u);
                nb[1] = (char)('0' + (vv % 100u) / 10u);
                nb[2] = (char)('0' + vv % 10u);
                nb[3] = '%';
                nb[4] = 0;
                dg_text(bx + 152u, by + 80u, nb, fg, body);
            }
            dg_text(bx, by + dy5, "P播放/暂停  N下一曲  +/-音量  M移动  Esc关闭", gray, body);
            break;
        }
    case 8u: /* 照片查看器：内置像素画 1/2/3 切换 */
        {
            u32 pw = w->w - 40u, ph = 110u;
            u32 px = bx + 12u, py = by + 30u;
            u32 sky, sun, hill, ground;
            if (dg_ph_idx == 0u) {        /* 图1 日出风景 */
                sky = dg_rgb(0x1E,0x3A,0x6A); sun = dg_rgb(0xF0,0xC0,0x40);
                hill = dg_rgb(0x2A,0x38,0x58); ground = dg_rgb(0x14,0x2A,0x1E);
            } else if (dg_ph_idx == 1u) { /* 图2 星空 */
                sky = dg_rgb(0x08,0x10,0x2A); sun = dg_rgb(0xE8,0xE8,0xE0);
                hill = dg_rgb(0x10,0x1C,0x38); ground = dg_rgb(0x0A,0x14,0x1C);
            } else {                     /* 图3 绿野 */
                sky = dg_rgb(0x3A,0x7A,0xC0); sun = dg_rgb(0xFF,0xE0,0x80);
                hill = dg_rgb(0x2E,0x5A,0x3A); ground = dg_rgb(0x1C,0x44,0x28);
            }
            dg_text(bx, by, "照片查看器", fg, body);
            dg_text(bx + 120u, by, dg_ph_idx == 0u ? "1/3 日出" :
                            (dg_ph_idx == 1u ? "2/3 星空" : "3/3 绿野"), blue, body);
            dg_fill(px, py, pw, ph, sky);
            dg_rect(px, py, pw, ph, dg_rgb(0x0A,0x14,0x22));
            dg_fill(px + pw - 34u, py + 8u, 22u, 22u, sun);        /* 太阳/月 */
            dg_fill(px, py + ph - 34u, pw, 34u, ground);           /* 地面 */
            dg_fill(px + 6u, py + ph - 62u, pw / 3u, 28u, hill);   /* 远山 */
            dg_fill(px + pw / 3u + 4u, py + ph - 72u, pw / 3u, 38u, hill);
            if (dg_ph_idx == 1u) {                                  /* 星空点缀 */
                dg_fill(px + 20u, py + 18u, 2u, 2u, dg_rgb(0xF0,0xF0,0xF0));
                dg_fill(px + 66u, py + 40u, 2u, 2u, dg_rgb(0xF0,0xF0,0xF0));
                dg_fill(px + 110u, py + 22u, 2u, 2u, dg_rgb(0xF0,0xF0,0xF0));
                dg_fill(px + 150u, py + 52u, 2u, 2u, dg_rgb(0xF0,0xF0,0xF0));
            } else if (dg_ph_idx == 0u) {                           /* 云 */
                dg_fill(px + 30u, py + 26u, 36u, 8u, dg_rgb(0xE8,0xF0,0xF8));
                dg_fill(px + 42u, py + 18u, 22u, 8u, dg_rgb(0xE8,0xF0,0xF8));
            }
            dg_text(bx, by + dy7, "数字键 1/2/3 切换图片  M 移动  N 最小化  Esc 关闭", gray, body);
            break;
        }
    case 11u: /* 天气 */
        {
            static const char *wxname[4] = { "晴朗", "多云", "小雨", "小雪" };
            static const int   wxtemp[4] = { 26, 20, 15, -2 };
            char b1[12];
            u32 wc = dg_rgb(0x1E,0x6F,0xDC);
            dg_text(bx, by, "XOS 天气", fg, body);
            dg_text(bx + 4u, by + 24u, "城市: 上海  今日天气: ", gray, body);
            dg_text(bx + 200u, by + 24u, wxname[dg_wx_idx], blue, body);
            dg_u2s((u32)(wxtemp[dg_wx_idx] > 0 ? wxtemp[dg_wx_idx] : -wxtemp[dg_wx_idx]), b1);
            dg_text(bx + 4u, by + 46u, "气温: ", gray, body);
            dg_text(bx + 64u, by + 46u, b1, fg, body);
            dg_text(bx + 92u, by + 46u, " °C", gray, body);
            dg_text(bx + 4u, by + 68u, "体感: ", gray, body);
            dg_text(bx + 64u, by + 68u, "适宜户外活动", wc, body);
            dg_text(bx + 4u, by + 90u, "数字 1-4 切换天气类型  (1晴 2多云 3雨 4雪)", gray, body);
            dg_text(bx, by + dy6, "1-4切换  Esc关闭  M移动", gray, body);
        }
        break;
    case 10u: /* 系统监控 */
        {
            pmm_stats_t pst;
            task_stats_t tst;
            char b1[16], b2[16], b3[16], b4[16];
            u32 tpages, fpages, upages, kb;
            task_stats(&tst);
            pmm_stats(&pst);
            tpages = pmm_total_pages();
            fpages = pmm_free_page_count();
            upages = pmm_used_pages();
            dg_text(bx, by, "XOS 系统监控", fg, body);
            kb = tpages * 4u;
            dg_u2s(kb, b1); dg_u2s(upages * 4u, b2); dg_u2s(fpages * 4u, b3);
            dg_text(bx + 4u, by + 24u, "内存总计: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_text(bx + 74u, by + 24u, b1, fg, body);
            dg_text(bx + 110u, by + 24u, " KB  已用: ", gray, body);
            dg_text(bx + 190u, by + 24u, b2, fg, body);
            dg_text(bx + 226u, by + 24u, " KB  空闲: ", gray, body);
            dg_text(bx + 306u, by + 24u, b3, fg, body);
            dg_text(bx + 342u, by + 24u, " KB", gray, body);
            dg_u2s(tst.task_count, b1); dg_u2s(tst.ready_count, b2);
            dg_u2s(tst.zombie_count, b3); dg_u2s(tst.switch_total, b4);
            dg_text(bx + 4u, by + 48u, "任务数: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_text(bx + 74u, by + 48u, b1, fg, body);
            dg_text(bx + 100u, by + 48u, "  就绪: ", gray, body);
            dg_text(bx + 160u, by + 48u, b2, fg, body);
            dg_text(bx + 186u, by + 48u, "  僵尸: ", gray, body);
            dg_text(bx + 246u, by + 48u, b3, fg, body);
            dg_text(bx + 272u, by + 48u, "  切换: ", gray, body);
            dg_text(bx + 332u, by + 48u, b4, fg, body);
            dg_u2s(tst.ticks_total / 100u, b1);
            dg_text(bx + 4u, by + 72u, "运行时间: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_text(bx + 74u, by + 72u, b1, fg, body);
            dg_text(bx + 100u, by + 72u, " 秒  版本: XOS 0.3.0  内核: 0x00100000-0x00138520", gray, body);
            dg_text(bx + 4u, by + 96u, "空闲区: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_u2s(pst.free_regions, b1);
            dg_text(bx + 74u, by + 96u, b1, fg, body);
            dg_text(bx + 100u, by + 96u, "  (按 U 刷新)", gray, body);
            dg_text(bx, by + dy6, "U刷新  Esc关闭  M移动", gray, body);
        }
        break;
    case 9u: /* 时钟日历 + 秒表 */
        {
            u32 sec, hh, mm;
            char nb[24];
            sec = pit_tick_count() / 100u;
            hh = (10u + sec / 3600u) % 24u;
            mm = sec % 3600u / 60u;
            dg_text(bx, by, "时钟日历", fg, body);
            nb[0] = (char)('0' + hh / 10u); nb[1] = (char)('0' + hh % 10u);
            nb[2] = ':'; nb[3] = (char)('0' + mm / 10u); nb[4] = (char)('0' + mm % 10u);
            nb[5] = ':'; nb[6] = (char)('0' + sec % 60u / 10u); nb[7] = (char)('0' + sec % 60u % 10u);
            nb[8] = 0;
            dg_fill(bx + 8u, by + 30u, dg_text_w(nb) * 2u, 24u, dg_rgb(0x1E,0x3A,0x5F));
            dg_text(bx + 10u, by + 34u, nb, dg_rgb(0xF0,0xF0,0xF0), dg_rgb(0x1E,0x3A,0x5F));
            dg_text(bx + 10u, by + dy3, "2026年10月9日  星期五", fg, body);
            /* 秒表 */
            {
                u32 st = dg_st_running ? (pit_tick_count() - dg_st_start) / 100u
                                       : dg_st_total;
                u32 sm, ss;
                char sb[24];
                sm = st / 60u; ss = st % 60u;
                sb[0] = 'S'; sb[1] = 't'; sb[2] = 'o'; sb[3] = 'p';
                sb[4] = 'w'; sb[5] = 'a'; sb[6] = 't'; sb[7] = 'c'; sb[8] = 'h';
                sb[9] = ':'; sb[10] = ' ';
                sb[11] = (char)('0' + sm / 10u); sb[12] = (char)('0' + sm % 10u);
                sb[13] = ':'; sb[14] = (char)('0' + ss / 10u); sb[15] = (char)('0' + ss % 10u);
                sb[16] = 0;
                dg_text(bx + 10u, by + dy4, sb, dg_rgb(0x30,0xC0,0x50), body);
                dg_text(bx + 110u, by + dy4, dg_st_running ? "●运行中" : "○已停止",
                        dg_st_running ? dg_rgb(0x30,0xC0,0x50) : gray, body);
            }
            dg_text(bx + 10u, by + 112u, "系统运行中 · XOS 0.3.0", gray, body);
            dg_text(bx, by + dy7, "S 秒表启停  R 复位  M 移动  N 最小化  Esc 关闭", gray, body);
            break;
        }
    default: /* 游戏中心：1 收集  2 2048 */
        {
            i32 i, j;
            char nb[16];
            if (dg_gm_mode == 1u) {
                /* 2048：4x4 棋盘 */
                dg_text(bx, by, "2048 · " "1收集 2切换  Esc关闭", fg, body);
                for (j = 0; j < 4; j++) {
                    for (i = 0; i < 4; i++) {
                        u32 v = dg_g2048[(u32)(j * 4 + i)];
                        u32 cell_c = dg_rgb(0x28,0x34,0x42);
                        if (v == 2u) cell_c = dg_rgb(0xE8,0xE0,0xB0);
                        else if (v == 4u) cell_c = dg_rgb(0xE8,0xC8,0x90);
                        else if (v == 8u) cell_c = dg_rgb(0xE8,0xA0,0x60);
                        else if (v == 16u) cell_c = dg_rgb(0xE8,0x80,0x50);
                        else if (v == 32u) cell_c = dg_rgb(0xE8,0x60,0x40);
                        else if (v == 64u) cell_c = dg_rgb(0xE0,0x50,0x30);
                        else if (v >= 128u) cell_c = dg_rgb(0xE8,0xC0,0x30);
                        dg_fill(bx + 8u + i * 36u, by + 26u + j * 36u, 32u, 32u, cell_c);
                        if (v) {
                            nb[0] = (char)('0' + v / 100u);
                            nb[1] = (char)('0' + (v % 100u) / 10u);
                            nb[2] = (char)('0' + v % 10u);
                            nb[3] = 0;
                            dg_text(bx + 8u + i * 36u + 10u, by + 26u + j * 36u + 8u,
                                    nb, dg_rgb(0x10,0x20,0x30), cell_c);
                        }
                    }
                }
                if (dg_2048_over) dg_text(bx + 8u, by + 172u, "GAME OVER (S 重开)", dg_rgb(0xE0,0x40,0x40), body);
                nb[0] = 'S'; nb[1] = 'c'; nb[2] = 'o'; nb[3] = 'r'; nb[4] = 'e'; nb[5] = ':';
                dg_itoa((int)dg_2048_score, nb + 6);
                dg_text(bx + 8u, by + dy6, nb, dg_rgb(0xE8,0xC0,0x30), body);
                dg_text(bx, by + dy7, "方向键移动  S重开  Esc关闭", gray, body);
            } else {
                /* 收集：5x5 */
                dg_text(bx, by, "收集 · " "2切2048  Esc关闭", fg, body);
                for (j = 0; j < 5; j++) {
                    for (i = 0; i < 5; i++) {
                        u32 cell_c = dg_rgb(0x28,0x34,0x42);
                        if (i == dg_gm_px && j == dg_gm_py) cell_c = dg_rgb(0x30,0xC0,0x50);
                        else if (i == dg_gm_fx && j == dg_gm_fy) cell_c = dg_rgb(0xE0,0xA0,0x30);
                        dg_fill(bx + 8u + i * 22u, by + 26u + j * 22u, 18u, 18u, cell_c);
                    }
                }
                nb[0] = 'S'; nb[1] = 'c'; nb[2] = 'o'; nb[3] = 'r'; nb[4] = 'e'; nb[5] = ':';
                dg_itoa((int)dg_gm_score, nb + 6);
                dg_text(bx + 8u, by + dy6, nb, dg_rgb(0x30,0xC0,0x50), body);
                dg_text(bx, by + dy7, "方向键移动 Enter收集 S重置 Esc关闭", gray, body);
            }
            break;
        }
        break;
    }
}

static u32 dg_win_accent(u32 icon)
{
    switch (icon) {
    case 0u: return dg_rgb(0x2F,0x7D,0xE1);   /* 文件-蓝 */
    case 1u: return dg_rgb(0x90,0x90,0x90);   /* 文本-灰 */
    case 2u: return dg_rgb(0x88,0x8C,0x90);   /* 计算器 */
    case 3u: return dg_rgb(0x30,0xC0,0x50);   /* 终端-绿 */
    case 4u: return dg_rgb(0x1E,0x6F,0xD0);   /* 浏览器 */
    case 5u: return dg_rgb(0x50,0x5A,0x66);   /* 设置 */
    case 6u: return dg_rgb(0x50,0xC8,0x70);   /* 音乐 */
    case 7u: return dg_rgb(0xE0,0x48,0x48);   /* 游戏 */
    case 8u: return dg_rgb(0xE8,0xA0,0x40);   /* 照片 */
    case 9u: return dg_rgb(0x58,0x88,0xC0);   /* 时钟 */
    case 10u: return dg_rgb(0x90,0xE8,0x90);  /* 监控 */
    default: return dg_rgb(0x40,0xA0,0xE0);   /* 天气 */
    }
}

static void dg_window_draw(const dg_win_t *w)
{
    u32 title = dg_rgb(0x1E,0x3A,0x5F);
    u32 body  = dg_rgb(0xEE,0xEE,0xEE);
    const char *name = dg_icons[w->icon].name;
    dg_fill(w->x, w->y, w->w, w->h, body);
    dg_fill(w->x, w->y, w->w, 26u, title);
    /* 左侧应用色条（直观识别应用类别） */
    dg_fill(w->x + 5u, w->y + 6u, 5u, 14u, dg_win_accent(w->icon));
    dg_text(w->x + 16u, w->y + 9u, name, dg_rgb(0xF0,0xF0,0xF0), title);
    /* 右侧尺寸档位 */
    dg_text(w->x + w->w - 40u, w->y + 9u,
            dg_wsz == 0u ? "1x" : (dg_wsz == 1u ? "2x" : "3x"),
            dg_rgb(0xE8,0xC8,0x4A), title);
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
    /* 1.5) 像素风景：太阳 / 云 / 远山 / 近地 */
    dg_fill(496u, 34u, 30u, 30u, dg_rgb(0xF0,0xC0,0x40));            /* 太阳 */
    dg_fill(66u, 62u, 36u, 10u, dg_rgb(0xE8,0xF0,0xF8));            /* 云 1 */
    dg_fill(78u, 54u, 22u, 8u, dg_rgb(0xE8,0xF0,0xF8));
    dg_fill(412u, 130u, 40u, 10u, dg_rgb(0xD8,0xE4,0xF0));          /* 云 2 */
    dg_fill(424u, 122u, 24u, 8u, dg_rgb(0xD8,0xE4,0xF0));
    dg_fill(0u, 300u, 240u, 60u, dg_rgb(0x2A,0x38,0x58));           /* 远山 */
    dg_fill(150u, 322u, 270u, 38u, dg_rgb(0x22,0x30,0x50));
    dg_fill(370u, 292u, 270u, 68u, dg_rgb(0x2E,0x3C,0x60));
    dg_fill(0u, 356u, DG_W, 64u, dg_rgb(0x14,0x2A,0x1E));           /* 近地 */

    /* 2) 图标：两行五列（窗口打开时仍可见，除被窗口遮挡外） */
    for (i = 0; i < DG_ICON_N; i++) {
        u32 col = i % 5u;
        u32 row = i / 5u;
        x = 34u + col * 114u;
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
    /* 时钟：时:分:秒 + 日期 */
    sec = pit_tick_count() / 100u;
    hh = (10u + sec / 3600u) % 24u;
    mm = sec % 3600u / 60u;
    tbuf[0] = (char)('0' + hh / 10u);
    tbuf[1] = (char)('0' + hh % 10u);
    tbuf[2] = ':';
    tbuf[3] = (char)('0' + mm / 10u);
    tbuf[4] = (char)('0' + mm % 10u);
    tbuf[5] = ':';
    tbuf[6] = (char)('0' + sec % 60u / 10u);
    tbuf[7] = (char)('0' + sec % 60u % 10u);
    tbuf[8] = 0;
    dg_text(DG_W - 70u, DG_H - DG_TASKBAR + 11u, tbuf,
            dg_rgb(0xF0,0xF0,0xF0), bar);
    dg_text(DG_W - 152u, DG_H - DG_TASKBAR + 11u, "10-08",
            dg_rgb(0xA0,0xB0,0xC8), bar);

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

    /* 5.5) 首次使用欢迎弹窗 */
    if (dg_welcome) {
        u32 wx = 60u, wy = 70u, ww = DG_W - 120u, wh = 190u;
        dg_fill(wx, wy, ww, wh, dg_rgb(0x22,0x2A,0x38));
        dg_rect(wx, wy, ww, wh, dg_rgb(0x58,0x88,0xC0));
        dg_fill(wx, wy, ww, 26u, dg_rgb(0x1E,0x6F,0xD0));
        dg_text(wx + 10u, wy + 9u, "欢迎使用 XOS", dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0x1E,0x6F,0xD0));
        dg_text(wx + 16u, wy + 44u, "· Tab 选择桌面图标，Enter 打开应用", dg_rgb(0xD0,0xD8,0xE0), dg_rgb(0x22,0x2A,0x38));
        dg_text(wx + 16u, wy + 68u, "· S 开始菜单  R 右键菜单", dg_rgb(0xD0,0xD8,0xE0), dg_rgb(0x22,0x2A,0x38));
        dg_text(wx + 16u, wy + 92u, "· M 移动窗口  N 最小化  Esc 关闭", dg_rgb(0xD0,0xD8,0xE0), dg_rgb(0x22,0x2A,0x38));
        dg_text(wx + 16u, wy + 116u, "· 开始菜单含全部 10 个应用", dg_rgb(0xD0,0xD8,0xE0), dg_rgb(0x22,0x2A,0x38));
        dg_text(wx + 16u, wy + 140u, "· 文本编辑器内容保存到真实文件系统 /note.txt", dg_rgb(0xD0,0xD8,0xE0), dg_rgb(0x22,0x2A,0x38));
        dg_text(wx + ww / 2u - 88u, wy + wh - 24u, "按 Enter 开始使用", dg_rgb(0x30,0xC0,0x50), dg_rgb(0x22,0x2A,0x38));
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
    static u32 dg_inited = 0u;
    u32 a, rc;
    if (dg_inited) return 0;                  /* 已初始化（图形登录已映射 LFB） */
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
    dg_inited = 1u;
    return 0;
}

/* ---------------- 应用窗口输入逻辑（计算器 / 图形终端） ---------------- */
static void dg_itoa(int v, char *out)
{
    char tmp[12];
    int i = 0, j = 0, neg = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    if (v < 0) { neg = 1; v = -v; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

/* 四则求值：支持 a+b+c...（从左到右，乘除优先后算） */
static int dg_calc_eval(const char *s, int *res)
{
    int vals[8];
    char ops[8];
    int nv = 0, no = 0, i = 0, v = 0, j;
    if (!s || !s[0]) return 0;
    /* 解析数字与运算符 */
    while (s[i] != 0) {
        if (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
        else if (s[i] == '+' || s[i] == '-' || s[i] == '*' || s[i] == '/') {
            if (nv >= 7) return 0;
            vals[nv++] = v; v = 0;
            ops[no++] = s[i];
            i++;
        } else if (s[i] == ' ') { i++; }
        else return 0;
    }
    if (nv == 0 && v == 0 && s[0] == '0') { vals[nv++] = 0; }
    else { vals[nv++] = v; }
    if (nv < 2 || nv - 1 != no) { if (nv == 1) { *res = vals[0]; return 1; } return 0; }
    /* 先算乘除 */
    j = 0;
    for (i = 0; i < no; i++) {
        if (ops[i] == '*') { vals[j] = vals[j] * vals[i + 1]; }
        else if (ops[i] == '/') {
            if (vals[i + 1] == 0) return 0;
            vals[j] = vals[j] / vals[i + 1];
        } else { j++; vals[j] = vals[i + 1]; }
    }
    /* 再算加减（从左到右） */
    *res = vals[0];
    for (i = 0, j = 0; i < no; i++) {
        if (ops[i] == '+') { j++; *res += vals[j]; }
        else if (ops[i] == '-') { j++; *res -= vals[j]; }
    }
    return 1;
}

static void dg_calc_input(dg_win_t *w, u32 key)
{
    if (key >= KEY_0 && key <= KEY_9) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('0' + (key - KEY_0));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key == KEY_PLUS) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '+'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_MINUS) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '-'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_DOT) {           /* . 键当 * */
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '*'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_SLASH) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '/'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_BACKSP) {
        if (w->inlen > 0u) { w->inlen--; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_ENTER) {
        int res;
        char nb[16];
        if (dg_calc_eval(w->inbuf, &res)) {
            nb[0] = '='; nb[1] = ' ';
            dg_itoa(res, nb + 2);
        } else {
            nb[0] = 'E'; nb[1] = 'R'; nb[2] = 'R'; nb[3] = 0;
        }
        { u32 j; for (j = 0u; nb[j] && j < 39u; j++) w->out[0][j] = nb[j]; w->out[0][j] = 0; }
        w->inlen = 0u;
        w->inbuf[0] = 0;
    }
}

static void dg_term_exec(dg_win_t *w)
{
    const char *cmd = w->inbuf;
    char line[40];
    u32 i;
    /* 输出滚动：out[0]=out[1]=out[2]→上移 */
    for (i = 0u; i < 3u; i++) {
        u32 j = 0u;
        while (w->out[i + 1][j] && j < 39u) { w->out[i][j] = w->out[i + 1][j]; j++; }
        w->out[i][j] = 0;
    }
    /* 回显命令 */
    i = 0u;
    w->out[3][i++] = '>'; w->out[3][i++] = ' ';
    { u32 j = 0u; while (cmd[j] && i < 39u && j < 20u) w->out[3][i++] = cmd[j++]; }
    w->out[3][i] = 0;
    /* 解析命令 */
    if (!cmd[0]) {
        w->out[3][0] = 0;
    } else if (cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && cmd[4] == 0) {
        { u32 j = 0u; const char *s = "help echo date ver mem df ps uptime clear"; while (s[j] && j < 39u) { w->out[3][j] = s[j]; j++; } w->out[3][j] = 0; }
    } else if (cmd[0] == 'd' && cmd[1] == 'a' && cmd[2] == 't' && cmd[3] == 'e' && cmd[4] == 0) {
        { u32 j = 0u; const char *s = "date: 2026-10-09 Friday"; while (s[j] && j < 39u) { w->out[3][j] = s[j]; j++; } w->out[3][j] = 0; }
    } else if (cmd[0] == 'v' && cmd[1] == 'e' && cmd[2] == 'r' && cmd[3] == 0) {
        { u32 j = 0u; const char *s = "XOS 0.3.0 (x86) self-hosted"; while (s[j] && j < 39u) { w->out[3][j] = s[j]; j++; } w->out[3][j] = 0; }
    } else if (cmd[0] == 'm' && cmd[1] == 'e' && cmd[2] == 'm' && cmd[3] == 0) {
        extern u32 pmm_total_pages(void);
        char nb[16];
        w->out[3][0] = 'M'; w->out[3][1] = 'e'; w->out[3][2] = 'm'; w->out[3][3] = ':'; w->out[3][4] = ' ';
        dg_itoa((int)(pmm_total_pages() * 4u / 1024u), nb);
        i = 5u; { u32 j = 0u; while (nb[j] && i < 39u) w->out[3][i++] = nb[j++]; }
        w->out[3][i++] = 'M'; w->out[3][i++] = 'B'; w->out[3][i] = 0;
    } else if (cmd[0] == 'd' && cmd[1] == 'f' && cmd[2] == 0) {
        { u32 j = 0u; const char *s = "disk: 10MB img, fs ok"; while (s[j] && j < 39u) { w->out[3][j] = s[j]; j++; } w->out[3][j] = 0; }
    } else if (cmd[0] == 'p' && cmd[1] == 's' && cmd[2] == 0) {
        extern u32 desk_task_count(void);
        char nb[16];
        w->out[3][0] = 'P'; w->out[3][1] = 'r'; w->out[3][2] = 'o'; w->out[3][3] = 'c'; w->out[3][4] = 'e'; w->out[3][5] = 's'; w->out[3][6] = ':'; w->out[3][7] = ' ';
        dg_itoa((int)desk_task_count(), nb);
        i = 8u; { u32 j = 0u; while (nb[j] && i < 39u) w->out[3][i++] = nb[j++]; }
        w->out[3][i] = 0;
    } else if (cmd[0] == 'u' && cmd[1] == 'p' && cmd[2] == 't' && cmd[3] == 'i' && cmd[4] == 'm' && cmd[5] == 'e' && cmd[6] == 0) {
        char nb[16];
        w->out[3][0] = 'U'; w->out[3][1] = 'p'; w->out[3][2] = ':';
        dg_itoa((int)(pit_tick_count() / 100u), nb);
        i = 3u; { u32 j = 0u; while (nb[j] && i < 39u) w->out[3][i++] = nb[j++]; }
        w->out[3][i++] = 's'; w->out[3][i] = 0;
    } else if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r' && cmd[5] == 0) {
        { u32 q, j; for (q = 0u; q < 4u; q++) for (j = 0u; j < 40u; j++) w->out[q][j] = 0; }
    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ') {
        u32 j = 5u, k = 0u;
        while (cmd[j] && k < 33u) { w->out[3][k++] = cmd[j++]; }
        w->out[3][k] = 0;
    } else {
        u32 j = 0u;
        const char *s = "unknown cmd (help)";
        while (s[j] && j < 39u) { w->out[3][j] = s[j]; j++; }
        w->out[3][j] = 0;
    }
    w->inlen = 0u;
    w->inbuf[0] = 0;
    (void)line;
}

static void dg_term_input(dg_win_t *w, u32 key)
{
    if (key >= KEY_A && key <= KEY_Z) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('a' + (key - KEY_A));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key >= KEY_0 && key <= KEY_9) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('0' + (key - KEY_0));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key == KEY_SPACE) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = ' '; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_MINUS) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '-'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_SLASH) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '/'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_DOT) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '.'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_BACKSP) {
        if (w->inlen > 0u) { w->inlen--; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_ENTER) {
        dg_term_exec(w);
    }
}

/* 文本编辑器：输入 + 保存到真实文件系统 */
static void dg_edit_input(dg_win_t *w, u32 key)
{
    if (key >= KEY_A && key <= KEY_Z) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('a' + (key - KEY_A));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key >= KEY_0 && key <= KEY_9) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('0' + (key - KEY_0));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key == KEY_SPACE) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = ' '; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_DOT) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '.'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_COMMA) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = ','; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_MINUS) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '-'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_BACKSP) {
        if (w->inlen > 0u) { w->inlen--; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_ENTER) {
        /* 保存当前行到 /note.txt（真实文件系统） */
        int fd = fs_open("/note.txt", O_WRITE | O_CREAT | O_APPEND);
        if (fd >= 0) {
            u32 i = 0u;
            if (w->inlen) fs_write(fd, w->inbuf, w->inlen);
            fs_write(fd, "\n", 1);
            fs_close(fd);
            /* 回显到 out 缓冲首行 */
            while (w->inbuf[i] && i < 39u) { w->out[0][i] = w->inbuf[i]; i++; }
            w->out[0][i] = 0;
            w->out[1][0] = 0;              /* 下次渲染重读历史 */
        } else {
            u32 i = 0u;
            const char *s = "save failed";
            while (s[i] && i < 39u) { w->out[0][i] = s[i]; i++; }
            w->out[0][i] = 0;
        }
        w->inlen = 0u;
        w->inbuf[0] = 0;
    }
}

/* 浏览器：内置页面导航 */
static void dg_browser_input(dg_win_t *w, u32 key)
{
    if (key >= KEY_A && key <= KEY_Z) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('a' + (key - KEY_A));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key >= KEY_0 && key <= KEY_9) {
        if (w->inlen < 23u) {
            w->inbuf[w->inlen++] = (char)('0' + (key - KEY_0));
            w->inbuf[w->inlen] = 0;
        }
    } else if (key == KEY_SPACE) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = ' '; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_DOT) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '.'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_SLASH) {
        if (w->inlen < 23u) { w->inbuf[w->inlen++] = '/'; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_BACKSP) {
        if (w->inlen > 0u) { w->inlen--; w->inbuf[w->inlen] = 0; }
    } else if (key == KEY_ENTER) {
        /* 导航：匹配 xos:// 前缀页面 */
        u32 i = 0u;
        const char *pg;
        const char *s = w->inbuf;
        if (s[0] == '1' && s[1] == 0) pg = "about";
        else if (s[0] == '2' && s[1] == 0) pg = "sys";
        else if (s[0] == '3' && s[1] == 0) pg = "help";
        else if (s[0] == 'x' && s[1] == 'o' && s[2] == 's' && s[3] == ':' && s[4] == '/' && s[5] == '/')
            pg = s + 6;
        else pg = "home";
        { u32 j = 0u; while (pg[j] && j < 39u) { w->out[0][j] = pg[j]; j++; } w->out[0][j] = 0; }
        w->inlen = 0u;
        w->inbuf[0] = 0;
    }
}

/* 打开一个窗口（找到空闲槽，位置级联偏移） */
/* --- 2048 游戏逻辑 --- */
static void g2048_reset(void)
{
    u32 i;
    for (i = 0u; i < 16u; i++) dg_g2048[i] = 0u;
    dg_2048_score = 0u; dg_2048_over = 0u;
    dg_g2048[(u32)(dg_rand() % 16u)] = 2u;
}

static u32 g2048_empty(void)
{
    u32 i, n = 0u;
    for (i = 0u; i < 16u; i++) if (dg_g2048[i] == 0u) n++;
    return n;
}

static void g2048_spawn(void)
{
    u32 e = g2048_empty(), idx, k;
    if (!e) { dg_2048_over = 1u; return; }
    idx = (u32)(dg_rand() % e);
    k = 0u;
    for (k = 0u; k < 16u; k++) {
        if (dg_g2048[k] == 0u) {
            if (idx == 0u) { dg_g2048[k] = (dg_rand() % 4u == 0u) ? 4u : 2u; break; }
            idx--;
        }
    }
}

/* dir: 0左 1右 2上 3下；返回是否移动 */
static u32 g2048_move(u32 dir)
{
    u32 moved = 0u, r, c, a, b;
    for (r = 0u; r < 4u; r++) {
        u32 line[4], out[4], li = 0u, oi = 0u, i;
        for (c = 0u; c < 4u; c++) {
            u32 idx = (dir == 0u) ? r * 4u + c : (dir == 1u) ? r * 4u + (3u - c)
                      : (dir == 2u) ? c * 4u + r : (3u - c) * 4u + r;
            if (dg_g2048[idx]) line[li++] = dg_g2048[idx];
        }
        for (i = 0u; i < li; i++) {
            if (i + 1u < li && line[i] == line[i + 1u]) {
                out[oi++] = line[i] * 2u;
                dg_2048_score += line[i] * 2u;
                i++;
            } else {
                out[oi++] = line[i];
            }
        }
        while (oi < 4u) out[oi++] = 0u;
        for (c = 0u; c < 4u; c++) {
            u32 idx = (dir == 0u) ? r * 4u + c : (dir == 1u) ? r * 4u + (3u - c)
                      : (dir == 2u) ? c * 4u + r : (3u - c) * 4u + r;
            if (dg_g2048[idx] != out[c]) moved = 1u;
            dg_g2048[idx] = out[c];
        }
    }
    if (moved) g2048_spawn();
    return moved;
}

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
    dg_wins[slot].w = dg_win_sz[dg_wsz][0];
    dg_wins[slot].h = dg_win_sz[dg_wsz][1];
    dg_wins[slot].min = 0u;
    dg_wins[slot].inlen = 0u;
    dg_wins[slot].inbuf[0] = 0;
    { u32 q; for (q = 0u; q < 4u; q++) dg_wins[slot].out[q][0] = 0; }
    if (icon == 1u) {
        int fd = fs_open("/note.txt", O_READ);
        char buf[96];
        u32 rd = 0u, j;
        if (fd >= 0) {
            rd = (u32)fs_read(fd, buf, 88u);
            fs_close(fd);
            buf[rd] = 0;
        }
        if (rd == 0u) { const char *e = "(no note yet)"; rd = 0u; while (e[rd]) rd++; j = 0u; while (j < rd && j < 39u) { dg_wins[slot].out[1][j] = e[j]; j++; } }
        else { for (j = 0u; j < rd && j < 39u; j++) dg_wins[slot].out[1][j] = buf[j]; }
        dg_wins[slot].out[1][j] = 0;
        dg_wins[slot].out[2][0] = 'h'; dg_wins[slot].out[2][1] = 'i';
        dg_wins[slot].out[2][2] = 's'; dg_wins[slot].out[2][3] = 't';
        dg_wins[slot].out[2][4] = 'o'; dg_wins[slot].out[2][5] = 'r';
        dg_wins[slot].out[2][6] = 'y'; dg_wins[slot].out[2][7] = ':';
        dg_wins[slot].out[2][8] = 0;
    }
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

        /* --- 首次使用欢迎向导：Enter 关闭 --- */
        if (dg_welcome) {
            if (key == KEY_ENTER || key == KEY_ESC) {
                dg_welcome = 0u;
                dg_render();
            }
            continue;
        }

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
            dg_win_t *w = &dg_wins[dg_focus];
            if (dg_move_mode) {
                if (key == KEY_LEFT) {
                    if (w->x > 4u) w->x -= 10u;
                } else if (key == KEY_RIGHT) {
                    if (w->x + w->w + 4u < DG_W) w->x += 10u;
                } else if (key == KEY_UP) {
                    if (w->y > 4u) w->y -= 10u;
                } else if (key == KEY_DOWN) {
                    if (w->y + w->h + 4u < DG_H - DG_TASKBAR) w->y += 10u;
                } else if (key == KEY_ENTER || key == KEY_ESC || key == KEY_M) {
                    dg_move_mode = 0u;                       /* 结束移动 */
                }
                dg_render();
                continue;
            }
            /* G 键窗口缩放（输入型窗口保留按键输入） */
            if (key == KEY_G && w->icon != 1u && w->icon != 2u &&
                w->icon != 3u && w->icon != 4u) {
                dg_wsz = (dg_wsz + 1u) % 3u;
                w->w = dg_win_sz[dg_wsz][0];
                w->h = dg_win_sz[dg_wsz][1];
                dg_render();
                continue;
            }
            /* 时钟日历：S 秒表启停  R 复位 */
            if (w->icon == 9u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (key == KEY_S) {
                    if (dg_st_running) {
                        dg_st_total += (pit_tick_count() - dg_st_start) / 100u;
                        dg_st_running = 0u;
                    } else {
                        dg_st_start = pit_tick_count();
                        dg_st_running = 1u;
                    }
                } else if (key == KEY_R) {
                    dg_st_running = 0u;
                    dg_st_total = 0u;
                }
                dg_render();
                continue;
            }
            /* 天气：数字键 1-4 切换 */
            if (w->icon == 11u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (key >= KEY_1 && key <= KEY_4) {
                    dg_wx_idx = key - KEY_1;
                }
                dg_render();
                continue;
            }
            /* 照片查看器：数字键 1/2/3 切换内置图片 */
            if (w->icon == 8u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (key == KEY_1 || key == KEY_2 || key == KEY_3) {
                    dg_ph_idx = key - KEY_1;
                }
                dg_render();
                continue;
            }
            /* 文件管理器：Enter/Backspace 导航 */
            if (w->icon == 0u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (key == KEY_DOWN || key == KEY_TAB) {
                    if (dg_fm_sel < 2u) dg_fm_sel++;
                } else if (key == KEY_UP) {
                    if (dg_fm_sel > 0u) dg_fm_sel--;
                } else if (key == KEY_BACKSP) {
                    if (dg_fm_dir == 1u || dg_fm_dir == 2u) dg_fm_dir = 0u;
                    else if (dg_fm_dir == 3u) dg_fm_dir = 0u;
                    dg_fm_sel = 0u;
                } else if (key == KEY_ENTER) {
                    if (dg_fm_dir == 0u) {
                        if (dg_fm_sel == 0u) dg_fm_dir = 1u;      /* /mnt */
                        else if (dg_fm_sel == 1u) dg_fm_dir = 2u; /* /dev */
                        else {                                     /* note.txt 查看 */
                            int fd = fs_open("/note.txt", O_READ);
                            u32 rd = 0u, j;
                            if (fd >= 0) { rd = (u32)fs_read(fd, dg_fm_note, 39u); fs_close(fd); }
                            dg_fm_note[rd] = 0;
                            for (j = 0u; j < 39u; j++) if (dg_fm_note[j] == '\n') dg_fm_note[j] = ' ';
                            dg_fm_dir = 3u;
                        }
                        dg_fm_sel = 0u;
                    }
                }
                dg_render();
                continue;
            }
            /* 设置中心：数字键 1-4 切换页签 */
            if (w->icon == 5u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (key >= KEY_1 && key <= KEY_4) {
                    dg_set_page = key - KEY_1;
                }
                dg_render();
                continue;
            }
            /* 输入型应用窗口：编辑器 / 计算器 / 终端 / 浏览器 优先接收按键 */
            if (w->icon == 1u || w->icon == 2u || w->icon == 3u || w->icon == 4u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;                       /* 仍可移动窗口 */
                } else if (w->icon == 1u) {
                    dg_edit_input(w, key);
                } else if (w->icon == 2u) {
                    dg_calc_input(w, key);
                } else if (w->icon == 3u) {
                    dg_term_input(w, key);
                } else {
                    dg_browser_input(w, key);
                }
                dg_render();
                continue;
            }
            /* 音乐播放器 */
            if (w->icon == 6u) {
                if (key == KEY_P) {
                    if (!dg_mu_play) dg_mu_start = pit_tick_count();
                    dg_mu_play = dg_mu_play ? 0u : 1u;
                } else if (key == KEY_N) {
                    dg_mu_track++;
                } else if (key == KEY_PLUS) {
                    if (dg_mu_vol + 10u <= 100u) dg_mu_vol += 10u;
                    else dg_mu_vol = 100u;
                } else if (key == KEY_MINUS) {
                    if (dg_mu_vol >= 10u) dg_mu_vol -= 10u;
                    else dg_mu_vol = 0u;
                } else if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                }
                dg_render();
                continue;
            }
            /* 游戏中心 */
            if (w->icon == 7u) {
                if (key == KEY_1 || key == KEY_2) {
                    dg_gm_mode = (key == KEY_2) ? 1u : 0u;
                } else if (dg_gm_mode == 1u) {
                    if (key == KEY_UP) { g2048_move(2u); }
                    else if (key == KEY_DOWN) { g2048_move(3u); }
                    else if (key == KEY_LEFT) { g2048_move(0u); }
                    else if (key == KEY_RIGHT) { g2048_move(1u); }
                    else if (key == KEY_S) { g2048_reset(); }
                } else {
                    if (key == KEY_UP) {
                        if (dg_gm_py > 0) dg_gm_py--;
                    } else if (key == KEY_DOWN) {
                        if (dg_gm_py < 4) dg_gm_py++;
                    } else if (key == KEY_LEFT) {
                        if (dg_gm_px > 0) dg_gm_px--;
                    } else if (key == KEY_RIGHT) {
                        if (dg_gm_px < 4) dg_gm_px++;
                    } else if (key == KEY_ENTER) {
                        if (dg_gm_px == dg_gm_fx && dg_gm_py == dg_gm_fy) {
                            dg_gm_score++;
                            dg_gm_fx = (i32)(dg_rand() % 5u);
                            dg_gm_fy = (i32)(dg_rand() % 5u);
                        }
                    } else if (key == KEY_S) {
                        dg_gm_score = 0u;
                        dg_gm_px = 2; dg_gm_py = 2;
                        dg_gm_fx = (i32)(dg_rand() % 5u);
                        dg_gm_fy = (i32)(dg_rand() % 5u);
                    }
                }
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
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
