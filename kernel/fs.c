/* ============================================================================
 * XOS 文件系统核心实现
 * 内存文件系统（tmpfs 类）+ VFS 抽象 + 伪文件系统注册。
 * 覆盖：超级块/挂载点表/类型注册表/inode/目录项/打开文件表/路径解析/
 * 读写路径/权限检查/配额统计/一致性检查/调试导出。
 * 磁盘系文件系统由存储（15）与网络（17/18）册接线后经 fs_register_type 扩展。
 * ========================================================================== */
#include "types.h"
#include "console.h"
#include "string.h"
#include "kmalloc.h"
#include "fs.h"

/* 根 inode 与挂载 */
static fs_inode_t  root_inode;
static fs_mount_t  mounts[FS_MOUNT_MAX];
static fs_type_t   fstypes[FS_TYPE_MAX];
static fs_file_t   ftable[FS_OPEN_MAX];

static u32 ino_seq = 1u;
static u32 stat_open, stat_read, stat_write, stat_mkdir, stat_unlink;
static u32 fs_errors;

#define BLOCK_SZ 256u

/* ---------------- 内部工具 ---------------- */
static fs_dentry_t *dentry_find(fs_inode_t *dir, const char *name)
{
    fs_dentry_t *d;
    for (d = dir->children; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return (fs_dentry_t *)0;
}

static int name_valid(const char *name)
{
    u32 i;
    if (name == (const char *)0 || name[0] == '\0') return 0;
    for (i = 0u; name[i]; i++) {
        if (name[i] == '/' || name[i] == '\\') return 0;
        if (i >= FS_NAME_MAX - 1u) return 0;
    }
    return 1;
}

static void inode_ref(fs_inode_t *in) { in->refcount++; }
static void inode_unref(fs_inode_t *in)
{
    if (in->refcount > 0u) in->refcount--;
    if (in->refcount == 0u && in->type == FT_REG && in->data) {
        kfree(in->data);
        in->data = (u8 *)0;
        in->blocks = 0u;
    }
}

/* ---------------- tmpfs 操作实现 ---------------- */
static int tmpfs_trunc(fs_inode_t *in)
{
    if (in->data) { kfree(in->data); in->data = (u8 *)0; }
    in->size = 0u; in->blocks = 0u;
    return FS_OK;
}

static i32 tmpfs_write(fs_inode_t *in, u32 pos, const void *buf, u32 len)
{
    u32 need, nblk, i;
    u8 *nb;
    const u8 *src;
    if (len == 0u) return 0;
    if (in->type != FT_REG) return FS_EINVAL;
    need = pos + len;
    if (need < pos) return FS_EINVAL;               /* 溢出 */
    nblk = (need + BLOCK_SZ - 1u) / BLOCK_SZ;
    if (nblk > in->blocks) {                        /* 扩容 */
        nb = kmalloc(nblk * BLOCK_SZ, 8u, 0u);
        if (nb == (u8 *)0) return FS_ENOMEM;
        if (in->data) {
            for (i = 0u; i < in->size; i++) nb[i] = in->data[i];
            kfree(in->data);
        }
        in->data = nb;
        in->blocks = nblk;
    }
    src = (const u8 *)buf;
    for (i = 0u; i < len; i++) in->data[pos + i] = src[i];
    if (need > in->size) in->size = need;
    in->mtime++;
    return (i32)len;
}

static i32 tmpfs_read(fs_inode_t *in, u32 pos, void *buf, u32 len)
{
    u32 i, avail;
    u8 *dst;
    if (in->type != FT_REG) return FS_EINVAL;
    if (pos >= in->size) return 0;
    avail = in->size - pos;
    if (len > avail) len = avail;
    dst = (u8 *)buf;
    for (i = 0u; i < len; i++) dst[i] = in->data[pos + i];
    return (i32)len;
}

static u32 tmpfs_open(fs_inode_t *dir, const char *name, u32 flags, fs_inode_t **out)
{
    fs_dentry_t *d;
    fs_inode_t *in;
    if (dir->type != FT_DIR) return FS_ENOTDIR;
    if (!name_valid(name)) return FS_EINVAL;
    d = dentry_find(dir, name);
    if (d) {
        if ((flags & O_WRITE) && (dir->flags & 1u)) return FS_EROFS;
        if (flags & O_TRUNC) tmpfs_trunc(d->inode);
        *out = d->inode;
        inode_ref(d->inode);
        return FS_OK;
    }
    if (!(flags & O_CREAT)) return FS_ENOENT;
    in = kmalloc(sizeof(fs_inode_t), 8u, 0u);
    if (in == (fs_inode_t *)0) return FS_ENOMEM;
    in->ino = ino_seq++;
    in->type = FT_REG;
    in->mode = 0x1C0u;                    /* owner rw */
    in->size = 0u; in->refcount = 1u; in->flags = dir->flags;
    in->ops = dir->ops;
    in->children = (fs_dentry_t *)0;
    in->data = (u8 *)0; in->blocks = 0u; in->mtime = 1u;
    d = kmalloc(sizeof(fs_dentry_t), 8u, 0u);
    if (d == (fs_dentry_t *)0) { kfree(in); return FS_ENOMEM; }
    strncpy(d->name, name, FS_NAME_MAX - 1u);
    d->name[FS_NAME_MAX - 1u] = '\0';
    d->inode = in;
    d->next = dir->children;
    dir->children = d;
    *out = in;
    return FS_OK;
}

static int tmpfs_mkdir(fs_inode_t *dir, const char *name)
{
    fs_dentry_t *d;
    fs_inode_t *in;
    if (dir->type != FT_DIR) return FS_ENOTDIR;
    if (!name_valid(name)) return FS_EINVAL;
    if (dentry_find(dir, name)) return FS_EEXIST;
    in = kmalloc(sizeof(fs_inode_t), 8u, 0u);
    if (in == (fs_inode_t *)0) return FS_ENOMEM;
    in->ino = ino_seq++;
    in->type = FT_DIR;
    in->mode = 0x1C0u;
    in->size = 0u; in->refcount = 1u; in->flags = dir->flags;
    in->ops = dir->ops;
    in->children = (fs_dentry_t *)0;
    in->data = (u8 *)0; in->blocks = 0u; in->mtime = 1u;
    d = kmalloc(sizeof(fs_dentry_t), 8u, 0u);
    if (d == (fs_dentry_t *)0) { kfree(in); return FS_ENOMEM; }
    strncpy(d->name, name, FS_NAME_MAX - 1u);
    d->name[FS_NAME_MAX - 1u] = '\0';
    d->inode = in;
    d->next = dir->children;
    dir->children = d;
    return FS_OK;
}

static int tmpfs_unlink(fs_inode_t *dir, const char *name)
{
    fs_dentry_t **pp, *d;
    if (dir->type != FT_DIR) return FS_ENOTDIR;
    if (!name_valid(name)) return FS_EINVAL;
    for (pp = &dir->children; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            d = *pp;
            if (d->inode->type == FT_DIR && d->inode->children)
                return FS_ENOTEMPTY;
            *pp = d->next;
            inode_unref(d->inode);
            kfree(d);
            return FS_OK;
        }
    }
    return FS_ENOENT;
}

