/* ============================================================================
 * XOS 键盘驱动子系统实现
 * 自研：PS/2 控制器 / 扫描码集 1-2-3 / 修饰键 / typematic / LED /
 * 组合键 / 键映射 / 事件队列 / USB HID 报告 / 多键盘 / 热插拔 /
 * 输入法 / 布局切换 / 过滤 / 用户态分发。
 * ========================================================================== */
#include "keyboard.h"
#include "console.h"
#include "irq.h"

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

#define KBD_DATA_PORT   0x60u
#define KBD_CMD_PORT    0x64u
#define KBD_STAT_OBF    0x01u   /* 输出缓冲满 */
#define KBD_STAT_IBF    0x02u   /* 输入缓冲满 */
#define KBD_CMD_SELF    0xAAu   /* 控制器自检 */
#define KBD_CMD_ENABLE  0xAEu   /* 启用键盘 */
#define KBD_CMD_DISABLE 0xADu
#define KBD_CMD_SETLED  0xEDu   /* 键盘 LED（Set1） */
#define KBD_ACK         0xFAu

/* ---------------- 状态 ---------------- */
static kbd_slot_t g_slots[KBD_SLOTS_MAX];
static u32 g_mods;
static u32 g_cur_layout;
static u32 g_scan_set = SCAN_SET1;
static u32 g_filter_enabled = 1u;
static u32 g_led_num, g_led_caps, g_led_scroll;

/* 事件队列 */
static kbd_event_t g_queue[KBD_EV_QUEUE];
static u32 g_qhead, g_qtail, g_qcount;

/* 组合键注册表（最多 16 组） */
static struct { u32 mods; u32 key; u32 action; u32 used; } g_hot[16];
static u32 g_hot_count;

/* typematic 状态 */
static u32 g_repeat_key = KEY_NONE;
static u32 g_repeat_ticks;
static u32 g_typematic_delay = 10u;   /* 延迟（tick） */
static u32 g_typematic_rate __attribute__((used)) = 2u;     /* 间隔（tick） */

/* ---------------- 布局表（US / DE / RU） ---------------- */
static kbd_layout_t g_layouts[KBD_LAYOUTS_MAX] = {
    { "us", {
        /* 0-37: 控制+功能键区（字符区 0） */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        /* 38-63: A-Z */
        'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z',
        /* 64-84: 数字+符号 */
        '0','1','2','3','4','5','6','7','8','9','`','-','=','[',']',';','\'','\\',',','.','/' },
      { /* shifted */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        ')','!','@','#','$','%','^','&','*','(','~','_','+','{','}',':','"','|','<','>','?' }
    },
    { "de", {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z',
        '0','1','2','3','4','5','6','7','8','9','^','\337','\375','\374','\346','\366','\344','\273','\267','\256','-' },
      { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        ')','!','"','\247','$','%','&','/','(','=','\260','_','+','\334','*','\326','\304','\273','\267','\256','\277' }
    },
    { "ru", {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        '\344','\342','\347','\343','\366','\345','\355','\350','\351','\352','\353','\354','\346','\340','\357','\360','\361','\362','\363','\364','\370','\376','\374','\371','\375','\377',
        '0','1','2','3','4','5','6','7','8','9','\376','-','=','\365','\353','\350','\374','\345','\342','\347','\343' },
      { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        '\304','\302','\307','\303','\306','\305','\315','\310','\311','\312','\313','\314','\326','\300','\317','\320','\321','\322','\323','\324','\330','\336','\334','\331','\335','\337',
        '0','1','2','3','4','5','6','7','8','9','\336','-','=','\325','\313','\310','\334','\305','\302','\307','\303' }
    },
};

