/* XOS 多用户与权限 —— 账户 / 用户组 / 登录认证 / 密码哈希 / 会话 */
#include "multiuser.h"
#include "console.h"

static mu_user_t    g_users[MU_MAX_USERS];
static mu_group_t   g_groups[MU_MAX_GROUPS];
static mu_session_t g_sessions[MU_MAX_SESSIONS];
static u32          g_uid_seq;
static u32          g_gid_seq;
static u32          g_sess_seq;

static int mu_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)(*a) - (u8)(*b);
}

static void mu_strncpy(char *d, const char *s, u32 n)
{
    u32 i;
    for (i = 0u; i < n && s[i]; i++) d[i] = s[i];
    if (n > 0u) d[i < n ? i : n - 1u] = 0;
}

/* FNV-1a 32 位哈希 → 32 字符十六进制（不可逆） */
static void mu_hash(const char *pass, char *out)
{
    u32 h = 2166136261u;
    u32 i;
    const char hex[] = "0123456789abcdef";
    for (i = 0u; pass[i]; i++) {
        h ^= (u8)pass[i];
        h *= 16777619u;
    }
    for (i = 0u; i < 8u; i++) {
        u8 b = (u8)(h >> (i * 4u));
        out[i * 2u] = hex[b & 0xFu];
        out[i * 2u + 1u] = hex[(b >> 4u) & 0xFu];
    }
    out[16u] = 0;
}

/* ---------- 账户管理 ---------- */

int mu_user_add(const char *name, const char *pass, u32 gid, u32 *uid)
{
    u32 i, free_slot = MU_MAX_USERS;
    for (i = 0u; i < MU_MAX_USERS; i++) {
        if (g_users[i].used && mu_strcmp(g_users[i].name, name) == 0)
            return -1;   /* 重名 */
        if (!g_users[i].used && free_slot == MU_MAX_USERS)
            free_slot = i;
    }
    if (free_slot == MU_MAX_USERS) return -2;   /* 满 */
    g_users[free_slot].uid = ++g_uid_seq;
    mu_strncpy(g_users[free_slot].name, name, MU_NAME_LEN - 1u);
    mu_hash(pass, g_users[free_slot].hash);
    g_users[free_slot].gid = gid;
    g_users[free_slot].flags = 1u;              /* 启用 */
    g_users[free_slot].fail_count = 0u;
    g_users[free_slot].used = 1u;
    if (uid) *uid = g_users[free_slot].uid;
    return 0;
}

int mu_user_del(u32 uid)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) {
            g_users[i].used = 0u;
            return 0;
        }
    return -1;
}

int mu_user_lookup(const char *name, u32 *uid)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && mu_strcmp(g_users[i].name, name) == 0) {
            if (uid) *uid = g_users[i].uid;
            return 0;
        }
    return -1;
}

int mu_user_setpass(u32 uid, const char *pass)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) {
            mu_hash(pass, g_users[i].hash);
            g_users[i].fail_count = 0u;
            return 0;
        }
    return -1;
}

int mu_user_lock(u32 uid, u32 lock)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) {
            if (lock) g_users[i].flags |= 2u;
            else g_users[i].flags &= ~2u;
            return 0;
        }
    return -1;
}

int mu_user_flags(u32 uid, u32 *flags)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) {
            if (flags) *flags = g_users[i].flags;
            return 0;
        }
    return -1;
}

int mu_user_fail_inc(u32 uid)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) {
            g_users[i].fail_count++;
            if (g_users[i].fail_count >= 5u) g_users[i].flags |= 2u;   /* 5 次锁定 */
            return 0;
        }
    return -1;
}

int mu_user_fail_reset(u32 uid)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) {
            g_users[i].fail_count = 0u;
            return 0;
        }
    return -1;
}

int mu_user_count(void)
{
    u32 i, c = 0u;
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used) c++;
    return (int)c;
}

/* ---------- 用户组 ---------- */

int mu_group_add(const char *name, u32 *gid)
{
    u32 i, free_slot = MU_MAX_GROUPS;
    for (i = 0u; i < MU_MAX_GROUPS; i++) {
        if (g_groups[i].used && mu_strcmp(g_groups[i].name, name) == 0)
            return -1;
        if (!g_groups[i].used && free_slot == MU_MAX_GROUPS)
            free_slot = i;
    }
    if (free_slot == MU_MAX_GROUPS) return -2;
    g_groups[free_slot].gid = ++g_gid_seq;
    mu_strncpy(g_groups[free_slot].name, name, MU_GROUP_NAME - 1u);
    g_groups[free_slot].member_cnt = 0u;
    g_groups[free_slot].used = 1u;
    if (gid) *gid = g_groups[free_slot].gid;
    return 0;
}

int mu_group_del(u32 gid)
{
    u32 i;
    for (i = 0u; i < MU_MAX_GROUPS; i++)
        if (g_groups[i].used && g_groups[i].gid == gid) {
            g_groups[i].used = 0u;
            return 0;
        }
    return -1;
}