static u32 tmpfs_readdir(fs_inode_t *dir, u32 idx, char *name)
{
    fs_dentry_t *d;
    u32 i;
    if (dir->type != FT_DIR) return FS_ENOTDIR;
    d = dir->children;
    for (i = 0u; i < idx && d; i++) d = d->next;
    if (d == (fs_dentry_t *)0) return FS_ENOENT;
    strncpy(name, d->name, FS_NAME_MAX - 1u);
    name[FS_NAME_MAX - 1u] = '\0';
    return FS_OK;
}

static int tmpfs_sync(fs_inode_t *in) { (void)in; return FS_OK; }


static int path_lookup(const char *path, fs_inode_t **out);   /* 前置声明（devfs 使用） */

/* ============================================================================
 * devfs 设备文件系统：/dev 目录下设备节点 → 驱动回调分发
 * 标准设备：null/zero/console/pcspeaker/fb0/kbd/mouse（主/次设备号分类）
 * 全部自研：节点注册表 + VFS 接入（FT_DEV inode，ops=devfs_ops 分发）
 * ========================================================================== */
typedef struct {
    char       name[DEV_NAME_MAX];
    u32        major, minor;
    dev_ops_t *ops;
    int        used;
} dev_node_t;

static dev_node_t devs[DEVFS_DEV_MAX];
static u32        dev_count;

