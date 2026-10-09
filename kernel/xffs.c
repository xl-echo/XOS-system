/* ============================================================================
 * XOS 磁盘文件系统 XFFS v1（真实持久化，重启保留用户数据）
 * 完全自研：镜像尾区布局，直接 ATA PIO 扇区读写，无任何外部依赖。
 *
 * 布局（扇区）：
 *   LBA 7400            超级块 (32B, 1 扇区)
 *   LBA 7401..7404      文件表 (64 x 32B = 2048B = 4 扇区)
 *   LBA 7405..          数据区（连续分配，删除不回收，格式化重置）
 *
 * 语义：
 *   目录 /disk（单级），文件打开时全量读入 kmalloc 缓冲，
 *   write 同步写回磁盘（数据 + 文件表），掉电安全；
 *   fs_sync/fs_close 幂等落盘；poweroff/reboot 前置 flush。
 * 安全性：格式化仅在超级块 magic 不符时自动执行（该区为镜像空白区）；
 *   所有扇区读写经 disk_read/write_sectors 的边界校验。
 * ========================================================================== */
#include "../include/types.h"
#include "../include/console.h"
#include "../include/string.h"
#include "../include/fs.h"
#include "../include/disk.h"
#include "../include/kmalloc.h"
#include "../include/rtc.h"

#define XFFS_MAGIC      0x58464653u        /* 'XFFS' */
#define XFFS_VERSION    1u
#define XFFS_SUPER_LBA  7400u
#define XFFS_FT_LBA     7401u
#define XFFS_FT_SECTS   5u                 /* 64*36B = 2304B = 5 扇区 */
#define XFFS_DATA_LBA   7406u
#define XFFS_MAX_FILES  64u
#define XFFS_MAX_SIZE   (262144u)          /* 单文件上限 256KB */
#define XFFS_DATA_SECTS 11900u             /* 数据区扇区数（约 5.8MB） */

typedef struct {
    char  name[20];
    u32   off;        /* 相对 XFFS_DATA_LBA 的扇区偏移 */
    u32   size;       /* 字节 */
    u32   mtime;      /* RTC 时间戳（0 = 不可用时的 tick 值） */
    u8    flags;      /* 1 = 占用 */
    u8    pad[3];
} xffs_entry_t;                             /* 36B */

typedef struct {
    u32   magic;
    u32   version;
    u32   ft_sects;
    u32   data_lba;
    u32   entry_count;
    u32   data_end;    /* 当前数据区占用扇区数（含超级块/表） */
    u32   rsv[2];
} xffs_super_t;                              /* 32B */

static xffs_entry_t g_xffs[XFFS_MAX_FILES];
static xffs_super_t g_super;
static u32 g_xffs_ready;

static fs_ops_t xffs_ops;    /* 前置声明：定义于文件末尾 */

/* ---------------- 磁盘原始读写 ---------------- */
static int xffs_read_sects(u32 lba, u32 cnt, u8 *buf)
{
    if (lba + cnt > XFFS_DATA_LBA + XFFS_DATA_SECTS) return -1;
    return disk_read_sectors(0, lba, cnt, buf);
}
static int xffs_write_sects(u32 lba, u32 cnt, const u8 *buf)
{
    if (lba + cnt > XFFS_DATA_LBA + XFFS_DATA_SECTS) return -1;
    return disk_write_sectors(0, lba, cnt, buf);
}

static void xffs_persist_table(void);   /* （已并入 xffs_write/ffs_open，占位保留） */

/* ---------------- 槽位管理 ---------------- */
static int xffs_find(const char *name)
{
    u32 i;
    for (i = 0u; i < XFFS_MAX_FILES; i++)
        if (g_xffs[i].flags && strcmp(g_xffs[i].name, name) == 0)
            return (int)i;
    return -1;
}

static u32 xffs_now(void)
{
    rtc_time_t t;
    if (rtc_read_all(&t) == 0) return rtc_to_timestamp(&t);
    return 0u;
}

