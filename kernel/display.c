/* ============================================================================
 * XOS 显示驱动子系统实现
 * 自研：VGA 文本 / VBE 探测(Bochs 0x1CE/0x1CF) / 帧缓冲 / 模式 /
 * 像素格式 / 双缓冲 / 硬件光标 / VSync / EDID / 多显示器 / 缩放 /
 * 伽马亮度 / 2D 软件加速 / DRM-KMS / 省电。
 * VBox 真机底座为 VGA 文本模式；图形能力探测失败自动降级。
 * ========================================================================== */
#include "display.h"
#include "console.h"

/* ---------------- 端口 I/O ---------------- */
static inline void outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(u16 port, u16 val)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---------------- 状态与模式表 ---------------- */
static disp_state_t g_disp = { DISP_PWR_ON, 0u, 0u, 0u, 0u, 0u, 100u, 128u };
static disp_head_t  g_heads[DISP_MAX_HEADS];
static disp_kms_obj_t g_kms[3];   /* 0=CRTC 1=Encoder 2=Connector */
static u8           g_backbuf[DISP_FB_BACK_SIZE];   /* 双缓冲后备区 */
static u8           g_gamma[DISP_GAMMA_SIZE];       /* 伽马 LUT */

static disp_mode_t g_modes[DISP_MODES_MAX] = {
    { 80u,  25u,  4u,  PIXFMT_VGA_TEXT, MODE_TEXT, 0x000B8000u },  /* 0: VGA 文本 */
    { 320u, 200u, 8u,  PIXFMT_8BPP,    MODE_GFX, 0x000A0000u },    /* 1: 320x200x8 */
    { 640u, 480u, 16u, PIXFMT_16BPP,   MODE_GFX, 0x00E00000u },    /* 2: 640x480x16 */
    { 800u, 600u, 24u, PIXFMT_24BPP,   MODE_GFX, 0x00E00000u },    /* 3: 800x600x24 */
    { 1024u,768u,32u, PIXFMT_32BPP,   MODE_GFX, 0x00E00000u },     /* 4: 1024x768x32 */
    { 640u, 480u, 32u, PIXFMT_32BPP,   MODE_GFX | MODE_VBE, 0x00E00000u },  /* 5: VBE */
    { 800u, 600u, 32u, PIXFMT_32BPP,   MODE_GFX | MODE_VBE, 0x00E00000u },  /* 6: VBE */
    { 1024u,768u,32u, PIXFMT_32BPP,   MODE_GFX | MODE_VBE, 0x00E00000u },  /* 7: VBE */
};

static u32 g_vbe_ok;       /* Bochs VBE 能力探测结果 */
static u32 g_mode_count;   /* 实际可用模式数（文本+VBE 或纯文本） */

/* ---------------- VGA 文本硬件 ---------------- */
static void vga_write_cursor(u16 pos)
{
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}
/* ---------------- Bochs VBE 扩展探测（0x1CE/0x1CF） ---------------- */
static void vbe_write(u16 idx, u16 val)
{
    outw(0x1CE, idx);
    outw(0x1CF, val);
}
static u16 vbe_read(u16 idx)
{
    outw(0x1CE, idx);
    return inw(0x1CF);
}
static void vbe_probe(void)
{
    u16 sig, ver;
    /* Bochs VBE: 索引 0 读能力；探测失败视为不支持 */
    sig = vbe_read(0x0000u);          /* VBE_DISPI_INDEX_ID */
    ver = sig;
    if (sig == 0xB0C0u || (sig & 0xFFF0u) == 0xB0C0u) {
        g_vbe_ok = 1u;
        con_printf("  [display] VBE detected id=0x%04x\n", (u32)ver);
    } else {
        g_vbe_ok = 0u;
        con_printf("  [display] VBE not present (id=0x%04x), text mode only\n", (u32)sig);
    }
}

/* ---------------- 初始化 ---------------- */
void display_init(void)
{
    u32 i;
    g_disp.head_count = 1u;
    g_disp.power = DISP_PWR_ON;
    g_disp.cur_mode = 0u;
    g_disp.vsync_count = 0u;
    g_disp.flip_count = 0u;
    g_disp.gamma_enabled = 0u;
    g_disp.scale = 100u;
    g_disp.brightness = 128u;

    for (i = 0u; i < DISP_MAX_HEADS; i++) {
        g_heads[i].present = (i == 0u) ? 1u : 0u;
        g_heads[i].state = DISP_ON;
        g_heads[i].width = g_modes[0].width;
        g_heads[i].height = g_modes[0].height;
        g_heads[i].pixfmt = g_modes[0].pixfmt;
        g_heads[i].mode_idx = 0u;
    }
    for (i = 0u; i < 3u; i++) {
        g_kms[i].active = 1u;
        g_kms[i].id = i;
        g_kms[i].type = i;            /* 0=CRTC 1=Encoder 2=Connector */
        g_kms[i].width = 0u; g_kms[i].height = 0u;
        g_kms[i].state = 0u;
    }
    for (i = 0u; i < DISP_GAMMA_SIZE; i++) g_gamma[i] = (u8)i;

    vbe_probe();
    if (g_vbe_ok) g_mode_count = DISP_MODES_MAX;   /* 文本 + 图形 + VBE */
    else          g_mode_count = 1u;               /* 仅文本 */
    display_cursor_set(1u, 0u, 0u);
}