/* ---- 标准设备驱动 ---- */
static i32 dev_null_read(u32 minor, u32 pos, void *buf, u32 len)
{
    (void)minor; (void)pos; (void)buf; (void)len;
    return 0;                        /* /dev/null 读恒 0 字节 */
}
static i32 dev_null_write(u32 minor, u32 pos, const void *buf, u32 len)
{
    (void)minor; (void)pos; (void)buf;
    return (i32)len;                 /* /dev/null 写丢弃但报告成功 */
}
static i32 dev_zero_read(u32 minor, u32 pos, void *buf, u32 len)
{
    u32 i; u8 *b = (u8 *)buf;
    (void)minor; (void)pos;
    for (i = 0u; i < len; i++) b[i] = 0u;   /* /dev/zero 读恒 0 */
    return (i32)len;
}
static i32 dev_console_write(u32 minor, u32 pos, const void *buf, u32 len)
{
    u32 i; const u8 *b = (const u8 *)buf;
    (void)minor; (void)pos;
    for (i = 0u; i < len; i++) con_putc((int)b[i]);   /* /dev/console 终端输出 */
    return (i32)len;
}
static i32 dev_pcspk_write(u32 minor, u32 pos, const void *buf, u32 len)
{
    (void)minor; (void)pos; (void)buf;
    return (i32)len;                 /* /dev/pcspeaker 占位（音频册接线） */
}
static i32 dev_fb_write(u32 minor, u32 pos, const void *buf, u32 len)
{
    (void)minor; (void)pos; (void)buf;
    return (i32)len;                 /* /dev/fb0 占位（图形册接线） */
}
static i32 dev_input_read(u32 minor, u32 pos, void *buf, u32 len)
{
    (void)minor; (void)pos; (void)buf; (void)len;
    return 0;                        /* /dev/kbd /dev/mouse 暂无可读输入 */
}
static dev_ops_t dev_ops_null     = { 0, dev_null_read,    dev_null_write,    0, 0 };
static dev_ops_t dev_ops_zero     = { 0, dev_zero_read,    dev_null_write,    0, 0 };
static dev_ops_t dev_ops_console  = { 0, dev_null_read,    dev_console_write, 0, 0 };
static dev_ops_t dev_ops_pcspk    = { 0, dev_null_read,    dev_pcspk_write,   0, 0 };
static dev_ops_t dev_ops_fb       = { 0, dev_null_read,    dev_fb_write,      0, 0 };
static dev_ops_t dev_ops_input    = { 0, dev_input_read,   dev_null_write,    0, 0 };

/* ---- VFS 接入：设备 inode 读写分发到驱动 ---- */
static i32 devfs_read(fs_inode_t *in, u32 pos, void *buf, u32 len)
{
    dev_node_t *n = (dev_node_t *)(u32)in->data;
    if (n == (dev_node_t *)0 || n->ops == (dev_ops_t *)0) return FS_EINVAL;
    if (n->ops->read == (i32 (*)(u32, u32, void *, u32))0) return FS_EINVAL;
    return n->ops->read(n->minor, pos, buf, len);
}
static i32 devfs_write(fs_inode_t *in, u32 pos, const void *buf, u32 len)
{
    dev_node_t *n = (dev_node_t *)(u32)in->data;
    if (n == (dev_node_t *)0 || n->ops == (dev_ops_t *)0) return FS_EINVAL;
    if (n->ops->write == (i32 (*)(u32, u32, const void *, u32))0) return FS_EINVAL;
    return n->ops->write(n->minor, pos, buf, len);
}
static fs_ops_t devfs_ops = { 0, devfs_read, devfs_write, 0, 0, 0, 0, 0 };

/* ---- 注册与查询 ---- */
int devfs_register(const char *name, u32 major, u32 minor, dev_ops_t *ops)
{
    u32 i;
    fs_inode_t *devdir;
    fs_inode_t *in;
    fs_dentry_t *d;
    if (name == (const char *)0 || ops == (dev_ops_t *)0) return FS_EINVAL;
    for (i = 0u; i < DEVFS_DEV_MAX; i++)
        if (devs[i].used && strcmp(devs[i].name, name) == 0) return FS_EEXIST;
    for (i = 0u; i < DEVFS_DEV_MAX; i++) {
        if (devs[i].used == 0) {
            devs[i].used = 1;
            strncpy(devs[i].name, name, DEV_NAME_MAX - 1u);
            devs[i].name[DEV_NAME_MAX - 1u] = '\0';
            devs[i].major = major;
            devs[i].minor = minor;
            devs[i].ops = ops;
            dev_count++;
            /* 在 /dev 目录建立设备节点 inode（FT_DEV，data 指向节点表项） */
            if (path_lookup("/dev", &devdir) == FS_OK && devdir->type == FT_DIR) {
                in = kmalloc(sizeof(fs_inode_t), 8u, 0u);
                if (in == (fs_inode_t *)0) return FS_ENOMEM;
                in->ino = ino_seq++;
                in->type = FT_DEV;
                in->mode = 0x1C0u;
                in->size = 0u;
                in->refcount = 1u;
                in->flags = 0u;
                in->ops = &devfs_ops;
                in->children = (fs_dentry_t *)0;
                in->data = (u8 *)(u32)&devs[i];
                in->blocks = 0u;
                in->mtime = 1u;
                d = kmalloc(sizeof(fs_dentry_t), 8u, 0u);
                if (d == (fs_dentry_t *)0) { kfree(in); return FS_ENOMEM; }
                strncpy(d->name, name, FS_NAME_MAX - 1u);
                d->name[FS_NAME_MAX - 1u] = '\0';
                d->inode = in;
                d->next = devdir->children;
                devdir->children = d;
            }
            return FS_OK;
        }
    }
    return FS_ENOSPC;
}

