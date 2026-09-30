/* XOS 安装程序 —— 介质引导 / 分区格式化 / 文件复制 / 引导装载 / 硬件检测驱动选择 */
#include "inst.h"
#include "console.h"

static inst_part_t g_parts[INST_MAX_PARTS];
static inst_copy_t g_copy[INST_MAX_COPY];
static inst_dev_t  g_devs[INST_MAX_DEVS];
static u32         g_media_ok;
static u32         g_boot_written;

static int inst_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)(*a) - (u8)(*b);
}

static void inst_strncpy(char *d, const char *s, u32 n)
{
    u32 i;
    for (i = 0u; i < n && s[i]; i++) d[i] = s[i];
    if (n > 0u) d[i < n ? i : n - 1u] = 0;
}

void inst_init(void)
{
    u32 i;
    for (i = 0u; i < INST_MAX_PARTS; i++) { g_parts[i].name[0] = 0; g_parts[i].used = 0u; g_parts[i].start = 0u; g_parts[i].size = 0u; }
    for (i = 0u; i < INST_MAX_COPY; i++) { g_copy[i].src[0] = 0; g_copy[i].dst[0] = 0; g_copy[i].bytes = 0u; g_copy[i].done = 0u; }
    for (i = 0u; i < INST_MAX_DEVS; i++) { g_devs[i].name[0] = 0; g_devs[i].used = 0u; g_devs[i].sel_drv = 0u; }
    g_media_ok = 0u;
    g_boot_written = 0u;
}

/* ---------- 安装介质引导 ---------- */

int inst_media_detect(const char *media, u32 *ok)
{
    if (media == 0 || media[0] == 0) return -1;
    g_media_ok = 1u;
    if (ok) *ok = 1u;
    return 0;
}

/* ---------- 分区与格式化 ---------- */

int inst_part_add(const char *name, u32 start, u32 size)
{
    u32 i, free_slot = INST_MAX_PARTS;
    for (i = 0u; i < INST_MAX_PARTS; i++) {
        if (g_parts[i].used && inst_strcmp(g_parts[i].name, name) == 0) return -1;
        if (!g_parts[i].used && free_slot == INST_MAX_PARTS) free_slot = i;
    }
    if (free_slot == INST_MAX_PARTS) return -2;
    inst_strncpy(g_parts[free_slot].name, name, INST_PART_LEN - 1u);
    g_parts[free_slot].start = start;
    g_parts[free_slot].size = size;
    g_parts[free_slot].used = 1u;
    return 0;
}

int inst_part_format(const char *name)
{
    u32 i;
    for (i = 0u; i < INST_MAX_PARTS; i++)
        if (g_parts[i].used && inst_strcmp(g_parts[i].name, name) == 0) {
            g_parts[i].start = 0u;   /* 格式化重置元数据 */
            return 0;
        }
    return -1;
}

int inst_part_state(const char *name, u32 *fmt)
{
    u32 i;
    for (i = 0u; i < INST_MAX_PARTS; i++)
        if (g_parts[i].used && inst_strcmp(g_parts[i].name, name) == 0) {
            if (fmt) *fmt = (g_parts[i].start == 0u) ? 1u : 0u;
            return 0;
        }
    return -1;
}

/* ---------- 系统文件复制 ---------- */

int inst_copy_add(const char *src, const char *dst, u32 bytes)
{
    u32 i, free_slot = INST_MAX_COPY;
    for (i = 0u; i < INST_MAX_COPY; i++)
        if (!g_copy[i].src[0] && free_slot == INST_MAX_COPY) free_slot = i;
    if (free_slot == INST_MAX_COPY) return -2;
    inst_strncpy(g_copy[free_slot].src, src, INST_COPY_LEN - 1u);
    inst_strncpy(g_copy[free_slot].dst, dst, INST_COPY_LEN - 1u);
    g_copy[free_slot].bytes = bytes;
    g_copy[free_slot].done = 0u;
    return 0;
}

int inst_copy_run(u32 *done_bytes)
{
    u32 i, total = 0u;
    for (i = 0u; i < INST_MAX_COPY; i++)
        if (g_copy[i].src[0]) {
            g_copy[i].done = g_copy[i].bytes;
            total += g_copy[i].bytes;
        }
    if (done_bytes) *done_bytes = total;
    return 0;
}

/* ---------- 引导装载程序安装 ---------- */

int inst_boot_write(const char *target, u32 *ok)
{
    if (target == 0 || target[0] == 0) return -1;
    g_boot_written = 1u;
    if (ok) *ok = 1u;
    return 0;
}

/* ---------- 硬件检测与驱动选择 ---------- */