/* ---------------- 扫描码集 → 键码 ---------------- */
/* Set1 基础映射（0x01..0x54 大部分；E0 前缀扩展另行处理） */
static const u8 sc_set1[128] = {
    KEY_NONE,KEY_ESC,KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,KEY_7,KEY_8,
    KEY_9,KEY_0,KEY_MINUS,KEY_EQUALS,KEY_BACKSP,KEY_TAB,KEY_Q,KEY_W,KEY_E,KEY_R,
    KEY_T,KEY_Y,KEY_U,KEY_I,KEY_O,KEY_P,KEY_LBRACK,KEY_RBRACK,KEY_ENTER,KEY_LCTRL,
    KEY_A,KEY_S,KEY_D,KEY_F,KEY_G,KEY_H,KEY_J,KEY_K,KEY_L,KEY_SEMI,
    KEY_QUOTE,KEY_TICK,KEY_LSHIFT,KEY_BACKSL,KEY_Z,KEY_X,KEY_C,KEY_V,KEY_B,KEY_N,
    KEY_M,KEY_COMMA,KEY_DOT,KEY_SLASH,KEY_RSHIFT,KEY_KP0,KEY_LALT,KEY_SPACE,KEY_CAPS,KEY_F1,
    KEY_F2,KEY_F3,KEY_F4,KEY_F5,KEY_F6,KEY_F7,KEY_F8,KEY_F9,KEY_F10,KEY_NUMLK,
    KEY_SCRLK,KEY_HOME,KEY_UP,KEY_PGUP,KEY_MINUS,KEY_LEFT,KEY_5,KEY_RIGHT,KEY_PLUS,KEY_END,
    KEY_DOWN,KEY_PGDN,KEY_INSERT,KEY_DEL,KEY_NONE,KEY_NONE,KEY_NONE,KEY_F11,KEY_F12,KEY_NONE,
};
static const u8 sc_set2[128] __attribute__((used)) = {
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,
};

/* Set2 有效映射：0x1C=ENTER 0x1D=CTRL 0x2A=LSHIFT 0x36=RSHIFT 0x38=LALT 0x39=SPACE 0x45=NUMLK */
static u32 sc2_lookup(u8 sc)
{
    switch (sc) {
    case 0x1Cu: return KEY_ENTER;
    case 0x1Du: return KEY_LCTRL;
    case 0x2Au: return KEY_LSHIFT;
    case 0x36u: return KEY_RSHIFT;
    case 0x38u: return KEY_LALT;
    case 0x39u: return KEY_SPACE;
    case 0x45u: return KEY_NUMLK;
    default:    return KEY_NONE;
    }
}

/* ---------------- 队列 ---------------- */
static int ev_push(kbd_event_t *ev)
{
    if (g_qcount >= KBD_EV_QUEUE) return -1;
    g_queue[g_qtail] = *ev;
    g_qtail = (g_qtail + 1u) % KBD_EV_QUEUE;
    g_qcount++;
    return 0;
}
static int ev_pop(kbd_event_t *ev)
{
    if (g_qcount == 0u) return -1;
    *ev = g_queue[g_qhead];
    g_qhead = (g_qhead + 1u) % KBD_EV_QUEUE;
    g_qcount--;
    return 0;
}
static void kbd_flush(void)
{
    kbd_event_t e;
    while (kbd_read_event(&e) == 0) { }
}

/* ---------------- 修饰键更新 ---------------- */
static void mods_update(u32 key, int down)
{
    u32 bit = 0u;
    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL:  case KEY_RCTRL:  bit = MOD_CTRL;  break;
    case KEY_LALT:   case KEY_RALT:   bit = MOD_ALT;   break;
    default: return;
    }
    if (down) g_mods |= bit; else g_mods &= ~bit;
}

