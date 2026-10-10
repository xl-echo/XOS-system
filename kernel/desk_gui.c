/* ============================================================================
 * XOS 图形桌面（真机 GUI 层）
 * 完全自研：Bochs VBE 线性帧缓冲 + 桌面渲染 + 键盘事件循环。
 *  - desk_gui_init(): 切换 VBE 图形模式(默认 1024x768x32)、映射 LFB(0xE0000000)、
 *                     分辨率随显示模式自适应(DG_W/DG_H 运行时变量)，失败自动回退
 *  - desk_gui_run():  渲染渐变壁纸 / 桌面图标(英文名，普通用户可直接识别) /
 *                     任务栏 / 真实时钟；
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
#include "sound.h"
#include "rtc.h"
#include "sysconf.h"
#include "xos_wallpaper.h"

extern void *memset(void *dst, int c, unsigned int n);

/* ---------------- 桌面分辨率（随显示模式自适应） ---------------- */
u32 g_dgw = 640u, g_dgh = 480u;
u32 g_font_scale = 1u;      /* 全局字体缩放：1=8px 2=16px 3=24px */
u32 g_tb_h = 30u;           /* 任务栏高度：随字体缩放自适应（1x=30 2x=38 3x=46） */

/* ---------------- 主题系统（深色 0 / 浅色 1，F6 切换） ---------------- */
u32 g_theme = 0u;           /* 0=深色（默认） 1=浅色 */

/* 主题取色：按主题返回界面关键色（普通用户可直接识别两套风格） */
static u32 dg_theme_c(u32 idx)
{
    if (g_theme == 1u) {
        switch (idx) {
        case 0u: return dg_rgb(0xD8,0xDC,0xE2);   /* 任务栏 */
        case 1u: return dg_rgb(0x18,0x18,0x18);   /* 主文本 */
        case 2u: return dg_rgb(0x2F,0x6F,0xB0);   /* 窗口标题栏 */
        case 3u: return dg_rgb(0xF8,0xF8,0xF8);   /* 窗口体 */
        case 4u: return dg_rgb(0x40,0x50,0x60);   /* 窗口边框 */
        case 5u: return dg_rgb(0x1E,0x66,0xD0);   /* 开始按钮 */
        case 6u: return dg_rgb(0x5A,0x7A,0x9A);   /* 时钟小组件底 */
        case 7u: return dg_rgb(0x90,0xA0,0xB0);   /* 次级文本 */
        case 8u: return dg_rgb(0xE8,0xEC,0xF0);   /* 菜单/列表底色 */
        }
        return dg_rgb(0x18,0x18,0x18);
    }
    switch (idx) {
    case 0u: return dg_rgb(0x18,0x1C,0x28);   /* 任务栏 */
    case 1u: return dg_rgb(0xE8,0xE8,0xE8);   /* 主文本 */
    case 2u: return dg_rgb(0x1E,0x3A,0x5F);   /* 窗口标题栏 */
    case 3u: return dg_rgb(0xEE,0xEE,0xEE);   /* 窗口体 */
    case 4u: return dg_rgb(0x0A,0x14,0x22);   /* 窗口边框 */
    case 5u: return dg_rgb(0x2F,0x7D,0xE1);   /* 开始按钮 */
    case 6u: return dg_rgb(0x1E,0x3A,0x5F);   /* 时钟小组件底 */
    case 7u: return dg_rgb(0x60,0x70,0x88);   /* 次级文本 */
    case 8u: return dg_rgb(0x18,0x20,0x2E);   /* 菜单/列表底色 */
    }
    return dg_rgb(0xE8,0xE8,0xE8);
}

static u32 dg_tb_txt_y(void)  /* 任务栏文字垂直居中偏移 */
{
    return (g_tb_h - 8u * g_font_scale) / 2u;
}

/* 字体缩放自动档位：随分辨率自适应（1280+ 放大 2x，1920+ 放大 3x） */
void dg_font_scale_auto(void)
{
    if (DG_W >= 1920u) g_font_scale = 3u;
    else if (DG_W >= 1280u) g_font_scale = 2u;
    else g_font_scale = 1u;
    g_tb_h = 22u + 8u * g_font_scale;   /* 任务栏高度随字体缩放 */
}

/* ---------------- 32bpp 帧缓冲访问 ---------------- */
volatile u32 *dg_fb = (volatile u32 *)DG_LFB;
static u32 dg_sel = 0u;
static void dg_itoa(int v, char *out);

/* 系统监视接口（pmm/task，真实数据） */
extern u32 pmm_total_pages(void);
extern u32 pmm_used_pages(void);
#include "task.h"

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
static const u16 dg_win_sz[3][2] = { {400u,260u}, {560u,360u}, {760u,480u} };
static u32 dg_alm_set   = 0u;         /* 闹钟：0无 1设置中 2已设定 */
static u32 dg_alm_min   = 0u;         /* 设置中的分钟数 0-99 */
static u32 dg_alm_target = 0u;        /* 响铃目标 tick */
static u32 dg_alm_ring  = 0u;         /* 响铃中 */
static u32 dg_tz        = 0u;         /* 时区索引：0北京 1伦敦 2纽约 3东京 4悉尼 */
static const i32  dg_tz_off[5] = { 0, -8, -12, 1, 2 };   /* 相对北京：伦敦-8 纽约-12 东京+1 悉尼+2 */
static const char *dg_tz_name[5] = { "Beijing", "London", "NewYork", "Tokyo", "Sydney" };
static u32 dg_last_repaint = 0u;      /* 桌面小组件刷新节流 */
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

static void dg_char(u32 x, u32 y, u8 ch, u32 fg, u32 bg, u32 scale)
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
            if (scale <= 1u) {
                dgpix(x + i, y + j, on ? fg : bg);
            } else {
                u32 px, py;
                for (px = 0u; px < scale; px++)
                    for (py = 0u; py < scale; py++)
                        dgpix(x + i * scale + px, y + j * scale + py, on ? fg : bg);
            }
        }
}

void dg_text(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    if (!s) return;
    while (*s) {
        dg_char(x, y, (u8)*s, fg, bg, g_font_scale);
        x += 8u * g_font_scale;
        s++;
    }
}

static u32 dg_text_w(const char *s)
{
    u32 w = 0u;
    if (!s) return 0u;
    while (*s) { w += 8u * g_font_scale; s++; }
    return w;
}

/* ---------------- 桌面图标 ---------------- */
typedef struct {
    const char *name;
    u32  icon;                 /* 图案索引 */
} dg_icon_t;

