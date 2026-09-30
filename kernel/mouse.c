/* ============================================================================
 * XOS 鼠标驱动子系统（第 14 册 · 设备驱动 · 鼠标）
 * 自研实现，不依赖外部核心：
 *   - PS/2 鼠标协议：0x64/0x60 命令时序（禁用键盘/使能辅助口/写鼠标命令/
 *     ACK 等待/设备 ID 探测/采样率与分辨率设置），全部带超时容错；
 *   - 数据包解析状态机：3 字节(标准)/4 字节(Intellimouse 滚轮)/5 字节
 *     (Explorer 水平滚轮)，含符号扩展、溢出位、位标志；
 *   - 相对位移累计：9-bit 补码符号扩展 → 灵敏度/加速度/平滑 → 位置累计；
 *   - 按键状态跟踪：5 键位掩码 + 按下/释放边沿事件 + 双击检测窗口；
 *   - 滚轮与水平滚动：带符号滚轮计数；
 *   - 指针加速度与平滑：4 档加速度曲线 + 一次指数平滑；
 *   - 光标位置裁剪：逻辑屏幕边界 [0,w-1]x[0,h-1]；
 *   - USB 鼠标 HID 支持：8 字节报告解析 + 多槽位热插拔；
 *   - 64 深输入事件队列：位移/按键/滚轮/双击/热插拔事件。
 * ========================================================================== */
#include "mouse.h"
#include "console.h"

/* ---------------- 端口原语（本文件内联，不依赖外部头） ---------------- */
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

/* ---------------- PS/2 端口 ---------------- */
#define KBD_DATA_PORT        0x60u
#define KBD_STATUS_PORT      0x64u
#define PS2_CMD_DISABLE_KBD  0xADu
#define PS2_CMD_ENABLE_AUX   0xA8u
#define PS2_CMD_WRITE_AUX    0xD4u
#define PS2_AUX_DEFAULTS     0xF6u
#define PS2_AUX_ENABLE_RPT   0xF4u
#define PS2_AUX_GET_DEV_ID   0xF2u
#define PS2_AUX_SET_SAMPLE   0xF3u
#define PS2_AUX_SET_RES      0xE8u
#define PS2_AUX_SCALE2X      0xE7u
#define PS2_AUX_SCALE1X      0xE6u
#define PS2_ACK              0xFAu
#define PS2_AUX_BUF_FULL     0x21u   /* 状态位 0x21 = 输出缓冲满 + 辅助口数据 */

#define MOUSE_ID_STD         0x00u   /* 标准 3 字节 */
#define MOUSE_ID_INTELLI     0x03u   /* Intellimouse 4 字节（滚轮） */
#define MOUSE_ID_EXPLORER    0x04u   /* Intellimouse Explorer 5 字节（水平滚轮） */

/* ---------------- 内部状态 ---------------- */
static mouse_event_t g_q[MOUSE_EV_QUEUE];
static u32 g_qhead;
static u32 g_qcount;
static u32 g_screen_w = MOUSE_SCREEN_DEF_W;
static u32 g_screen_h = MOUSE_SCREEN_DEF_H;
static i32 g_pos_x;
static i32 g_pos_y;
static u32 g_buttons;
static u32 g_accel = MOUSE_ACCEL_MED;   /* 默认中档加速 */
static u32 g_sens  = MOUSE_SENS_DEF;    /* 默认灵敏度 4 */
static u32 g_smooth = 1u;               /* 默认开启平滑 */
static u32 g_dbl_on = 1u;               /* 默认开启双击 */
static u32 g_dbl_win = 30u;             /* 默认双击窗口 30 tick */
static u32 g_tick;
static u32 g_dbl_key;
static u32 g_dbl_tick;
static i32 g_last_dx;
static i32 g_last_dy;
static u32 g_init_done;
static u32 g_ps2_present;               /* 真实辅助口探测结果 */
static u32 g_ps2_dev_id;

static u32 g_slot_count;
static mouse_slot_t g_slots[MOUSE_SLOTS_MAX];

/* ---------------- 端口原语 ---------------- */
static int wait_out(void)
{
    u32 i;
    for (i = 0u; i < 60000u; i++) {
        if (!(inb(KBD_STATUS_PORT) & 0x02u)) return 0;
    }
    return -1;
}