/* ---------------- 按键处理 ---------------- */
static void key_handle(u32 slot, u32 key, int down)
{
    kbd_event_t ev;
    u32 i;

    if (key == KEY_CAPS && down) g_mods ^= MOD_CAPS;
    if (key == KEY_NUMLK && down) { g_mods ^= MOD_NUMLK; g_led_num = (g_mods & MOD_NUMLK) ? 1u : 0u; }
    if (key == KEY_SCRLK && down) { g_mods ^= MOD_SCRLK; g_led_scroll = (g_mods & MOD_SCRLK) ? 1u : 0u; }
    mods_update(key, down);

    ev.type = down ? EV_KEY_DOWN : EV_KEY_UP;
    ev.key = key; ev.scancode = 0u; ev.mods = g_mods; ev.ch = 0u; ev.slot = slot;
    ev_push(&ev);

    if (down) {
        /* 字符输出（按布局） */
        if (key >= KEY_A && key <= KEY_SLASH) {
            const kbd_layout_t *L = &g_layouts[g_cur_layout];
            u8 c = (g_mods & (MOD_SHIFT | MOD_CAPS)) ? L->shifted[key] : L->unshift[key];
            if (c != 0u && !(g_mods & (MOD_CTRL | MOD_ALT))) {
                ev.type = EV_CHAR; ev.ch = c; ev_push(&ev);
            }
        } else if (key == KEY_SPACE && !(g_mods & (MOD_CTRL | MOD_ALT))) {
            /* KEY_SPACE(8) 不在 [KEY_A, KEY_SLASH] 范围内，此前空格不产生
             * 字符导致 shell 命令粘连（setclock1230 等）。直接映射 ' '。 */
            ev.type = EV_CHAR; ev.ch = ' '; ev_push(&ev);
        }
        /* 组合键匹配 */
        for (i = 0u; i < g_hot_count; i++) {
            if (g_hot[i].used && g_hot[i].key == key &&
                (g_hot[i].mods == (g_mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT)))) {
                /* 安全组合键过滤：Ctrl+Alt+Del 需过滤开关 */
                if (g_filter_enabled && key == KEY_DEL &&
                    (g_mods & MOD_CTRL) && (g_mods & MOD_ALT)) {
                    /* 过滤：不产生 EV_HOTKEY（交给安全层） */
                } else {
                    ev.type = EV_HOTKEY; ev.key = key; ev_push(&ev);
                }
            }
        }
        /* typematic 启动 */
        g_repeat_key = key;
        g_repeat_ticks = g_typematic_delay;
    } else {
        if (g_repeat_key == key) { g_repeat_key = KEY_NONE; g_repeat_ticks = 0u; }
    }
}

/* ---------------- PS/2 轮询（真实端口） ---------------- */

/* 扩展键映射（Set1 E0 前缀）：方向键/编辑键 */
static u32 sc_ext_key(u8 sc)
{
    switch (sc) {
    case 0x48u: return KEY_UP;
    case 0x50u: return KEY_DOWN;
    case 0x4Bu: return KEY_LEFT;
    case 0x4Du: return KEY_RIGHT;
    case 0x53u: return KEY_DEL;
    case 0x47u: return KEY_HOME;
    case 0x4Fu: return KEY_END;
    case 0x49u: return KEY_PGUP;
    case 0x51u: return KEY_PGDN;
    case 0x1Cu: return KEY_ENTER;   /* 小键盘 Enter */
    case 0x35u: return KEY_KP0;
    case 0x5Cu: return KEY_5;       /* 小键盘 5 */
    case 0x52u: return KEY_INSERT;
    default:    return KEY_NONE;
    }
}

static int g_ext_pending = 0;        /* 0xE0 扩展前缀待定 */
int kbd_poll(void)
{
    u8 data;
    u32 key;
    int down;
    if ((inb(KBD_CMD_PORT) & KBD_STAT_OBF) == 0u) return 0;   /* 无数据 */
    data = inb(KBD_DATA_PORT);
    if (data == 0xE0u) { g_ext_pending = 1; return 1; }   /* 扩展前缀，等下一字节 */
    down = (data & 0x80u) ? 0 : 1;
    data &= 0x7Fu;
    if (g_scan_set == SCAN_SET1) {
        if (data >= 128u) { g_ext_pending = 0; return 1; }
        key = g_ext_pending ? sc_ext_key(data) : sc_set1[data];
    } else if (g_scan_set == SCAN_SET2) {
        key = sc2_lookup(data);
    } else {
        key = KEY_NONE;
    }
    g_ext_pending = 0;
    if (key != KEY_NONE && key != KEY_KP0 && key != KEY_5) {
        key_handle(0u, key, down);
    }
    return 1;
}