int mu_group_add_member(u32 gid, u32 uid)
{
    u32 i, k;
    for (i = 0u; i < MU_MAX_GROUPS; i++)
        if (g_groups[i].used && g_groups[i].gid == gid) {
            for (k = 0u; k < g_groups[i].member_cnt; k++)
                if (g_groups[i].members[k] == uid) return 0;   /* 已存在 */
            if (g_groups[i].member_cnt >= MU_MAX_MEMBERS) return -2;
            g_groups[i].members[g_groups[i].member_cnt++] = uid;
            return 0;
        }
    return -1;
}

int mu_group_has_member(u32 gid, u32 uid)
{
    u32 i, k;
    for (i = 0u; i < MU_MAX_GROUPS; i++)
        if (g_groups[i].used && g_groups[i].gid == gid) {
            for (k = 0u; k < g_groups[i].member_cnt; k++)
                if (g_groups[i].members[k] == uid) return 1;
            return 0;
        }
    return -1;
}

int mu_group_count(void)
{
    u32 i, c = 0u;
    for (i = 0u; i < MU_MAX_GROUPS; i++)
        if (g_groups[i].used) c++;
    return (int)c;
}

/* ---------- 登录认证 ---------- */

int mu_authenticate(const char *name, const char *pass, u32 *uid)
{
    u32 i;
    char h[MU_HASH_LEN];
    u32 uname = 0u;
    mu_hash(pass, h);
    for (i = 0u; i < MU_MAX_USERS; i++) {
        if (!g_users[i].used) continue;
        if (mu_strcmp(g_users[i].name, name) != 0) continue;
        uname = 1u;
        if (g_users[i].flags & 2u) return -3;   /* 锁定 */
        if (mu_strcmp(g_users[i].hash, h) != 0) {
            mu_user_fail_inc(g_users[i].uid);
            return -1;                          /* 密码错 */
        }
        mu_user_fail_reset(g_users[i].uid);
        if (uid) *uid = g_users[i].uid;
        return 0;
    }
    (void)uname;
    return -1;   /* 用户不存在 */
}

/* ---------- 会话 ---------- */

int mu_session_open(u32 uid, char *token, u32 len)
{
    u32 i, free_slot = MU_MAX_SESSIONS;
    u32 j;
    const char hex[] = "0123456789abcdef";
    u32 h = 2166136261u ^ (uid << 16u) ^ g_sess_seq;
    for (i = 0u; i < MU_MAX_SESSIONS; i++) {
        if (!g_sessions[i].used) { free_slot = i; break; }
    }
    if (free_slot == MU_MAX_SESSIONS) return -2;
    g_sessions[free_slot].uid = uid;
    for (j = 0u; j < 16u; j++) {
        h ^= (uid + j) * 2654435761u;
        h *= 16777619u;
        if (j * 2u < len) token[j * 2u] = hex[h & 0xFu];
        if (j * 2u + 1u < len) token[j * 2u + 1u] = hex[(h >> 4u) & 0xFu];
    }
    if (len > 0u) token[(16u < len) ? 16u : len - 1u] = 0;
    mu_strncpy(g_sessions[free_slot].token, token, MU_SESS_LEN - 1u);
    g_sessions[free_slot].start_ticks = ++g_sess_seq;
    g_sessions[free_slot].used = 1u;
    return 0;
}

int mu_session_close(const char *token)
{
    u32 i;
    for (i = 0u; i < MU_MAX_SESSIONS; i++)
        if (g_sessions[i].used && mu_strcmp(g_sessions[i].token, token) == 0) {
            g_sessions[i].used = 0u;
            return 0;
        }
    return -1;
}

int mu_session_valid(const char *token, u32 *uid)
{
    u32 i;
    for (i = 0u; i < MU_MAX_SESSIONS; i++)
        if (g_sessions[i].used && mu_strcmp(g_sessions[i].token, token) == 0) {
            if (uid) *uid = g_sessions[i].uid;
            return 0;
        }
    return -1;
}

/* ---------- 初始化 / 转储 ---------- */

void mu_init(void)
{
    u32 i;
    for (i = 0u; i < MU_MAX_USERS; i++) g_users[i].used = 0u;
    for (i = 0u; i < MU_MAX_GROUPS; i++) g_groups[i].used = 0u;
    for (i = 0u; i < MU_MAX_SESSIONS; i++) g_sessions[i].used = 0u;
    g_uid_seq = 0u; g_gid_seq = 0u; g_sess_seq = 0u;
}

void mu_dump(void)
{
    u32 i;
    con_puts("  users=");
    for (i = 0u; i < MU_MAX_USERS; i++)
        if (g_users[i].used) {
            con_puts(g_users[i].name);
            con_puts("(uid");
            con_put_dec(g_users[i].uid);
            con_puts(") ");
        }
    con_puts(" groups=");
    for (i = 0u; i < MU_MAX_GROUPS; i++)
        if (g_groups[i].used) {
            con_puts(g_groups[i].name);
            con_puts("(gid");
            con_put_dec(g_groups[i].gid);
            con_puts(") ");
        }
    con_puts("\n");
}