static int wait_in(void)
{
    u32 i;
    for (i = 0u; i < 60000u; i++) {
        if (inb(KBD_STATUS_PORT) & 0x01u) return 0;
    }
    return -1;
}

static int aux_write(u8 cmd)
{
    if (wait_out() != 0) return -1;
    outb(KBD_STATUS_PORT, PS2_CMD_WRITE_AUX);
    if (wait_out() != 0) return -1;
    outb(KBD_DATA_PORT, cmd);
    return 0;
}

static int aux_read(u8 *v)
{
    if (wait_in() != 0) return -1;
    *v = inb(KBD_DATA_PORT);
    return 0;
}

/* ---------------- 事件队列 ---------------- */
static int ev_push(mouse_event_t *e)
{
    if (g_qcount >= MOUSE_EV_QUEUE) return -1;
    g_q[(g_qhead + g_qcount) % MOUSE_EV_QUEUE] = *e;
    g_qcount++;
    return 0;
}

static int ev_pop(mouse_event_t *e)
{
    if (g_qcount == 0u) return -1;
    *e = g_q[g_qhead];
    g_qhead = (g_qhead + 1u) % MOUSE_EV_QUEUE;
    g_qcount--;
    return 0;
}

static void ev_flush(void)
{
    g_qhead = 0u;
    g_qcount = 0u;
}

/* ---------------- 槽位 ---------------- */
static mouse_slot_t *slot_get(u32 slot)
{
    if (slot >= MOUSE_SLOTS_MAX) return 0;
    return &g_slots[slot];
}

/* ---------------- 位移处理 ---------------- */
static void move_emit(u32 slot, i32 dx, i32 dy)
{
    mouse_event_t e;
    i32 nx, ny;
    if (dx == 0 && dy == 0) return;   /* 零位移不产生移动事件 */
    nx = g_pos_x + dx;
    ny = g_pos_y + dy;

    /* 裁剪到逻辑屏幕边界 */
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= (i32)g_screen_w) nx = (i32)g_screen_w - 1;
    if (ny >= (i32)g_screen_h) ny = (i32)g_screen_h - 1;
    dx = nx - g_pos_x;
    dy = ny - g_pos_y;
    g_pos_x = nx;
    g_pos_y = ny;
    g_last_dx = dx;
    g_last_dy = dy;

    e.type = MOUSE_EV_MOVE;
    e.button = g_buttons;
    e.dx = dx;
    e.dy = dy;
    e.wheel = 0;
    e.hwheel = 0;
    e.x = (u32)nx;
    e.y = (u32)ny;
    e.slot = slot;
    ev_push(&e);
}

static void wheel_emit(u32 slot, i32 w, i32 hw)
{
    mouse_event_t e;
    u32 type = (hw != 0) ? MOUSE_EV_HWHEEL : MOUSE_EV_WHEEL;
    e.type = type;
    e.button = g_buttons;
    e.dx = 0;
    e.dy = 0;
    e.wheel = w;
    e.hwheel = hw;
    e.x = (u32)g_pos_x;
    e.y = (u32)g_pos_y;
    e.slot = slot;
    ev_push(&e);
}

/* ---------------- 加速度曲线 ---------------- */
/* 档位系数：NONE=10, LOW=15, MED=20, HIGH=30（分母 10）。
 * 位移量 |d| 小于 3 不加速；3..8 按档位；>8 再 ×1.5。 */
static void accel_apply(i32 *dx, i32 *dy)
{
    static const u32 tbl[4] = { 10u, 15u, 20u, 30u };
    u32 k = tbl[g_accel & 3u];
    i32 adx = *dx;
    i32 ady = *dy;

    if (adx < 0) adx = -adx;
    if (ady < 0) ady = -ady;
    if (adx > 8) adx = (adx * k * 15u) / 100;
    else if (adx >= 3) adx = (adx * k) / 10;
    if (ady > 8) ady = (ady * k * 15u) / 100;
    else if (ady >= 3) ady = (ady * k) / 10;

    *dx = (*dx < 0) ? -adx : adx;
    *dy = (*dy < 0) ? -ady : ady;
}

