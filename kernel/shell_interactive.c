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
#include "../include/desk_gui.h"
#include "../include/cpu.h"
#include "../include/user.h"
#include "../include/xos_hello_bin.h"
#include "../include/crash.h"
#include "../include/dbg.h"

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

/* 应用层命令（apps_*.c，全局导出） */
void cmd_calc(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_clock(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_setclock(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_sysinfo(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_df(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_ps(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_netinfo(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_about(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_edit(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_clip(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_note(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_snake(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_g2048(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_docs(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_gsearch(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_touch(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_beep(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_dmesg(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_crashdump(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_clearcrash(u32 argc, char (*argv)[SH_MAX_CMD]);
void cmd_uptime(u32 argc, char (*argv)[SH_MAX_CMD]);

/* 图形桌面（第 35 册）：从终端重新进入桌面 */
static void cmd_desktop(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("Starting graphical desktop...\n");
    con_flush();
    desk_gui_run();
    con_puts("Desktop exited, back to terminal.\n");
}

/* ring3 用户程序执行：加载内置 hello.elf → iret 进用户态 → 退出回内核 */
static void cmd_exec(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 entry = 0u, rc;

    (void)argc; (void)argv;
    if (!cpu_user_supported()) {
        con_puts("  [exec] ring3 not ready\n");
        return;
    }
    con_puts("  [exec] loading built-in user program...\n");
    rc = user_load_elf(xos_hello_bin, xos_hello_bin_len, &entry);
    if (rc != 0u) {
        con_puts("  [exec] load failed rc=");
        con_put_dec(rc);
        con_puts("\n");
        return;
    }
    con_puts("  [exec] entry=");
    con_put_hex32(entry);
    con_puts(" pages=");
    con_put_dec(user_pages_used());
    con_puts(" esp0=");
    con_put_hex32(cpu_tss_get_esp0());
    con_puts("\n  [exec] entering ring3...\n");
    con_flush();

    /* 进入 ring3：hello 在用户态运行，int 0x80 完成系统调用。
     * 用户程序退出/ring3 异常时 user_return_to_kernel 把 isr 帧改造成
     * iret 回 user_exit_stub（汇编桩）→ user_after_exit 清理 → 重入 shell，
     * 因此本函数不会沿原路返回（此处为防御性尾保护）。 */
    user_exec(entry);
    con_puts("  [exec] user program exited, back to kernel\n");
    user_cleanup();
}

/* 内核日志查看（dmesg）：上次关机日志 + 本次启动日志 */
void cmd_dmesg(u32 argc, char (*argv)[SH_MAX_CMD])
{
    (void)argc; (void)argv;
    dbg_log_show();
}

/* 崩溃转储查看：显示固定内存区保存的 panic 现场 */
void cmd_crashdump(u32 argc, char (*argv)[SH_MAX_CMD])
{
    (void)argc; (void)argv;
    crash_dump_show();
}

/* 崩溃转储清除 */
void cmd_clearcrash(u32 argc, char (*argv)[SH_MAX_CMD])
{
    (void)argc; (void)argv;
    crash_dump_clear();
}

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
    { "exec",     "run ring3 user prog", cmd_exec },
    /* 应用层 */
    { "calc",     "calculator",          cmd_calc },
    { "clock",    "show clock",          cmd_clock },
    { "setclock", "set clock HH MM",     cmd_setclock },
    { "sysinfo",  "system info panel",   cmd_sysinfo },
    { "df",       "memory disk stats",   cmd_df },
    { "ps",       "app process table",   cmd_ps },
    { "netinfo",  "network interface",   cmd_netinfo },
    { "about",    "version info",        cmd_about },
    { "edit",     "line editor <file>",  cmd_edit },
    { "clip",     "clipboard",           cmd_clip },
    { "note",     "note to /note.txt",   cmd_note },
    { "snake",    "play snake",          cmd_snake },
    { "g2048",    "play 2048",           cmd_g2048 },
    { "docs",     "builtin user manual", cmd_docs },
    { "gsearch",  "search file by name", cmd_gsearch },
    { "touch",    "create empty file",   cmd_touch },
    { "beep",     "speaker beep",        cmd_beep },
    { "dmesg",    "show kernel log",     cmd_dmesg },
    { "crashdump","show crash dump",     cmd_crashdump },
    { "clearcrash","clear crash dump",   cmd_clearcrash },
    { "uptime",   "uptime stats",        cmd_uptime },
    { "desktop",  "graphical desktop",   cmd_desktop },
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
    con_puts("Persisting kernel log...\n");
    con_flush();
    dbg_log_persist();               /* 关机前把日志缓冲落盘（磁盘末尾区） */
    con_puts("Rebooting...\n");
    con_flush();
    x_outb(0x64, 0xFE);          /* 8042 软复位（标准、安全） */
    for (;;) __asm__ __volatile__("hlt");
}

static void cmd_poweroff(u32 argc, char (*argv)[SH_MAX_CMD])
{
    con_puts("Persisting kernel log...\n");
    con_flush();
    dbg_log_persist();               /* 关机前把日志缓冲落盘（磁盘末尾区） */
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
            } else if (ev.type == EV_KEY_DOWN) {
                if (ev.key == KEY_ENTER) {
                    con_puts("\n");
                    g_line[g_len] = 0;
                    if (g_len > 0) sh_hist_add(g_line);
                    run_line(g_line);
                    con_puts("\n");
                    con_flush();
                    break;
                } else if (ev.key == KEY_BACKSP) {
                    if (g_len > 0) { g_len--; con_puts("\b \b"); }
                    con_flush();
                }
            }
        }
    }
}