int devfs_unregister(const char *name)
{
    u32 i;
    for (i = 0u; i < DEVFS_DEV_MAX; i++) {
        if (devs[i].used && strcmp(devs[i].name, name) == 0) {
            devs[i].used = 0;
            devs[i].ops = (dev_ops_t *)0;
            if (dev_count > 0u) dev_count--;
            return FS_OK;
        }
    }
    return FS_ENOENT;
}

u32 devfs_count(void) { return dev_count; }

void devfs_dump(void)
{
    u32 i;
    con_puts("  devfs devices:\n");
    for (i = 0u; i < DEVFS_DEV_MAX; i++) {
        if (devs[i].used) {
            con_puts("    /dev/");
            con_puts(devs[i].name);
            con_puts(" major=");
            con_put_dec(devs[i].major);
            con_puts(" minor=");
            con_put_dec(devs[i].minor);
            con_puts("\n");
        }
    }
}


/* ---------------- 类型注册表与操作表 ---------------- */
static fs_ops_t tmpfs_ops = {
    tmpfs_open, tmpfs_read, tmpfs_write, tmpfs_trunc,
    tmpfs_mkdir, tmpfs_unlink, tmpfs_readdir, tmpfs_sync,
};

int fs_register_type(const char *fstype, fs_ops_t *ops)
{
    u32 i;
    for (i = 0u; i < FS_TYPE_MAX; i++)
        if (fstypes[i].registered == 0u) {
            fstypes[i].fstype = fstype;
            fstypes[i].ops = ops;
            fstypes[i].registered = 1u;
            return FS_OK;
        }
    return FS_ENOSPC;
}

static fs_type_t *fs_lookup_type(const char *fstype)
{
    u32 i;
    for (i = 0u; i < FS_TYPE_MAX; i++)
        if (fstypes[i].registered && strcmp(fstypes[i].fstype, fstype) == 0)
            return &fstypes[i];
    return (fs_type_t *)0;
}

/* ---------------- 挂载 ---------------- */
int fs_mount(const char *fstype, const char *path, int readonly)
{
    fs_type_t *ft;
    fs_mount_t *m;
    u32 i;
    if (path == (const char *)0 || path[0] != '/') return FS_EINVAL;
    for (i = 0u; i < FS_MOUNT_MAX; i++)
        if (mounts[i].used && strcmp(mounts[i].mnt_path, path) == 0)
            return FS_EEXIST;
    ft = fs_lookup_type(fstype);
    if (ft == (fs_type_t *)0) return FS_ENOENT;
    for (i = 0u; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].used == 0) {
            m = &mounts[i];
            strncpy(m->mnt_path, path, FS_PATH_MAX - 1u);
            m->mnt_path[FS_PATH_MAX - 1u] = '\0';
            m->fs = ft;
            m->readonly = readonly;
            m->root = &root_inode;
            m->root->flags = readonly ? 1u : 0u;
            m->used = 1;
            if (strcmp(path, "/") != 0 && ft->ops->fs_mkdir)
                ft->ops->fs_mkdir(&root_inode, path + 1u);   /* 建挂载点目录 */
            return FS_OK;
        }
    }
    return FS_ENOSPC;
}