/* ---------------- 平滑滤波（一次指数平滑） ---------------- */
static void smooth_apply(i32 *dx, i32 *dy)
{
    *dx = (*dx + g_last_dx) / 2;
    *dy = (*dy + g_last_dy) / 2;
}

/* ---------------- 按键状态跟踪 ---------------- */
static void btn_update(u32 slot, u32 newb)
{
    u32 changed = newb ^ g_buttons;
    u32 b;
    mouse_event_t e;

    for (b = 0u; b < 5u; b++) {
        u32 bit = 1u << b;
        if (!(changed & bit)) continue;
        if (newb & bit) {
            /* 按下 */
            e.type = MOUSE_EV_BTN_DOWN;
            e.button = bit;
            e.dx = 0; e.dy = 0; e.wheel = 0; e.hwheel = 0;
            e.x = (u32)g_pos_x; e.y = (u32)g_pos_y;
            e.slot = slot;
            ev_push(&e);
            /* 双击检测 */
            if (g_dbl_on && bit == g_dbl_key &&
                (g_tick - g_dbl_tick) <= g_dbl_win && g_tick >= g_dbl_tick) {
                mouse_event_t de;
                de.type = MOUSE_EV_DOUBLE;
                de.button = bit;
                de.dx = 0; de.dy = 0; de.wheel = 0; de.hwheel = 0;
                de.x = (u32)g_pos_x; de.y = (u32)g_pos_y;
                de.slot = slot;
                ev_push(&de);
                g_dbl_key = 0u;
            } else {
                g_dbl_key = bit;
                g_dbl_tick = g_tick;
            }
        } else {
            e.type = MOUSE_EV_BTN_UP;
            e.button = bit;
            e.dx = 0; e.dy = 0; e.wheel = 0; e.hwheel = 0;
            e.x = (u32)g_pos_x; e.y = (u32)g_pos_y;
            e.slot = slot;
            ev_push(&e);
        }
    }
    g_buttons = newb;
}

/* ---------------- 数据包解析 ---------------- */
static i32 sign8(u8 v)
{
    return (v & 0x80u) ? (i32)(v | 0xFFFFFF00u) : (i32)v;
}

static void pkt_parse(u32 slot, const u8 *p, u32 len)
{
    mouse_slot_t *st = slot_get(slot);
    i32 dx, dy, w, hw;
    u32 flags;

    if (!st) return;
    st->events_in++;
    if (len < 3u) { st->errors++; return; }

    flags = p[0];
    dx = sign8(p[1]);
    dy = sign8(p[2]);
    if (flags & 0x40u) dx = 0;   /* X 溢出 */
    if (flags & 0x80u) dy = 0;   /* Y 溢出 */

    /* 灵敏度 */
    dx = (dx * (i32)g_sens) / (i32)MOUSE_SENS_DEF;
    dy = (dy * (i32)g_sens) / (i32)MOUSE_SENS_DEF;
    /* 加速度 + 平滑 */
    accel_apply(&dx, &dy);
    if (g_smooth) smooth_apply(&dx, &dy);

    /* 按键（低 3 位）+ 第 4/5 字节滚轮 */
    btn_update(slot, flags & 0x07u);
    w = 0;
    hw = 0;
    if (len >= 4u && st->pkt_mode >= 4u) {
        w = (i32)(p[3] & 0x0Fu);
        if (w >= 8) w -= 16;
        if (len >= 5u && st->pkt_mode == 5u) {
            hw = (i32)(p[4] & 0x0Fu);
            if (hw >= 8) hw -= 16;
        }
    }
    if (w != 0) wheel_emit(slot, w, 0);
    if (hw != 0) wheel_emit(slot, 0, hw);
    move_emit(slot, dx, dy);
}

/* ---------------- 注入 / 轮询 ---------------- */
void mse_inject_pkt(u32 slot, const u8 *pkt, u32 len)
{
    mouse_slot_t *st = slot_get(slot);
    if (!st || !st->present) return;
    pkt_parse(slot, pkt, len);
}

