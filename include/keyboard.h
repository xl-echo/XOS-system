/* ============================================================================
 * XOS 键盘驱动子系统头文件
 * 自研实现：PS/2 控制器(0x60/0x64)/扫描码集1-2-3解析/修饰键状态机/
 * typematic 重复/LED 控制/组合键/键映射表/输入事件队列/USB HID 报告解析/
 * 多键盘槽位/热插拔/输入法组合/布局切换/按键过滤安全组合键/用户态分发。
 * 全部自研，不依赖外部库；VBox 下以 PS/2 端口探测 + 软件注入真实验证。
 * ========================================================================== */
#ifndef XOS_KEYBOARD_H
#define XOS_KEYBOARD_H

#include "types.h"

#define KBD_MAGIC          0x4B424400u  /* "KBD\0" */
#define KBD_EV_QUEUE       64u          /* 输入事件队列深度 */
#define KBD_SLOTS_MAX      4u           /* 多键盘槽位 */
#define KBD_KEYS_MAX       128u         /* 键码空间 */
#define KBD_LAYOUTS_MAX    3u           /* 布局数 */

/* 键码（扫描码 1 集 → 键码） */
#define KEY_NONE   0u
#define KEY_ESC    1u
#define KEY_TAB    2u
#define KEY_CAPS   3u
#define KEY_LSHIFT 4u
#define KEY_LCTRL  5u
#define KEY_LALT   6u
#define KEY_LWIN   7u
#define KEY_SPACE  8u
#define KEY_BACKSP 9u
#define KEY_ENTER  10u
#define KEY_RSHIFT 11u
#define KEY_RCTRL  12u
#define KEY_RALT   13u
#define KEY_INSERT 14u
#define KEY_DEL    15u
#define KEY_HOME   16u
#define KEY_END    17u
#define KEY_PGUP   18u
#define KEY_PGDN   19u
#define KEY_UP     20u
#define KEY_DOWN   21u
#define KEY_LEFT   22u
#define KEY_RIGHT  23u
#define KEY_NUMLK  24u
#define KEY_SCRLK  25u
#define KEY_F1     26u
#define KEY_F2     27u
#define KEY_F3     28u
#define KEY_F4     29u
#define KEY_F5     30u
#define KEY_F6     31u
#define KEY_F7     32u
#define KEY_F8     33u
#define KEY_F9     34u
#define KEY_F10    35u
#define KEY_F11    36u
#define KEY_F12    37u
#define KEY_A      38u
#define KEY_B      39u
#define KEY_C      40u
#define KEY_D      41u
#define KEY_E      42u
#define KEY_F      43u
#define KEY_G      44u
#define KEY_H      45u
#define KEY_I      46u
#define KEY_J      47u
#define KEY_K      48u
#define KEY_L      49u
#define KEY_M      50u
#define KEY_N      51u
#define KEY_O      52u
#define KEY_P      53u
#define KEY_Q      54u
#define KEY_R      55u
#define KEY_S      56u
#define KEY_T      57u
#define KEY_U      58u
#define KEY_V      59u
#define KEY_W      60u
#define KEY_X      61u
#define KEY_Y      62u
#define KEY_Z      63u
#define KEY_0      64u
#define KEY_1      65u
#define KEY_2      66u
#define KEY_3      67u
#define KEY_4      68u
#define KEY_5      69u
#define KEY_6      70u
#define KEY_7      71u
#define KEY_8      72u
#define KEY_9      73u
#define KEY_TICK   74u
#define KEY_MINUS  75u
#define KEY_EQUALS 76u
#define KEY_LBRACK 77u
#define KEY_RBRACK 78u
#define KEY_SEMI   79u
#define KEY_QUOTE  80u
#define KEY_BACKSL 81u
#define KEY_COMMA  82u
#define KEY_DOT    83u
#define KEY_SLASH  84u
#define KEY_KP0    85u
#define KEY_PLUS   86u

/* 修饰键位 */
#define MOD_SHIFT  0x01u
#define MOD_CTRL   0x02u
#define MOD_ALT    0x04u
#define MOD_CAPS   0x08u
#define MOD_NUMLK  0x10u
#define MOD_SCRLK  0x20u

/* 事件类型 */
#define EV_KEY_DOWN 0u
#define EV_KEY_UP   1u
#define EV_CHAR     2u
#define EV_HOTKEY   3u
#define EV_LAYOUT   4u

/* 扫描码集 */
#define SCAN_SET1 1u
#define SCAN_SET2 2u
#define SCAN_SET3 3u

/* 键盘槽位类型 */
#define KB_PS2  0u
#define KB_USB  1u

/* ---------------- 输入事件 ---------------- */
typedef struct {
    u32 type;        /* EV_* */
    u32 key;         /* KEY_* 键码 */
    u32 scancode;    /* 原始扫描码 */
    u32 mods;        /* MOD_* 状态 */
    u32 ch;          /* EV_CHAR 的字符 */
    u32 slot;        /* 来源键盘槽位 */
} kbd_event_t;

/* ---------------- 键盘槽位 ---------------- */
typedef struct {
    u32 present;      /* 已连接 */
    u32 type;         /* KB_PS2 / KB_USB */
    u32 scan_set;     /* 当前扫描码集 */
    u32 layout;       /* 当前布局索引 */
    u32 events_in;    /* 本槽注入事件计数 */
} kbd_slot_t;

/* ---------------- 布局 ---------------- */
typedef struct {
    const char *name;
    u8 unshift[KBD_KEYS_MAX];   /* 无修饰字符 */
    u8 shifted[KBD_KEYS_MAX];   /* Shift 字符 */
} kbd_layout_t;

/* ---------------- 公共接口 ---------------- */
void   kbd_init(void);
int    kbd_poll(void);                       /* 轮询 PS/2 控制器（真实端口） */
void   kbd_inject(u32 slot, u32 scancode, int down);   /* 注入扫描码 */
int    kbd_read_event(kbd_event_t *ev);      /* 非阻塞取事件 */
u32    kbd_ev_count(void);
u32    kbd_mods(void);
int    kbd_set_led(u32 num, u32 caps, u32 scroll);     /* LED 控制 */
int    kbd_set_scan_set(u32 set);            /* 切换扫描码集 */
int    kbd_register_hotkey(u32 mods, u32 key, u32 action);  /* 组合键注册 */
int    kbd_set_layout(u32 idx);              /* 布局切换 */
u32    kbd_layout_count(void);
int    kbd_attach(u32 type, u32 slot);       /* 热插拔：连接 */
int    kbd_detach(u32 slot);                 /* 热插拔：断开 */
u32    kbd_slot_state(u32 slot, kbd_slot_t *out);
int    kbd_filter_set(u32 enable);           /* 安全组合键过滤 */
void   kbd_dump(void);
u32    kbd_selftest(void);

#endif /* XOS_KEYBOARD_H */
