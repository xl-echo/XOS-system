#ifndef XOS_PM_H
#define XOS_PM_H

#include "types.h"

#define PM_MAX_TABLES    8u
#define PM_TBL_NAME_LEN  8u
#define PM_MAX_WAKE      4u

/* 电源状态 */
#define PM_S0            0u   /* 工作 */
#define PM_S3            3u   /* 睡眠（挂起到内存语义） */
#define PM_S4            4u   /* 休眠（镜像语义） */
#define PM_S5            5u   /* 关机 */

/* 唤醒源 */
#define PM_WAKE_NONE     0u
#define PM_WAKE_RTC      1u
#define PM_WAKE_KEY      2u
#define PM_WAKE_LAN      3u
#define PM_WAKE_PWRBTN   4u

typedef struct {
    char  sig[PM_TBL_NAME_LEN];   /* ACPI 表签名，如 "RSDT"/"FADT" */
    u32   addr;                   /* 表地址（模拟） */
    u32   len;
    u32   used;
} pm_acpi_tab_t;

typedef struct {
    u32   state;                  /* S0/S3/S4/S5 */
    u32   wake_source;            /* 唤醒源 */
    u32   wake_count;
    u32   suspend_count;          /* 挂起次数 */
    u32   resume_count;
    u32   power_ok;               /* 电源正常标志 */
    u32   battery_pct;            /* 电池百分比（模拟） */
    u32   ac_online;
} pm_machine_t;

void pm_init(void);
int  pm_acpi_parse(const char *sig, u32 addr, u32 len);
int  pm_acpi_find(const char *sig, u32 *addr, u32 *len);
u32  pm_acpi_count(void);
int  pm_suspend(void);            /* S0 -> S3 */
int  pm_resume(u32 wake);         /* S3 -> S0 */
int  pm_hibernate(void);          /* S4 镜像语义 */
int  pm_shutdown(void);           /* S5 */
u32  pm_state(void);
int  pm_set_battery(u32 pct, u32 ac);
int  pm_battery(u32 *pct, u32 *ac);
u32  pm_wake_count(void);
void pm_dump(void);
int  pm_selftest(void);

#endif