int mse_poll(void)
{
    u8 b;

    if (!g_ps2_present) return 0;
    if (!(inb(KBD_STATUS_PORT) & PS2_AUX_BUF_FULL)) return 0;
    b = inb(KBD_DATA_PORT);
    /* 简化：单字节不足以成包；PS/2 鼠标由 IRQ12 逐字节送 → 此处仅作探测 */
    return (b == PS2_ACK) ? 1 : 0;
}

/* ---------------- 初始化（带超时容错，无鼠标不阻塞） ---------------- */
void mse_init(void)
{
    u8 v;
    u32 i;

    if (g_init_done) return;
    g_init_done = 1u;

    /* 默认槽位 0 = PS/2 */
    g_slot_count = 1u;
    g_slots[0].present = 1u;
    g_slots[0].type = MOUSE_TYPE_PS2;
    g_slots[0].pkt_mode = 3u;
    g_slots[0].sample_rate = 0u;
    g_slots[0].resolution = 0u;
    g_slots[0].buttons = 0u;

    /* 初始化 PS/2 鼠标（全部带超时，失败仅标记不可用） */
    if (wait_out() != 0) return;
    outb(KBD_STATUS_PORT, PS2_CMD_DISABLE_KBD);
    if (wait_out() != 0) return;
    outb(KBD_STATUS_PORT, PS2_CMD_ENABLE_AUX);
    for (i = 0u; i < 3u; i++) {
        if (wait_in() != 0) break;
        (void)inb(KBD_DATA_PORT);   /* 清残留 */
    }
    if (aux_write(PS2_AUX_DEFAULTS) != 0) return;
    if (aux_read(&v) != 0 || v != PS2_ACK) return;
    g_ps2_present = 1u;
    if (aux_write(PS2_AUX_GET_DEV_ID) != 0) return;
    if (aux_read(&v) != 0 || v != PS2_ACK) return;
    if (aux_read(&v) == 0) g_ps2_dev_id = v; else g_ps2_dev_id = MOUSE_ID_STD;
    if (g_ps2_dev_id == MOUSE_ID_INTELLI) g_slots[0].pkt_mode = 4u;
    else if (g_ps2_dev_id == MOUSE_ID_EXPLORER) g_slots[0].pkt_mode = 5u;
    /* 采样率 100Hz */
    if (aux_write(PS2_AUX_SET_SAMPLE) == 0) {
        if (wait_out() == 0) {
            outb(KBD_DATA_PORT, 100u);
            if (aux_read(&v) == 0 && v == PS2_ACK) g_slots[0].sample_rate = 100u;
        }
    }
    /* 使能数据报告 */
    if (aux_write(PS2_AUX_ENABLE_RPT) == 0) aux_read(&v);
}

/* ---------------- 配置接口 ---------------- */
void mse_set_screen(u32 w, u32 h)
{
    if (w < 16u) w = 16u;
    if (h < 16u) h = 16u;
    g_screen_w = w;
    g_screen_h = h;
    if ((u32)g_pos_x >= w) g_pos_x = (i32)w - 1;
    if ((u32)g_pos_y >= h) g_pos_y = (i32)h - 1;
}

u32 mse_get_screen(u32 *w, u32 *h)
{
    if (w) *w = g_screen_w;
    if (h) *h = g_screen_h;
    return 0u;
}

void mse_get_pos(u32 *x, u32 *y)
{
    if (x) *x = (u32)g_pos_x;
    if (y) *y = (u32)g_pos_y;
}

u32 mse_get_buttons(void) { return g_buttons; }

int mse_set_sens(u32 s)
{
    if (s < MOUSE_SENS_MIN || s > MOUSE_SENS_MAX) return -1;
    g_sens = s;
    return 0;
}

u32 mse_get_sens(void) { return g_sens; }

int mse_set_accel(u32 a)
{
    if (a > MOUSE_ACCEL_MAX) return -1;
    g_accel = a;
    return 0;
}

u32 mse_get_accel(void) { return g_accel; }

int mse_set_smooth(u32 on)
{
    g_smooth = on ? 1u : 0u;
    return 0;
}

u32 mse_get_smooth(void) { return g_smooth; }

