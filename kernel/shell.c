/* XOS 用户空间 Shell —— 命令行解析 / 环境变量 / 管道重定向 / 作业控制 / 命令历史 */
#include "shell.h"
#include "console.h"

static int sh_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)(*a) - (u8)(*b);
}

static void sh_strncpy(char *d, const char *s, u32 n)
{
    u32 i;
    for (i = 0u; i < n && s[i]; i++) d[i] = s[i];
    if (n > 0u) d[i < n ? i : n - 1u] = 0;
}

static u32 sh_strlen(const char *s)
{
    u32 n = 0u;
    while (s[n]) n++;
    return n;
}

static sh_env_t  g_env[SH_MAX_ENV];
static sh_job_t  g_jobs[SH_MAX_JOBS];
static char      g_hist[SH_HIST_MAX][SH_MAX_CMD];
static u32       g_hist_cnt;
static u32       g_hist_pos;
static u32       g_job_seq;

/* ---------- 命令行解析 ---------- */

static int sh_isspace(char c) { return c == ' ' || c == '\t'; }

static int sh_istoken(char c)
{
    return c == '|' || c == '>' || c == '<' || c == '&';
}

void sh_parse(const char *line, sh_cmdline_t *out)
{
    u32 i = 0u, argc = 0u, pos = 0u;
    u32 in_quote = 0u;
    u32 t;

    for (t = 0u; t < SH_MAX_TOKENS; t++) out->argv[t][0] = 0;
    out->argc = 0u; out->in_fd = 0u; out->out_fd = 0u;
    out->pipe_to = 0u; out->background = 0u;
    out->in_file[0] = 0; out->out_file[0] = 0;

    while (line[i] && i < SH_MAX_LINE) {
        char c = line[i];
        if (in_quote) {
            if (c == '"') { in_quote = 0u; i++; continue; }
            if (pos < SH_MAX_CMD - 1u && argc < SH_MAX_TOKENS)
                out->argv[argc][pos++] = c;
            i++;
            continue;
        }
        if (c == '"') { in_quote = 1u; i++; continue; }
        if (sh_isspace(c) || sh_istoken(c)) {
            if (argc < SH_MAX_TOKENS) out->argv[argc][pos] = 0;
            if (pos > 0u) argc++;
            pos = 0u;
            if (c == '|') { out->pipe_to = 1u; i++; continue; }
            if (c == '>') {
                out->out_fd = 1u;
                if (line[i + 1] == '>') { out->out_fd = 2u; i++; }
                i++;
                while (line[i] == ' ') i++;
                for (pos = 0u; line[i] && line[i] != ' ' && line[i] != '|' && line[i] != '>' && pos < SH_MAX_CMD - 1u; pos++)
                    out->out_file[pos] = line[i++];
                out->out_file[pos] = 0; pos = 0u;
                continue;
            }
            if (c == '<') {
                out->in_fd = 1u; i++;
                while (line[i] == ' ') i++;
                for (pos = 0u; line[i] && line[i] != ' ' && line[i] != '|' && line[i] != '<' && pos < SH_MAX_CMD - 1u; pos++)
                    out->in_file[pos] = line[i++];
                out->in_file[pos] = 0; pos = 0u;
                continue;
            }
            if (c == '&') { out->background = 1u; i++; continue; }
            i++;
            continue;
        }
        if (pos < SH_MAX_CMD - 1u && argc < SH_MAX_TOKENS)
            out->argv[argc][pos++] = c;
        i++;
    }
    if (argc < SH_MAX_TOKENS) out->argv[argc][pos] = 0;
    if (pos > 0u) argc++;
    out->argc = argc;
}

/* ---------- 环境变量 ---------- */

int sh_env_set(const char *name, const char *val)
{
    u32 i, free_slot = SH_MAX_ENV;
    for (i = 0u; i < SH_MAX_ENV; i++) {
        if (g_env[i].used && sh_strcmp(g_env[i].name, name) == 0)
            break;
        if (!g_env[i].used && free_slot == SH_MAX_ENV)
            free_slot = i;
    }
    if (i < SH_MAX_ENV) free_slot = i;
    if (free_slot == SH_MAX_ENV) return -1;
    sh_strncpy(g_env[free_slot].name, name, SH_ENV_NAME_LEN - 1u);
    sh_strncpy(g_env[free_slot].val, val, SH_ENV_VAL_LEN - 1u);
    g_env[free_slot].used = 1u;
    return 0;
}