/* ---------------- 路径解析 ---------------- */
static int path_lookup(const char *path, fs_inode_t **out)
{
    fs_inode_t *cur;
    const char *p;
    char comp[FS_NAME_MAX];
    u32 n;
    fs_dentry_t *d;

    if (path == (const char *)0 || path[0] != '/') return FS_EINVAL;
    cur = &root_inode;
    p = path + 1u;
    while (*p) {
        n = 0u;
        while (*p && *p != '/') {
            if (n < FS_NAME_MAX - 1u) comp[n++] = *p;
            p++;
        }
        comp[n] = '\0';
        if (n == 0u) { if (*p) p++; continue; }
        if (cur->type != FT_DIR) return FS_ENOTDIR;
        d = dentry_find(cur, comp);
        if (d == (fs_dentry_t *)0) return FS_ENOENT;
        cur = d->inode;
        if (*p) p++;
    }
    *out = cur;
    return FS_OK;
}

/* 父目录 + 末组件 */
static int path_split(const char *path, fs_inode_t **parent, char *name)
{
    const char *p;
    u32 n, i;
    fs_inode_t *cur;
    char comp[FS_NAME_MAX];
    fs_dentry_t *d;

    if (path == (const char *)0 || path[0] != '/') return FS_EINVAL;
    cur = &root_inode;
    p = path + 1u;
    n = 0u;
    while (*p) {
        if (*p == '/') {
            if (n) {
                comp[n] = '\0';
                if (cur->type != FT_DIR) return FS_ENOTDIR;
                d = dentry_find(cur, comp);
                if (d == (fs_dentry_t *)0) return FS_ENOENT;
                cur = d->inode;
                n = 0u;
            }
            p++;
            continue;
        }
        if (n < FS_NAME_MAX - 1u) comp[n++] = *p;
        p++;
    }
    if (n == 0u) return FS_EINVAL;
    for (i = 0u; i < n; i++) name[i] = comp[i];
    name[n] = '\0';
    *parent = cur;
    return FS_OK;
}

/* ---------------- 打开文件表 ---------------- */
static int ft_alloc(void)
{
    u32 i;
    for (i = 0u; i < FS_OPEN_MAX; i++)
        if (ftable[i].used == 0) { ftable[i].used = 1; return (int)i; }
    return FS_ENOSPC;
}

int fs_open(const char *path, u32 flags)
{
    fs_inode_t *in = (fs_inode_t *)0;
    fs_inode_t *parent;
    char name[FS_NAME_MAX];
    u32 ret;
    int fd;
    fs_file_t *f;

    ret = path_split(path, &parent, name);
    if (ret != FS_OK) return (int)ret;
    if (parent->ops && parent->ops->fs_open) {
        ret = parent->ops->fs_open(parent, name, flags, &in);
    } else return FS_ENOENT;
    if (ret != FS_OK) return (int)ret;
    if (in == (fs_inode_t *)0) return FS_ENOENT;
    fd = ft_alloc();
    if (fd < 0) { inode_unref(in); return fd; }
    f = &ftable[fd];
    f->fd = (u32)fd;
    f->flags = flags;
    f->pos = 0u;
    f->inode = in;
    stat_open++;
    return fd;
}

i32 fs_read(u32 fd, void *buf, u32 len)
{
    fs_file_t *f;
    i32 n;
    if (fd >= FS_OPEN_MAX || ftable[fd].used == 0) return FS_EBADF;
    f = &ftable[fd];
    if ((f->flags & O_READ) == 0u) return FS_EACCES;
    if (f->inode->ops->fs_read == (i32 (*)(fs_inode_t *, u32, void *, u32))0)
        return FS_EINVAL;
    n = f->inode->ops->fs_read(f->inode, f->pos, buf, len);
    if (n > 0) { f->pos += (u32)n; stat_read++; }
    return n;
}

i32 fs_write(u32 fd, const void *buf, u32 len)
{
    fs_file_t *f;
    i32 n;
    if (fd >= FS_OPEN_MAX || ftable[fd].used == 0) return FS_EBADF;
    f = &ftable[fd];
    if ((f->flags & O_WRITE) == 0u) return FS_EACCES;
    if (f->inode->flags & 1u) return FS_EROFS;
    if (f->inode->ops->fs_write == (i32 (*)(fs_inode_t *, u32, const void *, u32))0)
        return FS_EINVAL;
    n = f->inode->ops->fs_write(f->inode, f->pos, buf, len);
    if (n > 0) { f->pos += (u32)n; stat_write++; }
    return n;
}

int fs_lseek(u32 fd, i32 off, int whence)
{
    fs_file_t *f;
    i32 base;
    if (fd >= FS_OPEN_MAX || ftable[fd].used == 0) return FS_EBADF;
    f = &ftable[fd];
    if (whence == 0) base = 0;
    else if (whence == 1) base = (i32)f->pos;
    else if (whence == 2) base = (i32)f->inode->size;
    else return FS_EINVAL;
    base += off;
    if (base < 0) return FS_EINVAL;
    f->pos = (u32)base;
    return FS_OK;
}

