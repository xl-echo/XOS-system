/* ============================================================================
 * XOS 文本工具集（第 39/52 册配套应用）
 *   edit <file> — 行编辑器：列/追加/插入/删除/替换/保存
 *   clip        — 系统剪贴板：set/get/clear（内核内存，跨会话）
 *   note <text> — 记事本：追加写入 /note.txt
 * 全部自研、文本模式直接可用；经 shell 命令表接入。
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/string.h"
#include "../include/fs.h"
#include "../include/keyboard.h"
#include "../include/kmalloc.h"
#include "../include/shell.h"

#define KMALLOC_PLAIN(size) kmalloc((size), 8u, 0u)

#define EDIT_MAX_LINES 80u
#define EDIT_MAX_LEN   192u

/* 行数组：kmalloc 动态分配，避免占用 bss */
static char **g_lines;
static u32 g_nlines;
static u32 g_cap;
static int g_dirty;

/* ---------------- 行管理 ---------------- */
static int edit_line_add(const char *s, u32 pos)
{
    char *nl;
    u32 i;
    if (g_nlines >= EDIT_MAX_LINES || pos > g_nlines) return -1;
    if (g_nlines >= g_cap) {
        char **nw = KMALLOC_PLAIN((g_cap + 16u) * sizeof(char *));
        u32 old = g_cap;
        if (!nw) return -1;
        for (i = 0; i < g_nlines; i++) nw[i] = g_lines[i];
        if (g_lines) kfree(g_lines);
        g_lines = nw;
        g_cap = old + 16u;
    }
    nl = KMALLOC_PLAIN(strlen(s) + 1u);
    if (!nl) return -1;
    strcpy(nl, s);
    for (i = g_nlines; i > pos; i--) g_lines[i] = g_lines[i - 1];
    g_lines[pos] = nl;
    g_nlines++;
    g_dirty = 1;
    return 0;
}

static void edit_lines_free(void)
{
    u32 i;
    if (!g_lines) return;
    for (i = 0; i < g_nlines; i++) if (g_lines[i]) kfree(g_lines[i]);
    kfree(g_lines);
    g_lines = 0;
    g_nlines = 0;
    g_cap = 0;
    g_dirty = 0;
}

static int edit_load(const char *path)
{
    int fd;
    i32 n, i;
    char buf[EDIT_MAX_LEN + 1];
    char acc[EDIT_MAX_LEN + 1];
    u32 alen = 0;
    edit_lines_free();
    fd = fs_open(path, O_READ);
    if (fd < 0) return -1;
    for (;;) {
        n = fs_read(fd, buf, EDIT_MAX_LEN);
        if (n <= 0) break;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                acc[alen] = 0;
                if (edit_line_add(acc, g_nlines) != 0) { fs_close(fd); return -2; }
                alen = 0;
            } else if (alen < EDIT_MAX_LEN) {
                acc[alen++] = c;
            }
        }
    }
    fs_close(fd);
    if (alen || g_nlines == 0) {
        acc[alen] = 0;
        if (edit_line_add(acc, g_nlines) != 0) return -2;
    }
    g_dirty = 0;
    return 0;
}

