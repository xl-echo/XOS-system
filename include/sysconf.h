/* ============================================================================
 * XOS 系统配置持久化子系统（sysconf）
 * 自研：键值对配置文件 /disk/xos.conf（XFFS 持久化）
 * 用途：分辨率记忆恢复、用户偏好、系统参数等跨重启保存。
 * 格式：每行 "key=value\n"，# 开头为注释；大小写敏感；值仅 ASCII 数字。
 * ========================================================================== */
#ifndef XOS_SYSCONF_H
#define XOS_SYSCONF_H

#include "types.h"

/* 读取整数配置；文件或键不存在时返回 dflt */
u32 sysconf_get_u32(const char *key, u32 dflt);

/* 写入整数配置（新建或覆盖键）；0=成功，非 0=失败（磁盘错误/内存不足） */
int sysconf_set_u32(const char *key, u32 val);

/* 打印全部配置（调试用） */
void sysconf_dump(void);

#endif