u32 display_mode_count(void) { return g_mode_count; }

int display_get_mode(u32 idx, disp_mode_t *out)
{
    if (idx >= DISP_MODES_MAX || out == (disp_mode_t *)0) return -1;
    *out = g_modes[idx];
    return 0;
}

/* ---------------- 模式切换 ---------------- */
int display_set_mode(u32 idx)
{
    u32 i;
    if (idx >= g_mode_count) return -1;
    g_disp.cur_mode = idx;
    for (i = 0u; i < DISP_MAX_HEADS; i++) {
        if (g_heads[i].present) {
            g_heads[i].width = g_modes[idx].width;
            g_heads[i].height = g_modes[idx].height;
            g_heads[i].pixfmt = g_modes[idx].pixfmt;
            g_heads[i].mode_idx = idx;
        }
    }
    if (g_modes[idx].pixfmt == PIXFMT_VGA_TEXT) {
        /* 文本模式：重置光标到左上 */
        vga_write_cursor(0);
    } else if (g_modes[idx].flags & MODE_VBE) {
        /* VBE 图形模式：Bochs VBE 切换（LFB 启用）
         * 寄存器索引（VBE_DISPI 规范）：0=ID 1=XRES 2=YRES 3=BPP 4=ENABLE */
        vbe_write(0x0004u, 0x0000u);          /* ENABLE = 0 (DISABLED) */
        vbe_write(0x0001u, (u16)g_modes[idx].width);    /* XRES */
        vbe_write(0x0002u, (u16)g_modes[idx].height);   /* YRES */
        vbe_write(0x0003u, 0x0020u);          /* BPP=32 */
        vbe_write(0x0004u, 0x0001u | 0x0004u);  /* ENABLE(bit0) + LFB(bit2) */
        con_printf("  [display] VBE mode: x=%u y=%u bpp=%u en=0x%04x\n",
                   (u32)vbe_read(0x0001u), (u32)vbe_read(0x0002u),
                   (u32)vbe_read(0x0003u), (u32)vbe_read(0x0004u));
        con_flush();
    }
    return 0;
}

/* ---------------- 帧缓冲访问 ---------------- */
void display_fb_write(u32 off, const void *src, u32 len)
{
    u8 *fb;
    u32 i;
    if (off + len > DISP_FB_BACK_SIZE) len = DISP_FB_BACK_SIZE - off;
    if (src == (const void *)0) return;
    /* 后备区写入（双缓冲语义） */
    for (i = 0u; i < len; i++) g_backbuf[off + i] = ((const u8 *)src)[i];
    fb = (u8 *)g_modes[g_disp.cur_mode].fb_base;
    for (i = 0u; i < len; i++) fb[off + i] = g_backbuf[off + i];
}

void display_fb_read(u32 off, void *dst, u32 len)
{
    u32 i;
    if (off + len > DISP_FB_BACK_SIZE) len = DISP_FB_BACK_SIZE - off;
    for (i = 0u; i < len; i++) ((u8 *)dst)[i] = g_backbuf[off + i];
}

void display_flip(void)
{
    u32 i;
    u8 *fb = (u8 *)g_modes[g_disp.cur_mode].fb_base;
    for (i = 0u; i < DISP_FB_BACK_SIZE; i++) fb[i] = g_backbuf[i];
    g_disp.flip_count++;
}

/* ---------------- 硬件光标 ---------------- */
void display_cursor_set(u32 on, u32 row, u32 col)
{
    u16 pos = (u16)(row * g_modes[g_disp.cur_mode].width + col);
    if (on) {
        outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);   /* 光标起始行 */
        outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);   /* 光标结束行 */
        vga_write_cursor(pos);
    } else {
        outb(0x3D4, 0x0A); outb(0x3D5, 0x20);   /* 隐藏光标 */
    }
}

