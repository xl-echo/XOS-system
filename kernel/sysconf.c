/* ============================================================================
 * XOS 系统配置持久化子系统实现
 * 自研：键值对配置文件 /disk/xos.conf（XFFS 持久化，重启保留）
 * 格式：每行 "key=value\n"，'#' 开头为注释，值仅 ASCII 数字
 * ========================================================================== */
#include "sysconf.h"
#include "fs.h"
#include "xffs.h"
#include "console.h"
#include "string.h"

#define SYSCONF_PATH   "/disk/xos.conf"
#define SYSCONF_MAX    8192u        /* 配置文件上限 8KB（安全钳制） */
#define SYSCONF_KEYMAX 48u

/* 读取整文件内容到 buf；返回字节数（不含文件时返回 0，文件过大返回 -1） */
static i32 sysconf_read_all(char *buf, u32 cap)
{
    i32 fd = fs_open(SYSCONF_PATH, O_READ);
    i32 rd, total = 0;
    if (fd < 0) return 0;                 /* 文件不存在 = 空配置 */
    for (;;) {
        rd = fs_read(fd, buf + total, (i32)(cap - (u32)total));
        if (rd <= 0) break;
        total += rd;
        if ((u32)total >= cap - 1u) { fs_close(fd); return -1; }
    }
    fs_close(fd);
    buf[total] = 0;
    return total;
}

/* 解析无符号十进制（自研，非 atoi 依赖） */
static u32 sc_parse_u32(const char *s)
{
    u32 v = 0u;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (u32)(*s - '0');
        s++;
    }
    return v;
}

static char sc_buf[SYSCONF_MAX];      /* 共享工作缓冲（内核镜像体积受限，避免重复 BSS） */
static char sc_out[SYSCONF_MAX];

u32 sysconf_get_u32(const char *key, u32 dflt)
{
    char *buf = sc_buf;
    char k[SYSCONF_KEYMAX];
    char *p, *eol, *eq;
    i32 total;
    u32 i, klen;

    if (key == (const char *)0 || key[0] == 0) return dflt;
    klen = 0u;
    while (key[klen] && klen < SYSCONF_KEYMAX - 1u) { k[klen] = key[klen]; klen++; }
    k[klen] = 0;

    total = sysconf_read_all(buf, SYSCONF_MAX);
    if (total <= 0) return dflt;

    p = buf;
    for (i = 0u; (u32)i < (u32)total && *p; ) {
        eol = p;
        while (*eol && *eol != '\n') eol++;
        eq = p;
        while (eq < eol && *eq != '=') eq++;
        if (eq < eol) {
            u32 n = (u32)(eq - p);
            if (n == klen && memcmp(p, k, n) == 0) {
                return sc_parse_u32(eq + 1);
            }
        }
        i += (u32)(eol - p) + 1u;
        p = eol + 1u;
    }
    return dflt;
}

int sysconf_set_u32(const char *key, u32 val)
{
    char *buf = sc_buf;
    char line[SYSCONF_KEYMAX + 24u];
    char *p, *eol, *eq;
    u32 i, klen, n, llen;
    i32 total;
    int fd;
    u32 v = val;
    u32 ndig = 1u;

    if (key == (const char *)0 || key[0] == 0) return -1;
    klen = 0u;
    while (key[klen] && klen < SYSCONF_KEYMAX - 1u) { klen++; }
    if (klen == 0u || klen >= SYSCONF_KEYMAX - 1u) return -1;

    /* 构造 "key=val\n"（十进制） */
    while (v >= 10u) { v /= 10u; ndig++; }
    llen = klen + 1u + ndig + 1u;
    if (llen > sizeof(line)) return -1;
    n = 0u;
    while (n < klen) { line[n] = key[n]; n++; }
    line[n++] = '=';
    {
        u32 w = val, d, t = ndig;
        char tmp[16];
        while (t--) { tmp[t] = (char)('0' + w % 10u); w /= 10u; }
        for (d = 0u; d < ndig; d++) line[n++] = tmp[d];
    }
    line[n++] = '\n';

    /* 读旧内容，原地替换同名键行；没有则追加 */
    total = sysconf_read_all(buf, SYSCONF_MAX);
    if (total < 0) return -1;

    {
        char *out = sc_out;
        u32 o = 0u, replaced = 0u;
        p = buf;
        for (i = 0u; (u32)i < (u32)total && *p; ) {
            eol = p;
            while (*eol && *eol != '\n') eol++;
            eq = p;
            while (eq < eol && *eq != '=') eq++;
            if (eq < eol && (u32)(eq - p) == klen && memcmp(p, key, klen) == 0) {
                if (o + llen < SYSCONF_MAX) {
                    u32 c;
                    for (c = 0u; c < llen; c++) out[o++] = line[c];
                    replaced = 1u;
                }
            } else {
                u32 rlen = (u32)(eol - p);
                if (o + rlen + 1u < SYSCONF_MAX) {
                    u32 c;
                    for (c = 0u; c < rlen; c++) out[o++] = p[c];
                    out[o++] = '\n';
                }
            }
            i += (u32)(eol - p) + 1u;
            p = eol + 1u;
        }
        if (!replaced) {
            if (o + llen < SYSCONF_MAX) {
                u32 c;
                for (c = 0u; c < llen; c++) out[o++] = line[c];
            }
        }
        out[o] = 0;

        fd = fs_open(SYSCONF_PATH, O_WRITE | O_CREAT);
        if (fd < 0) return -2;
        if (fs_write(fd, out, (i32)o) != (i32)o) { fs_close(fd); return -3; }
        fs_close(fd);
    }
    return 0;
}

void sysconf_dump(void)
{
    char *buf = sc_buf;
    i32 total = sysconf_read_all(buf, SYSCONF_MAX);
    con_puts("  sysconf: /disk/xos.conf");
    if (total <= 0) { con_puts(" (empty)\n"); return; }
    con_puts("\n");
    con_puts(buf);
}