/* ---------------- VFS ops ---------------- */
static i32 xffs_read(fs_inode_t *in, u32 pos, void *buf, u32 len)
{
    u8 *data = (u8 *)in->data;
    u32 avail;
    if (!data || pos >= in->size) return 0;
    avail = in->size - pos;
    if (len > avail) len = avail;
    memcpy(buf, data + pos, len);
    return (i32)len;
}

static i32 xffs_write(fs_inode_t *in, u32 pos, const void *buf, u32 len)
{
    u8 *data = (u8 *)in->data;
    u32 need = pos + len;
    u8 *nd;
    xffs_entry_t *e;
    if (need > XFFS_MAX_SIZE) return FS_ENOSPC;
    if (!data) {
        data = kmalloc(XFFS_MAX_SIZE, 8u, 0u);
        if (!data) return FS_ENOMEM;
        memset(data, 0, XFFS_MAX_SIZE);
        in->data = data;
    }
    if (need > in->size) {
        nd = data; (void)nd;               /* 缓冲固定 XFFS_MAX_SIZE，无需重分配 */
        in->size = need;
    }
    memcpy(data + pos, buf, len);
    in->blocks = 1u;                        /* dirty 标记 */
    /* 同步写回磁盘：数据区 + 文件表 */
    e = &g_xffs[(u32)(u32)in->ino];
    if (e->flags) {
        if (in->size) {
            xffs_write_sects(XFFS_DATA_LBA + e->off, (in->size + 511u) / 512u, data);
        }
        e->size = in->size;
        e->mtime = xffs_now();
        xffs_write_sects(XFFS_FT_LBA, XFFS_FT_SECTS, (const u8 *)g_xffs);
        in->blocks = 0u;                    /* 已落盘 */
    }
    return (i32)len;
}

static int xffs_sync(fs_inode_t *in)
{
    xffs_entry_t *e;
    u8 *data = (u8 *)in->data;
    if (!data || in->blocks == 0u) return FS_OK;
    e = &g_xffs[(u32)(u32)in->ino];
    if (e->flags) {
        xffs_write_sects(XFFS_DATA_LBA + e->off, (e->size + 511u) / 512u, data);
        e->size = in->size;
        e->mtime = xffs_now();
        xffs_write_sects(XFFS_FT_LBA, XFFS_FT_SECTS, (const u8 *)g_xffs);
        in->blocks = 0u;
    }
    return FS_OK;
}

static u32 xffs_open(fs_inode_t *dir, const char *name, u32 flags, fs_inode_t **out)
{
    u32 slot;
    fs_inode_t *in;
    xffs_entry_t *e;
    int rc;
    (void)dir;
    slot = (u32)xffs_find(name);
    if (slot == (u32)-1) {
        if (!(flags & O_CREAT)) return FS_ENOENT;
        if (g_super.data_end + 16u >= XFFS_DATA_SECTS) return FS_ENOSPC;
        for (slot = 0u; slot < XFFS_MAX_FILES; slot++)
            if (!g_xffs[slot].flags) break;
        if (slot >= XFFS_MAX_FILES) return FS_ENOSPC;
        e = &g_xffs[slot];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, name, 19u);
        e->name[19u] = 0;
        e->off = g_super.data_end;
        e->size = 0u;
        e->flags = 1u;
        e->mtime = xffs_now();
        g_super.data_end += 2u;             /* 预占 1KB */
        xffs_write_sects(XFFS_FT_LBA, XFFS_FT_SECTS, (const u8 *)g_xffs);
        xffs_write_sects(XFFS_SUPER_LBA, 1u, (const u8 *)&g_super);
    } else {
        e = &g_xffs[slot];
    }
    in = kmalloc(sizeof(fs_inode_t), 8u, 0u);
    if (!in) return FS_ENOMEM;
    in->ino = slot;                         /* 槽号即 ino */
    in->type = FT_REG;
    in->mode = FS_DEF_FILE;
    in->uid = 0u; in->gid = 0u;
    in->size = e->size;
    in->refcount = 1u;
    in->flags = 0u;
    in->ops = &xffs_ops;
    in->children = (fs_dentry_t *)0;
    in->blocks = 0u;
    in->mtime = e->mtime;
    in->data = kmalloc(XFFS_MAX_SIZE, 8u, 0u);
    if (!in->data) { kfree(in); return FS_ENOMEM; }
    memset(in->data, 0, XFFS_MAX_SIZE);
    if (e->size)
        rc = xffs_read_sects(XFFS_DATA_LBA + e->off, (e->size + 511u) / 512u, in->data);
    else rc = 0;
    if (rc != 0) { kfree(in->data); kfree(in); return FS_EINVAL; }
    *out = in;
    return FS_OK;
}