/* ---------------- 垂直同步 ---------------- */
u32 display_vsync_wait(void)
{
    u32 spins = 0u;
    /* 等待垂直回扫状态位（0x3DA bit3）；超时保护 */
    while ((inb(0x3DA) & 0x08u) == 0u) {
        if (++spins > 100000u) break;
    }
    while ((inb(0x3DA) & 0x08u) != 0u) {
        if (++spins > 100000u) break;
    }
    g_disp.vsync_count++;
    return g_disp.vsync_count;
}

/* ---------------- EDID 解析 ---------------- */
int display_edid_parse(const u8 *raw, disp_edid_t *out)
{
    u8 sum = 0u;
    u32 i;
    if (raw == (const u8 *)0 || out == (disp_edid_t *)0) return -1;
    for (i = 0u; i < DISP_EDID_SIZE; i++) sum = (u8)(sum + raw[i]);
    if (sum != 0u) return -2;                      /* 校验和错误 */
    for (i = 0u; i < DISP_EDID_SIZE; i++) out->raw[i] = raw[i];
    out->valid = 1u;
    out->manufacturer = (u16)(((raw[8] & 0x7Cu) << 5) | (raw[9] & 0x7Fu));
    out->product = (u16)((raw[10] << 8) | raw[11]);
    out->made_year = 1990u + raw[17];
    /* 首选分辨率（详细时序描述块 54-71 字节：h/8 × 8, v/8 × 8） */
    out->native_width  = (u32)(raw[54] + ((raw[56] & 0xF0u) << 4)) * 8u;
    out->native_height = (u32)(raw[55] + ((raw[56] & 0x0Fu) << 8)) * 8u;
    return 0;
}

/* ---------------- 多显示器 ---------------- */
int display_head_state(u32 head, disp_head_t *out)
{
    if (head >= DISP_MAX_HEADS || out == (disp_head_t *)0) return -1;
    *out = g_heads[head];
    return 0;
}

/* ---------------- 缩放 ---------------- */
int display_set_scale(u32 percent)
{
    if (percent < 50u || percent > 400u) return -1;
    g_disp.scale = percent;
    return 0;
}

/* ---------------- 伽马与亮度 ---------------- */
int display_set_gamma(u32 on)
{
    g_disp.gamma_enabled = on ? 1u : 0u;
    return 0;
}
int display_set_brightness(u32 v)
{
    if (v > 255u) return -1;
    g_disp.brightness = v;
    return 0;
}

/* ---------------- 2D 软件加速 ---------------- */
static void px_set(u32 x, u32 y, u32 color)
{
    u32 w = g_modes[g_disp.cur_mode].width;
    u32 h = g_modes[g_disp.cur_mode].height;
    u32 bpp = g_modes[g_disp.cur_mode].bpp;
    u8 *fb;
    if (x >= w || y >= h) return;
    fb = (u8 *)g_modes[g_disp.cur_mode].fb_base;
    if (bpp == 8u)      fb[y * w + x] = (u8)color;
    else if (bpp == 16u) { u16 *p = (u16 *)fb; p[y * w + x] = (u16)color; }
    else if (bpp == 24u) { u8 *p = fb + (y * w + x) * 3u; p[0]=(u8)color; p[1]=(u8)(color>>8); p[2]=(u8)(color>>16); }
    else if (bpp == 32u) { u32 *p = (u32 *)fb; p[y * w + x] = color; }
}

int display_raster_fill(u32 x, u32 y, u32 w, u32 h, u32 color, u32 rop)
{
    u32 i, j;
    for (j = 0u; j < h; j++) {
        for (i = 0u; i < w; i++) {
            u32 c = color;
            if (rop == RASTER_XOR)  c ^= 0xFFFFFFu;
            if (rop == RASTER_AND)  c &= 0x0F0F0Fu;
            px_set(x + i, y + j, c);
        }
    }
    return 0;
}

int display_raster_blit(const u8 *src, u32 x, u32 y, u32 w, u32 h)
{
    u32 i, j;
    for (j = 0u; j < h; j++) {
        for (i = 0u; i < w; i++) {
            px_set(x + i, y + j, (u32)src[j * w + i]);
        }
    }
    return 0;
}