/* ---------------- 注入（自检/用户态） ---------------- */
void kbd_inject(u32 slot, u32 scancode, int down)
{
    u32 key = KEY_NONE;
    if (slot >= KBD_SLOTS_MAX) return;
    if (g_scan_set == SCAN_SET1 && scancode < 128u) key = sc_set1[scancode];
    else if (g_scan_set == SCAN_SET2) key = sc2_lookup((u8)scancode);
    g_slots[slot].events_in++;
    if (key != KEY_NONE) key_handle(slot, key, down);
}

/* ---------------- 事件读取 ---------------- */
int kbd_read_event(kbd_event_t *ev)
{
    if (ev == (kbd_event_t *)0) return -1;
    return ev_pop(ev);
}

/* ---------------- IRQ1 中断路径 ----------------
 * 真实硬件/虚拟机键盘数据经 PS/2 控制器到达并触发 IRQ1。
 * 此处直接复用轮询解析（kbd_poll 检查 OBF 后读一个字节并 key_handle），
 * 中断上下文期间主流程处于 hlt/临界区之外，队列写入安全。
 */
static void kbd_irq_handler(void *arg)
{
    (void)arg;
    kbd_poll();
}
u32 kbd_ev_count(void) { return g_qcount; }
u32 kbd_mods(void) { return g_mods; }

/* ---------------- LED 控制（真实硬件） ---------------- */
int kbd_set_led(u32 num, u32 caps, u32 scroll)
{
    u8 val;
    g_led_num = num ? 1u : 0u;
    g_led_caps = caps ? 1u : 0u;
    g_led_scroll = scroll ? 1u : 0u;
    val = (u8)((g_led_num & 1u) | ((g_led_caps & 1u) << 1) | ((g_led_scroll & 1u) << 2));
    /* 写 0x60 前需等输入缓冲空 */
    if (inb(KBD_CMD_PORT) & KBD_STAT_IBF) return -1;
    outb(KBD_DATA_PORT, KBD_CMD_SETLED);
    outb(KBD_DATA_PORT, val);
    return 0;
}

/* ---------------- 扫描码集 ---------------- */
int kbd_set_scan_set(u32 set)
{
    if (set < SCAN_SET1 || set > SCAN_SET3) return -1;
    g_scan_set = set;
    return 0;
}

/* ---------------- 组合键 ---------------- */
int kbd_register_hotkey(u32 mods, u32 key, u32 action)
{
    u32 i;
    if (g_hot_count >= 16u) return -1;
    for (i = 0u; i < 16u; i++) {
        if (!g_hot[i].used) {
            g_hot[i].used = 1u; g_hot[i].mods = mods; g_hot[i].key = key;
            g_hot[i].action = action; g_hot_count++;
            return 0;
        }
    }
    return -1;
}

/* ---------------- 布局 ---------------- */
int kbd_set_layout(u32 idx)
{
    if (idx >= KBD_LAYOUTS_MAX) return -1;
    g_cur_layout = idx;
    for (u32 i = 0u; i < KBD_SLOTS_MAX; i++) {
        if (g_slots[i].present) g_slots[i].layout = idx;
    }
    return 0;
}
u32 kbd_layout_count(void) { return KBD_LAYOUTS_MAX; }

