#ifndef XOS_BUILD_H
#define XOS_BUILD_H

#include "types.h"

#define BD_MAX_CFG      12u
#define BD_CFG_KEY_LEN  24u
#define BD_CFG_VAL_LEN  32u
#define BD_MAX_TARGETS  12u
#define BD_TGT_LEN      24u
#define BD_MAX_SYMS     32u
#define BD_SYM_LEN      24u

/* 目标文件格式 */
#define BD_FMT_ELF      0u
#define BD_FMT_MACHO    1u
#define BD_FMT_COFF     2u
#define BD_FMT_UNKNOWN  3u

typedef struct {
    char key[BD_CFG_KEY_LEN];
    char val[BD_CFG_VAL_LEN];
    u32  used;
} bd_cfg_t;

typedef struct {
    char name[BD_TGT_LEN];
    u32  deps;            /* 依赖计数 */
    u32  built;           /* 已构建 */
    u32  dirty;           /* 待重建 */
} bd_target_t;

typedef struct {
    char name[BD_SYM_LEN];
    u32  addr;
    u32  defined;
} bd_sym_t;

void bd_init(void);
int  bd_cfg_set(const char *key, const char *val);
int  bd_cfg_get(const char *key, char *out, u32 max);
int  bd_target_add(const char *name, u32 deps);
int  bd_target_state(const char *name, u32 *dirty);
int  bd_build_one(const char *name);        /* 编译调度：构建一个目标 */
int  bd_build_all(u32 *done, u32 *total);   /* 全量构建 */
int  bd_link(const char *out, u32 nsyms, const char *const *syms, u32 *addrs);
int  bd_obj_format(const u8 *hdr, u32 len); /* 目标文件格式识别 */
int  bd_selftest(void);

#endif