static const dg_icon_t dg_icons[DG_ICON_N] = {
    { "Files", 0u }, { "Note", 1u }, { "Calc", 2u }, { "Term", 3u },
    { "Web", 4u }, { "Settings", 5u }, { "Music", 6u }, { "Games", 7u },
    { "Photos", 8u }, { "Clock", 9u }, { "Monit", 10u }, { "Weather", 11u },
    { "Tasks", 12u }, { "Pkgs", 13u }, { "Pics", 14u }, { "Shot", 15u }, { "Video", 16u },
    { "PDF", 17u }, { "Code", 18u }, { "Search", 19u }, { "Help", 20u }, { "Boot", 21u },
    { "Text", 22u }, { "Mail", 23u },
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
    case 12u: c1 = dg_rgb(0x1E,0x1E,0x28); c2 = dg_rgb(0x50,0xE0,0x90); c3 = dg_rgb(0x20,0x50,0x30); break; /* 任务管理器 */
    case 13u: c1 = dg_rgb(0x3A,0x2E,0x1E); c2 = dg_rgb(0xE8,0xC8,0x60); c3 = dg_rgb(0x60,0x48,0x18); break; /* 包管理器 */
    case 14u: c1 = dg_rgb(0x2A,0x2A,0x3A); c2 = dg_rgb(0xF0,0x88,0x50); c3 = dg_rgb(0x80,0x40,0x20); break; /* 图片查看器 */
    case 15u: c1 = dg_rgb(0x28,0x30,0x3C); c2 = dg_rgb(0xE0,0xE0,0xE0); c3 = dg_rgb(0x50,0x68,0x80); break; /* 截图工具 */
    case 16u: c1 = dg_rgb(0x20,0x18,0x30); c2 = dg_rgb(0xE0,0x50,0x50); c3 = dg_rgb(0x40,0x30,0x60); break; /* 视频播放器 */
    case 17u: c1 = dg_rgb(0x3A,0x1E,0x1E); c2 = dg_rgb(0xE0,0x90,0x90); c3 = dg_rgb(0x60,0x20,0x20); break; /* PDF阅读器 */
    case 18u: c1 = dg_rgb(0x14,0x1E,0x28); c2 = dg_rgb(0x60,0xC8,0xE0); c3 = dg_rgb(0x20,0x50,0x68); break; /* 代码编辑器 */
    case 19u: c1 = dg_rgb(0x1E,0x28,0x1E); c2 = dg_rgb(0x90,0xE0,0x90); c3 = dg_rgb(0x30,0x50,0x30); break; /* 全局搜索 */
    case 20u: c1 = dg_rgb(0x28,0x24,0x1E); c2 = dg_rgb(0xE8,0xD0,0x80); c3 = dg_rgb(0x60,0x50,0x30); break; /* 帮助文档 */
    case 21u: c1 = dg_rgb(0x1E,0x28,0x34); c2 = dg_rgb(0x60,0xA0,0xE0); c3 = dg_rgb(0x20,0x40,0x60); break; /* 开机自启 */
    case 22u: c1 = dg_rgb(0x28,0x1E,0x30); c2 = dg_rgb(0xE0,0xA0,0xE0); c3 = dg_rgb(0x60,0x30,0x60); break; /* 文字处理 */
    case 23u: c1 = dg_rgb(0x1E,0x2E,0x28); c2 = dg_rgb(0x80,0xD0,0xA0); c3 = dg_rgb(0x28,0x50,0x40); break; /* 邮件客户端 */
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
        dg_char(x + 8u, y + 12u, '>', c2, c1, g_font_scale);
        dg_char(x + 16u, y + 12u, '_', c2, c1, g_font_scale);
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
        dg_char(x + 10u, y + 12u, '1', c3, c2, g_font_scale);
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
    case 12u:   /* 任务管理器：条状图 */
        dg_fill(x + 4u, y + 4u, 24u, 24u, c1);
        dg_fill(x + 8u, y + 22u, 5u, 4u, c2);
        dg_fill(x + 14u, y + 18u, 5u, 8u, c2);
        dg_fill(x + 20u, y + 12u, 5u, 14u, c2);
        break;
    case 13u:   /* 包管理器：盒子 + 标签 */
        dg_fill(x + 5u, y + 12u, 22u, 14u, c2);
        dg_fill(x + 8u, y + 6u, 16u, 8u, c1);
        dg_fill(x + 10u, y + 8u, 12u, 4u, c3);
        break;
    case 14u:   /* 图片查看器：画框 + 山 */
        dg_fill(x + 5u, y + 5u, 22u, 22u, c1);
        dg_fill(x + 8u, y + 20u, 6u, 6u, c2);
        dg_fill(x + 14u, y + 16u, 6u, 10u, c3);
        dg_fill(x + 20u, y + 20u, 4u, 6u, c2);
        break;
    case 15u:   /* 截图工具：相机 */
        dg_fill(x + 5u, y + 10u, 22u, 16u, c2);
        dg_fill(x + 10u, y + 6u, 12u, 6u, c1);
        dg_fill(x + 10u, y + 12u, 12u, 10u, c3);
        dg_fill(x + 12u, y + 14u, 8u, 6u, c1);
        break;
    case 16u:   /* 视频播放器：播放三角 */
        dg_fill(x + 5u, y + 5u, 22u, 22u, c1);
        dg_fill(x + 12u, y + 10u, 8u, 12u, c2);
        dg_fill(x + 12u, y + 10u, 2u, 12u, c3);
        break;
    case 17u:   /* PDF阅读器：书页 */
        dg_fill(x + 6u, y + 4u, 20u, 24u, c2);
        dg_fill(x + 9u, y + 8u, 12u, 2u, c3);
        dg_fill(x + 9u, y + 12u, 12u, 2u, c3);
        dg_fill(x + 9u, y + 16u, 8u, 2u, c3);
        break;
    case 18u:   /* 代码编辑器：尖括号 */
        dg_fill(x + 5u, y + 5u, 22u, 22u, c1);
        dg_fill(x + 9u, y + 12u, 4u, 8u, c2);
        dg_fill(x + 15u, y + 12u, 4u, 8u, c2);
        dg_fill(x + 12u, y + 10u, 6u, 4u, c3);
        break;
    case 19u:   /* 全局搜索：放大镜 */
        dg_fill(x + 6u, y + 6u, 12u, 12u, c2);
        dg_fill(x + 8u, y + 8u, 8u, 8u, c3);
        dg_fill(x + 16u, y + 16u, 6u, 4u, c2);
        break;
    case 20u:   /* 帮助文档：问号 */
        dg_fill(x + 6u, y + 5u, 20u, 22u, c2);
        dg_fill(x + 11u, y + 9u, 10u, 8u, c1);
        dg_fill(x + 14u, y + 20u, 4u, 4u, c3);
        break;
    case 21u:   /* 开机自启：电源 */
        dg_fill(x + 14u, y + 4u, 4u, 14u, c2);
        dg_fill(x + 8u, y + 9u, 4u, 8u, c3);
        dg_fill(x + 20u, y + 9u, 4u, 8u, c3);
        dg_fill(x + 8u, y + 16u, 16u, 6u, c3);
        break;
    case 22u:   /* 文字处理：文档 + 铅笔 */
        dg_fill(x + 6u, y + 4u, 20u, 24u, c2);
        dg_fill(x + 9u, y + 8u, 12u, 2u, c1);
        dg_fill(x + 9u, y + 12u, 12u, 2u, c1);
        dg_fill(x + 9u, y + 16u, 12u, 2u, c1);
        dg_fill(x + 17u, y + 17u, 8u, 8u, c3);
        break;
    case 23u:   /* 邮件客户端：信封 */
        dg_fill(x + 4u, y + 8u, 24u, 16u, c2);
        dg_fill(x + 4u, y + 8u, 24u, 4u, c3);
        dg_fill(x + 4u, y + 8u, 12u, 8u, c1);
        dg_fill(x + 16u, y + 8u, 12u, 8u, c1);
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
static u32  dg_pkg_sel = 0u;            /* 包管理器选中项 */
static u32  dg_img_idx = 0u;            /* 图片查看器页 */
static u32  dg_cap_cnt = 0u;            /* 截图计数 */
static u32  dg_vid_play = 1u;           /* 视频播放状态 */
static u32  dg_vid_seek = 0u;           /* 视频帧偏移 */
static u32  dg_tm_refresh = 0u;         /* 任务管理器刷新标记 */
static u32  dg_pdf_page = 0u;           /* PDF 阅读页 */
static u32  dg_code_row = 0u;           /* 代码编辑器滚动行 */
static char dg_src_buf[24];             /* 全局搜索词 */
static u32  dg_src_len = 0u;
static u32  dg_src_sel = 0u;            /* 搜索结果选中 */
static u32  dg_help_page = 0u;          /* 帮助文档页 */
static u32  dg_auto_sel = 0u;           /* 自启选中项 */
static u32  dg_wp_row = 0u;             /* 文字处理滚动行 */
static u32  dg_mail_sel = 0u;           /* 邮件选中项 */

static const char *dg_apps[DG_ICON_N] = {
    "Files", "Note", "Calc", "Term",
    "Web", "Settings", "Music", "Games",
    "Photos", "Clock", "Monit", "Weather",
    "Tasks", "Pkgs", "Pics", "Shot", "Video",
    "PDF", "Code", "Search", "Help", "Boot",
    "Text", "Mail",
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
                dg_text(bx + 4u, by + 24u, "note.txt content:", dg_rgb(0x1E,0x6F,0xD0), body);
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
            dg_text(bx, by + dy6, "Enter open  Backspace back  Esc close  M move", gray, body);
        }
        break;
    case 1u: /* 文本编辑器：真实输入 + 保存到 /note.txt + 历史回读（打开时预读） */
        dg_text(bx, by, "note.txt - Text Editor", fg, body);
        dg_text(bx + 4u, by + dy, w->inlen ? w->inbuf : "(type text, Enter save)",
                dg_rgb(0xF0,0xF0,0xF0), body);
        dg_text(bx + 4u, by + dy2, w->out[0], gray, body);
        dg_text(bx + 4u, by + dy3, w->out[2], blue, body);
        dg_text(bx + 4u, by + dy4, w->out[1], fg, body);
        dg_text(bx, by + dy6, "alnum input  Enter save  Esc close  M move", gray, body);
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
            dg_text(bx, by + dy5, "digits+ops  Enter=  Backspace=del  Esc=close", gray, body);
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
            dg_text(bx, by + dy6, "cmd: help echo mem df ps uptime clear", gray, body);
            break;
        }
    case 4u: /* 浏览器：地址栏 + 内置页面 */
        {
            const char *pg = w->out[0][0] ? w->out[0] : "home";
            dg_text(bx, by, "addr: xos://", fg, body);
            dg_text(bx + 68u, by, w->inlen ? w->inbuf : "home", blue, body);
            if (pg[0] == 'a' && pg[1] == 'b' && pg[2] == 'o' && pg[3] == 'u' && pg[4] == 't') {
                dg_text(bx + 4u, by + 28u, "About XOS", fg, body);
                dg_text(bx + 4u, by + 50u, "XOS 0.3.0 self-built x86 desktop OS", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "kernel / fs / GUI self-implemented", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 94u, "no external commercial/closed components", dg_rgb(0xE0,0xE0,0xE0), body);
            } else if (pg[0] == 's' && pg[1] == 'y' && pg[2] == 's') {
                extern u32 pmm_total_pages(void);
                extern u32 desk_task_count(void);
                char nb[16];
                dg_text(bx + 4u, by + 28u, "System Status", fg, body);
                dg_text(bx + 4u, by + 50u, "Mem: 128 MB (4K pages)", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "Procs: ", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_itoa((int)desk_task_count(), nb);
                dg_text(bx + 72u, by + 72u, nb, dg_rgb(0x30,0xC0,0x50), body);
                dg_text(bx + 4u, by + 94u, "Display: 640x480x32 VBE fb", dg_rgb(0xE0,0xE0,0xE0), body);
            } else if (pg[0] == 'h' && pg[1] == 'e' && pg[2] == 'l' && pg[3] == 'p') {
                dg_text(bx + 4u, by + 28u, "Desktop Keys", fg, body);
                dg_text(bx + 4u, by + 50u, "M move  N minimize  1-4 restore", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "W/Tab switch  S Start  R menu", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 94u, "Esc close / exit desktop", dg_rgb(0xE0,0xE0,0xE0), body);
            } else {
                dg_text(bx + 4u, by + 28u, "XOS Web", fg, body);
                dg_text(bx + 4u, by + 50u, "pages: 1 About  2 System  3 Help", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 72u, "type xos://about + Enter", dg_rgb(0xE0,0xE0,0xE0), body);
                dg_text(bx + 4u, by + 94u, "(offline built-in pages)", gray, body);
            }
            dg_text(bx, by + dy6, "type addr  Enter go  Esc close  M move", gray, body);
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
                dg_text(bx, by, "Settings - Overview [1]", fg, body);
                dg_text(bx + 4u, by + 24u, "System: XOS 0.3.0", fg, body);
                dg_text(bx + 4u, by + 46u, "Arch: x86 (Intel 32-bit)", fg, body);
                dg_text(bx + 4u, by + 68u, "Mem:  ", fg, body);
                dg_u2s(pmem, nb);
                dg_text(bx + 64u, by + 68u, nb, blue, body);
                dg_text(bx + 92u, by + 68u, " MB", gray, body);
                dg_text(bx + 4u, by + 90u, "Procs:  ", fg, body);
                dg_u2s(pproc, nb);
                dg_text(bx + 64u, by + 90u, nb, blue, body);
                dg_text(bx + 4u, by + 112u, "Display: 640x480 32-bit", fg, body);
                dg_text(bx, by + 136u, "1-4 tabs  Esc close  M move", gray, body);
            } else if (dg_set_page == 1u) {
                dg_text(bx, by, "Settings - Drivers [2]", fg, body);
                dg_text(bx + 4u, by + 24u, "Keyboard: PS/2 loaded", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx + 4u, by + 46u, "Mouse: not connected", gray, body);
                dg_text(bx + 4u, by + 68u, "Display: Bochs VBE 640x480x32", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx + 4u, by + 90u, "Disk: ATA loaded (xos.img)", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx + 4u, by + 112u, "COM1 loaded (debug log)", dg_rgb(0x22,0x88,0x22), body);
                dg_text(bx, by + 136u, "1-4 tabs  Esc close  M move", gray, body);
            } else if (dg_set_page == 2u) {
                char nb[16];
                int fd = fs_open("/note.txt", O_READ);
                u32 sz = 0u;
                char t[8];
                if (fd >= 0) { sz = (u32)fs_read(fd, t, 7u); fs_close(fd); }
                dg_text(bx, by, "Settings - Storage [3]", fg, body);
                dg_text(bx + 4u, by + 24u, "Disk: 10 MB (xos.img)", fg, body);
                dg_text(bx + 4u, by + 46u, "Root: /mnt /dev /note.txt", fg, body);
                dg_text(bx + 4u, by + 68u, "note.txt size: ", fg, body);
                dg_u2s(sz, nb);
                dg_text(bx + 140u, by + 68u, nb, blue, body);
                dg_text(bx + 160u, by + 68u, " bytes", gray, body);
                dg_text(bx + 4u, by + 90u, "FS: XOS-FS v1 (self)", fg, body);
                dg_text(bx, by + 136u, "1-4 tabs  Esc close  M move", gray, body);
            } else {
                dg_text(bx, by, "Settings - About [4]", fg, body);
                dg_text(bx + 4u, by + 24u, "XOS OS 0.3.0", blue, body);
                dg_text(bx + 4u, by + 46u, "self-built x86 kernel + GUI", fg, body);
                dg_text(bx + 4u, by + 68u, "no external kernel/closed components", fg, body);
                dg_text(bx + 4u, by + 90u, "account: admin / admin123", fg, body);
                dg_text(bx + 4u, by + 112u, "support: terminal help cmd", fg, body);
                dg_text(bx, by + 136u, "1-4 tabs  Esc close  M move", gray, body);
            }
        }
        break;
    case 6u: /* 音乐播放器：真实时钟进度 */
        {
            u32 sec = 0u;
            const char *tracks[3] = { "XOS Theme", "Boot Concerto", "Desktop Waltz" };
            if (dg_mu_play) sec = (pit_tick_count() - dg_mu_start) / 100u;
            dg_text(bx, by, "Music Player", fg, body);
            dg_text(bx + 4u, by + dy, dg_mu_play ? "PLAYING" : "PAUSED", blue, body);
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
            dg_text(bx + 4u, by + 80u, "Vol: ", gray, body);
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
            dg_text(bx, by + dy5, "P play/pause  N next  +/- vol  M move  Esc close", gray, body);
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
            dg_text(bx, by, "Photos", fg, body);
            dg_text(bx + 120u, by, dg_ph_idx == 0u ? "1/3 Sunrise" :
                            (dg_ph_idx == 1u ? "2/3 Starry" : "3/3 Green"), blue, body);
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
            dg_text(bx, by + dy7, "1/2/3 switch  M move  N min  Esc close", gray, body);
            break;
        }
    case 11u: /* 天气 */
        {
            static const char *wxname[4] = { "Sunny", "Cloudy", "Rain", "Snow" };
            static const int   wxtemp[4] = { 26, 20, 15, -2 };
            char b1[12];
            u32 wc = dg_rgb(0x1E,0x6F,0xDC);
            dg_text(bx, by, "XOS Weather", fg, body);
            dg_text(bx + 4u, by + 24u, "City: Shanghai  Today: ", gray, body);
            dg_text(bx + 200u, by + 24u, wxname[dg_wx_idx], blue, body);
            dg_u2s((u32)(wxtemp[dg_wx_idx] > 0 ? wxtemp[dg_wx_idx] : -wxtemp[dg_wx_idx]), b1);
            dg_text(bx + 4u, by + 46u, "Temp: ", gray, body);
            dg_text(bx + 64u, by + 46u, b1, fg, body);
            dg_text(bx + 92u, by + 46u, " C", gray, body);
            dg_text(bx + 4u, by + 68u, "Feels: ", gray, body);
            dg_text(bx + 64u, by + 68u, "good for outdoor", wc, body);
            dg_text(bx + 4u, by + 90u, "1-4 switch type (1Sun 2Cloud 3Rain 4Snow)", gray, body);
            dg_text(bx, by + dy6, "1-4 switch  Esc close  M move", gray, body);
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
            dg_text(bx, by, "XOS Monitor", fg, body);
            kb = tpages * 4u;
            dg_u2s(kb, b1); dg_u2s(upages * 4u, b2); dg_u2s(fpages * 4u, b3);
            dg_text(bx + 4u, by + 24u, "Mem total: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_text(bx + 74u, by + 24u, b1, fg, body);
            dg_text(bx + 110u, by + 24u, " KB  used: ", gray, body);
            dg_text(bx + 190u, by + 24u, b2, fg, body);
            dg_text(bx + 226u, by + 24u, " KB  free: ", gray, body);
            dg_text(bx + 306u, by + 24u, b3, fg, body);
            dg_text(bx + 342u, by + 24u, " KB", gray, body);
            dg_u2s(tst.task_count, b1); dg_u2s(tst.ready_count, b2);
            dg_u2s(tst.zombie_count, b3); dg_u2s(tst.switch_total, b4);
            dg_text(bx + 4u, by + 48u, "Tasks: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_text(bx + 74u, by + 48u, b1, fg, body);
            dg_text(bx + 100u, by + 48u, "  ready: ", gray, body);
            dg_text(bx + 160u, by + 48u, b2, fg, body);
            dg_text(bx + 186u, by + 48u, "  zombie: ", gray, body);
            dg_text(bx + 246u, by + 48u, b3, fg, body);
            dg_text(bx + 272u, by + 48u, "  switch: ", gray, body);
            dg_text(bx + 332u, by + 48u, b4, fg, body);
            dg_u2s(tst.ticks_total / 100u, b1);
            dg_text(bx + 4u, by + 72u, "Uptime: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_text(bx + 74u, by + 72u, b1, fg, body);
            dg_text(bx + 100u, by + 72u, " sec  ver: XOS 0.3.0  kern: 0x00100000-0x00138520", gray, body);
            dg_text(bx + 4u, by + 96u, "Free regs: ", dg_rgb(0xAA,0xFF,0xAA), body);
            dg_u2s(pst.free_regions, b1);
            dg_text(bx + 74u, by + 96u, b1, fg, body);
            dg_text(bx + 100u, by + 96u, "  (U refresh)", gray, body);
            dg_text(bx, by + dy6, "U refresh  Esc close  M move", gray, body);
        }
        break;
    case 12u: /* 任务管理器：进程/内存/运行概览 */
        {
            task_stats_t tst;
            pmm_stats_t pst;
            char b1[16], b2[16], b3[16];
            u32 tpages, upages;
            task_stats(&tst);
            pmm_stats(&pst);
            tpages = pmm_total_pages();
            upages = pmm_used_pages();
            dg_text(bx, by, "Task Manager", fg, body);
            dg_u2s(tst.task_count, b1); dg_u2s(tst.ready_count, b2); dg_u2s(tst.zombie_count, b3);
            dg_text(bx + 4u, by + 24u, "Tasks: ", dg_rgb(0x50,0xE0,0x90), body);
            dg_text(bx + 74u, by + 24u, b1, fg, body);
            dg_text(bx + 96u, by + 24u, "  ready: ", gray, body);
            dg_text(bx + 156u, by + 24u, b2, fg, body);
            dg_text(bx + 182u, by + 24u, "  zombie: ", gray, body);
            dg_text(bx + 242u, by + 24u, b3, fg, body);
            dg_u2s(upages * 4u, b1); dg_u2s((tpages - upages) * 4u, b2);
            dg_text(bx + 4u, by + 48u, "Mem used: ", dg_rgb(0x50,0xE0,0x90), body);
            dg_text(bx + 90u, by + 48u, b1, fg, body);
            dg_text(bx + 130u, by + 48u, " KB  free: ", gray, body);
            dg_text(bx + 190u, by + 48u, b2, fg, body);
            dg_text(bx + 230u, by + 48u, " KB", gray, body);
            dg_u2s(tst.switch_total, b1);
            dg_text(bx + 4u, by + 72u, "Switches: ", dg_rgb(0x50,0xE0,0x90), body);
            dg_text(bx + 90u, by + 72u, b1, fg, body);
            dg_text(bx + 130u, by + 72u, " ", gray, body);
            dg_fill(bx + 4u, by + 92u, 220u, 20u, dg_rgb(0x18,0x18,0x20));
            dg_fill(bx + 4u, by + 92u, 220u * tst.ready_count / (tst.task_count ? tst.task_count : 1u),
                    20u, dg_rgb(0x30,0xC0,0x60));
            dg_text(bx + 4u, by + 114u, "green=ready%  1 refresh (live kernel)", gray, body);
            dg_text(bx, by + dy6, "1 refresh  Esc close  M move", gray, body);
        }
        break;
    case 13u: /* 包管理器：内置软件包清单 */
        {
            static const char *pkgs[] = {
                "xos-gui", "xos-kernel", "xos-fs", "xos-net", "xos-sound",
                "xos-apps", "xos-disk", "xos-term", "xos-edit", "xos-browser",
                "xos-calc", "xos-weather", "xos-music", "xos-games", "xos-taskmgr",
                "xos-pkgmgr", "xos-imgview", "xos-capture", "xos-video", "xos-widgets"
            };
            u32 i, n = sizeof(pkgs) / sizeof(pkgs[0]);
            dg_text(bx, by, "Pkg Manager - builtin", fg, body);
            for (i = 0u; i < n; i++) {
                u32 yy = by + 22u + i * 16u;
                if (yy + 14u > by + dy6 - 6u) break;
                if (i == dg_pkg_sel)
                    dg_fill(bx - 4u, yy - 2u, 300u, 14u, dg_rgb(0x2A,0x4A,0x6A));
                dg_text(bx + 2u, yy, pkgs[i],
                        i == dg_pkg_sel ? dg_rgb(0xF0,0xF0,0xF0) : dg_rgb(0xC0,0xC8,0xD0),
                        i == dg_pkg_sel ? dg_rgb(0x2A,0x4A,0x6A) : body);
                dg_text(bx + 150u, yy, "[installed]", dg_rgb(0x50,0xE0,0x90),
                        i == dg_pkg_sel ? dg_rgb(0x2A,0x4A,0x6A) : body);
                dg_text(bx + 232u, yy, "1.0.0", gray,
                        i == dg_pkg_sel ? dg_rgb(0x2A,0x4A,0x6A) : body);
            }
            dg_text(bx, by + dy6, "Up/Dn select  1 install/update  Esc close", gray, body);
        }
        break;
    case 14u: /* 图片查看器：内置示例图片 */
        {
            u32 i, j;
            dg_text(bx, by, "Image Viewer - sample ", fg, body);
            { char nb[8]; dg_u2s(dg_img_idx + 1u, nb); dg_text(bx + 148u, by, nb, blue, body); }
            dg_text(bx + 160u, by, "/ 3", gray, body);
            dg_fill(bx + 4u, by + 22u, 220u, 140u, dg_rgb(0x10,0x10,0x18));
            dg_rect(bx + 4u, by + 22u, 220u, 140u, dg_rgb(0x80,0x80,0x80));
            for (i = 0u; i < 220u; i += 4u) {
                u32 r = 0x20u + (i * 3u) % 0x60u;
                u32 g = 0x40u + (i * 5u) % 0x80u;
                u32 b = 0x60u + (i * 7u) % 0xA0u;
                dg_fill(bx + 4u + i, by + 22u, 4u, 140u, dg_rgb(r, g, b));
            }
            if (dg_img_idx == 0u) {
                dg_fill(bx + 4u, by + 22u, 220u, 70u, dg_rgb(0xE0,0x90,0x40));
                dg_fill(bx + 4u, by + 92u, 220u, 70u, dg_rgb(0x20,0x60,0x20));
                dg_fill(bx + 30u, by + 80u, 40u, 82u, dg_rgb(0x10,0x40,0x10));
                dg_fill(bx + 90u, by + 70u, 50u, 92u, dg_rgb(0x18,0x50,0x18));
                dg_fill(bx + 170u, by + 85u, 34u, 77u, dg_rgb(0x10,0x38,0x10));
                dg_fill(bx + 96u, by + 30u, 28u, 28u, dg_rgb(0xF8,0xD0,0x60));
            } else if (dg_img_idx == 1u) {
                dg_fill(bx + 4u, by + 22u, 220u, 140u, dg_rgb(0x10,0x48,0x88));
                for (j = 0u; j < 6u; j++)
                    dg_fill(bx + 4u, by + 30u + j * 20u, 220u - (j % 2u) * 40u, 4u,
                            dg_rgb(0xE0,0xF0,0xFF));
                dg_fill(bx + 40u, by + 118u, 36u, 44u, dg_rgb(0x88,0x60,0x30));
                dg_fill(bx + 46u, by + 108u, 24u, 12u, dg_rgb(0x30,0xA0,0x60));
            } else {
                for (j = 0u; j < 30u; j++)
                    dg_fill(bx + 8u + (j * 37u) % 200u, by + 26u + (j * 53u) % 130u, 2u, 2u,
                            dg_rgb(0xF0,0xF0,0xF0));
                dg_fill(bx + 90u, by + 60u, 30u, 30u, dg_rgb(0xE8,0xC8,0x60));
                dg_fill(bx + 60u, by + 110u, 70u, 30u, dg_rgb(0x30,0x30,0x40));
            }
            dg_text(bx, by + dy6, "1-3 switch  Esc close  M move", gray, body);
        }
        break;
    case 15u: /* 截图工具 */
        {
            char nb[16], nb2[12];
            dg_text(bx, by, "Screen Capture", fg, body);
            dg_text(bx + 4u, by + 24u, "Press Enter to capture", dg_rgb(0x50,0xE0,0x90), body);
            dg_text(bx + 4u, by + 46u, "Captured: ", gray, body);
            dg_u2s(dg_cap_cnt, nb);
            dg_text(bx + 74u, by + 46u, nb, fg, body);
            dg_text(bx + 100u, by + 46u, "", gray, body);
            dg_text(bx + 4u, by + 68u, "Save to: /mnt/cap", gray, body);
            dg_u2s(dg_cap_cnt, nb2);
            dg_text(bx + 140u, by + 68u, nb2, fg, body);
            dg_text(bx + 170u, by + 68u, ".bin", gray, body);
            dg_fill(bx + 4u, by + 88u, 220u, 90u, dg_rgb(0x18,0x20,0x28));
            dg_text(bx + 14u, by + 120u, "Preview (fb snapshot)", gray, dg_rgb(0x18,0x20,0x28));
            dg_text(bx, by + dy6, "Enter capture  Esc close  M move", gray, body);
        }
        break;
    case 16u: /* 视频播放器：内置帧动画 */
        {
            u32 f = pit_tick_count() / 40u + dg_vid_seek * 10u;
            u32 base = f % 8u;
            u32 i, j;
            dg_text(bx, by, "Video - demo", fg, body);
            dg_text(bx + 140u, by, dg_vid_play ? " [PLAYING]" : " [PAUSED]",
                    dg_vid_play ? dg_rgb(0x50,0xE0,0x90) : gray, body);
            dg_fill(bx + 4u, by + 22u, 220u, 140u, dg_rgb(0x10,0x10,0x18));
            dg_rect(bx + 4u, by + 22u, 220u, 140u, dg_rgb(0x60,0x60,0x60));
            for (j = 0u; j < 14u; j++) {
                u32 wv = (base + j) % 8u;
                u32 lvl = (wv < 4u) ? wv : 8u - wv;
                u32 yv = by + 22u + 120u - lvl * 12u - (j % 3u) * 4u;
                dg_fill(bx + 8u + j * 14u, yv, 10u, 10u, dg_rgb(0x30, 0xA0u + lvl * 20u, 0xE0));
            }
            for (i = 0u; i < 3u; i++) {
                u32 xv = bx + 30u + (base * 9u + i * 60u) % 160u;
                dg_fill(xv, by + 120u - i * 6u, 12u, 4u, dg_rgb(0xE0,0x50,0x50));
            }
            dg_text(bx + 4u, by + 170u, "fps: 25  res: 220x140  fmt: XAV", gray, body);
            dg_text(bx, by + dy6, "P play/pause  N frame  Esc close  M move", gray, body);
        }
        break;
    case 17u: /* PDF阅读器：分页文档 */
        {
            static const char *lines[] = {
                "XOS Whitepaper", "Chapter 1  Overview", "XOS is a fully self-built x86 desktop OS.",
                "Kernel, filesystem and GUI are", "implemented independently by our", "team with no external or closed",
                "components.",
                "Chapter 2  Architecture", "Microkernel with modular design:", "MM, scheduler and drivers are", "decoupled for stability and safety.",
                "Chapter 3  Apps", "Builtin web, files, editor, calc,", "music, weather, task manager", "and 20+ native apps.",
                "(page 1/3)"
            };
            u32 n = sizeof(lines) / sizeof(lines[0]);
            u32 i, st = dg_pdf_page * 10u;
            dg_text(bx, by, "PDF Reader - XOS whitepaper", fg, body);
            dg_text(bx + 200u, by, "p.", blue, body);
            { char nb[8]; dg_u2s(dg_pdf_page + 1u, nb); dg_text(bx + 216u, by, nb, blue, body); }
            dg_text(bx + 228u, by, "/3", gray, body);
            dg_fill(bx + 4u, by + 22u, 250u, 132u, dg_rgb(0xFA,0xF4,0xE8));
            dg_rect(bx + 4u, by + 22u, 250u, 132u, dg_rgb(0x80,0x60,0x40));
            for (i = 0u; i < 9u; i++) {
                u32 li = st + i;
                if (li >= n) break;
                if (i == 0u)
                    dg_text(bx + 12u, by + 30u + i * 14u, lines[li], dg_rgb(0x20,0x30,0x60), dg_rgb(0xFA,0xF4,0xE8));
                else if (lines[li][0] == 'C' || lines[li][0] == 'X' || lines[li][0] == '(' ||
                    (lines[li][0] >= '0' && lines[li][0] <= '9'))
                    dg_text(bx + 12u, by + 30u + i * 14u, lines[li], dg_rgb(0x18,0x18,0x18), dg_rgb(0xFA,0xF4,0xE8));
                else
                    dg_text(bx + 12u, by + 30u + i * 14u, lines[li], dg_rgb(0x30,0x30,0x30), dg_rgb(0xFA,0xF4,0xE8));
            }
            dg_text(bx, by + dy6, "Up/Dn page  Esc close  M move", gray, body);
        }
        break;
    case 18u: /* 代码编辑器：行号 + 示例代码 */
        {
            static const char *code[] = {
                "#include <xos/kernel.h>",
                "int kmain(void) {",
                "    pmm_init();        // memory",
                "    task_init();       // scheduler",
                "    fs_init();         // filesystem",
                "    gui_init();        // graphics",
                "    desk_start();      // desktop",
                "    return 0;",
                "}",
                "/* XOS kernel entry, fully self-built */"
            };
            u32 i;
            dg_text(bx, by, "Code Editor - kernel.c", fg, body);
            dg_fill(bx + 4u, by + 22u, 250u, 132u, dg_rgb(0x10,0x18,0x20));
            dg_rect(bx + 4u, by + 22u, 250u, 132u, dg_rgb(0x30,0x50,0x68));
            for (i = 0u; i < 8u; i++) {
                u32 li = dg_code_row + i;
                char nb[6];
                if (li >= sizeof(code) / sizeof(code[0])) break;
                dg_u2s(li + 1u, nb);
                dg_text(bx + 8u, by + 28u + i * 15u, nb, dg_rgb(0x60,0x88,0xA0), dg_rgb(0x10,0x18,0x20));
                dg_text(bx + 40u, by + 28u + i * 15u, code[li],
                        li == dg_code_row ? dg_rgb(0xF0,0xF0,0xF0) : dg_rgb(0xB0,0xD0,0xE0),
                        dg_rgb(0x10,0x18,0x20));
            }
            dg_text(bx + 4u, by + 160u, "line#/syntax/scroll  C kernel source", gray, body);
            dg_text(bx, by + dy6, "Up/Dn scroll  Esc close  M move", gray, body);
        }
        break;
    case 19u: /* 全局搜索 */
        {
            static const char *res[] = {
                "File: /boot/stage2.bin",
                "File: /kernel.bin",
                "App: Web (xos-browser)",
                "App: Note (xos-edit)",
                "App: Settings (xos-gui)",
                "Doc: User guide",
                "Cmd: help / mem / df / ps",
                "Set: wallpaper / sound / display"
            };
            u32 i;
            dg_text(bx, by, "Global Search", fg, body);
            dg_text(bx + 4u, by + 24u, "Query: ", gray, body);
            dg_text(bx + 72u, by + 24u, dg_src_len ? dg_src_buf : "(type letters)", blue, body);
            dg_text(bx, by + 46u, "Results:", fg, body);
            for (i = 0u; i < 8u; i++) {
                u32 yy = by + 68u + i * 16u;
                if (yy + 14u > by + dy6 - 6u) break;
                if (i == dg_src_sel) dg_fill(bx - 4u, yy - 2u, 300u, 14u, dg_rgb(0x1E,0x3A,0x28));
                dg_text(bx + 2u, yy, res[i],
                        i == dg_src_sel ? dg_rgb(0xF0,0xF0,0xF0) : dg_rgb(0xA0,0xC0,0xA0),
                        i == dg_src_sel ? dg_rgb(0x1E,0x3A,0x28) : body);
            }
            dg_text(bx, by + dy6, "alnum  Up/Dn select  Esc close", gray, body);
        }
        break;
    case 20u: /* 帮助文档系统 */
        {
            dg_text(bx, by, "XOS Help", fg, body);
            dg_text(bx + 4u, by + 24u, "TOC: 1 Overview  2 Desktop", gray, body);
            dg_text(bx + 4u, by + 42u, "      3 Apps  4 Shutdown", gray, body);
            dg_fill(bx + 4u, by + 60u, 260u, 90u, dg_rgb(0xF8,0xF0,0xDC));
            dg_rect(bx + 4u, by + 60u, 260u, 90u, dg_rgb(0x80,0x70,0x40));
            if (dg_help_page == 0u) {
                dg_text(bx + 12u, by + 68u, "XOS is a self-built x86 desktop OS", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 84u, "Login: admin / admin123", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 100u, "Double-click icon to open app", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 116u, "S Start  R menu  Esc back", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 132u, "Power: S menu -> shutdown", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
            } else if (dg_help_page == 1u) {
                dg_text(bx + 12u, by + 68u, "Window: M move  N minimize", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 84u, "W/Tab switch  1-4 restore", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 100u, "Esc close window", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 116u, "Mouse left double-click to launch", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
            } else if (dg_help_page == 2u) {
                dg_text(bx + 12u, by + 68u, "20+ builtin apps:", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 84u, "Web/Files/Note/Term/Settings", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 100u, "Music/Weather/Games/Tasks/Pkgs", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 116u, "Pics/Shot/Video/PDF/Code/Mail", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
            } else {
                dg_text(bx + 12u, by + 68u, "Shutdown: S menu -> shutdown", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 84u, "or Esc exit desktop to kernel term", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 100u, "type poweroff in terminal", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
                dg_text(bx + 12u, by + 116u, "system reboot-safe, host unaffected", dg_rgb(0x18,0x18,0x18), dg_rgb(0xF8,0xF0,0xDC));
            }
            dg_text(bx, by + dy6, "1-4 topic  Esc close  M move", gray, body);
        }
        break;
    case 21u: /* 开机自启管理 */
        {
            static const char *items[] = { "Desktop (desk)", "Terminal (term)", "Music (music)", "Monitor (monitor)", "Clock (clock)", "Web (browser)" };
            u32 i;
            dg_text(bx, by, "Startup Manager", fg, body);
            dg_text(bx + 4u, by + 24u, "select item, Space toggle:", gray, body);
            for (i = 0u; i < 6u; i++) {
                u32 yy = by + 46u + i * 20u;
                if (i == dg_auto_sel) dg_fill(bx - 4u, yy - 2u, 300u, 16u, dg_rgb(0x1E,0x3A,0x50));
                dg_text(bx + 2u, yy, items[i],
                        i == dg_auto_sel ? dg_rgb(0xF0,0xF0,0xF0) : dg_rgb(0xC0,0xD0,0xE0),
                        i == dg_auto_sel ? dg_rgb(0x1E,0x3A,0x50) : body);
                dg_text(bx + 200u, yy, (i < 3u) ? "[ON]" : "[OFF]",
                        (i < 3u) ? dg_rgb(0x50,0xE0,0x90) : gray,
                        i == dg_auto_sel ? dg_rgb(0x1E,0x3A,0x50) : body);
            }
            dg_text(bx + 4u, by + 172u, "loaded at next boot", gray, body);
            dg_text(bx, by + dy6, "Up/Dn select  Space toggle  Esc close", gray, body);
        }
        break;
    case 22u: /* 文字处理：带格式文档 */
        {
            static const char *doc[] = {
                "Title: XOS word demo",
                "Body:  this document is edited by",
                "the XOS word processor, with title",
                "and body styles, wrap and scroll.",
                "Body:  version 0.3.0, self-built.",
                "Title: Feature list",
                "Body:  edit / layout / scroll / save.",
                "Body:  for document layout, richer",
                "than the plain text editor."
            };
            u32 i;
            dg_text(bx, by, "Word - doc.xod", fg, body);
            dg_fill(bx + 4u, by + 22u, 250u, 132u, dg_rgb(0xF8,0xF4,0xEC));
            dg_rect(bx + 4u, by + 22u, 250u, 132u, dg_rgb(0x90,0x80,0x60));
            for (i = 0u; i < 8u; i++) {
                u32 li = dg_wp_row + i;
                if (li >= sizeof(doc) / sizeof(doc[0])) break;
                if (doc[li][0] == 'T')
                    dg_text(bx + 12u, by + 30u + i * 15u, doc[li], dg_rgb(0x18,0x30,0x60), dg_rgb(0xF8,0xF4,0xEC));
                else
                    dg_text(bx + 12u, by + 30u + i * 15u, doc[li], dg_rgb(0x28,0x28,0x28), dg_rgb(0xF8,0xF4,0xEC));
            }
            dg_text(bx + 4u, by + 160u, "title blue bold, body normal", gray, body);
            dg_text(bx, by + dy6, "Up/Dn scroll  Esc close  M move", gray, body);
        }
        break;
    case 23u: /* 邮件客户端 */
        {
            static const char *subj[] = { "System: wallpaper updated", "Dev: phase-3 apps online", "Welcome to XOS Mail", "Security: change your password" };
            static const char *body1[] = {
                "XOS Mail", "Subject: System notice", "Wallpaper has been updated with",
                "the system; switch in Settings.", "-- XOS System Team"
            };
            static const char *body2[] = {
                "XOS Mail", "Subject: Phase-3 apps", "Task mgr, pkg mgr, image viewer",
                "and video are now built in.", "-- XOS Dev Team"
            };
            static const char *body3[] = {
                "XOS Mail", "Subject: Welcome", "This is the builtin demo inbox,",
                "with list browse and body read.", "-- XOS Mail Team"
            };
            static const char *body4[] = {
                "XOS Mail", "Subject: Security", "Please change your password",
                "regularly to keep data safe.", "-- XOS Security Center"
            };
            u32 i;
            const char *(*b)[5] = NULL;
            dg_text(bx, by, "Mail - Inbox", fg, body);
            dg_text(bx + 160u, by, "4", gray, body);
            for (i = 0u; i < 4u; i++) {
                u32 yy = by + 24u + i * 18u;
                if (i == dg_mail_sel) dg_fill(bx - 4u, yy - 2u, 300u, 16u, dg_rgb(0x1E,0x3A,0x30));
                dg_text(bx + 2u, yy, subj[i],
                        i == dg_mail_sel ? dg_rgb(0xF0,0xF0,0xF0) : dg_rgb(0xB0,0xD0,0xB0),
                        i == dg_mail_sel ? dg_rgb(0x1E,0x3A,0x30) : body);
            }
            if (dg_mail_sel == 0u) b = &body1;
            else if (dg_mail_sel == 1u) b = &body2;
            else if (dg_mail_sel == 2u) b = &body3;
            else b = &body4;
            dg_fill(bx + 4u, by + 100u, 260u, 60u, dg_rgb(0xEC,0xF4,0xEC));
            dg_rect(bx + 4u, by + 100u, 260u, 60u, dg_rgb(0x40,0x70,0x50));
            for (i = 0u; i < 5u; i++)
                dg_text(bx + 12u, by + 106u + i * 11u, (*b)[i], dg_rgb(0x20,0x30,0x28), dg_rgb(0xEC,0xF4,0xEC));
            dg_text(bx, by + dy6, "Up/Dn select  Enter read  Esc close", gray, body);
        }
        break;
    case 9u: /* 时钟日历 + 秒表 */
        {
            char nb[24];
            rtc_time_t rt;
            if (rtc_read_all(&rt) != 0) {
                rt.hour = 0; rt.min = 0; rt.sec = 0;
                rt.year = 2026; rt.mon = 10; rt.day = 9; rt.dow = 5;
            }
            dg_text(bx, by, "Clock & Cal", fg, body);
            dg_text(bx + dg_text_w("Clock & Cal") + 8u, by, dg_tz_name[dg_tz],
                    dg_rgb(0xE8,0xC0,0x30), body);
            nb[0] = (char)('0' + rt.hour / 10u); nb[1] = (char)('0' + rt.hour % 10u);
            nb[2] = ':'; nb[3] = (char)('0' + rt.min / 10u); nb[4] = (char)('0' + rt.min % 10u);
            nb[5] = ':'; nb[6] = (char)('0' + rt.sec / 10u); nb[7] = (char)('0' + rt.sec % 10u);
            nb[8] = 0;
            dg_fill(bx + 8u, by + 30u, dg_text_w(nb) * 2u, 24u, dg_rgb(0x1E,0x3A,0x5F));
            dg_text(bx + 10u, by + 34u, nb, dg_rgb(0xF0,0xF0,0xF0), dg_rgb(0x1E,0x3A,0x5F));
            {
                static const char *cDOW[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
                char db[40];
                u32 i = 0;
                u32 k;
                db[i++] = (char)('0' + rt.year / 1000u % 10u);
                db[i++] = (char)('0' + rt.year / 100u % 10u);
                db[i++] = (char)('0' + rt.year / 10u % 10u);
                db[i++] = (char)('0' + rt.year % 10u);
                db[i++] = '-';
                db[i++] = (char)('0' + rt.mon / 10u);
                db[i++] = (char)('0' + rt.mon % 10u);
                db[i++] = '-';
                db[i++] = (char)('0' + rt.day / 10u);
                db[i++] = (char)('0' + rt.day % 10u);
                db[i++] = ' '; db[i++] = ' ';
                for (k = 0; k < 3u && cDOW[rt.dow % 7u][k]; k++) db[i++] = cDOW[rt.dow % 7u][k];
                db[i++] = 0;
                dg_text(bx + 10u, by + dy3, db, fg, body);
            }
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
                dg_text(bx + 110u, by + dy4, dg_st_running ? " RUNNING" : " STOPPED",
                        dg_st_running ? dg_rgb(0x30,0xC0,0x50) : gray, body);
            }
            /* 闹钟状态行 */
            if (dg_alm_ring) {
                dg_text(bx + 10u, by + 112u, "ALARM! any key to stop", dg_rgb(0xE0,0x30,0x30), body);
            } else if (dg_alm_set == 1u) {
                char ab[16];
                u32 v = dg_alm_min;
                dg_text(bx + 10u, by + 112u, "Alarm set: min (0-99) Enter ok", dg_rgb(0x1E,0x6F,0xD0), body);
                ab[0] = (char)('0' + v / 10u);
                ab[1] = (char)('0' + v % 10u);
                ab[2] = ' '; ab[3] = 'm'; ab[4] = 'i'; ab[5] = 'n'; ab[6] = 0;
                dg_text(bx + 10u, by + dy6, ab, dg_rgb(0xF0,0xF0,0xF0), body);
            } else if (dg_alm_set == 2u) {
                u32 rem = (dg_alm_target > pit_tick_count()) ? (dg_alm_target - pit_tick_count()) / 100u : 0u;
                char ab[16];
                u32 s2 = rem % 60u, m2 = rem / 60u;
                dg_text(bx + 10u, by + 112u, "Alarm set  left: ", dg_rgb(0x30,0xC0,0x50), body);
                ab[0] = (char)('0' + m2 / 10u); ab[1] = (char)('0' + m2 % 10u);
                ab[2] = ':'; ab[3] = (char)('0' + s2 / 10u); ab[4] = (char)('0' + s2 % 10u); ab[5] = 0;
                dg_text(bx + 10u, by + dy6, ab, dg_rgb(0xE0,0xE0,0xE0), body);
            } else {
                dg_text(bx + 10u, by + 112u, "Alarm: none (A set)", gray, body);
            }
            /* 日历视图：仅 2x/3x 大窗口显示（1x 无空间） */
            if (w->h > 260u) {
                u32 i2, j2, d, cx, cy;
                const char *wk = "M T W T F S S";
                dg_text(bx + 8u, by + dy6 - 16u, wk, dg_rgb(0xC8,0xD8,0xE8), body);
                for (j2 = 0u; j2 < 5u; j2++) {
                    for (i2 = 0u; i2 < 7u; i2++) {
                        char cb[4];
                        u32 col;
                        d = (u32)(j2 * 7u + i2) + 1u - 3u;      /* 10/1=周四(列3) */
                        cx = bx + 8u + i2 * 16u;
                        cy = by + dy6 + j2 * 16u;
                        if (d >= 1u && d <= 31u) {
                            col = (d == 9u) ? dg_rgb(0x2F,0x7D,0xE1) : dg_rgb(0x24,0x30,0x40);
                            dg_fill(cx, cy, 14u, 14u, col);
                            cb[0] = (char)('0' + d / 10u);
                            cb[1] = (char)('0' + d % 10u);
                            cb[2] = 0;
                            dg_text(cx + 1u, cy + 1u, cb, dg_rgb(0xE0,0xE0,0xE0), col);
                        }
                    }
                }
                dg_text(bx + 8u, by + dy6 + 84u, "* today", dg_rgb(0x2F,0x7D,0xE1), body);
            } else {
                dg_text(bx + 8u, by + dy6, "(G enlarge for calendar)", gray, body);
            }
            dg_text(bx, by + dy7, "S stopw R reset A alarm T tz M move N min Esc close", gray, body);
            break;
        }
    default: /* 游戏中心：1 收集  2 2048 */
        {
            i32 i, j;
            char nb[16];
            if (dg_gm_mode == 1u) {
                /* 2048：4x4 棋盘 */
                dg_text(bx, by, "2048 - 1/2 switch  Esc close", fg, body);
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
                if (dg_2048_over) dg_text(bx + 8u, by + 172u, "GAME OVER (S restart)", dg_rgb(0xE0,0x40,0x40), body);
                nb[0] = 'S'; nb[1] = 'c'; nb[2] = 'o'; nb[3] = 'r'; nb[4] = 'e'; nb[5] = ':';
                dg_itoa((int)dg_2048_score, nb + 6);
                dg_text(bx + 8u, by + dy6, nb, dg_rgb(0xE8,0xC0,0x30), body);
                dg_text(bx, by + dy7, "Arrows move  S restart  Esc close", gray, body);
            } else {
                /* 收集：5x5 */
                dg_text(bx, by, "Collect - 2 to 2048  Esc close", fg, body);
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
                dg_text(bx, by + dy7, "Arrows move  Enter collect  S reset  Esc close", gray, body);
            }
            break;
        }
        break;
    }
}

/* 取 UTF-8 名称前 2 个字符（中文 3 字节/字）用于任务栏按钮 */
static void dg_name2(char *dst, const char *name)
{
    u32 n = 0u, c = 0u;
    while (name[n] && c < 2u) {
        if ((name[n] & 0x80u) == 0u) { dst[n] = name[n]; n++; c++; }
        else {
            u32 k;
            for (k = 0u; k < 3u && name[n + k]; k++) dst[n + k] = name[n + k];
            n += 3u; c++;
        }
    }
    dst[n] = 0;
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
    u32 title = dg_theme_c(2u);
    u32 body  = dg_theme_c(3u);
    u32 frm   = dg_theme_c(4u);
    const char *name = dg_icons[w->icon].name;
    dg_fill(w->x, w->y, w->w, w->h, body);
    dg_fill(w->x, w->y, w->w, 26u, title);
    /* 左侧应用色条（直观识别应用类别） */
    dg_fill(w->x + 5u, w->y + 6u, 5u, 14u, dg_win_accent(w->icon));
    dg_text(w->x + 16u, w->y + 9u, name, dg_theme_c(1u), title);
    /* 右侧尺寸档位 */
    dg_text(w->x + w->w - 40u, w->y + 9u,
            dg_wsz == 0u ? "1x" : (dg_wsz == 1u ? "2x" : "3x"),
            dg_rgb(0xE8,0xC8,0x4A), title);
    /* 3D 边框：上/左亮线 + 右/下暗线 + 外描边（立体可识别） */
    dg_fill(w->x, w->y, w->w, 1u, g_theme == 1u ? dg_rgb(0xFF,0xFF,0xFF) : dg_rgb(0x38,0x4C,0x68));
    dg_fill(w->x, w->y, 1u, w->h, g_theme == 1u ? dg_rgb(0xFF,0xFF,0xFF) : dg_rgb(0x2A,0x3C,0x54));
    dg_fill(w->x + w->w - 1u, w->y, 1u, w->h, dg_rgb(0x30,0x40,0x50));
    dg_fill(w->x, w->y + w->h - 1u, w->w, 1u, dg_rgb(0x30,0x40,0x50));
    dg_rect(w->x, w->y, w->w, w->h, frm);
    /* 关闭按钮 X */
    dg_fill(w->x + w->w - 24u, w->y + 5u, 18u, 16u, dg_rgb(0xC0,0x30,0x30));
    dg_char(w->x + w->w - 20u, w->y + 9u, 'X', dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0xC0,0x30,0x30), g_font_scale);
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
    u32 bar = dg_theme_c(0u);
    u32 txt = dg_theme_c(1u);
    u32 sel_c = dg_rgb(0xFF,0xFF,0xFF);
    u32 sec, hh, mm;
    char tbuf[16];

    /* 1) 设计稿壁纸：XOS 1.0 桌面主界面（320x240 拉伸） */
    for (y = 0u; y < DG_H - DG_TASKBAR; y++) {
        u32 wy = (y * 120u) / (DG_H - DG_TASKBAR);
        for (x = 0u; x < DG_W; x++) {
            u32 wx = (x * 160u) / DG_W;
            dg_fb[y * DG_W + x] = xos_wallpaper[wy * 160u + wx];
        }
    }

    /* 2) 图标：按分辨率+字体缩放自适应网格（列宽=图标96+文字宽度、行距随字高缩放） */
    {
        u32 cw = 96u, rh = 94u, cols;
        u32 maxw = 0u;
        u32 j;
        for (j = 0u; j < DG_ICON_N; j++) {
            u32 tw = dg_text_w(dg_icons[j].name);
            if (tw > maxw) maxw = tw;
        }
        if (maxw + 28u > cw) cw = maxw + 28u;          /* 文字自适应列宽 */
        rh = 36u + 16u + 8u * g_font_scale + 34u;      /* 行距随字体缩放 */
        cols = DG_W / cw;
        if (cols < 1u) cols = 1u;
        for (i = 0u; i < DG_ICON_N; i++) {
            u32 col = i % cols;
            u32 row = i / cols;
            x = 24u + col * cw;
            y = 40u + row * rh;
            dg_icon_pattern(x, y, dg_icons[i].icon);
            if (i == dg_sel && dg_nwin == 0u)
                dg_rect(x - 3u, y - 3u, 38u, 38u, sel_c);
            dg_text(x + 2u, y + 36u, dg_icons[i].name, txt, dg_rgb(0x0E,0x22,0x40));
        }
    }

    /* 3) 窗口：非最小化按 Z 序绘制（焦点最后=最上） */
    for (k = 0u; k < DG_WIN_MAX; k++) {
        i = (dg_focus + 1u + k) % DG_WIN_MAX;      /* 从焦点后开始，保证焦点最后画 */
        if (dg_wins[i].open && !dg_wins[i].min)
            dg_window_draw(&dg_wins[i]);
    }

    /* 3.5) 桌面时钟小组件（实时刷新；仅桌面无窗口时显示） */
    if (dg_nwin == 0u) {
        u32 tsc = pit_tick_count() / 100u;
        u32 th = (10u + tsc / 3600u) % 24u;
        u32 tm = tsc % 3600u / 60u;
        u32 ts = tsc % 60u;
        char wb[16];
        wb[0] = (char)('0' + th / 10u); wb[1] = (char)('0' + th % 10u);
        wb[2] = ':'; wb[3] = (char)('0' + tm / 10u); wb[4] = (char)('0' + tm % 10u);
        wb[5] = ':'; wb[6] = (char)('0' + ts / 10u); wb[7] = (char)('0' + ts % 10u); wb[8] = 0;
        dg_fill(DG_W - 144u, 4u, dg_text_w(wb) * 2u, 14u, dg_theme_c(6u));
        dg_text(DG_W - 142u, 6u, wb, dg_theme_c(1u), dg_theme_c(6u));
    }
    /* 4) 任务栏 */
    dg_fill(0u, DG_H - DG_TASKBAR, DG_W, DG_TASKBAR, bar);
    dg_fill(0u, DG_H - DG_TASKBAR, DG_W, 2u, g_theme == 1u ? dg_rgb(0xA8,0xB0,0xBC) : dg_rgb(0x2E,0x3A,0x4E));
    /* 开始按钮 */
    dg_fill(6u, DG_H - DG_TASKBAR + (g_tb_h - 20u) / 2u, 56u, 20u, dg_theme_c(5u));
    dg_text(14u, DG_H - DG_TASKBAR + dg_tb_txt_y(), "XOS", dg_rgb(0xFF,0xFF,0xFF), dg_theme_c(5u));
    /* 桌面版本标识 */
    dg_text(78u, DG_H - DG_TASKBAR + dg_tb_txt_y(), "v1.0", dg_theme_c(7u), bar);
    /* 最小化窗口的恢复按钮（点击概念：按对应数字键恢复） */
    if (dg_nwin > 0u) {
        u32 rbx = 300u;
        for (i = 0u; i < DG_WIN_MAX; i++) {
            if (dg_wins[i].open) {
                u32 col = dg_wins[i].min ? dg_theme_c(7u)
                        : (i == dg_focus) ? dg_rgb(0x2F,0x7D,0xE1) : dg_rgb(0x2E,0x3E,0x52);
                dg_fill(rbx, DG_H - DG_TASKBAR + (g_tb_h - 18u) / 2u, 40u, 18u, col);
                dg_name2(tbuf, dg_icons[dg_wins[i].icon].name);
                dg_text(rbx + 6u, DG_H - DG_TASKBAR + dg_tb_txt_y(), tbuf,
                        dg_rgb(0xE8,0xE8,0xE8), bar);
                rbx += 46u;
            }
        }
    }
    /* 系统监视：内存占用条（真实 pmm 数据）+ 进程数（真实调度数据） */
    {
        static u32 dbg_once = 0u;
        u32 tp = pmm_total_pages();
        u32 up = pmm_used_pages();
        u32 fp = pmm_free_page_count();
        u32 pct = (tp > 0u) ? (((tp - fp) * 100u) / tp) : 0u;
        if (!dbg_once) {
            dbg_once = 1u;
            con_printf("  [desk_gui] mem tp=%u up=%u fp=%u pct=%u\n", tp, up, fp, pct);
            con_flush();
        }
        u32 bx = DG_W - 236u;
        u32 by = DG_H - DG_TASKBAR + (g_tb_h - 12u) / 2u;
        u32 c1 = (pct < 70u) ? dg_rgb(0x30,0xC0,0x50)
             : (pct < 90u) ? dg_rgb(0xE0,0xB0,0x40) : dg_rgb(0xE0,0x48,0x48);
        dg_rect(bx, by, 34u, 12u, dg_theme_c(7u));
        if (pct > 0u) dg_fill(bx + 1u, by + 1u, (34u - 2u) * pct / 100u, 10u, c1);
        if (pct > 0u && (34u - 2u) * pct / 100u >= 34u) dg_fill(bx + 1u, by + 1u, 32u, 10u, c1);
        tbuf[0] = (char)('0' + pct / 10u % 10u);
        tbuf[1] = (char)('0' + pct % 10u);
        tbuf[2] = '%';
        tbuf[3] = ' ';
        tbuf[4] = 'P';
        {
            task_stats_t st;
            task_stats(&st);
            tbuf[5] = (char)('0' + (st.task_count % 10u));
        }
        tbuf[6] = 0;
        dg_text(bx + 38u, DG_H - DG_TASKBAR + dg_tb_txt_y(), tbuf, dg_theme_c(1u), bar);
    }
    /* 系统托盘：网络 + 音量 + 电池 + 通知角标 */
    dg_fill(DG_W - 150u, DG_H - DG_TASKBAR + (g_tb_h - 10u) / 2u, 10u, 10u, dg_rgb(0x30,0xC0,0x50));   /* 网络绿点 */
    dg_fill(DG_W - 128u, DG_H - DG_TASKBAR + (g_tb_h - 12u) / 2u, 12u, 12u, dg_rgb(0xE0,0xB0,0x40));   /* 音量 */
    dg_fill(DG_W - 128u, DG_H - DG_TASKBAR + (g_tb_h - 8u) / 2u, 2u, 8u, dg_rgb(0xE0,0xB0,0x40));
    dg_rect(DG_W - 108u, DG_H - DG_TASKBAR + (g_tb_h - 12u) / 2u, 14u, 12u, dg_rgb(0x80,0xE0,0x90));    /* 电池 */
    dg_fill(DG_W - 108u, DG_H - DG_TASKBAR + (g_tb_h - 12u) / 2u, 10u, 12u, dg_rgb(0x20,0x50,0x30));
    /* 时钟：真实 CMOS RTC（日期 + 时:分:秒） */
    {
        char wb2[16];
        rtc_time_t rt;
        if (rtc_read_all(&rt) == 0) {
            tbuf[0] = (char)('0' + rt.hour / 10u);
            tbuf[1] = (char)('0' + rt.hour % 10u);
            tbuf[2] = ':';
            tbuf[3] = (char)('0' + rt.min / 10u);
            tbuf[4] = (char)('0' + rt.min % 10u);
            tbuf[5] = ':';
            tbuf[6] = (char)('0' + rt.sec / 10u);
            tbuf[7] = (char)('0' + rt.sec % 10u);
            tbuf[8] = 0;
            wb2[0] = (char)('0' + rt.mon / 10u);
            wb2[1] = (char)('0' + rt.mon % 10u);
            wb2[2] = '-';
            wb2[3] = (char)('0' + rt.day / 10u);
            wb2[4] = (char)('0' + rt.day % 10u);
            wb2[5] = 0;
            dg_text(DG_W - 70u, DG_H - DG_TASKBAR + dg_tb_txt_y(), tbuf,
                    dg_theme_c(1u), bar);
            dg_text(DG_W - 152u, DG_H - DG_TASKBAR + dg_tb_txt_y(), wb2,
                    dg_theme_c(7u), bar);
        } else {
            dg_text(DG_W - 70u, DG_H - DG_TASKBAR + dg_tb_txt_y(), "--:--:--",
                    dg_rgb(0xF0,0xF0,0xF0), bar);
        }
    }

    /* 5) 开始菜单浮层 */
    if (dg_menu == DG_MENU_START) {
        u32 mx = 6u, my = DG_H - DG_TASKBAR - 24u * 14u - 18u;
        u32 mw = 210u, mh = 24u * 14u + 18u;
        u32 mbg = dg_theme_c(8u), mfr = dg_theme_c(7u);
        if (my < 8u) my = 8u;
        dg_fill(mx, my, mw, mh, mbg);
        dg_rect(mx, my, mw, mh, mfr);
        dg_text(mx + 8u, my + 4u, "XOS Applications (24)", dg_rgb(0x50,0xA8,0xE8), mbg);
        for (i = 0u; i < DG_ICON_N; i++) {
            u32 iy = my + 18u + i * 14u;
            if (i == dg_menusel) dg_fill(mx + 4u, iy, mw - 8u, 12u, dg_rgb(0x2F,0x7D,0xE1));
            dg_text(mx + 10u, iy + 3u, dg_apps[i],
                    i == dg_menusel ? dg_rgb(0xFF,0xFF,0xFF) : dg_theme_c(1u),
                    i == dg_menusel ? dg_rgb(0x2F,0x7D,0xE1) : mbg);
        }
        dg_text(mx + 8u, my + mh - 14u, "Up/Dn select  Enter start  Esc close",
                dg_theme_c(7u), mbg);
    }

    /* 5.5) 首次使用欢迎弹窗 */
    if (dg_welcome) {
        u32 wx = 60u, wy = 70u, ww = DG_W - 120u, wh = 190u;
        u32 mbg = dg_theme_c(8u);
        dg_fill(wx, wy, ww, wh, mbg);
        dg_rect(wx, wy, ww, wh, dg_rgb(0x58,0x88,0xC0));
        dg_fill(wx, wy, ww, 26u, dg_rgb(0x1E,0x6F,0xD0));
        dg_text(wx + 10u, wy + 9u, "Welcome to XOS", dg_rgb(0xFF,0xFF,0xFF), dg_rgb(0x1E,0x6F,0xD0));
        dg_text(wx + 16u, wy + 44u, "Tab select icon, Enter open app", dg_theme_c(1u), mbg);
        dg_text(wx + 16u, wy + 68u, "S Start menu   R Right menu", dg_theme_c(1u), mbg);
        dg_text(wx + 16u, wy + 92u, "M move   N minimize   Esc close", dg_theme_c(1u), mbg);
        dg_text(wx + 16u, wy + 116u, "Start menu: 24 apps", dg_theme_c(1u), mbg);
        dg_text(wx + 16u, wy + 140u, "Note editor saves to /note.txt", dg_theme_c(1u), mbg);
        dg_text(wx + ww / 2u - 88u, wy + wh - 24u, "Press Enter to start", dg_rgb(0x30,0xC0,0x50), mbg);
    }

    /* 6) 右键菜单浮层 */
    if (dg_menu == DG_MENU_RIGHT) {
        static const char *ritems[3] = { "Open", "Properties", "Close" };
        u32 cols = DG_W / 100u;
        u32 rx = 24u + (dg_sel % cols) * 100u + 30u;
        u32 ry = 40u + (dg_sel / cols) * 94u + 10u;
        u32 mbg = dg_theme_c(8u), mfr = dg_theme_c(7u);
        dg_fill(rx, ry, 150u, 66u, mbg);
        dg_rect(rx, ry, 150u, 66u, mfr);
        for (i = 0u; i < 3u; i++) {
            u32 iy = ry + 6u + i * 20u;
            if (i == dg_menusel) dg_fill(rx + 4u, iy, 142u, 18u, dg_rgb(0x2F,0x7D,0xE1));
            dg_text(rx + 10u, iy + 5u, ritems[i],
                    i == dg_menusel ? dg_rgb(0xFF,0xFF,0xFF) : dg_theme_c(1u),
                    i == dg_menusel ? dg_rgb(0x2F,0x7D,0xE1) : mbg);
        }
    }

    con_flush();
}

/* ---------------- 初始化与运行 ---------------- */
int desk_gui_init(void)
{
    static u32 dg_inited = 0u;
    u32 a, rc, m_idx = 7u, saved;
    disp_mode_t dm;
    if (dg_inited) return 0;                  /* 已初始化（图形登录已映射 LFB） */
    /* 分辨率记忆恢复：上次成功使用的显示模式优先（5/6/7 合法，其余钳制） */
    saved = sysconf_get_u32("screen.mode", 7u);
    if (saved >= 5u && saved <= 7u) m_idx = saved;
    rc = display_set_mode(m_idx);             /* VBE 图形模式（5=640x480 6=800x600 7=1024x768） */
    if (rc != 0) {
        con_puts("  [desk_gui] display_set_mode(7) failed, try 640x480\n");
        con_flush();
        m_idx = 5u;
        rc = display_set_mode(m_idx);         /* 回退 640x480x32 */
        if (rc != 0) return -1;
    }
    if (display_get_mode(m_idx, &dm) != 0) {
        g_dgw = 640u; g_dgh = 480u;
    } else {
        g_dgw = dm.width; g_dgh = dm.height;
    }
    con_printf("  [desk_gui] desktop %ux%ux32 (mode %u)\n", g_dgw, g_dgh, m_idx);
    con_flush();
    dg_font_scale_auto();             /* 字体缩放随分辨率自适应 */
    con_printf("  [desk_gui] font scale %ux\n", g_font_scale);
    con_flush();
    if (vmm_map_device_huge(vmm_kernel_mm(), DG_LFB, DG_LFB, PTE_P | PTE_RW) != VMM_OK) {
        con_puts("  [desk_gui] huge device map failed, fallback 4KB pages\n");
        con_flush();
        /* 回退：4KB 逐页设备映射（LFB 1024x768x4 = 3MB = 768 页） */
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
    /* 窗口边界约束：打开位置钳制在屏幕内（含任务栏留白），防越界不可达 */
    if (dg_wins[slot].x + dg_wins[slot].w + 8u > DG_W)
        dg_wins[slot].x = (DG_W > dg_wins[slot].w + 8u) ? DG_W - dg_wins[slot].w - 8u : 8u;
    if (dg_wins[slot].y + dg_wins[slot].h + 8u > DG_H - DG_TASKBAR)
        dg_wins[slot].y = (DG_H - DG_TASKBAR > dg_wins[slot].h + 8u)
                              ? DG_H - DG_TASKBAR - dg_wins[slot].h - 8u : 8u;
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
            /* 闹钟心跳：无键时由 PIT 定时中断唤醒检查到点 */
            if (dg_alm_set == 2u && !dg_alm_ring &&
                pit_tick_count() >= dg_alm_target) {
                dg_alm_ring = 1u;
                sound_beep();
                sound_beep();
                dg_render();
            }
            /* 桌面小组件节流刷新：无窗口/无菜单时每 50 tick(0.5s) 重绘 */
            if (dg_nwin == 0u && dg_menu == DG_MENU_NONE && !dg_move_mode &&
                pit_tick_count() - dg_last_repaint >= 50u) {
                dg_last_repaint = pit_tick_count();
                dg_render();
            }
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

        /* --- 全局分辨率热键：F2=640x480  F3=800x600  F4=1024x768 --- */
        if (key == KEY_F2 || key == KEY_F3 || key == KEY_F4) {
            u32 mi = (key == KEY_F2) ? 5u : (key == KEY_F3) ? 6u : 7u;
            if (display_set_mode(mi) == 0) {
                disp_mode_t dm;
                if (display_get_mode(mi, &dm) == 0) { g_dgw = dm.width; g_dgh = dm.height; }
                sysconf_set_u32("screen.mode", mi);   /* 分辨率记忆恢复 */
                dg_font_scale_auto();                 /* 字体随分辨率重算档位 */
                dg_render();
            }
            continue;
        }

        /* --- 全局字体缩放热键：F5 循环 1x/2x/3x --- */
        if (key == KEY_F5) {
            g_font_scale = (g_font_scale >= 3u) ? 1u : g_font_scale + 1u;
            g_tb_h = 22u + 8u * g_font_scale;   /* 任务栏高度联动 */
            dg_render();
            continue;
        }

        /* --- 全局主题热键：F6 切换 深色/浅色 --- */
        if (key == KEY_F6) {
            g_theme = (g_theme == 0u) ? 1u : 0u;
            dg_render();
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
            /* 时钟日历：S 秒表启停  R 复位  A 闹钟 */
            if (w->icon == 9u) {
                if (dg_alm_ring) {
                    dg_alm_ring = 0u;                       /* 任意键关闭闹钟 */
                    dg_alm_set = 0u;
                    sound_silence();
                } else if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_A) {
                    if (dg_alm_set == 2u || dg_alm_set == 1u) {
                        dg_alm_set = 0u; dg_alm_min = 0u;   /* 取消/退出设置 */
                    } else {
                        dg_alm_set = 1u; dg_alm_min = 0u;
                    }
                } else if (dg_alm_set == 1u) {
                    if (key >= KEY_0 && key <= KEY_9) {
                        u32 n = key - KEY_0;
                        if (dg_alm_min < 10u) dg_alm_min = dg_alm_min * 10u + n;
                    } else if (key == KEY_ENTER) {
                        if (dg_alm_min > 0u) {
                            dg_alm_target = pit_tick_count() + dg_alm_min * 6000u;
                            dg_alm_set = 2u;
                        } else {
                            dg_alm_set = 0u;                 /* 0 分钟 = 取消 */
                        }
                    } else if (key == KEY_BACKSP) {
                        dg_alm_min /= 10u;
                    }
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
                } else if (key == KEY_T) {
                    dg_tz = (dg_tz + 1u) % 5u;              /* 循环切换时区 */
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                }
                /* 响铃检测 */
                if (dg_alm_set == 2u && !dg_alm_ring &&
                    pit_tick_count() >= dg_alm_target) {
                    dg_alm_ring = 1u;
                    sound_beep();
                    sound_beep();
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
            /* 第四期应用：PDF/代码/搜索/帮助/自启/文字/邮件 */
            if (w->icon >= 17u && w->icon <= 23u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (w->icon == 17u) {
                    if (key == KEY_UP) { if (dg_pdf_page > 0u) dg_pdf_page--; }
                    else if (key == KEY_DOWN) { if (dg_pdf_page < 2u) dg_pdf_page++; }
                } else if (w->icon == 18u) {
                    if (key == KEY_UP) { if (dg_code_row > 0u) dg_code_row--; }
                    else if (key == KEY_DOWN) { if (dg_code_row < 4u) dg_code_row++; }
                } else if (w->icon == 19u) {
                    if (key >= KEY_A && key <= KEY_Z) {
                        if (dg_src_len < 23u) { dg_src_buf[dg_src_len++] = (char)('a' + (key - KEY_A)); dg_src_buf[dg_src_len] = 0; }
                    } else if (key == KEY_BACKSP) {
                        if (dg_src_len > 0u) { dg_src_len--; dg_src_buf[dg_src_len] = 0; }
                    } else if (key == KEY_UP) { if (dg_src_sel > 0u) dg_src_sel--; }
                    else if (key == KEY_DOWN) { if (dg_src_sel < 7u) dg_src_sel++; }
                } else if (w->icon == 20u) {
                    if (key >= KEY_1 && key <= KEY_4) dg_help_page = key - KEY_1;
                } else if (w->icon == 21u) {
                    if (key == KEY_UP) { if (dg_auto_sel > 0u) dg_auto_sel--; }
                    else if (key == KEY_DOWN) { if (dg_auto_sel < 5u) dg_auto_sel++; }
                } else if (w->icon == 22u) {
                    if (key == KEY_UP) { if (dg_wp_row > 0u) dg_wp_row--; }
                    else if (key == KEY_DOWN) { if (dg_wp_row < 2u) dg_wp_row++; }
                } else if (w->icon == 23u) {
                    if (key == KEY_UP) { if (dg_mail_sel > 0u) dg_mail_sel--; }
                    else if (key == KEY_DOWN) { if (dg_mail_sel < 3u) dg_mail_sel++; }
                }
                dg_render();
                continue;
            }
            /* 第三期应用：任务管理器/包管理器/图片查看器/截图工具/视频播放器 */
            if (w->icon >= 12u && w->icon <= 16u) {
                if (key == KEY_ESC) {
                    dg_win_close();
                } else if (key == KEY_M) {
                    dg_move_mode = 1u;
                } else if (w->icon == 12u && key == KEY_1) {
                    dg_tm_refresh++;
                } else if (w->icon == 13u) {
                    if (key == KEY_UP) { if (dg_pkg_sel > 0u) dg_pkg_sel--; }
                    else if (key == KEY_DOWN) { if (dg_pkg_sel < 19u) dg_pkg_sel++; }
                } else if (w->icon == 14u) {
                    if (key >= KEY_1 && key <= KEY_3) dg_img_idx = key - KEY_1;
                } else if (w->icon == 15u) {
                    if (key == KEY_ENTER) dg_cap_cnt++;
                } else if (w->icon == 16u) {
                    if (key == KEY_P) dg_vid_play = dg_vid_play ? 0u : 1u;
                    else if (key == KEY_N) dg_vid_seek++;
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
        } else if (key >= KEY_1 && key <= KEY_9) {
            dg_win_open(key - KEY_1);                    /* 1-9 → 图标0-8 */
        } else if (key == KEY_0) {
            dg_win_open(9u);                             /* 0 → 时钟 */
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
    con_puts("  XOS desktop exited to text terminal. Type 'desktop' to re-enter.\n");
    con_flush();
}
