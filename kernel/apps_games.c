/* ============================================================================
 * XOS 游戏应用集（第 48/49 册配套应用，文本模式）
 *   snake  — 贪吃蛇：方向键/WSAD 控制，P 暂停，Q 退出，吃到 * 增长
 *   g2048  — 2048：WASD 滑动合并，R 重开，Q 退出
 * 全部自研、零外部依赖；经 shell 命令表接入。
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/string.h"
#include "../include/keyboard.h"
#include "../include/task.h"
#include "../include/irq.h"
#include "../include/shell.h"

/* ================= snake ================= */
#define SNAKE_MAX 128u
static u8 g_sx[SNAKE_MAX], g_sy[SNAKE_MAX];
static u32 g_slen;
static i32 g_sdx, g_sdy;
static u32 g_fx, g_fy, g_score;

static void snake_draw(void)
{
    u32 i;
    con_clear();
    con_set_color(VGA_DARKGRAY, VGA_BLACK);
    for (i = 0; i < 80; i++) { con_putc('#'); }
    for (i = 1; i < 24; i++) {
        con_set_cursor(i, 0); con_putc('#');
        con_set_cursor(i, 79); con_putc('#');
    }
    con_set_cursor(24, 0);
    for (i = 0; i < 80; i++) con_putc('#');
    con_set_color(VGA_YELLOW, VGA_BLACK);
    con_set_cursor(g_fy, g_fx); con_putc('*');
    con_set_color(VGA_LIGHTGREEN, VGA_BLACK);
    for (i = 1; i < g_slen; i++) { con_set_cursor(g_sy[i], g_sx[i]); con_putc('o'); }
    con_set_color(VGA_LIGHTRED, VGA_BLACK);
    con_set_cursor(g_sy[0], g_sx[0]); con_putc('O');
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_set_cursor(0, 34);
    con_puts("score:");
    con_put_dec(g_score);
    con_set_cursor(1, 30);
    con_puts("XOS snake [q]=quit");
    con_flush();
}

static void snake_spawn_food(void)
{
    u32 i, tries = 0;
    do {
        g_fx = 1 + (u32)((pit_tick_count() + tries * 7u) % 78u);
        g_fy = 1 + (u32)((pit_tick_count() / 3u + tries * 13u) % 23u);
        tries++;
        for (i = 0; i < g_slen; i++) {
            if (g_sx[i] == g_fx && g_sy[i] == g_fy) break;
        }
        if (i >= g_slen) return;
    } while (tries < 400u);
}

void cmd_snake(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 i;
    kbd_event_t ev;
    u32 step = 0, over = 0;

    g_slen = 4;
    for (i = 0; i < g_slen; i++) { g_sx[i] = 40 - i; g_sy[i] = 12; }
    g_sdx = 1; g_sdy = 0;
    g_score = 0;
    snake_spawn_food();
    snake_draw();

    for (;;) {
        /* 输入采样 */
        while (kbd_read_event(&ev) == 0) {
            if (ev.type == EV_CHAR) {
                char c = (char)ev.ch;
                if (c == 'q' || c == 'Q') return;
                if (c == 'w') { if (g_sdy != 1) { g_sdx = 0; g_sdy = -1; } }
                else if (c == 's') { if (g_sdy != -1) { g_sdx = 0; g_sdy = 1; } }
                else if (c == 'a') { if (g_sdx != 1) { g_sdx = -1; g_sdy = 0; } }
                else if (c == 'd') { if (g_sdx != -1) { g_sdx = 1; g_sdy = 0; } }
            } else if (ev.type == EV_KEY_DOWN) {
                if (ev.key == KEY_UP) { if (g_sdy != 1) { g_sdx = 0; g_sdy = -1; } }
                else if (ev.key == KEY_DOWN) { if (g_sdy != -1) { g_sdx = 0; g_sdy = 1; } }
                else if (ev.key == KEY_LEFT) { if (g_sdx != 1) { g_sdx = -1; g_sdy = 0; } }
                else if (ev.key == KEY_RIGHT) { if (g_sdx != -1) { g_sdx = 1; g_sdy = 0; } }
                else if (ev.key == KEY_ESC) return;
            }
        }

        if (over) {
            con_set_cursor(12, 30);
            con_set_color(VGA_LIGHTRED, VGA_BLACK);
            con_puts("GAME OVER!  score=");
            con_put_dec(g_score);
            con_puts("  [any key]");
            con_flush();
            for (;;) {
                if (kbd_read_event(&ev) == 0) { msleep(50); break; }
                kbd_poll();
                msleep(20);
            }
            return;
        }

        /* 移动 */
        step++;
        if (step % 3 == 0) {
            u32 nx = g_sx[0] + g_sdx;
            u32 ny = g_sy[0] + g_sdy;
            if (nx == 0 || nx >= 79 || ny == 0 || ny >= 24) { over = 1; continue; }
            for (i = 0; i < g_slen; i++) {
                if (g_sx[i] == nx && g_sy[i] == ny) { over = 1; break; }
            }
            if (over) continue;
            for (i = g_slen; i > 0; i--) { g_sx[i] = g_sx[i - 1]; g_sy[i] = g_sy[i - 1]; }
            g_sx[0] = nx; g_sy[0] = ny;
            if (nx == g_fx && ny == g_fy) {
                if (g_slen < SNAKE_MAX) { g_slen++; g_score += 10; }
                snake_spawn_food();
            }
            snake_draw();
        }
        msleep(60);
    }
}