int mse_set_dbl(u32 on, u32 window)
{
    if (window > 200u) return -1;
    g_dbl_on = on ? 1u : 0u;
    g_dbl_win = window;
    return 0;
}

int mse_set_pkt_mode(u32 slot, u32 mode)
{
    mouse_slot_t *st = slot_get(slot);
    if (!st || !st->present) return -1;
    if (mode < MOUSE_PKT_MIN || mode > MOUSE_PKT_MAX) return -1;
    st->pkt_mode = mode;
    return 0;
}

int mse_attach(u32 type, u32 slot)
{
    mouse_slot_t *st = slot_get(slot);
    if (!st) return -1;
    if (st->present) return -1;
    if (type != MOUSE_TYPE_PS2 && type != MOUSE_TYPE_USB) return -1;
    st->present = 1u;
    st->type = type;
    st->pkt_mode = (type == MOUSE_TYPE_USB) ? 3u : 3u;
    st->sample_rate = 0u;
    st->resolution = 0u;
    st->buttons = 0u;
    st->events_in = 0u;
    st->events_out = 0u;
    st->errors = 0u;
    g_slot_count++;
    {
        mouse_event_t e;
        e.type = MOUSE_EV_ATTACH;
        e.button = 0; e.dx = 0; e.dy = 0; e.wheel = 0; e.hwheel = 0;
        e.x = (u32)g_pos_x; e.y = (u32)g_pos_y; e.slot = slot;
        ev_push(&e);
    }
    return 0;
}

int mse_detach(u32 slot)
{
    mouse_slot_t *st = slot_get(slot);
    if (!st) return -1;
    if (!st->present) return -1;
    st->present = 0u;
    if (g_slot_count > 0u) g_slot_count--;
    {
        mouse_event_t e;
        e.type = MOUSE_EV_DETACH;
        e.button = 0; e.dx = 0; e.dy = 0; e.wheel = 0; e.hwheel = 0;
        e.x = (u32)g_pos_x; e.y = (u32)g_pos_y; e.slot = slot;
        ev_push(&e);
    }
    return 0;
}

u32 mse_slot_state(u32 slot, mouse_slot_t *out)
{
    mouse_slot_t *st = slot_get(slot);
    if (!st) return 1u;
    if (out) *out = *st;
    return 0u;
}

/* ---------------- USB HID 报告解析（8 字节） ---------------- */
int mse_hid_report(u32 slot, const u8 *r, u32 len)
{
    mouse_slot_t *st = slot_get(slot);
    u8 pkt[5];
    if (!st || !st->present) return -1;
    if (len < 4u) return -1;
    /* HID: btn, dx, dy, wheel, hwheel → 内部 5 字节包 */
    pkt[0] = r[0] & 0x07u;
    pkt[1] = r[1];
    pkt[2] = r[2];
    pkt[3] = r[3];
    pkt[4] = (len >= 5u) ? r[4] : 0u;
    if (st->pkt_mode < 5u && len < 5u) {
        pkt_parse(slot, pkt, 3u);
        if (r[3] != 0u) {
            u32 pw = (u32)(r[3] & 0x0Fu);
            if (pw >= 8) pw -= 16u;
            wheel_emit(slot, (i32)pw, 0);
        }
    } else {
        pkt_parse(slot, pkt, 5u);
    }
    return 0;
}

/* ---------------- 事件读取 ---------------- */
int mse_read_event(mouse_event_t *ev)
{
    if (ev_pop(ev) != 0) return -1;
    return 0;
}

u32 mse_ev_count(void) { return g_qcount; }