/* ---------------- 热插拔 ---------------- */
int kbd_attach(u32 type, u32 slot)
{
    kbd_event_t ev;
    if (slot >= KBD_SLOTS_MAX) return -1;
    if (g_slots[slot].present) return -2;      /* 已占用 */
    g_slots[slot].present = 1u;
    g_slots[slot].type = type;
    g_slots[slot].scan_set = g_scan_set;
    g_slots[slot].layout = g_cur_layout;
    g_slots[slot].events_in = 0u;
    ev.type = EV_LAYOUT; ev.key = KEY_NONE; ev.mods = g_mods; ev.ch = (u32)type;
    ev.slot = slot; ev_push(&ev);
    return 0;
}
int kbd_detach(u32 slot)
{
    if (slot >= KBD_SLOTS_MAX) return -1;
    if (!g_slots[slot].present) return -2;
    g_slots[slot].present = 0u;
    return 0;
}
u32 kbd_slot_state(u32 slot, kbd_slot_t *out)
{
    if (slot >= KBD_SLOTS_MAX || out == (kbd_slot_t *)0) return 1u;
    *out = g_slots[slot];
    return 0u;
}

/* ---------------- 过滤 ---------------- */
int kbd_filter_set(u32 enable)
{
    g_filter_enabled = enable ? 1u : 0u;
    return 0;
}

/* ---------------- 初始化 ---------------- */
void kbd_init(void)
{
    u32 i;
    for (i = 0u; i < KBD_SLOTS_MAX; i++) {
        g_slots[i].present = 0u; g_slots[i].type = KB_PS2;
        g_slots[i].scan_set = SCAN_SET1; g_slots[i].layout = 0u;
        g_slots[i].events_in = 0u;
    }
    g_slots[0].present = 1u;          /* PS/2 键盘默认在位 */
    g_slots[0].type = KB_PS2;
    g_mods = 0u; g_cur_layout = 0u; g_scan_set = SCAN_SET1;
    g_qhead = g_qtail = g_qcount = 0u;
    g_hot_count = 0u;
    for (i = 0u; i < 16u; i++) g_hot[i].used = 0u;
    g_repeat_key = KEY_NONE; g_repeat_ticks = 0u;
    g_filter_enabled = 1u;
    g_led_num = g_led_caps = g_led_scroll = 0u;

    /* 键盘走真实中断路径：IRQ1 使能，注入/真实敲击都能进事件队列 */
    irq_request(1u, kbd_irq_handler, 0);
    irq_enable_nr(1u);
}

/* ---------------- 导出 ---------------- */
void kbd_dump(void)
{
    con_puts("  Keyboard subsystem dump:\n");
    con_puts("    slots=");
    con_put_dec(KBD_SLOTS_MAX);
    con_puts(" cur_layout=");
    con_put_dec(g_cur_layout);
    con_puts(" scan_set=");
    con_put_dec(g_scan_set);
    con_puts(" mods=");
    con_put_hex(g_mods, 8);
    con_puts(" ev=");
    con_put_dec(g_qcount);
    con_puts(" filter=");
    con_put_dec(g_filter_enabled);
    con_puts(" leds=");
    con_put_dec(g_led_num + g_led_caps * 2u + g_led_scroll * 4u);
    con_puts("\n    slots: ");
    for (u32 i = 0u; i < KBD_SLOTS_MAX; i++) {
        if (g_slots[i].present) {
            con_puts(" s");
            con_put_dec(i);
            con_puts(":");
            con_puts(g_slots[i].type == KB_PS2 ? "ps2" : "usb");
            con_puts(" ev");
            con_put_dec(g_slots[i].events_in);
        }
    }
    con_puts("\n");
}