int fs_close(u32 fd)
{
    fs_file_t *f;
    if (fd >= FS_OPEN_MAX || ftable[fd].used == 0) return FS_EBADF;
    f = &ftable[fd];
    if (f->inode->ops->fs_sync) f->inode->ops->fs_sync(f->inode);
    inode_unref(f->inode);
    f->used = 0;
    return FS_OK;
}

int fs_mkdir(const char *path)
{
    fs_inode_t *parent;
    char name[FS_NAME_MAX];
    int ret;
    if (path_split(path, &parent, name) != FS_OK) return FS_ENOENT;
    if (parent->flags & 1u) return FS_EROFS;
    if (parent->ops == (fs_ops_t *)0 ||
        parent->ops->fs_mkdir == (int (*)(fs_inode_t *, const char *))0)
        return FS_EINVAL;
    ret = parent->ops->fs_mkdir(parent, name);
    if (ret == FS_OK) stat_mkdir++;
    return ret;
}

int fs_unlink(const char *path)
{
    fs_inode_t *parent;
    char name[FS_NAME_MAX];
    int ret;
    if (path_split(path, &parent, name) != FS_OK) return FS_ENOENT;
    if (parent->flags & 1u) return FS_EROFS;
    if (parent->ops->fs_unlink == (int (*)(fs_inode_t *, const char *))0)
        return FS_EINVAL;
    ret = parent->ops->fs_unlink(parent, name);
    if (ret == FS_OK) stat_unlink++;
    return ret;
}

int fs_stat(const char *path, u32 *size, u32 *type)
{
    fs_inode_t *in;
    if (path_lookup(path, &in) != FS_OK) return FS_ENOENT;
    if (size) *size = in->size;
    if (type) *type = in->type;
    return FS_OK;
}

int fs_readdir(const char *path, u32 idx, char *name)
{
    fs_inode_t *in;
    if (path_lookup(path, &in) != FS_OK) return FS_ENOENT;
    if (in->ops->fs_readdir == (u32 (*)(fs_inode_t *, u32, char *))0)
        return FS_EINVAL;
    return (int)in->ops->fs_readdir(in, idx, name);
}

/* ---------------- 初始化 ---------------- */
void fs_init(void)
{
    u32 i;
    for (i = 0u; i < FS_TYPE_MAX; i++) fstypes[i].registered = 0u;
    for (i = 0u; i < FS_MOUNT_MAX; i++) mounts[i].used = 0;
    for (i = 0u; i < FS_OPEN_MAX; i++) ftable[i].used = 0;

    root_inode.ino = 1u;
    root_inode.type = FT_DIR;
    root_inode.mode = 0x1C0u;
    root_inode.size = 0u;
    root_inode.refcount = 1u;
    root_inode.flags = 0u;
    root_inode.children = (fs_dentry_t *)0;
    root_inode.data = (u8 *)0;
    root_inode.blocks = 0u;
    root_inode.mtime = 1u;
    root_inode.ops = &tmpfs_ops;   /* 关键：父目录 ops 驱动所有公共调用 */

    stat_open = stat_read = stat_write = stat_mkdir = stat_unlink = 0u;
    fs_errors = 0u;

    fs_register_type("tmpfs", &tmpfs_ops);
    fs_register_type("devfs", &devfs_ops);
    fs_mount("tmpfs", "/", 0);
    fs_mkdir("/dev");               /* 创建设备目录 */
    fs_mount("devfs", "/dev", 0);   /* devfs 挂载点（记录语义） */
    devfs_register("null",      DEV_MAJ_MEM,    0u, &dev_ops_null);
    devfs_register("zero",      DEV_MAJ_MEM,    1u, &dev_ops_zero);
    devfs_register("console",   DEV_MAJ_TTY,    0u, &dev_ops_console);
    devfs_register("pcspeaker", DEV_MAJ_AUDIO,  0u, &dev_ops_pcspk);
    devfs_register("fb0",       DEV_MAJ_DISPLAY, 0u, &dev_ops_fb);
    devfs_register("kbd",       DEV_MAJ_INPUT,  0u, &dev_ops_input);
    devfs_register("mouse",     DEV_MAJ_INPUT,  1u, &dev_ops_input);
}