int sh_env_get(const char *name, char *buf, u32 len)
{
    u32 i;
    for (i = 0u; i < SH_MAX_ENV; i++)
        if (g_env[i].used && sh_strcmp(g_env[i].name, name) == 0) {
            sh_strncpy(buf, g_env[i].val, len);
            return 0;
        }
    return -1;
}

int sh_env_unset(const char *name)
{
    u32 i;
    for (i = 0u; i < SH_MAX_ENV; i++)
        if (g_env[i].used && sh_strcmp(g_env[i].name, name) == 0) {
            g_env[i].used = 0u;
            return 0;
        }
    return -1;
}

int sh_env_expand(const char *src, char *dst, u32 len)
{
    u32 i = 0u, o = 0u;
    while (src[i] && o < len - 1u) {
        if (src[i] == '$' && src[i + 1]) {
            char nm[SH_ENV_NAME_LEN]; u32 k = 0u; char buf[SH_ENV_VAL_LEN];
            i++;
            while (src[i] && src[i] != ' ' && src[i] != '\t' && src[i] != '$' && src[i] != '"' && k < SH_ENV_NAME_LEN - 1u)
                nm[k++] = src[i++];
            nm[k] = 0;
            if (sh_env_get(nm, buf, sizeof(buf)) == 0) {
                u32 j;
                for (j = 0u; buf[j] && o < len - 1u; j++) dst[o++] = buf[j];
            }
            continue;
        }
        dst[o++] = src[i++];
    }
    dst[o] = 0;
    return 0;
}

void sh_env_export(void)
{
    u32 i, k;
    for (i = 0u; i < SH_MAX_ENV; i++)
        if (g_env[i].used)
            for (k = 0u; k < SH_ENV_VAL_LEN && g_env[i].val[k]; k++)
                con_putc(g_env[i].val[k]);
}

/* ---------- 作业控制 ---------- */

void sh_job_start(u32 pid, u32 *jid)
{
    u32 i;
    for (i = 0u; i < SH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) {
            g_jobs[i].used = 1u;
            g_jobs[i].pid = pid;
            g_jobs[i].state = 0u;
            g_jobs[i].jid = ++g_job_seq;
            if (jid) *jid = g_jobs[i].jid;
            return;
        }
    }
    if (jid) *jid = 0u;
}

int sh_job_wait(u32 jid)
{
    u32 i;
    for (i = 0u; i < SH_MAX_JOBS; i++)
        if (g_jobs[i].used && g_jobs[i].jid == jid) {
            g_jobs[i].state = 2u;
            return 0;
        }
    return -1;
}

/* ---------- 命令历史 ---------- */

void sh_hist_add(const char *line)
{
    if (sh_strlen(line) == 0u) return;
    if (g_hist_cnt > 0u && sh_strcmp(g_hist[(g_hist_pos + SH_HIST_MAX - 1u) % SH_HIST_MAX], line) == 0)
        return;
    sh_strncpy(g_hist[g_hist_pos], line, SH_MAX_CMD - 1u);
    g_hist_pos = (g_hist_pos + 1u) % SH_HIST_MAX;
    if (g_hist_cnt < SH_HIST_MAX) g_hist_cnt++;
}

u32 sh_hist_count(void) { return g_hist_cnt; }

const char *sh_hist_get(u32 idx)
{
    if (idx >= g_hist_cnt) return 0;
    return g_hist[(g_hist_pos + SH_HIST_MAX - g_hist_cnt + idx) % SH_HIST_MAX];
}

/* ---------- 初始化 / 转储 ---------- */

void shell_init(void)
{
    u32 i;
    for (i = 0u; i < SH_MAX_ENV; i++) g_env[i].used = 0u;
    for (i = 0u; i < SH_MAX_JOBS; i++) g_jobs[i].used = 0u;
    g_hist_cnt = 0u; g_hist_pos = 0u; g_job_seq = 0u;
}

void shell_dump(void)
{
    u32 i, k;
    con_puts("  shell: env=");
    for (i = 0u; i < SH_MAX_ENV; i++)
        if (g_env[i].used) {
            con_puts(g_env[i].name);
            con_puts("=");
            for (k = 0u; g_env[i].val[k]; k++) con_putc(g_env[i].val[k]);
            con_puts(" ");
        }
    con_puts(" hist=");
    for (i = 0u; i < g_hist_cnt; i++) {
        con_puts("[");
        con_puts(g_hist[i]);
        con_puts("] ");
    }
    con_puts("\n");
}

/* ---------- 自检 ---------- */

