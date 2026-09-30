#ifndef XOS_INST_H
#define XOS_INST_H

#include "types.h"

#define INST_MAX_PARTS   8u
#define INST_PART_LEN    12u
#define INST_MAX_COPY    8u
#define INST_COPY_LEN    16u
#define INST_MAX_DEVS    8u
#define INST_DEV_LEN     12u

typedef struct {
    char  name[INST_PART_LEN];
    u32   start;
    u32   size;
    u32   used;
} inst_part_t;

typedef struct {
    char  src[INST_COPY_LEN];
    char  dst[INST_COPY_LEN];
    u32   bytes;
    u32   done;
} inst_copy_t;

typedef struct {
    char  name[INST_DEV_LEN];
    u32   class;          /* 0=disk 1=net 2=input 3=display */
    u32   sel_drv;        /* 选中驱动 */
    u32   used;
} inst_dev_t;

void inst_init(void);
int  inst_media_detect(const char *media, u32 *ok);   /* 安装介质引导 */
int  inst_part_add(const char *name, u32 start, u32 size);
int  inst_part_format(const char *name);              /* 分区格式化 */
int  inst_part_state(const char *name, u32 *fmt);
int  inst_copy_add(const char *src, const char *dst, u32 bytes);
int  inst_copy_run(u32 *done_bytes);                  /* 系统文件复制 */
int  inst_boot_write(const char *target, u32 *ok);    /* 引导装载程序安装 */
int  inst_dev_add(const char *name, u32 cls);
int  inst_dev_select(const char *name, u32 drv);      /* 硬件检测与驱动选择 */
int  inst_selftest(void);

#endif
