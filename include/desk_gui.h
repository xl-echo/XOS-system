/* ============================================================================
 * XOS 图形桌面（真机 GUI 层）接口
 * 完全自研：Bochs VBE 640x480x32 帧缓冲 + 桌面渲染 + 键盘事件循环。
 * ========================================================================== */
#ifndef XOS_DESK_GUI_H
#define XOS_DESK_GUI_H

#include "types.h"

#define DG_W          640u
#define DG_H          480u
#define DG_LFB        0xE0000000u      /* Bochs VBE 线性帧缓冲基址 */
#define DG_TASKBAR    30u              /* 任务栏高度（像素） */
#define DG_ICON_N     12u              /* 桌面图标数量 */

int  desk_gui_init(void);              /* 切 VBE 640x480x32 + 映射 LFB；0=成功 */
void desk_gui_run(void);               /* 渲染桌面 + 事件循环；Esc 退出回文本 Shell */
/* 首次使用欢迎向导（首登创建账户后置位，Enter 关闭） */
extern u32 dg_welcome;

/* 渲染原语（供登录界面等图形模块复用） */
u32  dg_rgb(u32 r, u32 g, u32 b);
extern volatile u32 *dg_fb;
void dg_fill(u32 x, u32 y, u32 w, u32 h, u32 color);
void dg_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void dg_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);

#endif /* XOS_DESK_GUI_H */