static int edit_save(const char *path)
{
    int fd;
    u32 i;
    fd = fs_open(path, O_WRITE | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    for (i = 0; i < g_nlines; i++) {
        if (fs_write(fd, g_lines[i], strlen(g_lines[i])) < 0) { fs_close(fd); return -2; }
        if (fs_write(fd, "\n", 1) < 0) { fs_close(fd); return -2; }
    }
    fs_close(fd);
    g_dirty = 0;
    return 0;
}

static void edit_list(u32 from, u32 cnt)
{
    u32 i, end;
    if (g_nlines == 0) { con_puts("  (空文件)\n"); return; }
    end = from + cnt;
    if (end > g_nlines) end = g_nlines;
    for (i = from; i < end; i++) {
        con_put_dec(i + 1);
        con_puts(": ");
        con_puts(g_lines[i]);
        con_puts("\n");
    }
}

void cmd_edit(u32 argc, char (*argv)[SH_MAX_CMD])
{
    const char *path = (argc > 1) ? argv[1] : "/edit.txt";
    char cmd[SH_MAX_LINE];
    char work[EDIT_MAX_LEN + 1];
    u32 len, i;
    int loaded = 0;

    if (edit_load(path) == 0) loaded = 1;
    con_puts("XOS edit  ");
    con_puts(path);
    con_puts(loaded ? "  (已载入 " : "  (新建 ");
    con_put_dec(g_nlines);
    con_puts(" 行)\n");
    con_puts("  命令: l[ n]=列表  a <text>=追加  i <n> <text>=插入  d <n>=删除\n");
    con_puts("        s <n> <text>=替换  w=保存  q=退出  ?=帮助\n");

    for (;;) {
        kbd_event_t ev;
        con_puts(": ");
        con_flush();
        len = 0;
        for (;;) {
            if (kbd_read_event(&ev) != 0) { kbd_poll(); __asm__ __volatile__("hlt"); continue; }
            if (ev.type == EV_CHAR || (ev.type == EV_KEY_DOWN && ev.ch != 0)) {
                char c = (char)ev.ch;
                if (c == '\n' || c == '\r') { con_puts("\n"); break; }
                if (c == '\b' || c == 127) { if (len) { len--; con_puts("\b \b"); } }
                else if (c >= 32 && c < 127 && len < SH_MAX_LINE - 1) { cmd[len++] = c; con_putc(c); }
                con_flush();
            } else if (ev.type == EV_KEY_DOWN && ev.key == KEY_ENTER) { con_puts("\n"); break; }
        }
        cmd[len] = 0;

        if (cmd[0] == 'q' && cmd[1] == 0) break;
        if (cmd[0] == 'w' && cmd[1] == 0) {
            if (edit_save(path) == 0) con_puts("  saved.\n");
            else con_puts("  save failed!\n");
            continue;
        }
        if (cmd[0] == '?' ) { con_puts("  l / a <t> / i <n> <t> / d <n> / s <n> <t> / w / q\n"); continue; }
        if (cmd[0] == 'l') {
            edit_list(0, g_nlines);
            continue;
        }
        if (cmd[0] == 'a' && cmd[1] == ' ') {
            strcpy(work, cmd + 2);
            if (edit_line_add(work, g_nlines) != 0) con_puts("  fail: 行数上限\n");
            continue;
        }
        if (cmd[0] == 'i' && cmd[1] == ' ') {
            /* i <n> <text> */
            u32 pos = (u32)atoi(cmd + 2);
            char *sp = strchr(cmd + 2, ' ');
            if (pos == 0 || !sp) { con_puts("  usage: i <行号> <文本>\n"); continue; }
            while (*sp == ' ') sp++;
            if (edit_line_add(sp, pos - 1) != 0) con_puts("  fail\n");
            continue;
        }
        if (cmd[0] == 'd' && cmd[1] == ' ') {
            u32 pos = (u32)atoi(cmd + 2);
            if (pos == 0 || pos > g_nlines) { con_puts("  usage: d <行号>\n"); continue; }
            if (g_lines[pos - 1]) kfree(g_lines[pos - 1]);
            for (i = pos; i < g_nlines; i++) g_lines[i - 1] = g_lines[i];
            g_nlines--;
            g_dirty = 1;
            continue;
        }
        if (cmd[0] == 's' && cmd[1] == ' ') {
            u32 pos = (u32)atoi(cmd + 2);
            char *sp = strchr(cmd + 2, ' ');
            char *nl;
            if (pos == 0 || pos > g_nlines || !sp) { con_puts("  usage: s <行号> <文本>\n"); continue; }
            while (*sp == ' ') sp++;
            nl = KMALLOC_PLAIN(strlen(sp) + 1u);
            if (!nl) { con_puts("  fail\n"); continue; }
            strcpy(nl, sp);
            kfree(g_lines[pos - 1]);
            g_lines[pos - 1] = nl;
            g_dirty = 1;
            continue;
        }
        con_puts("  ? 查看命令\n");
    }
    if (g_dirty) {
        con_puts("  未保存修改! 输入 y 保存 / 其他放弃: ");
        con_flush();
        len = 0;
        for (;;) {
            kbd_event_t ev;
            if (kbd_read_event(&ev) != 0) { kbd_poll(); __asm__ __volatile__("hlt"); continue; }
            if (ev.type == EV_CHAR || (ev.type == EV_KEY_DOWN && ev.ch != 0)) {
                char c = (char)ev.ch;
                if (c == '\n' || c == '\r') break;
                if ((c == 'y' || c == 'Y') && len == 0) { len = 1; con_putc(c); con_flush(); }
            }
        }
        con_puts("\n");
        if (len == 1 && edit_save(path) == 0) con_puts("  saved.\n");
    }
    edit_lines_free();
}

/* ---------------- clip：系统剪贴板 ---------------- */
#define CLIP_MAX 240u
static char g_clip[CLIP_MAX + 1];
static u32 g_clip_len;

void cmd_clip(u32 argc, char (*argv)[SH_MAX_CMD])
{
    u32 n;
    if (argc < 2) {
        if (g_clip_len == 0) { con_puts("clip: (空)\n"); return; }
        con_puts("clip: ");
        con_puts(g_clip);
        con_puts("\n");
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        g_clip_len = 0; g_clip[0] = 0;
        con_puts("clip: 已清空\n");
        return;
    }
    if (strcmp(argv[1], "set") == 0 && argc > 2) {
        g_clip[0] = 0;
        for (n = 2; n < argc; n++) {
            if (strlen(g_clip) + strlen(argv[n]) + 1 > CLIP_MAX) break;
            if (n > 2) strcat(g_clip, " ");
            strcat(g_clip, argv[n]);
        }
        g_clip_len = strlen(g_clip);
        con_puts("clip: 已复制 ");
        con_put_dec(g_clip_len);
        con_puts(" 字符\n");
        return;
    }
    if (strcmp(argv[1], "get") == 0) {
        if (g_clip_len == 0) con_puts("clip: (空)\n");
        else { con_puts("clip: "); con_puts(g_clip); con_puts("\n"); }
        return;
    }
    /* clip <text> 等价 set */
    g_clip[0] = 0;
    for (n = 1; n < argc; n++) {
        if (strlen(g_clip) + strlen(argv[n]) + 1 > CLIP_MAX) break;
        if (n > 1) strcat(g_clip, " ");
        strcat(g_clip, argv[n]);
    }
    g_clip_len = strlen(g_clip);
    con_puts("clip: 已复制\n");
}

/* ---------------- note：记事本（追加 /note.txt） ---------------- */
void cmd_note(u32 argc, char (*argv)[SH_MAX_CMD])
{
    int fd;
    u32 n;
    char line[SH_MAX_LINE];
    if (argc < 2) { con_puts("usage: note <内容>\n"); return; }
    fd = fs_open("/note.txt", O_WRITE | O_CREAT | O_APPEND);
    if (fd < 0) { con_puts("note: 无法打开 /note.txt\n"); return; }
    line[0] = 0;
    for (n = 1; n < argc; n++) {
        if (strlen(line) + strlen(argv[n]) + 1 >= sizeof(line)) break;
        if (n > 1) strcat(line, " ");
        strcat(line, argv[n]);
    }
    fs_write(fd, line, strlen(line));
    fs_write(fd, "\n", 1);
    fs_close(fd);
    con_puts("note: 已记录\n");
}