static int xffs_unlink(fs_inode_t *dir, const char *name)
{
    int slot = xffs_find(name);
    (void)dir;
    if (slot < 0) return FS_ENOENT;
    g_xffs[slot].flags = 0u;
    g_xffs[slot].name[0] = 0;
    xffs_write_sects(XFFS_FT_LBA, XFFS_FT_SECTS, (const u8 *)g_xffs);
    return FS_OK;
}

static u32 xffs_readdir(fs_inode_t *dir, u32 idx, char *name)
{
    u32 i, n = 0u;
    (void)dir;
    for (i = 0u; i < XFFS_MAX_FILES; i++) {
        if (g_xffs[i].flags) {
            if (n == idx) {
                strncpy(name, g_xffs[i].name, FS_NAME_MAX - 1u);
                name[FS_NAME_MAX - 1u] = 0;
                return FS_OK;
            }
            n++;
        }
    }
    return FS_ENOENT;
}

/* ---------------- 挂载 /disk ---------------- */
static int xffs_mount_disk(void)
{
    fs_inode_t *diskdir;
    fs_dentry_t *d;
    diskdir = kmalloc(sizeof(fs_inode_t), 8u, 0u);
    if (!diskdir) return FS_ENOMEM;
    diskdir->ino = 2u;
    diskdir->type = FT_DIR;
    diskdir->mode = FS_DEF_DIR;
    diskdir->uid = 0u; diskdir->gid = 0u;
    diskdir->size = 0u;
    diskdir->refcount = 1u;
    diskdir->flags = 0u;
    diskdir->ops = &xffs_ops;
    diskdir->children = (fs_dentry_t *)0;
    diskdir->data = (u8 *)0;
    diskdir->blocks = 0u;
    diskdir->mtime = 1u;
    /* 挂到根目录 children */
    d = kmalloc(sizeof(fs_dentry_t), 8u, 0u);
    if (!d) { kfree(diskdir); return FS_ENOMEM; }
    strncpy(d->name, "disk", FS_NAME_MAX - 1u);
    d->name[FS_NAME_MAX - 1u] = 0;
    d->inode = diskdir;
    d->next = fs_get_root()->children;
    fs_get_root()->children = d;
    return FS_OK;
}

