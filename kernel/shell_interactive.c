/* ============================================================================
 * XOS 交互式终端 Shell（第 25 册配套 · 用户可直接操作的命令行界面）
 * 自检全部通过后由 kmain 接管控制台：读键盘 → 行编辑 → 解析 → 分派执行。
 * 完全自研：命令集直接调用本系统内核 API（fs / pmm / pm / shell 解析框架）。
 * 不依赖任何外部库。bss 占用约 0.3KB，栈峰值 < 1KB，代码体积裁剪以适配
 * 内核扁平二进制 331776B 上限。
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/keyboard.h"
#include "../include/shell.h"
#include "../include/fs.h"
#include "../include/pmm.h"
#include "../include/pm.h"

/* ---------------- 端口 IO（8042 软复位用） ---------------- */
static inline void x_outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ---------------- 行缓冲（bss，256B） ---------------- */
static char g_line[SH_MAX_LINE];
static u32  g_len;
static u32  g_exit;

/* ---------------- 命令表（.rodata，不进 bss） ---------------- */
typedef void (*cmd_fn_t)(u32 argc, char (*argv)[SH_MAX_CMD]);

struct cmd {
    const char *name;
    const char *help;
    cmd_fn_t    fn;
};

static int x_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

static void cmd_help(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_clear(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_echo(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_ls(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_cat(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_mem(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_reboot(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_poweroff(u32 argc, char (*argv)[SH_MAX_CMD]);
static void cmd_exit(u32 argc, char (*argv)[SH_MAX_CMD]);

static const struct cmd cmds[] = {
    { "help",     "list commands",       cmd_help },
    { "clear",    "clear screen",        cmd_clear },
    { "echo",     "print args ($VAR)",   cmd_echo },
    { "ls",       "list dir",            cmd_ls },
    { "cat",      "print file",          cmd_cat },
    { "mem",      "memory stats",        cmd_mem },
    { "reboot",   "reboot system",       cmd_reboot },
    { "poweroff", "power off",           cmd_poweroff },
    { "shutdown", "power off",           cmd_poweroff },
    { "exit",     "exit shell",          cmd_exit },
    { 0, 0, 0 }
};

/* ---------------- 命令实现 ---------------- */
static void cmd_help(u32 argc, char (*argv)[SH_MAX_CMD])
{
    const struct cmd *c;
    for (c = cmds; c->name; c++) {
        con_puts("  ");
        con_puts(c->name);
        con_puts(" - ");
        con_puts(c->help);
        con_puts("\n");
    }
}

static void cmd_clear(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_clear();
}

static void cmd_echo(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 i;
    char exp[SH_MAX_LINE];
    for (i = 1; i < argc; i++) {
        if (sh_env_expand(argv[i], exp, sizeof(exp)) == 0) con_puts(exp);
        else con_puts(argv[i]);
        con_puts(" ");
    }
    con_puts("\n");
}

static void cmd_ls(u32 argc, char (*argv)[SH_MAX_CMD])
{
    const char *path = (argc > 1) ? argv[1] : "/";
    char name[128];
    u32 idx = 0;
    u32 shown = 0;
    while (idx < 128) {
        if (fs_readdir(path, idx, name) != 0) break;
        if (name[0] == 0) break;
        con_puts(name);
        con_puts("  ");
        shown++;
        idx++;
    }
    if (!shown) con_puts("(empty)");
    con_puts("\n");
}

static void cmd_cat(u32 argc, char (*argv)[SH_MAX_CMD])
{
    int fd;
    i32 n;
    char buf[128];
    u32 i;
    if (argc < 2) { con_puts("usage: cat <file>\n"); return; }
    fd = fs_open(argv[1], O_READ);
    if (fd < 0) { con_puts("cat: cannot open\n"); return; }
    for (;;) {
        n = fs_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (i = 0; i < (u32)n; i++) con_putc(buf[i]);
    }
    fs_close(fd);
    con_puts("\n");
}

static void cmd_mem(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("  total pages : "); con_put_dec(pmm_total_pages()); con_puts("\n");
    con_puts("  used  pages : "); con_put_dec(pmm_used_pages()); con_puts("\n");
    con_puts("  reserved    : "); con_put_dec(pmm_reserved_pages()); con_puts("\n");
    con_puts("  managed KB  : "); con_put_dec(pmm_managed_bytes() / 1024u); con_puts("\n");
    con_puts("  largest free run pages: "); con_put_dec(pmm_largest_free_run()); con_puts("\n");
}

static void cmd_reboot(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("Rebooting...\n");
    con_flush();
    x_outb(0x64, 0xFE);          /* 8042 软复位（标准、安全） */
    for (;;) __asm__ __volatile__("hlt");
}

static void cmd_poweroff(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("Powering off...\n");
    con_flush();
    pm_shutdown();
    for (;;) __asm__ __volatile__("hlt");
}

static void cmd_exit(u32 argc, char (*argv)[SH_MAX_CMD])
{
    g_exit = 1u;
}

/* ---------------- 命令分派 ---------------- */
static void run_line(const char *line)
{
    sh_cmdline_t cl;
    u32 i;
    sh_parse(line, &cl);
    if (cl.argc == 0) return;
    for (i = 0; cmds[i].name; i++) {
        if (x_strcmp(cl.argv[0], cmds[i].name) == 0) {
            cmds[i].fn(cl.argc, cl.argv);
            return;
        }
    }
    con_puts("unknown command: ");
    con_puts(cl.argv[0]);
    con_puts("  (type 'help')\n");
}

/* ---------------- 交互主循环 ---------------- */
void shell_interactive(void)
{
    kbd_event_t ev;

    con_set_color(VGA_LIGHTGREEN, VGA_BLACK);
    con_puts("XOS v1.0 self-built OS | interactive terminal | type 'help'\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_flush();

    g_exit = 0;
    for (;;) {
        if (g_exit) { con_puts("bye.\n"); con_flush(); return; }

        con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
        con_puts("XOS # ");
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        g_len = 0;

        for (;;) {
            if (kbd_read_event(&ev) != 0) {
                kbd_poll();                     /* 轮询 PS/2 控制器，吸收真实按键 */
                __asm__ __volatile__("hlt");
                continue;
            }
            if (ev.type == EV_CHAR || (ev.type == EV_KEY_DOWN && ev.ch != 0)) {
                char c = (char)ev.ch;
                if (c == '\n' || c == '\r') {
                    con_puts("\n");
                    g_line[g_len] = 0;
                    if (g_len > 0) sh_hist_add(g_line);
                    run_line(g_line);
                    con_puts("\n");
                    con_flush();
                    break;
                } else if (c == '\b' || c == 127) {
                    if (g_len > 0) { g_len--; con_puts("\b \b"); }
                } else if (c >= 32 && c < 127) {
                    if (g_len < SH_MAX_LINE - 1) { g_line[g_len++] = c; con_putc(c); }
                }
                con_flush();
            }
        }
    }
}