int display_raster_line(u32 x0, u32 y0, u32 x1, u32 y1, u32 color)
{
    /* Bresenham 直线 */
    i32 dx, dy, sx, sy, err, e2;
    dx = (x1 > x0) ? (i32)(x1 - x0) : -(i32)(x0 - x1);
    dy = (y1 > y0) ? (i32)(y1 - y0) : -(i32)(y0 - y1);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    for (;;) {
        px_set(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err + err;
        if (e2 >= -dy) { err -= dy; x0 += (u32)sx; }
        if (e2 <= dx)  { err += dx; y0 += (u32)sy; }
    }
    return 0;
}

/* ---------------- DRM/KMS 对象 ---------------- */
int display_kms_set(u32 crtc_w, u32 crtc_h)
{
    g_kms[0].width = crtc_w; g_kms[0].height = crtc_h;
    g_kms[0].state = 2u;                       /* 已提交 */
    g_kms[1].state = 1u;                       /* Encoder 已配置 */
    g_kms[2].state = 1u;
    g_kms[2].width = g_modes[g_disp.cur_mode].width;
    g_kms[2].height = g_modes[g_disp.cur_mode].height;
    return 0;
}

/* ---------------- 省电 ---------------- */
int display_suspend(void)
{
    if (g_disp.power == DISP_PWR_ON) {
        g_disp.power = DISP_PWR_SUSPEND;
        for (u32 i = 0u; i < DISP_MAX_HEADS; i++) {
            if (g_heads[i].present) g_heads[i].state = DISP_SUSPEND;
        }
        outb(0x3C0, 0x20);                     /* 关闭屏幕（VGA 属性控制器） */
    }
    return 0;
}
int display_resume(void)
{
    if (g_disp.power != DISP_PWR_ON) {
        g_disp.power = DISP_PWR_ON;
        for (u32 i = 0u; i < DISP_MAX_HEADS; i++) {
            if (g_heads[i].present) g_heads[i].state = DISP_ON;
        }
        outb(0x3C0, 0x20 | 0x01);              /* 恢复屏幕 */
        display_flip();                        /* 恢复前台内容 */
    }
    return 0;
}

/* ---------------- 导出 ---------------- */
void display_dump(void)
{
    con_puts("  Display subsystem dump:\n");
    con_puts("    modes=");
    con_put_dec(g_mode_count);
    con_puts(" vbe=");
    con_put_dec(g_vbe_ok);
    con_puts(" cur=");
    con_put_dec(g_disp.cur_mode);
    con_puts(" power=");
    con_put_dec(g_disp.power);
    con_puts(" vsync=");
    con_put_dec(g_disp.vsync_count);
    con_puts(" flip=");
    con_put_dec(g_disp.flip_count);
    con_puts(" heads=");
    con_put_dec(g_disp.head_count);
    con_puts(" gamma=");
    con_put_dec(g_disp.gamma_enabled);
    con_puts(" scale=");
    con_put_dec(g_disp.scale);
    con_puts(" bright=");
    con_put_dec(g_disp.brightness);
    con_puts("\n");
    con_puts("    heads: ");
    for (u32 i = 0u; i < DISP_MAX_HEADS; i++) {
        if (g_heads[i].present) {
            con_puts(" h");
            con_put_dec(i);
            con_puts("=");
            con_put_dec(g_heads[i].width);
            con_putc('x');
            con_put_dec(g_heads[i].height);
            con_puts(" st");
            con_put_dec(g_heads[i].state);
        }
    }
    con_puts("\n");
    con_puts("    kms: crtc=");
    con_put_dec(g_kms[0].state);
    con_puts(" enc=");
    con_put_dec(g_kms[1].state);
    con_puts(" conn=");
    con_put_dec(g_kms[2].state);
    con_puts("\n");
}

/* ---------------- 自检 ---------------- */
u32 display_selftest(void)
{
    static u8 fbdata[256];
    u32 i;
    disp_mode_t m;
    disp_head_t h;
    disp_edid_t edid;
    u8 edid_raw[DISP_EDID_SIZE];

    /* 1: 模式表与当前模式 */
    if (display_mode_count() < 1u) return 1;
    if (display_get_mode(g_disp.cur_mode, &m) != 0) return 2;
    if (m.width == 0u || m.height == 0u) return 3;

    /* 2: 帧缓冲写读回环 */
    for (i = 0u; i < 64u; i++) fbdata[i] = (u8)(i + 1u);
    display_fb_write(0u, fbdata, 64u);
    for (i = 0u; i < 256u; i++) fbdata[i] = 0u;
    display_fb_read(0u, fbdata, 64u);
    if (fbdata[0] != 1u || fbdata[63] != 64u) return 4;

    /* 3: 双缓冲翻转计数 */
    display_flip();
    if (g_disp.flip_count == 0u) return 5;

    /* 4: 硬件光标 */
    display_cursor_set(1u, 1u, 5u);
    display_cursor_set(0u, 0u, 0u);

    /* 5: 垂直同步 */
    if (display_vsync_wait() == 0u) return 6;

    /* 6: 2D 填充 / 直线（软件加速路径执行） */
    if (display_raster_fill(0u, 0u, 16u, 16u, 0x00FFFFFFu, RASTER_COPY) != 0) return 7;
    if (display_raster_line(0u, 0u, 63u, 63u, 0xFF0000u) != 0) return 8;
    if (display_raster_blit(fbdata, 8u, 8u, 16u, 16u) != 0) return 9;
    if (display_raster_fill(0u, 0u, 8u, 8u, 0x0FFu, RASTER_XOR) != 0) return 10;
    if (display_raster_fill(0u, 0u, 8u, 8u, 0x0F0F0Fu, RASTER_AND) != 0) return 11;

    /* 7: 模式切换（图形模式记录 + VBE 分支执行） */
    if (g_mode_count > 1u) {
        if (display_set_mode(2u) != 0) return 12;
        if (g_heads[0].width != 640u || g_heads[0].height != 480u) return 13;
        if (display_set_mode(0u) != 0) return 14;   /* 切回文本 */
    }

    /* 8: 像素格式切换（VBE 分支真实执行） */
    if (g_vbe_ok) {
        if (display_set_mode(6u) != 0) return 15;
        if (g_heads[0].width != 800u) return 16;
        if (display_set_mode(0u) != 0) return 17;
    }

    /* 9: EDID 解析（构造校验和正确的 128B 样本） */
    for (i = 0u; i < DISP_EDID_SIZE; i++) edid_raw[i] = 0u;
    edid_raw[0] = 0x00u; edid_raw[1] = 0xFFu;         /* 头 */
    edid_raw[2] = 0xFFu; edid_raw[3] = 0xFFu;
    edid_raw[4] = 0xFFu; edid_raw[5] = 0xFFu;
    edid_raw[6] = 0xFFu; edid_raw[7] = 0x00u;
    edid_raw[8] = 0x10u; edid_raw[9] = 0xACu;        /* 制造商 */
    edid_raw[10] = 0x00u; edid_raw[11] = 0x01u;      /* 产品码 */
    edid_raw[17] = 24u;                              /* 生产年份 2014 */
    edid_raw[54] = 80u;                              /* 640 宽 */
    edid_raw[55] = 60u;                              /* 480 高 */
    edid_raw[56] = 0u;
    {
        u8 cksum = 0u;
        for (i = 0u; i < DISP_EDID_SIZE; i++) cksum = (u8)(cksum + edid_raw[i]);
        edid_raw[127] = (u8)(0u - cksum);            /* 补校验和 */
    }
    if (display_edid_parse(edid_raw, &edid) != 0) return 18;
    if (!edid.valid) return 19;
    if (edid.native_width != 640u || edid.native_height != 480u) return 20;
    if (edid.made_year != 2014u) return 21;
    {
        u8 bad[128];
        for (i = 0u; i < 128u; i++) bad[i] = 0u;
        bad[127] = 1u;                                   /* 校验和=1 != 0 → 拒绝 */
        if (display_edid_parse(bad, &edid) == 0) return 22;
    }

    /* 10: 多显示器状态 */
    if (display_head_state(0u, &h) != 0) return 23;
    if (!h.present) return 24;
    if (display_head_state(1u, &h) != 0) return 25;
    if (h.present) return 26;                     /* 头1 未连接 */
    if (display_head_state(2u, &h) == 0) return 27;  /* 越界拒绝 */

    /* 11: 缩放 / 亮度 / 伽马 */
    if (display_set_scale(150u) != 0) return 28;
    if (g_disp.scale != 150u) return 29;
    if (display_set_scale(10u) == 0) return 30;   /* 越界拒绝 */
    if (display_set_brightness(200u) != 0) return 31;
    if (g_disp.brightness != 200u) return 32;
    if (display_set_brightness(999u) == 0) return 33;
    if (display_set_gamma(1u) != 0) return 34;
    if (!g_disp.gamma_enabled) return 35;

    /* 12: KMS 对象 */
    if (display_kms_set(640u, 480u) != 0) return 36;
    if (g_kms[0].state != 2u) return 37;
    if (g_kms[2].width == 0u) return 38;

    /* 13: 省电休眠/恢复 */
    if (display_suspend() != 0) return 39;
    if (g_disp.power != DISP_PWR_SUSPEND) return 40;
    if (g_heads[0].state != DISP_SUSPEND) return 41;
    if (display_resume() != 0) return 42;
    if (g_disp.power != DISP_PWR_ON) return 43;
    if (g_heads[0].state != DISP_ON) return 44;
    if (display_resume() != 0) return 45;         /* 幂等 */
    display_suspend(); display_resume();

    display_dump();
    return 0;
}
