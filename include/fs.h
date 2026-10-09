/* ============================================================================
 * XOS 文件系统核心头文件
 * 自研 VFS 抽象 + 内存文件系统（tmpfs 类）：
 * 超级块 / inode / 目录项 / 文件描述符表 / 文件系统类型注册表 / 挂载点表 /
 * 路径解析 / 读写路径 / 权限 / 配额统计 / 一致性检查 / 调试导出。
 * 磁盘系（ext/FAT/NTFS/NFS/ISO）由存储与网络册接线后扩展注册。
 * ========================================================================== */
#ifndef XOS_FS_H
#define XOS_FS_H

#include "types.h"

#define FS_MAGIC        0x584F5346u   /* "XOSF" */
#define FS_NAME_MAX     28
#define FS_PATH_MAX     128
#define FS_OPEN_MAX     16            /* 打开文件表项数 */
#define FS_MOUNT_MAX    8
#define FS_TYPE_MAX     8

/* 文件类型 */
#define FT_NONE     0u
#define FT_DIR      1u
#define FT_REG      2u
#define FT_DEV      3u
#define FT_PROC     4u

/* 权限位（inode.mode 低 9 位，类 Unix rwx） */
#define FS_S_IRWXU  0x1C0u   /* owner rwx */
#define FS_S_IRUSR  0x100u
#define FS_S_IWUSR  0x080u
#define FS_S_IXUSR  0x040u
#define FS_S_IRWXG  0x038u   /* group rwx */
#define FS_S_IRGRP  0x020u
#define FS_S_IWGRP  0x010u
#define FS_S_IXGRP  0x008u
#define FS_S_IRWXO  0x007u   /* other rwx */
#define FS_S_IROTH  0x004u
#define FS_S_IWOTH  0x002u
#define FS_S_IXOTH  0x001u

/* 常用默认权限 */
#define FS_DEF_DIR  0x1FFu   /* drwxrwxrwx */
#define FS_DEF_FILE 0x1B6u   /* -rw-rw-rw- */

/* 访问意图掩码（fs_perm_check 的 want 参数）：每档内 r/w/x 位 */
#define FS_ACC_R  4u
#define FS_ACC_W  2u
#define FS_ACC_X  1u

/* 打开方式 */
#define O_READ      0x01u
#define O_WRITE     0x02u
#define O_CREAT     0x04u
#define O_TRUNC     0x08u
#define O_APPEND    0x10u

/* 错误码 */
#define FS_OK        0
#define FS_ENOENT    (-1)
#define FS_EINVAL    (-2)
#define FS_ENOMEM    (-3)
#define FS_EEXIST    (-4)
#define FS_EBADF     (-5)
#define FS_EACCES    (-6)
#define FS_ENOSPC    (-7)
#define FS_EROFS     (-8)
#define FS_ENOTDIR   (-9)
#define FS_ENOTEMPTY (-10)

typedef struct fs_inode  fs_inode_t;
typedef struct fs_dentry fs_dentry_t;
typedef struct fs_file   fs_file_t;

/* 文件系统操作表 */
typedef struct {
    u32  (*fs_open)(fs_inode_t *dir, const char *name, u32 flags, fs_inode_t **out);
    i32  (*fs_read)(fs_inode_t *in, u32 pos, void *buf, u32 len);
    i32  (*fs_write)(fs_inode_t *in, u32 pos, const void *buf, u32 len);
    int  (*fs_trunc)(fs_inode_t *in);
    int  (*fs_mkdir)(fs_inode_t *dir, const char *name);
    int  (*fs_unlink)(fs_inode_t *dir, const char *name);
    u32  (*fs_readdir)(fs_inode_t *dir, u32 idx, char *name);
    int  (*fs_sync)(fs_inode_t *in);
} fs_ops_t;

/* 文件系统类型注册项 */
typedef struct {
    const char *fstype;
    fs_ops_t   *ops;
    u32         registered;
} fs_type_t;

/* inode */
struct fs_inode {
    u32           ino;
    u32           type;      /* FT_* */
    u32           mode;      /* 低 9 位权限位：owner/group/other rwx */
    u32           uid;       /* 属主 */
    u32           gid;       /* 属组 */
    u32           size;
    u32           refcount;
    u32           flags;     /* 位0=只读挂载 */
    fs_ops_t     *ops;
    fs_dentry_t  *children;  /* 目录子项链 */
    u8           *data;      /* tmpfs 数据（常规文件） */
    u32           blocks;    /* 已分配数据块数（256B/块） */
    u32           mtime;
};

/* 目录项 */
struct fs_dentry {
    char          name[FS_NAME_MAX];
    fs_inode_t   *inode;
    fs_dentry_t  *next;
};

/* 打开文件 */
struct fs_file {
    u32           fd;
    u32           flags;
    u32           pos;
    fs_inode_t   *inode;
    int           used;
};

/* 挂载点 */
typedef struct {
    char          mnt_path[FS_PATH_MAX];
    fs_inode_t   *root;
    fs_type_t    *fs;
    int           used;
    int           readonly;
} fs_mount_t;

/* ---------------- devfs 设备文件系统 ---------------- */
#define DEVFS_DEV_MAX  16u
#define DEV_NAME_MAX   24u
#define DEV_MAJ_MEM    1u   /* 内存设备（null/zero） */
#define DEV_MAJ_TTY    2u   /* 控制台 */
#define DEV_MAJ_AUDIO  4u   /* 音频 */
#define DEV_MAJ_DISPLAY 5u  /* 显示 */
#define DEV_MAJ_INPUT  6u   /* 输入（键盘/鼠标） */
typedef struct {
    i32 (*open)(u32 minor);
    i32 (*read)(u32 minor, u32 pos, void *buf, u32 len);
    i32 (*write)(u32 minor, u32 pos, const void *buf, u32 len);
    i32 (*close)(u32 minor);
    i32 (*ioctl)(u32 minor, u32 cmd, u32 arg);
} dev_ops_t;

int  devfs_register(const char *name, u32 major, u32 minor, dev_ops_t *ops);
int  devfs_unregister(const char *name);
u32  devfs_count(void);
void devfs_dump(void);

/* ---------------- 公共接口 ---------------- */
void   fs_init(void);
int    fs_register_type(const char *fstype, fs_ops_t *ops);
int    fs_mount(const char *fstype, const char *path, int readonly);
int    fs_mkdir(const char *path);
int    fs_unlink(const char *path);
int    fs_open(const char *path, u32 flags);
i32    fs_read(u32 fd, void *buf, u32 len);
i32    fs_write(u32 fd, const void *buf, u32 len);
int    fs_lseek(u32 fd, i32 off, int whence);
int    fs_close(u32 fd);
int    fs_stat(const char *path, u32 *size, u32 *type);
int    fs_readdir(const char *path, u32 idx, char *name);
void   fs_dump(void);
u32    fs_selftest(void);

/* FFS v2：权限与属主 */
void   fs_set_cur_uid(u32 uid);
u32    fs_get_cur_uid(void);
int    fs_chmod(const char *path, u32 mode);
int    fs_chown(const char *path, u32 uid, u32 gid);
int    fs_perm_check(const fs_inode_t *in, u32 want);

#endif /* XOS_FS_H */