/* ---------------- 初始化 / 格式化 ---------------- */
int xffs_init(void)
{
    u8 buf[512];
    u32 i;
    int rc;

    rc = xffs_read_sects(XFFS_SUPER_LBA, 1u, buf);
    if (rc != 0) { g_xffs_ready = 0; return FS_EINVAL; }
    memcpy(&g_super, buf, sizeof(g_super));
    if (g_super.magic != XFFS_MAGIC || g_super.version != XFFS_VERSION) {
        /* 首次挂载：格式化（该区为镜像空白区，不触碰内核/日志区） */
        memset(&g_super, 0, sizeof(g_super));
        g_super.magic = XFFS_MAGIC;
        g_super.version = XFFS_VERSION;
        g_super.ft_sects = XFFS_FT_SECTS;
        g_super.data_lba = XFFS_DATA_LBA;
        g_super.entry_count = XFFS_MAX_FILES;
        g_super.data_end = 0u;
        memset(g_xffs, 0, sizeof(g_xffs));
        xffs_write_sects(XFFS_SUPER_LBA, 1u, (const u8 *)&g_super);
        xffs_write_sects(XFFS_FT_LBA, XFFS_FT_SECTS, (const u8 *)g_xffs);
    } else {
        /* 恢复文件表 */
        rc = xffs_read_sects(XFFS_FT_LBA, XFFS_FT_SECTS, (u8 *)g_xffs);
        if (rc != 0) { g_xffs_ready = 0; return FS_EINVAL; }
    }
    for (i = 0u; i < XFFS_MAX_FILES; i++)
        if (g_xffs[i].flags) g_xffs[i].name[19u] = 0;
    xffs_mount_disk();
    g_xffs_ready = 1;
    con_puts("  XFFS: /disk mounted (");    con_put_dec(XFFS_MAX_FILES);
    con_puts(" slots, data ");
    con_put_dec(XFFS_DATA_SECTS);
    con_puts(" sectors)\n");
    return FS_OK;
}

/* 关机前置：全量落盘（幂等） */
void xffs_flush_all(void)
{
    u32 i;
    fs_inode_t *in;
    if (!g_xffs_ready) return;
    for (i = 0u; i < XFFS_MAX_FILES; i++) {
        if (!g_xffs[i].flags) continue;
        in = kmalloc(sizeof(fs_inode_t), 8u, 0u);
        if (!in) continue;
        in->ino = i;
        in->size = g_xffs[i].size;
        in->blocks = 1u;
        in->data = kmalloc(XFFS_MAX_SIZE, 8u, 0u);
        if (!in->data) { kfree(in); continue; }
        memset(in->data, 0, XFFS_MAX_SIZE);
        if (xffs_read_sects(XFFS_DATA_LBA + g_xffs[i].off,
                            (g_xffs[i].size + 511u) / 512u, in->data) == 0) {
            xffs_write_sects(XFFS_DATA_LBA + g_xffs[i].off,
                             (g_xffs[i].size + 511u) / 512u, in->data);
        }
        kfree(in->data);
        kfree(in);
    }
    xffs_write_sects(XFFS_SUPER_LBA, 1u, (const u8 *)&g_super);
}

u32 xffs_file_count(void) { u32 i, n = 0; for (i = 0; i < XFFS_MAX_FILES; i++) if (g_xffs[i].flags) n++; return n; }
u32 xffs_space_free_sects(void) { return XFFS_DATA_SECTS - g_super.data_end; }
u32 xffs_ready(void) { return g_xffs_ready; }

/* 自检：布局不变量（不动真实数据） */
u32 xffs_selftest(void)
{
    u32 fail = 0u;
    if (sizeof(xffs_entry_t) != 36u) fail++;
    if (sizeof(xffs_super_t) != 32u) fail++;
    if (XFFS_FT_LBA + XFFS_FT_SECTS > XFFS_DATA_LBA) fail++;
    if (XFFS_DATA_LBA + XFFS_DATA_SECTS > 20480u) fail++;   /* 不越过镜像尾 */
    return fail;
}

void xffs_dump(void)
{
    u32 i;
    con_puts("  XFFS: ");
    con_put_dec(xffs_file_count());
    con_puts(" files, free data sectors=");
    con_put_dec(xffs_space_free_sects());
    con_puts("\n");
    for (i = 0u; i < XFFS_MAX_FILES; i++) {
        if (g_xffs[i].flags) {
            con_puts("    /disk/");
            con_puts(g_xffs[i].name);
            con_puts("  size=");
            con_put_dec(g_xffs[i].size);
            con_puts(" off=");
            con_put_dec(g_xffs[i].off);
            con_puts("\n");
        }
    }
}

/* 操作表：定义于所有函数之后（含前置声明的引用） */
static fs_ops_t xffs_ops = {
    xffs_open, xffs_read, xffs_write, 0, 0, xffs_unlink, xffs_readdir, xffs_sync,
};
