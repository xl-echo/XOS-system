#ifndef XOS_SHELL_H
#define XOS_SHELL_H

#include "types.h"

/* 命令行解析 */
#define SH_MAX_CMD      64u     /* 单条命令最大长度（bss 裁剪） */
#define SH_MAX_TOKENS   16u     /* 每行最大 token 数 */
#define SH_MAX_LINE     256u    /* 输入行缓冲 */

/* 环境变量 */
#define SH_MAX_ENV      12u     /* 环境变量表上限（bss 裁剪） */
#define SH_ENV_NAME_LEN 16u
#define SH_ENV_VAL_LEN  32u

/* 命令历史 */
#define SH_HIST_MAX     8u      /* 历史条目上限（bss 裁剪） */

/* 解析结果 */
typedef struct {
    u32  argc;
    char argv[SH_MAX_TOKENS][SH_MAX_CMD];
    u32  in_fd;       /* 0=无；1=文件重定向 */
    u32  out_fd;      /* 0=无；1=覆盖 >；2=追加 >> */
    u32  pipe_to;     /* 是否有管道下游 */
    u32  background;  /* 后台 & */
    char in_file[SH_MAX_CMD];
    char out_file[SH_MAX_CMD];
} sh_cmdline_t;

/* 环境变量项 */
typedef struct {
    char name[SH_ENV_NAME_LEN];
    char val[SH_ENV_VAL_LEN];
    u32  used;
} sh_env_t;

/* 作业控制 */
typedef struct {
    u32  jid;         /* 作业号 */
    u32  pid;         /* 关联进程 */
    u32  state;       /* 0=运行 1=停止 2=完成 */
    u32  used;
} sh_job_t;

#define SH_MAX_JOBS    4u

void sh_parse(const char *line, sh_cmdline_t *out);
int  sh_env_set(const char *name, const char *val);
int  sh_env_get(const char *name, char *buf, u32 len);
int  sh_env_unset(const char *name);
int  sh_env_expand(const char *src, char *dst, u32 len);   /* $VAR 展开 */
void sh_env_export(void);                                   /* 导出全部变量 */
void sh_job_start(u32 pid, u32 *jid);
int  sh_job_wait(u32 jid);
void shell_init(void);
void shell_dump(void);
void sh_hist_add(const char *line);
u32  sh_hist_count(void);
const char *sh_hist_get(u32 idx);
int  sh_selftest(void);

#endif
