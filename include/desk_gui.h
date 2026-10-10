/* ============================================================================
 * XOS 图形桌面（真机 GUI 层）接口
 * 完全自研：Bochs VBE 帧缓冲 + 桌面渲染 + 键盘事件循环。
 * 分辨率自适应：DG_W/DG_H 为运行时变量，随显示模式(1024x768/800x600/640x480)
 * 自动变化，桌面图标网格、任务栏、托盘、时钟等布局按实际分辨率重排。
 * ========================================================================== */
#ifndef XOS_DESK_GUI_H
#define XOS_DESK_GUI_H

#include "types.h"

#define DG_W          g_dgw          /* 实际桌面宽度（自适应） */
#define DG_H          g_dgh          /* 实际桌面高度（自适应） */
#define DG_LFB        0xE0000000u    /* Bochs VBE 线性帧缓冲基址 */
#define DG_TASKBAR    g_tb_h         /* 任务栏高度：随字体缩放自适应（1x=30 2x=38 3x=46） */
#define DG_ICON_N     24u            /* 桌面图标数量（含三期 12 新应用） */

extern u32 g_dgw, g_dgh;             /* 当前桌面分辨率（desk_gui 初始化时按模式设置） */
extern u32 g_font_scale;             /* 全局字体缩放档位（1=8px 2=16px 3=24px） */
extern u32 g_tb_h;                   /* 任务栏高度（随字体缩放） */
void dg_font_scale_auto(void);       /* 按当前分辨率自动设置字体缩放档位 */

int  desk_gui_init(void);            /* 切 VBE 1024x768x32 + 映射 LFB；0=成功 */
void desk_gui_run(void);             /* 渲染桌面 + 事件循环；Esc 退出回文本 Shell */
/* 首次使用欢迎向导（首登创建账户后置位，Enter 关闭） */
extern u32 dg_welcome;

/* 渲染原语（供登录界面等图形模块复用） */
u32  dg_rgb(u32 r, u32 g, u32 b);
extern volatile u32 *dg_fb;
void dg_fill(u32 x, u32 y, u32 w, u32 h, u32 color);
void dg_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void dg_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);

#endif /* XOS_DESK_GUI_H */