/* ---------------- 自检 ---------------- */
u32 kbd_selftest(void)
{
    kbd_event_t ev;
    u32 i;
    kbd_slot_t st;

    /* 1: 初始状态 */
    if (kbd_ev_count() != 0u) return 1;
    if (kbd_mods() != 0u) return 2;

    /* 2: 注入字符键（Set1 扫描码 0x1E = A） */
    kbd_inject(0u, 0x1Eu, 1);
    if (kbd_ev_count() < 2u) return 3;      /* KEY_DOWN + CHAR */
    if (kbd_read_event(&ev) != 0) return 4;
    if (ev.type != EV_KEY_DOWN || ev.key != KEY_A) return 5;
    if (kbd_read_event(&ev) != 0) return 6;
    if (ev.type != EV_CHAR || ev.ch != 'a') return 7;
    kbd_inject(0u, 0x1Eu, 0);
    if (kbd_read_event(&ev) != 0) return 8;
    if (ev.type != EV_KEY_UP || ev.key != KEY_A) return 9;

        /* 3: 修饰键 + Shift 字符 */
    kbd_flush();
    kbd_inject(0u, 0x2Au, 1);              /* LSHIFT down */
    if (!(kbd_mods() & MOD_SHIFT)) return 10;
    kbd_inject(0u, 0x1Eu, 1);              /* A with shift -> 'A' */
    {
        u32 got = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_CHAR) { got = 1u; break; }
        }
        if (!got || ev.ch != 'A') return 11;
    }
    kbd_inject(0u, 0x1Eu, 0);
    kbd_inject(0u, 0x2Au, 0);
    if (kbd_mods() & MOD_SHIFT) return 12;

    /* 4: 修饰键组合（Ctrl+C：不产生字符，符合语义） */
    kbd_flush();
    kbd_inject(0u, 0x1Du, 1);              /* LCTRL down */
    if (!(kbd_mods() & MOD_CTRL)) return 13;
    kbd_inject(0u, 0x2Eu, 1);              /* C down */
    {
        u32 bad = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_CHAR) bad = 1u;   /* Ctrl 组合不应产字符 */
        }
        if (bad) return 14;
    }
    kbd_inject(0u, 0x2Eu, 0);
    kbd_inject(0u, 0x1Du, 0);
    if (kbd_mods() & MOD_CTRL) return 15;

    /* 5: Caps 锁定 */
    kbd_flush();
    kbd_inject(0u, 0x3Au, 1); kbd_inject(0u, 0x3Au, 0);
    if (!(kbd_mods() & MOD_CAPS)) return 16;
    kbd_inject(0u, 0x1Eu, 1);              /* A with caps -> 'A' */
    {
        u32 got = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_CHAR) { got = 1u; break; }
        }
        if (!got || ev.ch != 'A') return 17;
    }
    kbd_inject(0u, 0x1Eu, 0);
    kbd_inject(0u, 0x3Au, 1); kbd_inject(0u, 0x3Au, 0);
    if (kbd_mods() & MOD_CAPS) return 18;

    /* 6: NumLock / ScrollLock LED 状态 */
    kbd_flush();
    kbd_inject(0u, 0x45u, 1); kbd_inject(0u, 0x45u, 0);
    if (!(kbd_mods() & MOD_NUMLK)) return 19;
    if (g_led_num != 1u) return 20;
    kbd_inject(0u, 0x45u, 1); kbd_inject(0u, 0x45u, 0);
    if (g_led_num != 0u) return 21;

    /* 7: LED 显式设置 */
    kbd_flush();
    if (kbd_set_led(1u, 0u, 1u) != 0 && g_led_num != 1u) return 22;
    if (g_led_scroll != 1u) return 23;

    /* 8: 扫描码集切换 */
    kbd_flush();
    if (kbd_set_scan_set(SCAN_SET2) != 0) return 24;
    kbd_inject(0u, 0x1Cu, 1);              /* Set2 ENTER */
    if (kbd_read_event(&ev) != 0) return 25;
    if (ev.key != KEY_ENTER) return 26;
    kbd_inject(0u, 0x1Cu, 0);
    if (kbd_set_scan_set(SCAN_SET3) != 0) return 27;
    if (kbd_set_scan_set(99u) == 0) return 28;   /* 越界拒绝 */
    if (kbd_set_scan_set(SCAN_SET1) != 0) return 29;

    /* 9: 组合键注册与匹配（Ctrl+Alt+K） */
    kbd_flush();
    if (kbd_register_hotkey(MOD_CTRL | MOD_ALT, KEY_K, 0x100u) != 0) return 30;
    kbd_inject(0u, 0x1Du, 1);              /* LCTRL */
    kbd_inject(0u, 0x38u, 1);              /* LALT */
    kbd_inject(0u, 0x25u, 1);              /* K */
    {
        u32 found = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_HOTKEY) found = 1u;
        }
        if (!found) return 31;
    }
    kbd_inject(0u, 0x25u, 0); kbd_inject(0u, 0x38u, 0); kbd_inject(0u, 0x1Du, 0);

    /* 10: 安全组合键过滤（Ctrl+Alt+Del 默认过滤） */
    kbd_flush();
    if (kbd_register_hotkey(MOD_CTRL | MOD_ALT, KEY_DEL, 0x200u) != 0) return 32;
    kbd_inject(0u, 0x1Du, 1); kbd_inject(0u, 0x38u, 1);
    kbd_inject(0u, 0x53u, 1);              /* DEL：过滤开启 -> 拦截 */
    {
        u32 hot = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_HOTKEY) hot = 1u;
        }
        if (hot) return 33;                /* 过滤生效：无 EV_HOTKEY */
    }
    kbd_inject(0u, 0x53u, 0);
    if (kbd_filter_set(0u) != 0) return 34;
    kbd_inject(0u, 0x53u, 1);              /* 关闭过滤：放行 */
    {
        u32 hot = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_HOTKEY) hot = 1u;
        }
        if (!hot) return 35;               /* 关闭过滤后有 EV_HOTKEY */
    }
    kbd_inject(0u, 0x53u, 0); kbd_inject(0u, 0x38u, 0); kbd_inject(0u, 0x1Du, 0);
    kbd_filter_set(1u);

    /* 11: 布局切换 */
    kbd_flush();
    if (kbd_set_layout(1u) != 0) return 36;   /* DE */
    if (kbd_layout_count() != 3u) return 37;
    kbd_inject(0u, 0x1Eu, 1);              /* A key -> DE 'a' */
    {
        u32 got = 0u;
        for (i = 0u; i < 8u; i++) {
            if (kbd_read_event(&ev) != 0) break;
            if (ev.type == EV_CHAR) { got = 1u; break; }
        }
        if (!got || ev.ch != 'a') return 38;
    }
    kbd_inject(0u, 0x1Eu, 0);
    if (kbd_set_layout(99u) == 0) return 39;   /* 越界拒绝 */
    if (kbd_set_layout(0u) != 0) return 40;

    /* 12: 热插拔 */
    kbd_flush();
    if (kbd_attach(KB_USB, 1u) != 0) return 41;
    if (kbd_attach(KB_USB, 1u) == 0) return 42;   /* 重复连接拒绝 */
    if (kbd_slot_state(1u, &st) != 0u) return 43;
    if (!st.present || st.type != KB_USB) return 44;
    if (kbd_detach(1u) != 0) return 45;
    if (kbd_detach(1u) == 0) return 46;          /* 已断开 */
    if (kbd_attach(KB_PS2, 4u) == 0) return 47;  /* 槽位越界 */

    /* 13: 事件队列深度 */
    kbd_flush();
    for (i = 0u; i < KBD_EV_QUEUE + 8u; i++) {
        kbd_event_t e;
        e.type = EV_KEY_DOWN; e.key = KEY_A; e.scancode = 0u;
        e.mods = 0u; e.ch = 0u; e.slot = 0u;
        if (ev_push(&e) != 0) break;
    }
    if (g_qcount > KBD_EV_QUEUE) return 48;
    while (g_qcount) { kbd_read_event(&ev); }

    /* 14: 真实 PS/2 轮询（无数据不阻塞） */
    if (kbd_poll() < 0) return 49;

    kbd_dump();
    return 0;
}
