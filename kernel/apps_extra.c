/* ============================================================================
 * XOS 工具与应用集（第 56/72 册配套应用，文本模式）
 *   docs     — 内置使用手册（分页浏览）
 *   gsearch  — 简易文件搜索（按名称关键字，当前目录）
 *   touch    — 创建空文件
 *   beep     — PC 扬声器蜂鸣（PIT 通道2，安全端口操作）
 *   uptime   — 运行时间与调度统计
 * 全部自研、零外部依赖；经 shell 命令表接入。
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/string.h"
#include "../include/fs.h"
#include "../include/keyboard.h"
#include "../include/task.h"
#include "../include/irq.h"
#include "../include/shell.h"
#include "../include/rtc.h"

static inline u8 x_inb(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void x_outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ================= docs：内置手册（.rodata 分页） ================= */
static const char *const g_docs[] = {
    "================ XOS 用户手册 ================",
    "XOS v1.0 完全自研操作系统   [空格]下一页 [b]上一页 [q]退出",
    "",
    "一、系统架构",
    "  * 引导: BIOS -> MBR -> Stage2 -> 内核(0x10000)",
    "  * 内存: E820 探测 + 分页虚拟内存 + slab 内核堆",
    "  * 进程: 任务描述符/CFS 调度/信号/等待队列",
    "  * 文件: VFS + tmpfs/devfs + 权限与一致性",
    "  * 显示: VGA 文本模式(80x25) + VBE 图形探测",
    "  * 网络: 以太网驱动 + ARP/IP/TCP/UDP 协议栈",
    "  * 安全: RNG/SHA256/密钥环/审计/权限矩阵",
    "  * 多用户: 账户/密码哈希/登录会话",
    "",
    "二、基础命令",
    "  help     命令列表        clear   清屏",
    "  ls <dir> 列目录          cat <f> 查看文件",
    "  mem      内存统计        df      内存/可用率",
    "  ps       应用进程表      sysinfo 系统信息面板",
    "  netinfo  网卡信息        about   版本信息",
    "  reboot   重启            poweroff 关机",
    "",
    "三、应用命令",
    "  calc     计算器(表达式+括号+ans)",
    "  clock    时钟显示        date    显示/设置日期时间",
    "  setclock <时> <分> 设置时间",
    "  edit <f> 文本编辑器      clip    系统剪贴板",
    "  note     记事本追加      touch   创建空文件",
    "  gsearch  文件搜索        beep    扬声器蜂鸣",
    "  snake    贪吃蛇          g2048   2048 游戏",
    "  uptime   运行时间(含RTC) dmesg   内核日志",
    "  ps       内核任务表      crashdump 崩溃转储",
    "",
    "三.5、date 用法",
    "  date                 显示当前日期时间",
    "  date YYYY MM DD HH MM [SS]  设置日期时间",
    "  * 基于真实 CMOS RTC，重启后保留",
    "",
    "四、登录与安全",
    "  * 首次开机创建管理员(admin/admin123)",
    "  * 密码经哈希存储，连续失败锁定账户",
    "  * 内核态/用户态隔离，系统调用审计",
    "",
    "五、开发与扩展",
    "  * 全部源码在 kernel/ 下，C + 少量汇编",
    "  * 构建: build/build.py (GCC i686-elf)",
    "  * 镜像: build/xos.img 可直接入虚拟机",
    "  * 不依赖任何外部内核/闭源/收费方案",
    "",
    "（按空格翻页查看全部内容）",
};