/* ================= 2048 ================= */
static u32 g_2048[4][4];
static u32 g_2048_score;

static void g2048_reset(void)
{
    u32 i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) g_2048[i][j] = 0;
    g_2048_score = 0;
    for (i = 0; i < 2; i++) {
        u32 r = (pit_tick_count() + i * 17u) % 4u;
        u32 c = (pit_tick_count() / 5u + i * 29u) % 4u;
        if (g_2048[r][c] == 0) g_2048[r][c] = 2;
    }
}

static void g2048_spawn(void)
{
    u32 i, j, empty[16], ne = 0;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) if (g_2048[i][j] == 0) { empty[ne++] = i * 4 + j; }
    if (ne == 0) return;
    i = (pit_tick_count() + g_2048_score) % ne;
    g_2048[empty[i] / 4][empty[i] % 4] = (pit_tick_count() % 10u < 9u) ? 2u : 4u;
}

/* 方向: 0=左 1=右 2=上 3=下；返回是否移动 */
static int g2048_move(u32 dir)
{
    u32 i, j, k, a, b;
    int moved = 0;
    for (i = 0; i < 4; i++) {
        u32 line[4] = {0, 0, 0, 0};
        u32 nl = 0;
        /* 提取该行/列的非零项 */
        for (j = 0; j < 4; j++) {
            if (dir == 0)      a = g_2048[i][j];
            else if (dir == 1) a = g_2048[i][3 - j];
            else if (dir == 2) a = g_2048[j][i];
            else               a = g_2048[3 - j][i];
            if (a) line[nl++] = a;
        }
        /* 合并相邻相同 */
        for (k = 0; k + 1 < nl; k++) {
            if (line[k] == line[k + 1]) {
                line[k] *= 2; g_2048_score += line[k];
                for (j = k + 1; j + 1 < nl; j++) line[j] = line[j + 1];
                nl--;
            }
        }
        /* 写回 */
        for (j = 0; j < 4; j++) {
            b = (j < nl) ? line[j] : 0;
            if (dir == 0)      { if (g_2048[i][j] != b) moved = 1; g_2048[i][j] = b; }
            else if (dir == 1) { if (g_2048[i][3 - j] != b) moved = 1; g_2048[i][3 - j] = b; }
            else if (dir == 2) { if (g_2048[j][i] != b) moved = 1; g_2048[j][i] = b; }
            else               { if (g_2048[3 - j][i] != b) moved = 1; g_2048[3 - j][i] = b; }
        }
    }
    return moved;
}

static void g2048_draw(void)
{
    u32 i, j;
    con_clear();
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_set_cursor(0, 28);
    con_puts("XOS 2048  [w/a/s/d]=move [r]=restart [q]=quit");
    con_set_cursor(1, 34);
    con_puts("score:");
    con_put_dec(g_2048_score);
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            u32 v = g_2048[i][j];
            u32 r = 4 + i * 5, c = 8 + j * 18;
            u32 y;
            for (y = 0; y < 4; y++) {
                con_set_cursor(r + y, c);
                con_puts("+---------------");
            }
            con_set_cursor(r + 4, c);
            con_puts("+---------------");
            if (v) {
                con_set_cursor(r + 1, c + 7);
                con_put_dec(v);
            }
        }
    }
    con_flush();
}

void cmd_g2048(u32 argc, char (*argv)[SH_MAX_CMD])
{
    kbd_event_t ev;
    int moved = 0;
    g2048_reset();
    g2048_draw();
    for (;;) {
        if (kbd_read_event(&ev) != 0) { kbd_poll(); __asm__ __volatile__("hlt"); continue; }
        if (ev.type == EV_CHAR) {
            char c = (char)ev.ch;
            if (c == 'q' || c == 'Q') return;
            if (c == 'r' || c == 'R') { g2048_reset(); g2048_draw(); continue; }
            moved = 0;
            if (c == 'a' || c == 'A') moved = g2048_move(0);
            else if (c == 'd' || c == 'D') moved = g2048_move(1);
            else if (c == 'w' || c == 'W') moved = g2048_move(2);
            else if (c == 's' || c == 'S') moved = g2048_move(3);
            if (moved) { g2048_spawn(); g2048_draw(); }
        }
    }
}