int inst_dev_add(const char *name, u32 cls)
{
    u32 i, free_slot = INST_MAX_DEVS;
    for (i = 0u; i < INST_MAX_DEVS; i++) {
        if (g_devs[i].used && inst_strcmp(g_devs[i].name, name) == 0) return -1;
        if (!g_devs[i].used && free_slot == INST_MAX_DEVS) free_slot = i;
    }
    if (free_slot == INST_MAX_DEVS) return -2;
    inst_strncpy(g_devs[free_slot].name, name, INST_DEV_LEN - 1u);
    g_devs[free_slot].class = cls;
    g_devs[free_slot].sel_drv = 0u;
    g_devs[free_slot].used = 1u;
    return 0;
}

int inst_dev_select(const char *name, u32 drv)
{
    u32 i;
    for (i = 0u; i < INST_MAX_DEVS; i++)
        if (g_devs[i].used && inst_strcmp(g_devs[i].name, name) == 0) {
            g_devs[i].sel_drv = drv;
            return 0;
        }
    return -1;
}

/* ---------- 自检 ---------- */

int inst_selftest(void)
{
    u32 ok, v;

    /* 1-3: 介质检测 */
    if (inst_media_detect("usb", &ok) != 0) return 1;
    if (ok != 1u) return 2;
    if (inst_media_detect("", &ok) != -1) return 3;
    if (inst_media_detect(0, &ok) != -1) return 4;

    /* 5-8: 分区 */
    if (inst_part_add("root", 2048u, 512000u) != 0) return 5;
    if (inst_part_add("swap", 512000u, 65536u) != 0) return 6;
    if (inst_part_add("root", 0u, 0u) != -1) return 7;   /* 重复 */
    if (inst_part_state("root", &v) != 0) return 8;
    if (v != 0u) return 9;   /* 未格式化 */

    /* 10-12: 格式化 */
    if (inst_part_format("root") != 0) return 10;
    if (inst_part_state("root", &v) != 0) return 11;
    if (v != 1u) return 12;
    if (inst_part_format("nope") != -1) return 13;

    /* 14-16: 复制 */
    if (inst_copy_add("boot.bin", "/boot/boot.bin", 8192u) != 0) return 14;
    if (inst_copy_add("kernel.bin", "/xos/kernel.bin", 325000u) != 0) return 15;
    if (inst_copy_run(&v) != 0) return 16;
    if (v != 333192u) return 17;

    /* 18-19: 引导装载 */
    if (inst_boot_write("vda", &ok) != 0) return 18;
    if (ok != 1u) return 19;
    if (inst_boot_write("", &ok) != -1) return 20;

    /* 21-24: 硬件检测驱动选择 */
    if (inst_dev_add("sda", 0u) != 0) return 21;
    if (inst_dev_add("eth0", 1u) != 0) return 22;
    if (inst_dev_add("kbd", 2u) != 0) return 23;
    if (inst_dev_add("sda", 1u) != -1) return 24;
    if (inst_dev_select("eth0", 3u) != 0) return 25;
    if (inst_dev_select("nope", 1u) != -1) return 26;

    /* 27-29: 表满边界 */
    {
        u32 k;
        for (k = 0u; k < INST_MAX_PARTS; k++) {
            char nm[8];
            nm[0] = 'p'; nm[1] = (char)('0' + k); nm[2] = 0;
            (void)inst_part_add(nm, 100u + k, 100u);
        }
        if (inst_part_add("full", 0u, 0u) != -2) return 27;
        if (inst_part_add("root", 0u, 0u) != -1) return 28;
        for (k = 0u; k < INST_MAX_COPY; k++) {
            char nm[8];
            nm[0] = 'c'; nm[1] = (char)('0' + k); nm[2] = 0;
            (void)inst_copy_add(nm, nm, 100u);
        }
        if (inst_copy_add("full", "full", 1u) != -2) return 29;
        for (k = 0u; k < INST_MAX_DEVS; k++) {
            char nm[8];
            nm[0] = 'd'; nm[1] = (char)('0' + k); nm[2] = 0;
            (void)inst_dev_add(nm, 0u);
        }
        if (inst_dev_add("full", 0u) != -2) return 30;
    }
    if (inst_dev_select("eth0", 3u) != 0) return 31;   /* 已有设备可再选 */
    if (inst_dev_select("kbd", 5u) != 0) return 32;
    if (inst_media_detect("iso", &ok) != 0) return 33;
    if (inst_copy_run(&v) != 0) return 34;
    if (v < 100u) return 35;
    if (inst_boot_write("vdb", &ok) != 0) return 36;

    return 0;
}
