/* ============================================================================
 * XOS 鼠标驱动子系统头文件
 * 自研实现：PS/2 鼠标协议(0x60/0x64)/数据包解析(3-4-5字节)/相对位移累计/
 * 按键状态跟踪(边沿+双击)/滚轮与水平滚动/指针加速度与平滑/光标位置裁剪/
 * USB HID 报告解析/多鼠标槽位/热插拔/灵敏度与包模式配置/输入事件队列。
 * 全部自研，不依赖外部库；VBox 下以 PS/2 端口探测 + 软件注入真实验证。
 * ========================================================================== */
#ifndef XOS_MOUSE_H
#define XOS_MOUSE_H

#include "types.h"

#define MOUSE_MAGIC         0x4D4F5500u  /* "MOU\0" */
#define MOUSE_EV_QUEUE      64u          /* 输入事件队列深度 */
#define MOUSE_SLOTS_MAX     4u           /* 多鼠标槽位 */
#define MOUSE_SCREEN_DEF_W  640u         /* 默认屏幕逻辑宽度(像素) */
#define MOUSE_SCREEN_DEF_H  480u         /* 默认屏幕逻辑高度(像素) */
#define MOUSE_ACCEL_MAX     3u           /* 加速度档位上限 */
#define MOUSE_SENS_MIN      1u           /* 灵敏度下限 */
#define MOUSE_SENS_MAX      16u          /* 灵敏度上限 */
#define MOUSE_SENS_DEF      4u           /* 默认灵敏度 */
#define MOUSE_PKT_MIN       3u
#define MOUSE_PKT_MAX       5u
#define MOUSE_TICK_MS       10u          /* 单调 tick 粒度(毫秒, 逻辑) */

/* 按键位 */
#define MOUSE_BTN_LEFT      0x01u
#define MOUSE_BTN_RIGHT     0x02u
#define MOUSE_BTN_MIDDLE    0x04u
#define MOUSE_BTN_SIDE1     0x08u
#define MOUSE_BTN_SIDE2     0x10u

/* 鼠标类型 */
#define MOUSE_TYPE_PS2      1u
#define MOUSE_TYPE_USB      2u

/* 加速度档 */
#define MOUSE_ACCEL_NONE    0u
#define MOUSE_ACCEL_LOW     1u
#define MOUSE_ACCEL_MED     2u
#define MOUSE_ACCEL_HIGH    3u

/* 事件类型 */
#define MOUSE_EV_MOVE       0u   /* 位移（含裁剪后实际落点） */
#define MOUSE_EV_BTN_DOWN   1u
#define MOUSE_EV_BTN_UP     2u
#define MOUSE_EV_WHEEL      3u
#define MOUSE_EV_HWHEEL     4u
#define MOUSE_EV_DOUBLE     5u
#define MOUSE_EV_ATTACH     6u
#define MOUSE_EV_DETACH     7u

/* ---------------- 输入事件 ---------------- */
typedef struct {
    u32  type;        /* MOUSE_EV_* */
    u32  button;      /* 按键位 */
    i32  dx;          /* 本次位移 X（加速/平滑/裁剪后） */
    i32  dy;          /* 本次位移 Y */
    i32  wheel;       /* 垂直滚轮（带符号） */
    i32  hwheel;      /* 水平滚轮（带符号） */
    u32  x;           /* 裁剪后的光标 X */
    u32  y;           /* 裁剪后的光标 Y */
    u32  slot;        /* 来源鼠标槽位 */
} mouse_event_t;

/* ---------------- 鼠标槽位 ---------------- */
typedef struct {
    u32 present;      /* 已连接 */
    u32 type;         /* MOUSE_TYPE_PS2 / MOUSE_TYPE_USB */
    u32 pkt_mode;     /* 3 / 4 / 5 字节 */
    u32 sample_rate;  /* 采样率(Hz) */
    u32 resolution;   /* 分辨率 */
    u32 buttons;      /* 当前按键位掩码 */
    u32 events_in;    /* 本槽注入事件计数 */
    u32 events_out;   /* 本槽读出事件计数 */
    u32 errors;       /* 解析错误计数 */
} mouse_slot_t;

/* ---------------- 公共接口 ---------------- */
void   mse_init(void);                       /* PS/2 初始化 + 默认槽位 */
int    mse_poll(void);                       /* 轮询 PS/2 辅助口（真实端口，不阻塞） */
void   mse_inject_pkt(u32 slot, const u8 *pkt, u32 len);  /* 注入数据包 */
int    mse_read_event(mouse_event_t *ev);    /* 非阻塞取事件 */
u32    mse_ev_count(void);
void   mse_set_screen(u32 w, u32 h);         /* 设置逻辑屏幕边界 */
u32    mse_get_screen(u32 *w, u32 *h);
void   mse_get_pos(u32 *x, u32 *y);          /* 当前光标位置 */
void   mse_set_pos(u32 x, u32 y);            /* 绝对定位光标（钳制屏幕内） */
u32    mse_get_buttons(void);                /* 当前按键位掩码 */
int    mse_set_sens(u32 s);                  /* 灵敏度 1..16 */
u32    mse_get_sens(void);
int    mse_set_accel(u32 a);                 /* 加速度档 0..3 */
u32    mse_get_accel(void);
int    mse_set_smooth(u32 on);               /* 平滑滤波开关 */
u32    mse_get_smooth(void);
int    mse_set_dbl(u32 on, u32 window);      /* 双击检测开关与窗口(tick) */
int    mse_set_pkt_mode(u32 slot, u32 mode); /* 3/4/5 字节包模式 */
int    mse_attach(u32 type, u32 slot);       /* 热插拔：连接 */
int    mse_detach(u32 slot);                 /* 热插拔：断开 */
u32    mse_slot_state(u32 slot, mouse_slot_t *out);
int    mse_hid_report(u32 slot, const u8 *r, u32 len);  /* USB HID 报告解析 */
void   mse_dump(void);
u32    mse_selftest(void);

#endif /* XOS_MOUSE_H */
