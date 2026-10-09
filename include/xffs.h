/* ============================================================================
 * XOS 磁盘文件系统 XFFS v1 头文件
 * 真实持久化：/disk 挂载点，重启保留用户数据。
 * ========================================================================== */
#ifndef XOS_XFFS_H
#define XOS_XFFS_H

#include "types.h"

int  xffs_init(void);          /* 挂载 /disk（magic 不符时自动格式化空白区） */void xffs_flush_all(void);     /* 关机前置：幂等落盘 */
u32  xffs_file_count(void);
u32  xffs_space_free_sects(void);
u32  xffs_ready(void);
u32  xffs_selftest(void);
void xffs_dump(void);

#endif
