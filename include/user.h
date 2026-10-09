#ifndef _XOS_USER_H
#define _XOS_USER_H
#include "types.h"
#include "idt.h"

#define USER_BASE      0x08000000u   /* 用户空间基址（与 syscall.c user_ptr_valid 一致） */
#define USER_TOP       0xC0000000u   /* 用户空间顶（内核空间起点） */
/* 用户栈顶采用 vmm.h 的 USER_STACK_TOP（0xB0000000，用户空间内） */

u32  user_load_elf(const u8 *img, u32 size, u32 *entry);
void user_exec(u32 entry);
void user_cleanup(void);
int  user_selftest(void);
u32  user_pages_used(void);
int  user_is_active(void);

/* 内核恢复点：user_exec 前保存，用户程序退出/崩溃后 iret 返回此处 */
void user_set_shell_entry(u32 e);
/* 用户程序退出或 ring3 异常：把异常帧改造成回内核的 iret 帧 */
void user_return_to_kernel(isr_regs_t *r);
/* 用户程序退出后的内核清理与 shell 重入（被 user_exit_stub 调用） */
void user_after_exit(void);

#endif
