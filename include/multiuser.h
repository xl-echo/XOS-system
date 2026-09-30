#ifndef XOS_MULTIUSER_H
#define XOS_MULTIUSER_H

#include "types.h"

/* 用户账户 */
#define MU_MAX_USERS    8u
#define MU_NAME_LEN     16u
#define MU_PASS_LEN     32u
#define MU_HASH_LEN     20u   /* 实际哈希 16 字符 + 终止 */
#define MU_MAX_GROUPS   4u
#define MU_GROUP_NAME   24u
#define MU_MAX_MEMBERS  8u
#define MU_MAX_SESSIONS 4u
#define MU_SESS_LEN     24u

typedef struct {
    u32  uid;
    char name[MU_NAME_LEN];
    char hash[MU_HASH_LEN];      /* 密码哈希（十六进制串） */
    u32  gid;
    u32  flags;                  /* 位0=启用 位1=锁定 */
    u32  fail_count;
    u32  used;
} mu_user_t;

typedef struct {
    u32  gid;
    char name[MU_GROUP_NAME];
    u32  members[MU_MAX_MEMBERS];
    u32  member_cnt;
    u32  used;
} mu_group_t;

typedef struct {
    u32  uid;
    char token[MU_SESS_LEN];
    u32  start_ticks;
    u32  used;
} mu_session_t;

int  mu_user_add(const char *name, const char *pass, u32 gid, u32 *uid);
int  mu_user_del(u32 uid);
int  mu_user_lookup(const char *name, u32 *uid);
int  mu_user_setpass(u32 uid, const char *pass);
int  mu_user_lock(u32 uid, u32 lock);
int  mu_user_flags(u32 uid, u32 *flags);
int  mu_user_fail_inc(u32 uid);
int  mu_user_fail_reset(u32 uid);
int  mu_group_add(const char *name, u32 *gid);
int  mu_group_del(u32 gid);
int  mu_group_add_member(u32 gid, u32 uid);
int  mu_group_has_member(u32 gid, u32 uid);
int  mu_group_count(void);
int  mu_authenticate(const char *name, const char *pass, u32 *uid);
int  mu_session_open(u32 uid, char *token, u32 len);
int  mu_session_close(const char *token);
int  mu_session_valid(const char *token, u32 *uid);
int  mu_user_count(void);
void mu_init(void);
void mu_dump(void);
int  mu_selftest(void);

#endif
