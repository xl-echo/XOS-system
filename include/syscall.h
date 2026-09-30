/* ============================================================================
 * XOS 系统调用接口头文件
 * 调用约定：int 0x80，eax=系统调用号，ebx/ecx/edx=参数1/2/3，返回值写回 eax。
 * 自研实现：编号表 / 分发表 / 参数校验 / 用户态指针拷贝 / 错误码 /
 * 审计环形缓冲 / seccomp 白名单 / 限流配额 / ABI 版本 / 统计导出。
 * ========================================================================== */
#ifndef XOS_SYSCALL_H
#define XOS_SYSCALL_H

#include "types.h"

/* ---------------- 系统调用号（ABI 稳定：一经发布不重排） ---------------- */
#define SYS_EXIT       0u   /* 进程退出（占位，用户态循环退出） */
#define SYS_GETPID     1u   /* 返回当前任务 pid */
#define SYS_GETTICKS   2u   /* 返回调度 tick 计数 */
#define SYS_MSLEEP     3u   /* 睡眠毫秒 */
#define SYS_YIELD      4u   /* 主动让出 CPU */
#define SYS_WRITE      5u   /* 写终端：a1=字符编码 */
#define SYS_ECHO       6u   /* 回显测试：返回 a1 */
#define SYS_GETVER     7u   /* 返回系统调用 ABI 版本 */
#define SYS_FILTER     8u   /* 调试：设置 seccomp/配额（仅内核态测试用） */
#define SYS_NR         16u  /* 槽位数（含预留，保证 0..15） */

/* ---------------- 系统调用返回错误码（负值，ABI 稳定） ---------------- */
#define SYSC_OK        0
#define SYSC_EINVAL    (-1)   /* 非法参数 */
#define SYSC_ENOSYS    (-2)   /* 未实现/未知系统调用号 */
#define SYSC_EFAULT    (-3)   /* 用户指针非法/越界 */
#define SYSC_EPERM     (-4)   /* 权限不足（seccomp 拒绝） */
#define SYSC_EAGAIN    (-5)   /* 资源暂不可用（限流/配额） */
#define SYSC_EOVERFLOW (-6)   /* 数值溢出 */

#define SYSCALL_ABI_VERSION 1u

typedef u32 (*syscall_fn_t)(u32 a1, u32 a2, u32 a3);

/* ---------------- 审计记录 ---------------- */
#define AUDIT_RING 8
typedef struct {
    u32 nr, a1, a2, a3;
    i32 ret;
    u32 ticks;
} audit_rec_t;

/* ---------------- vDSO（虚拟动态共享对象：无陷入快速路径） ---------------- */
#define VDSO_MAGIC    0x5644534Fu   /* "VDSO" */
#define VDSO_API_MAX  8u
typedef struct {
    u32 magic;
    u32 version;
    volatile u32 seq;              /* 顺序锁：奇数=更新中 */
    volatile u32 ticks;            /* 时钟快照（每 tick 刷新） */
    volatile u32 pid;              /* 当前任务 pid 快照（切换刷新） */
    u32 api[VDSO_API_MAX];         /* 快速路径 API 地址表（0=未提供） */
} vdso_data_t;

int  vdso_map(u32 user_addr);             /* 映射 vDSO 页到用户空间（只读） */
void vdso_update(void);                   /* 调度/中断路径刷新快照 */
u32  vdso_getticks_fast(u32 user_addr);   /* 用户态无陷入读时钟（模拟快速路径） */
u32  vdso_getpid_fast(u32 user_addr);     /* 用户态无陷入读 pid（模拟快速路径） */

/* ---------------- 32/64 位兼容层 ---------------- */
#define SYSCALL_ABI_32 1u
#define SYSCALL_ABI_64 2u
#define ABI_ARG_MAX    6u
typedef struct {
    u32 regs[ABI_ARG_MAX];   /* 参数寄存器映射（32位: eax=号 ebx/ecx/edx/esi/edi） */
    u32 abi;                 /* 调用 ABI（SYSCALL_ABI_32/64） */
} syscall_args_t;

u32 syscall_dispatch_abi(u32 nr, u32 abi, u32 *regs, u32 n);  /* 按 ABI 分发 */

/* ---------------- 系统调用文档表（文档生成） ---------------- */
#define SYSCALL_DOC_NAME_MAX 24u
typedef struct {
    u32 nr;
    const char *name;
    const char *sig;
    const char *desc;
    const char *errs;
} syscall_doc_t;

void syscall_doc_dump(void);   /* 输出完整文档表（名/签名/说明/错误码） */
u32  syscall_doc_count(void);  /* 文档条目数 */

/* ---------------- 系统调用与信号交互 ---------------- */
int  syscall_check_pending(void);           /* 返回是否有挂起信号（EINTR 判据） */
int  sys_sleep_interruptible(u32 ms);       /* 可被信号中断的睡眠：返回 0=完成 -7=被信号中断 */
#define SYSC_EINTR (-7)

/* ---------------- 线程局部存储（TLS） ---------------- */
int  tls_set_base(u32 pid, u32 base);   /* 设置任务 TLS 基址（0x10000000..0xC0000000 校验） */
u32  tls_get_base(u32 pid);             /* 读取任务 TLS 基址（0=未设置） */

/* ---------------- 公共接口 ---------------- */
void   syscall_init(void);
u32    syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3);
int    user_ptr_valid(u32 ptr, u32 len);
int    copy_from_user(void *dst, u32 user_ptr, u32 len);
int    copy_to_user(u32 user_ptr, const void *src, u32 len);
void   syscall_filter_set(u32 nr, int allow);       /* seccomp 白名单 */
void   syscall_set_quota(u32 nr, u32 q);            /* 限流配额/ticks 窗口 */
u32    syscall_audit_count(void);
void   syscall_dump(void);
u32    syscall_selftest(void);

#endif /* XOS_SYSCALL_H */