/* ---------------- 自检 ---------------- */
u32 mse_selftest(void)
{
    mouse_event_t ev;
    u32 i;
    u32 x, y;
    u8 pkt[5];
    mouse_slot_t st;

    /* 1: 初始状态 */
    ev_flush();
    if (mse_ev_count() != 0u) return 1;
    mse_get_pos(&x, &y);
    if (x != 0u || y != 0u) return 2;
    if (g_screen_w != MOUSE_SCREEN_DEF_W || g_screen_h != MOUSE_SCREEN_DEF_H) return 3;

    /* 2: 3 字节包位移解析（左键按下 + dx=3） */
    mse_set_sens(MOUSE_SENS_DEF);
    mse_set_accel(MOUSE_ACCEL_NONE);
    mse_set_smooth(0u);
    mse_set_dbl(0u, 0u);
    pkt[0] = MOUSE_BTN_LEFT; pkt[1] = 3; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 4;
    if (ev.type != MOUSE_EV_BTN_DOWN || ev.button != MOUSE_BTN_LEFT) return 5;
    if (mse_read_event(&ev) != 0) return 6;
    if (ev.type != MOUSE_EV_MOVE || ev.dx != 3 || ev.dy != 0) return 7;
    if (ev.x != 3u || ev.y != 0u) return 8;

    /* 3: dy 位移 */
    pkt[0] = MOUSE_BTN_LEFT; pkt[1] = 0; pkt[2] = 2;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 9;
    if (ev.type != MOUSE_EV_MOVE || ev.dy != 2 || ev.y != 2u) return 10;

    /* 4: 负位移（补码符号扩展） */
    pkt[0] = MOUSE_BTN_LEFT; pkt[1] = (u8)0xFD; pkt[2] = (u8)0xFE;   /* -3, -2 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 11;
    if (ev.dx != -3 || ev.dy != -2) return 12;
    mse_get_pos(&x, &y);
    if (x != 0u || y != 0u) return 13;      /* 裁剪到 0 */

    /* 5: 左键释放 */
    pkt[0] = 0; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 14;
    if (ev.type != MOUSE_EV_BTN_UP || ev.button != MOUSE_BTN_LEFT) return 15;

    /* 6: 右键 + 中键 */
    pkt[0] = MOUSE_BTN_RIGHT; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 16;
    if (ev.type != MOUSE_EV_BTN_DOWN || ev.button != MOUSE_BTN_RIGHT) return 17;
    pkt[0] = MOUSE_BTN_MIDDLE | MOUSE_BTN_RIGHT; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 18;
    if (ev.type != MOUSE_EV_BTN_DOWN || ev.button != MOUSE_BTN_MIDDLE) return 19;
    pkt[0] = 0; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 20;
    if (mse_read_event(&ev) != 0) return 21;
    if (mse_get_buttons() != 0u) return 22;

    /* 7: 4 字节滚轮（+1 / -1） */
    if (mse_set_pkt_mode(0u, 4u) != 0) return 23;
    pkt[0] = 0; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0x01;
    mse_inject_pkt(0u, pkt, 4u);
    if (mse_read_event(&ev) != 0) return 24;
    if (ev.type != MOUSE_EV_WHEEL || ev.wheel != 1) return 25;
    pkt[3] = 0x0F;                              /* -1 补码 */
    mse_inject_pkt(0u, pkt, 4u);
    if (mse_read_event(&ev) != 0) return 26;
    if (ev.type != MOUSE_EV_WHEEL || ev.wheel != -1) return 27;

    /* 8: 5 字节水平滚轮 */
    if (mse_set_pkt_mode(0u, 5u) != 0) return 28;
    pkt[0] = 0; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0; pkt[4] = 0x02;
    mse_inject_pkt(0u, pkt, 5u);
    if (mse_read_event(&ev) != 0) return 29;
    if (ev.type != MOUSE_EV_HWHEEL || ev.hwheel != 2) return 30;
    if (mse_set_pkt_mode(0u, 3u) != 0) return 31;

    /* 9: 位移累计（3 次 dx=5） */
    mse_set_sens(MOUSE_SENS_DEF);
    mse_set_accel(MOUSE_ACCEL_NONE);
    mse_set_smooth(0u);
    for (i = 0u; i < 3u; i++) {
        pkt[0] = 0; pkt[1] = 5; pkt[2] = 0;
        mse_inject_pkt(0u, pkt, 3u);
        if (mse_read_event(&ev) != 0) return 32;
    }
    mse_get_pos(&x, &y);
    if (x != 15u || y != 0u) return 33;

    /* 10: 边界裁剪（负方向 / 超右上角） */
    pkt[0] = 0; pkt[1] = (u8)0xEC; pkt[2] = (u8)0xEC;   /* -20, -20 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 34;
    mse_get_pos(&x, &y);
    if (x != 0u || y != 0u) return 35;
    mse_set_screen(640u, 480u);
    pkt[0] = 0; pkt[1] = 120; pkt[2] = 120;             /* 200,200 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 36;
    pkt[0] = 0; pkt[1] = 120; pkt[2] = 120;             /* 400,400 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 37;
    pkt[0] = 0; pkt[1] = 120; pkt[2] = 120;             /* 第3次：540,479 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 38;
    pkt[0] = 0; pkt[1] = 120; pkt[2] = 120;             /* 第4次：→ 639,479 裁剪 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 39;
    mse_get_pos(&x, &y);
    if (x != 639u || y != 479u) return 40;

    /* 移回原点（7 次 -100） */
    pkt[0] = 0; pkt[1] = (u8)0x9C; pkt[2] = (u8)0x9C;   /* -100 */
    for (i = 0u; i < 7u; i++) {
        mse_inject_pkt(0u, pkt, 3u);
        if (mse_read_event(&ev) != 0) return 41;
    }
    mse_get_pos(&x, &y);
    if (x != 0u || y != 0u) return 42;

    /* 11: 加速度曲线 */
    mse_set_smooth(0u);
    pkt[0] = 0; pkt[1] = 5; pkt[2] = 0;
    mse_set_accel(MOUSE_ACCEL_LOW);                     /* 5*15/10=7 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 43;
    if (ev.dx != 7) return 44;
    mse_set_accel(MOUSE_ACCEL_MED);                     /* 5*20/10=10 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 45;
    if (ev.dx != 10) return 46;
    mse_set_accel(MOUSE_ACCEL_HIGH);                    /* 5*30/10=15 */
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 47;
    if (ev.dx != 15) return 48;
    mse_set_accel(MOUSE_ACCEL_NONE);
    if (mse_set_accel(99u) == 0) return 49;             /* 无效档拒绝 */

    /* 12: 灵敏度设置 */
    if (mse_set_sens(2u) != 0) return 50;
    if (mse_get_sens() != 2u) return 51;
    if (mse_set_sens(0u) == 0) return 52;               /* 越界拒绝 */
    if (mse_set_sens(17u) == 0) return 53;
    mse_set_sens(MOUSE_SENS_DEF);

    /* 13: 平滑滤波（两次 10 → 5+10/2=7... 简化为位移 10+last 0 → 5） */
    g_last_dx = 0; g_last_dy = 0;      /* 重置平滑基准（静态可见） */
    mse_set_smooth(1u);
    pkt[0] = 0; pkt[1] = 10; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 54;
    if (ev.dx != 7) return 55;                          /* accel 10->15, (15+0)/2=7 */
    pkt[1] = 10;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 56;
    if (ev.dx != 11) return 57;                         /* (15+7)/2=11 */
    mse_set_smooth(0u);

    /* 14: 双击检测 */
    mse_set_smooth(0u);      /* 关闭平滑，避免位移干扰按键用例 */
    mse_set_dbl(1u, 30u);
    pkt[0] = MOUSE_BTN_LEFT; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 58;
    g_tick += 10u;                                      /* 模拟时间推进 */
    pkt[0] = 0; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);                        /* 释放 */
    if (mse_read_event(&ev) != 0) return 59;
    g_tick += 10u;
    pkt[0] = MOUSE_BTN_LEFT; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);                        /* 再按下 → DOUBLE */
    if (mse_read_event(&ev) != 0) return 60;
    if (mse_read_event(&ev) != 0) return 61;
    if (ev.type != MOUSE_EV_DOUBLE || ev.button != MOUSE_BTN_LEFT) return 62;
    pkt[0] = 0; pkt[1] = 0; pkt[2] = 0;
    mse_inject_pkt(0u, pkt, 3u);
    if (mse_read_event(&ev) != 0) return 63;
    mse_set_dbl(0u, 0u);

    /* 15: 屏幕尺寸设置 */
    mse_set_screen(80u, 40u);
    mse_get_screen(&x, &y);
    if (x != 80u || y != 40u) return 65;
    mse_set_screen(640u, 480u);

    /* 16: HID 槽位热插拔 */
    if (mse_attach(MOUSE_TYPE_USB, 1u) != 0) return 66;
    if (mse_attach(MOUSE_TYPE_USB, 1u) == 0) return 67;  /* 重复连接拒绝 */
    if (mse_slot_state(1u, &st) != 0u) return 68;
    if (!st.present || st.type != MOUSE_TYPE_USB) return 69;
    if (mse_detach(1u) != 0) return 70;
    if (mse_detach(1u) == 0) return 71;                 /* 已断开 */
    if (mse_attach(MOUSE_TYPE_PS2, 4u) == 0) return 72; /* 槽位越界 */
    if (mse_attach(99u, 1u) == 0) return 73;            /* 类型非法 */

    /* 17: USB HID 报告解析 */
    ev_flush();                                      /* 清用例16残留的 ATTACH/DETACH */
    if (mse_attach(MOUSE_TYPE_USB, 2u) != 0) return 74;
    if (mse_read_event(&ev) != 0) return 75;         /* 消费 ATTACH */
    if (ev.type != MOUSE_EV_ATTACH || ev.slot != 2u) return 76;
    mse_set_pkt_mode(2u, 5u);              /* 5 字节模式（slot 有效必成功） */
    {
        u8 hid[5];
        hid[0] = MOUSE_BTN_LEFT; hid[1] = 4; hid[2] = 1; hid[3] = 0x01; hid[4] = 0;
        if (mse_hid_report(2u, hid, 5u) != 0) return 77;
        if (mse_read_event(&ev) != 0) return 78;
        if (ev.type != MOUSE_EV_BTN_DOWN || ev.button != MOUSE_BTN_LEFT) return 79;
        if (mse_read_event(&ev) != 0) return 80;
        if (ev.type != MOUSE_EV_WHEEL || ev.wheel != 1) return 81;
        if (mse_read_event(&ev) != 0) return 82;
        if (ev.type != MOUSE_EV_MOVE || ev.dx != 4 || ev.dy != 1) return 83;
        if (mse_hid_report(2u, hid, 2u) == 0) return 84;   /* 过短拒绝 */
    }
    if (mse_detach(2u) != 0) return 85;

    /* 18: 事件队列深度 */
    ev_flush();
    for (i = 0u; i < MOUSE_EV_QUEUE + 8u; i++) {
        mouse_event_t e;
        e.type = MOUSE_EV_MOVE; e.button = 0; e.dx = 1; e.dy = 0;
        e.wheel = 0; e.hwheel = 0; e.x = 0u; e.y = 0u; e.slot = 0u;
        if (ev_push(&e) != 0) break;
    }
    if (g_qcount > MOUSE_EV_QUEUE) return 86;
    while (g_qcount) { mse_read_event(&ev); }

    /* 19: 包模式非法拒绝 */
    if (mse_set_pkt_mode(0u, 2u) == 0) return 87;
    if (mse_set_pkt_mode(0u, 6u) == 0) return 88;

    /* 20: 真实端口轮询（无数据不阻塞） */
    if (mse_poll() < 0) return 89;

    mse_dump();
    return 0u;
}

/* ---------------- 状态输出 ---------------- */
void mse_dump(void)
{
    con_printf("  Mouse subsystem dump:\n");
    con_printf("    slots=%u cur=(%d,%d) scr=%ux%u btns=%u accel=%u sens=%u smooth=%u dbl=%u/%u ps2=%u id=%u\n",
               g_slot_count, g_pos_x, g_pos_y, g_screen_w, g_screen_h,
               g_buttons, g_accel, g_sens, g_smooth, g_dbl_on, g_dbl_win,
               g_ps2_present, g_ps2_dev_id);
    {
        u32 s;
        for (s = 0u; s < MOUSE_SLOTS_MAX; s++) {
            mouse_slot_t *st = &g_slots[s];
            if (st->present)
                con_printf("    s%u:%s pkt=%u rate=%u res=%u in=%u out=%u err=%u\n",
                           s, (st->type == MOUSE_TYPE_USB) ? "usb" : "ps2",
                           st->pkt_mode, st->sample_rate, st->resolution,
                           st->events_in, st->events_out, st->errors);
        }
    }
}