void cmd_docs(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 total = sizeof(g_docs) / sizeof(g_docs[0]);
    u32 page = 0, per = 21, pages;
    kbd_event_t ev;
    pages = (total + per - 1) / per;
    for (;;) {
        u32 i, from = page * per, to = from + per;
        con_clear();
        if (to > total) to = total;
        for (i = from; i < to; i++) {
            con_puts(g_docs[i]);
            con_puts("\n");
        }
        con_set_color(VGA_DARKGRAY, VGA_BLACK);
        con_puts("--- page ");
        con_put_dec(page + 1);
        con_puts("/");
        con_put_dec(pages);
        con_puts(" ---");
        con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        con_flush();
        for (;;) {
            if (kbd_read_event(&ev) != 0) { kbd_poll(); __asm__ __volatile__("hlt"); continue; }
            if (ev.type == EV_CHAR) {
                char c = (char)ev.ch;
                if (c == ' ') { if (page + 1 < pages) page++; break; }
                if (c == 'b' || c == 'B') { if (page) page--; break; }
                if (c == 'q' || c == 'Q') { con_clear(); return; }
            } else if (ev.type == EV_KEY_DOWN && ev.key == KEY_ESC) { con_clear(); return; }
        }
    }
}

/* ================= gsearch：文件名搜索 ================= */
void cmd_gsearch(u32 argc, char (*argv)[SH_MAX_CMD])
{
    const char *kw = (argc > 1) ? argv[1] : "";
    const char *path = (argc > 2) ? argv[2] : "/";
    char name[FS_NAME_MAX];
    u32 idx = 0, found = 0;
    if (kw[0] == 0) { con_puts("usage: gsearch <关键字> [目录]\n"); return; }
    con_puts("搜索 ");
    con_puts(path);
    con_puts(" 中匹配 \"");
    con_puts(kw);
    con_puts("\" 的文件:\n");
    while (idx < 512) {
        if (fs_readdir(path, idx, name) != 0) break;
        if (name[0] == 0) break;
        if (strstr(name, kw)) {
            con_puts("  [*] ");
            con_puts(name);
            con_puts("\n");
            found++;
        }
        idx++;
    }
    if (!found) con_puts("  (无匹配)\n");
    else { con_puts("  共 "); con_put_dec(found); con_puts(" 个匹配\n"); }
}

/* ================= touch：创建空文件 ================= */
void cmd_touch(u32 argc, char (*argv)[SH_MAX_CMD])
{
    int fd;
    if (argc < 2) { con_puts("usage: touch <file>\n"); return; }
    fd = fs_open(argv[1], O_WRITE | O_CREAT);
    if (fd < 0) { con_puts("touch: failed rc="); con_put_dec((u32)(-fd)); con_puts("\n"); return; }
    fs_close(fd);
    con_puts("touch: ok\n");
}

/* ================= beep：PC 扬声器 ================= */
static void beep_tone(u32 freq_hz, u32 ms)
{
    u32 div, v;
    if (freq_hz == 0) return;
    div = 1193180u / freq_hz;
    x_outb(0x43, 0xB6);
    x_outb(0x42, (u8)(div & 0xFF));
    x_outb(0x42, (u8)((div >> 8) & 0xFF));
    v = x_inb(0x61);
    x_outb(0x61, (u8)(v | 0x03));
    msleep(ms);
    x_outb(0x61, (u8)(v & ~0x03));
}

void cmd_beep(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 freq = (argc > 1) ? (u32)atoi(argv[1]) : 880u;
    u32 dur  = (argc > 2) ? (u32)atoi(argv[2]) : 150u;
    if (freq < 20 || freq > 20000) { con_puts("beep: 频率 20-20000Hz\n"); return; }
    con_puts("beep: ");
    con_put_dec(freq);
    con_puts("Hz ");
    con_put_dec(dur);
    con_puts("ms\n");
    beep_tone(freq, dur);
}

/* ================= uptime：运行时间 ================= */
void cmd_uptime(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 ticks = pit_tick_count();
    u32 sec = ticks / 100u;
    char buf[32];
    rtc_time_t t;
    con_puts("  当前时间 : ");
    if (rtc_read_all(&t) == 0) { rtc_format(buf, sizeof(buf), &t); con_puts(buf); }
    else con_puts("(RTC 不可用)");
    con_puts("\n");
    con_puts("  运行时间 : ");
    con_put_dec(sec / 3600u); con_puts("时 ");
    con_put_dec((sec % 3600u) / 60u); con_puts("分 ");
    con_put_dec(sec % 60u); con_puts("秒\n");
    con_puts("  心跳计数 : "); con_put_dec(ticks); con_puts("\n");
}
