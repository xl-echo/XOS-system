/* ============================================================================
 * XOS 应用程序生态子系统（第 24 册 · 应用程序生态）
 *   - 可执行文件格式 ELF（ELF32 头/程序头/节头解析与校验）
 *   - 程序加载与启动（段映射 / 页对齐 / 入口点 / 状态机）
 *   - 动态链接器（符号表 / 重定位 / GOT-PLT 模型）
 *   - 共享库管理（soname / 依赖 / 导出导入 / 搜索路径）
 * 完全自研：不依赖任何外部 ELF 加载器，遵循 System V ELF 规范。
 * ========================================================================== */
#ifndef __XOS_APP_H__
#define __XOS_APP_H__

#include "types.h"

#define APP_MAX_PROGS     6u       /* 可同时登记的用户程序描述符数 */
#define APP_MAX_SEGS      4u       /* 每程序段描述符上限 */
#define APP_MAX_SYMS      16u      /* 每程序符号表条目上限 */
#define APP_MAX_DEPS      8u       /* 每程序共享库依赖上限 */
#define APP_MAX_LIBS      8u       /* 已加载共享库上限 */
#define APP_MAX_IMPORTS   8u       /* 每程序导入符号上限 */

#define APP_ELF_MAG0      0x7Fu
#define APP_ELF_MAG123     0x454C46u      /* 'ELF' */

/* ELF 常量 */
#define APP_ELFCLASS32    1u
#define APP_ELFDATA2LSB   1u
#define APP_ET_EXEC       2u
#define APP_ET_DYN        3u
#define APP_EM_386        3u
#define APP_PT_LOAD       1u
#define APP_PT_PHDR       6u
#define APP_PF_X          1u
#define APP_PF_W          2u
#define APP_PF_R          4u
#define APP_R_386_32      1u
#define APP_R_386_PC32    2u

/* 状态机 */
#define APP_STATE_NONE    0u
#define APP_STATE_LOADED  1u
#define APP_STATE_LINKED  2u
#define APP_STATE_STARTED 3u

/* 段权限位 */
#define APP_ACC_R         1u
#define APP_ACC_W         2u
#define APP_ACC_X         4u

typedef struct app_seg {
    u32 type;          /* PT_LOAD / PT_PHDR */
    u32 vaddr;         /* 虚拟地址 */
    u32 filesz;        /* 文件内大小 */
    u32 memsz;         /* 内存大小 */
    u32 flags;         /* PF_R/W/X */
    u32 off;           /* 文件偏移 */
    u32 present;
} app_seg_t;

typedef struct app_sym {
    u32 name_off;      /* strtab 偏移 */
    u32 value;
    u32 size;
    u8  info;          /* 绑定+类型 */
    u32 present;
} app_sym_t;

typedef struct app_reloc {
    u32 off;           /* 重定位位置（相对段基址） */
    u32 sym_idx;
    u8  type;          /* R_386_32 / R_386_PC32 */
    u32 present;
} app_reloc_t;

typedef struct app_prog {
    u32 id;
    u32 state;
    u32 entry;         /* 入口点 */
    u32 base;          /* 加载基址 */
    u32 seg_count;
    app_seg_t segs[APP_MAX_SEGS];
    u32 sym_count;
    app_sym_t syms[APP_MAX_SYMS];
    u32 rel_count;
    app_reloc_t rels[APP_MAX_SYMS];
    u32 dep_count;
    u32 deps[APP_MAX_DEPS];      /* 依赖库 id */
    u32 import_count;
    u32 imports[APP_MAX_IMPORTS];/* 已绑定导入符号索引 */
    u32 mapped;        /* 已映射页数（统计） */
    u32 magic;
} app_prog_t;

typedef struct app_lib {
    u32 id;
    u32 soname_hash;
    u32 export_count;
    u32 exports[APP_MAX_IMPORTS];
    u32 export_vals[APP_MAX_IMPORTS];
    u32 dep_count;
    u32 deps[APP_MAX_DEPS];
    u32 state;
    u32 magic;
} app_lib_t;

int  app_init(void);
u32  app_elf_check(const u8 *img, u32 size);
u32  app_elf_ehdr(const u8 *img, u32 size, u32 *entry, u32 *phoff, u32 *phnum, u32 *shoff, u32 *shnum);
u32  app_elf_phdrs(const u8 *img, u32 size, app_seg_t *segs, u32 max);
u32  app_load(const u8 *img, u32 size, u32 *pid);
u32  app_start(u32 pid);
u32  app_state_get(u32 pid);
u32  app_sym_lookup(u32 pid, const char *name);
u32  app_reloc_apply(u32 pid, u32 type, u32 loc, u32 symval);
u32  app_lib_register(const char *soname, u32 *lib_id);
u32  app_lib_find(const char *soname, u32 *lib_id);
u32  app_lib_export(u32 lib_id, const char *name, u32 val);
u32  app_lib_resolve(u32 lib_id, const char *name, u32 *val);
u32  app_link(u32 pid);
u32  app_dep_add(u32 pid, u32 lib_id);
u32  app_dep_cycle(u32 pid);
u32  app_lib_dep_add_check(u32 lib_id, u32 dep_id);
u32  app_lib_dep_cycle_check(u32 lib_id);
int  app_selftest(void);
void app_dump(void);

#endif /* __XOS_APP_H__ */