int sh_selftest(void)
{
    sh_cmdline_t cl;
    char buf[64];
    u32 i, pid;

    /* 1-4: 基础解析 */
    sh_parse("ls -l /tmp", &cl);
    if (cl.argc != 3u) return 1;
    if (sh_strcmp(cl.argv[0], "ls") != 0) return 2;
    if (sh_strcmp(cl.argv[1], "-l") != 0) return 3;
    if (sh_strcmp(cl.argv[2], "/tmp") != 0) return 4;

    /* 5-6: 引号 */
    sh_parse("echo \"hello world\"", &cl);
    if (cl.argc != 2u) return 5;
    if (sh_strcmp(cl.argv[1], "hello world") != 0) return 6;

    /* 7-8: 重定向 */
    sh_parse("ls > out.txt", &cl);
    if (cl.out_fd != 1u) return 7;
    if (sh_strcmp(cl.out_file, "out.txt") != 0) return 8;

    /* 9: 追加 */
    sh_parse("ls >> log.txt", &cl);
    if (cl.out_fd != 2u) return 9;

    /* 10: 输入重定向 */
    sh_parse("wc < in.txt", &cl);
    if (cl.in_fd != 1u) return 10;
    if (sh_strcmp(cl.in_file, "in.txt") != 0) return 11;

    /* 12-13: 管道 / 后台 */
    sh_parse("cat a | grep b", &cl);
    if (cl.pipe_to != 1u) return 12;
    sh_parse("sleep 5 &", &cl);
    if (cl.background != 1u) return 13;

    /* 14-18: 环境变量 */
    if (sh_env_set("HOME", "/root") != 0) return 14;
    if (sh_env_get("HOME", buf, sizeof(buf)) != 0) return 15;
    if (sh_strcmp(buf, "/root") != 0) return 16;
    if (sh_env_set("PATH", "/bin:/usr/bin") != 0) return 17;
    if (sh_env_get("PATH", buf, sizeof(buf)) != 0) return 18;

    /* 19-21: 覆盖 / 展开 */
    if (sh_env_set("HOME", "/home/xos") != 0) return 19;
    if (sh_env_get("HOME", buf, sizeof(buf)) != 0) return 20;
    if (sh_strcmp(buf, "/home/xos") != 0) return 21;

    /* 22: $VAR 展开 */
    sh_env_expand("cd $HOME", buf, sizeof(buf));
    if (sh_strcmp(buf, "cd /home/xos") != 0) return 22;

    /* 23: 未定义变量不展开 */
    sh_env_expand("x$NOPE y", buf, sizeof(buf));
    if (sh_strcmp(buf, "x y") != 0) return 23;

    /* 24: 删除 */
    if (sh_env_unset("NOPE") != -1) return 24;
    if (sh_env_unset("PATH") != 0) return 25;
    if (sh_env_get("PATH", buf, sizeof(buf)) != -1) return 26;

    /* 27-29: 作业 */
    pid = 7u;
    sh_job_start(pid, &i);
    if (i == 0u) return 27;
    if (sh_job_wait(i) != 0) return 28;
    pid = 9u;
    sh_job_start(pid, &i);
    if (i == 0u) return 29;

    /* 30-34: 历史 */
    sh_hist_add("ls");
    sh_hist_add("cat x");
    sh_hist_add("cat x");   /* 相邻重复去重 */
    if (sh_hist_count() != 2u) return 30;
    if (sh_strcmp(sh_hist_get(0u), "ls") != 0) return 31;
    if (sh_strcmp(sh_hist_get(1u), "cat x") != 0) return 32;
    sh_hist_add("");
    if (sh_hist_count() != 2u) return 33;
    sh_hist_add("cat x");   /* 与最后一条相同 → 去重 */
    if (sh_hist_count() != 2u) return 34;

    /* 35-38: 复杂行 */
    sh_parse("a b c > f | d &", &cl);
    if (cl.argc != 4u) return 35;   /* a b c d 四个命令 token */
    if (cl.out_fd != 1u) return 36;
    if (cl.pipe_to != 1u) return 37;
    if (cl.background != 1u) return 38;

    /* 39-40: 环境上限（清空后填满不同名变量，拒绝新增，释放后恢复） */
    for (i = 0u; i < SH_MAX_ENV; i++) g_env[i].used = 0u;
    for (i = 0u; i < SH_MAX_ENV; i++) {
        char nm[4];
        nm[0] = 'K'; nm[1] = (char)('0' + i); nm[2] = 0;
        if (sh_env_set(nm, "1") != 0) return 39;
    }
    if (sh_env_set("FULL", "x") != -1) return 39;
    if (sh_env_unset("K0") != 0) return 39;
    if (sh_env_set("FULL", "x") != 0) return 40;

    return 0;
}