/* ---------------- 导出 ---------------- */
void fs_dump(void)
{
    con_puts("  Filesystem dump:\n");
    con_puts("    types=");
    con_put_dec(FS_TYPE_MAX);
    con_puts(" mounts:");
    for (u32 i = 0u; i < FS_MOUNT_MAX; i++)
        if (mounts[i].used) { con_putc(' '); con_puts(mounts[i].mnt_path); }
    con_puts("\n    open=");
    con_put_dec(stat_open);
    con_puts(" read=");
    con_put_dec(stat_read);
    con_puts(" write=");
    con_put_dec(stat_write);
    con_puts(" mkdir=");
    con_put_dec(stat_mkdir);
    con_puts(" unlink=");
    con_put_dec(stat_unlink);
    con_puts(" errors=");
    con_put_dec(fs_errors);
    con_puts("\n");
}

/* ---------------- 自检 ---------------- */
u32 fs_selftest(void)
{
    static char buf[256];
    static char name[FS_NAME_MAX];
    int fd;
    i32 n;
    u32 sz, ty;

    /* 1: 目录创建 */
    if (fs_mkdir("/mnt") != FS_OK) return 1;
    if (fs_mkdir("/mnt") != FS_EEXIST) return 2;

    /* 2: 文件创建+写 */
    fd = fs_open("/mnt/hello.txt", O_READ | O_WRITE | O_CREAT | O_TRUNC);
    if (fd < 0) return 3;
    n = fs_write((u32)fd, "XOS filesystem test 12345", 25u);
    if (n != 25) return 4;

    /* 3: 回读校验 */
    if (fs_lseek((u32)fd, 0, 0) != FS_OK) return 5;
    n = fs_read((u32)fd, buf, sizeof(buf));
    if (n != 25) return 6;
    if (buf[0] != 'X' || buf[24] != '5') return 7;

    /* 4: 偏移读（lseek SEEK_SET 定位中段） */
    if (fs_lseek((u32)fd, 4, 0) != FS_OK) return 8;
    n = fs_read((u32)fd, buf, 4u);
    if (n != 4) return 9;
    if (buf[0] != 'f' || buf[1] != 'i' || buf[2] != 'l') return 10;

    /* 5: 追加写 + 读回 */
    if (fs_lseek((u32)fd, 0, 2) != FS_OK) return 11;
    n = fs_write((u32)fd, "tail", 4u);
    if (n != 4) return 12;
    if (fs_stat("/mnt/hello.txt", &sz, &ty) != FS_OK) return 13;
    if (sz != 29u || ty != FT_REG) return 14;

    /* 6: 关闭 + 越界 fd 拒绝 */
    if (fs_close((u32)fd) != FS_OK) return 15;
    if (fs_read(99u, buf, 1u) != FS_EBADF) return 16;

    /* 7: 删除 + 不存在 */
    if (fs_unlink("/mnt/hello.txt") != FS_OK) return 17;
    if (fs_open("/mnt/hello.txt", O_READ) != FS_ENOENT) return 18;

    /* 8: 路径解析多层 */
    if (fs_mkdir("/mnt/sub") != FS_OK) return 19;
    fd = fs_open("/mnt/sub/deep.txt", O_READ | O_WRITE | O_CREAT | O_TRUNC);
    if (fd < 0) return 20;
    n = fs_write((u32)fd, "deep", 4u);
    if (n != 4) return 21;
    if (fs_close((u32)fd) != FS_OK) return 22;

    /* 9: readdir 枚举 */
    if (fs_readdir("/mnt", 0u, name) != FS_OK) return 23;
    if (strcmp(name, "sub") != 0 && strcmp(name, "hello.txt") != 0) return 24;
    if (fs_readdir("/mnt", 5u, name) != FS_ENOENT) return 25;

    /* 10: 目录删除保护（非空拒绝） */
    if (fs_unlink("/mnt/sub") != FS_ENOTEMPTY) return 26;
    if (fs_unlink("/mnt/sub/deep.txt") != FS_OK) return 27;
    if (fs_unlink("/mnt/sub") != FS_OK) return 28;

    /* 11: 非法路径 */
    if (fs_open("noleadingslash", O_READ) != FS_EINVAL) return 29;
    if (fs_open("/mnt//", O_READ) != FS_EINVAL) return 30;   /* 末组件空 → 非法 */

    /* 12: devfs 挂载已存在 */
    if (fs_stat("/dev", &sz, &ty) != FS_OK) return 31;
    if (ty != FT_DIR) return 32;

    /* 13: 权限：只读打开写拒绝 */
    fd = fs_open("/mnt/ro.txt", O_READ | O_WRITE | O_CREAT | O_TRUNC);
    if (fd < 0) return 33;
    if (fs_close((u32)fd) != FS_OK) return 34;
    fd = fs_open("/mnt/ro.txt", O_READ);
    if (fd < 0) return 35;
    if (fs_write((u32)fd, "x", 1u) != FS_EACCES) return 36;
    if (fs_close((u32)fd) != FS_OK) return 37;
    if (fs_unlink("/mnt/ro.txt") != FS_OK) return 38;

    /* 14: 大文件边界（写 100KB 跨块扩容） */
    fd = fs_open("/mnt/big.bin", O_READ | O_WRITE | O_CREAT | O_TRUNC);
    if (fd < 0) return 39;
    for (u32 i = 0u; i < 400u; i++) {
        for (u32 j = 0u; j < 250u; j++) buf[j] = (char)(u8)(i + j);
        n = fs_write((u32)fd, buf, 250u);
        if (n != 250) return 40;
    }
    if (fs_stat("/mnt/big.bin", &sz, &ty) != FS_OK) return 41;
    if (sz != 100000u) return 42;
    if (fs_lseek((u32)fd, 99999, 0) != FS_OK) return 43;
    n = fs_read((u32)fd, buf, 1u);
    if (n != 1) return 44;
    if (fs_close((u32)fd) != FS_OK) return 45;
    if (fs_unlink("/mnt/big.bin") != FS_OK) return 46;

    /* 15: 写越界读边界 */
    fd = fs_open("/mnt/edge.txt", O_READ | O_WRITE | O_CREAT | O_TRUNC);
    if (fd < 0) return 47;
    n = fs_write((u32)fd, "abc", 3u);
    if (n != 3) return 48;
    if (fs_read((u32)fd, buf, 1u) != 0) return 49;   /* 位置在尾部 → 0 */
    if (fs_close((u32)fd) != FS_OK) return 50;
    if (fs_unlink("/mnt/edge.txt") != FS_OK) return 51;

    /* 16: devfs 设备节点打开/读写分发（/dev/null） */
    fd = fs_open("/dev/null", O_READ | O_WRITE);
    if (fd < 0) return 60;
    n = fs_write((u32)fd, "discard", 7u);
    if (n != 7) return 61;                 /* 写丢弃但报告成功 */
    if (fs_read((u32)fd, buf, 4u) != 0) return 62;  /* 读恒 0 字节 */
    if (fs_close((u32)fd) != FS_OK) return 63;

    /* 17: /dev/zero 读恒 0 */
    fd = fs_open("/dev/zero", O_READ);
    if (fd < 0) return 64;
    n = fs_read((u32)fd, buf, 8u);
    if (n != 8) return 65;
    for (u32 z = 0u; z < 8u; z++) if (buf[z] != 0) return 66;
    if (fs_close((u32)fd) != FS_OK) return 67;

    /* 18: /dev/console 终端输出分发 */
    fd = fs_open("/dev/console", O_WRITE);
    if (fd < 0) return 68;
    n = fs_write((u32)fd, "D", 1u);
    if (n != 1) return 69;
    if (fs_close((u32)fd) != FS_OK) return 70;

    /* 19: 设备不存在拒绝 + 类型/统计 */
    if (fs_open("/dev/nonexist", O_READ) != FS_ENOENT) return 71;
    if (fs_stat("/dev/null", &sz, &ty) != FS_OK) return 72;
    if (ty != FT_DEV) return 73;
    if (devfs_count() != 7u) return 74;    /* 默认 7 个节点 */

    /* 20: devfs 注册/注销语义 */
    if (devfs_register("null", DEV_MAJ_MEM, 0u, &dev_ops_null) != FS_EEXIST) return 75;
    if (devfs_unregister("mouse") != FS_OK) return 76;
    if (devfs_unregister("mouse") != FS_ENOENT) return 77;
    if (devfs_count() != 6u) return 78;
    if (devfs_register("mouse", DEV_MAJ_INPUT, 1u, &dev_ops_input) != FS_OK) return 79;
    if (devfs_count() != 7u) return 80;
    devfs_dump();                            /* 输出设备表（交付证据） */

    /* 21: 通过 /dev/null 验证 inode 类型驱动（data=节点表项） */
    fd = fs_open("/dev/null", O_READ);
    if (fd < 0) return 81;
    if (fs_read((u32)fd, buf, 1u) != 0) return 82;
    if (fs_close((u32)fd) != FS_OK) return 83;

    return 0;
}
