/* ============================================================================
 * XOS 显示驱动子系统头文件
 * 自研实现：VGA 文本模式 / VBE 图形模式探测 / 帧缓冲管理 / 模式切换 /
 * 像素格式 / 双缓冲与页面翻转 / 硬件光标 / 垂直同步 / EDID 解析 /
 * 多显示器 / 分辨率缩放 / 亮度伽马 / 2D 软件加速 / DRM-KMS 对象 / 省电。
 * 全部自研，不依赖任何外部库；VBox 下以 VGA 文本为真实底座，
 * Bochs VBE I/O 端口(0x1CE/0x1CF)探测图形能力，不可用则降级。
 * ========================================================================== */
#ifndef XOS_DISPLAY_H
#define XOS_DISPLAY_H

#include "types.h"

#define DISP_MAGIC         0x44495350u   /* "DISP" */
#define DISP_MAX_HEADS     2u
#define DISP_MODES_MAX     8u
#define DISP_EDID_SIZE     128u
#define DISP_GAMMA_SIZE    256u
#define DISP_FB_BASE       0x000B8000u   /* VGA 文本帧缓冲 */
#define DISP_FB_BACK_SIZE  4096u         /* 双缓冲后备区（4KB=整帧，bss 裁剪） */

/* 像素格式 */
#define PIXFMT_VGA_TEXT  0u
#define PIXFMT_8BPP      1u
#define PIXFMT_15BPP     2u
#define PIXFMT_16BPP     3u
#define PIXFMT_24BPP     4u
#define PIXFMT_32BPP     5u

/* 显示头状态 */
#define DISP_OFF     0u
#define DISP_ON      1u
#define DISP_SUSPEND 2u

/* 电源状态 */
#define DISP_PWR_ON     0u
#define DISP_PWR_SUSPEND 1u
#define DISP_PWR_OFF    2u

/* 模式标志 */
#define MODE_TEXT 0x01u
#define MODE_GFX  0x02u
#define MODE_VBE  0x04u

/* 2D 光栅操作 */
#define RASTER_COPY 0u
#define RASTER_XOR  1u
#define RASTER_AND  2u

/* ---------------- 显示模式 ---------------- */
typedef struct {
    u32 width, height;      /* 像素（文本模式为字符列×行） */
    u32 bpp;                /* 颜色深度 */
    u32 pixfmt;             /* 像素格式 */
    u32 flags;              /* MODE_* */
    u32 fb_base;            /* 帧缓冲基址 */
} disp_mode_t;

/* ---------------- 显示头（多显示器） ---------------- */
typedef struct {
    u32 present;            /* 是否连接 */
    u32 state;              /* DISP_* */
    u32 width, height;
    u32 pixfmt;
    u32 mode_idx;
} disp_head_t;

/* ---------------- EDID（128B 结构） ---------------- */
typedef struct {
    u8  raw[DISP_EDID_SIZE];
    u8  valid;              /* 校验和通过 */
    u16 manufacturer;       /* 3 字符代码 */
    u16 product;            /* 产品码 */
    u32 made_year;          /* 生产年份 */
    u32 native_width, native_height;  /* 首选分辨率 */
} disp_edid_t;

/* ---------------- DRM/KMS 模式对象 ---------------- */
typedef struct {
    u32 active;             /* 对象是否激活 */
    u32 id;
    u32 type;               /* 0=CRTC 1=Encoder 2=Connector */
    u32 width, height;      /* CRTC 分辨率 / Connector 模式 */
    u32 state;              /* 0=空闲 1=已配置 2=已提交 */
} disp_kms_obj_t;

/* ---------------- 显示状态 ---------------- */
typedef struct {
    u32 power;              /* DISP_PWR_* */
    u32 head_count;         /* 活动显示头数 */
    u32 cur_mode;           /* 当前模式索引 */
    u32 vsync_count;        /* 垂直同步计数 */
    u32 flip_count;         /* 页面翻转计数 */
    u32 gamma_enabled;
    u32 scale;              /* 缩放百分比（100=原始） */
    u32 brightness;         /* 亮度 0..255（128=标准） */
} disp_state_t;

/* ---------------- 公共接口 ---------------- */
void   display_init(void);
int    display_set_mode(u32 idx);             /* 切换显示模式 */
int    display_get_mode(u32 idx, disp_mode_t *out);
u32    display_mode_count(void);
void   display_fb_write(u32 off, const void *src, u32 len);   /* 帧缓冲写 */
void   display_fb_read(u32 off, void *dst, u32 len);          /* 帧缓冲读 */
void   display_flip(void);                                    /* 页面翻转（后备→前台） */
void   display_cursor_set(u32 on, u32 row, u32 col);          /* 硬件光标 */
u32    display_vsync_wait(void);                              /* 等待垂直同步，返回计数 */
int    display_edid_parse(const u8 *raw, disp_edid_t *out);   /* EDID 解析 */
int    display_head_state(u32 head, disp_head_t *out);        /* 多显示器状态 */
int    display_set_scale(u32 percent);
int    display_set_gamma(u32 on);
int    display_set_brightness(u32 v);
int    display_raster_fill(u32 x, u32 y, u32 w, u32 h, u32 color, u32 rop);
int    display_raster_blit(const u8 *src, u32 x, u32 y, u32 w, u32 h);
int    display_raster_line(u32 x0, u32 y0, u32 x1, u32 y1, u32 color);
int    display_kms_set(u32 crtc_w, u32 crtc_h);               /* KMS 提交 */
int    display_suspend(void);
int    display_resume(void);
void   display_dump(void);
u32    display_selftest(void);

#endif /* XOS_DISPLAY_H */