/* ---------- 自检 ---------- */

int mu_selftest(void)
{
    u32 uid1, uid2, gid1, gid2, flags, s_uid;
    char tok[MU_SESS_LEN];
    u32 i;

    /* 1-4: 账户创建 */
    if (mu_user_add("root", "toor", 0u, &uid1) != 0) return 1;
    if (uid1 == 0u) return 2;
    if (mu_user_add("alice", "pass1", 0u, &uid2) != 0) return 3;
    if (uid2 <= uid1) return 4;

    /* 5-6: 重名拒绝 */
    if (mu_user_add("root", "x", 0u, &i) != -1) return 5;
    if (mu_user_count() != 2) return 6;

    /* 7-8: 查找 */
    if (mu_user_lookup("alice", &i) != 0) return 7;
    if (i != uid2) return 8;
    if (mu_user_lookup("nobody", &i) != -1) return 9;

    /* 10-13: 认证 */
    if (mu_authenticate("alice", "pass1", &i) != 0) return 10;
    if (i != uid2) return 11;
    if (mu_authenticate("alice", "wrong", &i) != -1) return 12;
    if (mu_authenticate("ghost", "x", &i) != -1) return 13;

    /* 14-16: 密码修改 */
    if (mu_user_setpass(uid2, "newpass") != 0) return 14;
    if (mu_authenticate("alice", "pass1", &i) != -1) return 15;
    if (mu_authenticate("alice", "newpass", &i) != 0) return 16;

    /* 17-19: 失败锁定 */
    for (i = 0u; i < 5u; i++) mu_user_fail_inc(uid2);
    if (mu_authenticate("alice", "newpass", &i) != -3) return 17;
    if (mu_user_flags(uid2, &flags) != 0) return 18;
    if (!(flags & 2u)) return 19;

    /* 20: 解锁 */
    if (mu_user_lock(uid2, 0u) != 0) return 20;
    if (mu_authenticate("alice", "newpass", &i) != 0) return 21;

    /* 22-26: 用户组 */
    if (mu_group_add("admin", &gid1) != 0) return 22;
    if (gid1 == 0u) return 23;
    if (mu_group_add("staff", &gid2) != 0) return 24;
    if (gid2 <= gid1) return 25;
    if (mu_group_count() != 2) return 26;

    /* 27-29: 组成员 */
    if (mu_group_add_member(gid1, uid1) != 0) return 27;
    if (mu_group_has_member(gid1, uid1) != 1) return 28;
    if (mu_group_has_member(gid1, uid2) != 0) return 29;
    if (mu_group_add_member(gid1, uid1) != 0) return 30;

    /* 31-32: 组删除 */
    if (mu_group_del(gid2) != 0) return 31;
    if (mu_group_has_member(gid2, uid1) != -1) return 32;

    /* 33-36: 会话 */
    if (mu_session_open(uid1, tok, sizeof(tok)) != 0) return 33;
    if (mu_session_valid(tok, &s_uid) != 0) return 34;
    if (s_uid != uid1) return 35;
    if (mu_session_valid("bogus", &s_uid) != -1) return 36;

    /* 37: 会话关闭 */
    if (mu_session_close(tok) != 0) return 37;
    if (mu_session_valid(tok, &s_uid) != -1) return 38;

    /* 39-40: 删除账户 */
    if (mu_user_del(uid2) != 0) return 39;
    if (mu_user_lookup("alice", &i) != -1) return 40;

    /* 41-42: 账户满 */
    for (i = 0u; i < MU_MAX_USERS; i++) {
        char nm[8];
        nm[0] = 'u'; nm[1] = (char)('0' + i); nm[2] = 0;
        mu_user_add(nm, "p", 0u, &uid2);
    }
    if (mu_user_add("last", "p", 0u, &uid2) != -2) return 41;
    if ((u32)mu_user_count() > MU_MAX_USERS) return 42;

    /* 43-45: 锁定标志位 */
    if (mu_user_lock(uid1, 1u) != 0) return 43;
    if (mu_user_flags(uid1, &flags) != 0) return 44;
    if (!(flags & 2u)) return 45;

    /* 自检不污染运行时：清空全部用户/组/会话，恢复全新状态 */
    for (i = 0u; i < MU_MAX_USERS; i++) g_users[i].used = 0u;
    g_uid_seq = 0u;
    for (i = 0u; i < MU_MAX_GROUPS; i++) g_groups[i].used = 0u;
    g_gid_seq = 0u;
    for (i = 0u; i < MU_MAX_SESSIONS; i++) g_sessions[i].used = 0u;
    g_sess_seq = 0u;

    return 0;
}
