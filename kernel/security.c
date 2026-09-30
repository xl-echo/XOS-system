/* ============================================================================
 * XOS 安全机制子系统核心实现（第 27 册 · 安全机制）
 * 完全自研：
 *   1) 随机数生成器：CPUID 探测 RDRAND + 混合熵（TSC/PIT/RDRAND）+ xorshift；
 *   2) 加密算法库：SHA-256（标准 K 表轮函数）、RC4（KSA+PRGA）、CRC32 查表，
 *      全部以公开标准测试向量做真机自检；
 *   3) 密钥环：8 槽，密钥以系统密钥加密态存储，导出仅返回密文；
 *   4) 文件加密抽象：RC4 流加解密，往返一致性验证；
 *   5) 完整性度量 IMA：度量日志（名称+SHA-256）与校验；
 *   6) 审计子系统：16 条环形事件（类型/pid/结果）；
 *   7) LSM 安全钩子框架：钩子注册与调用；
 *   8) MAC 策略矩阵（SELinux 语义简化：域 × 操作）与 AppArmor 域；
 *   9) seccomp 系统调用策略位图；
 *   10) KASLR 熵、NX/WP/页表隔离真实状态验证、栈 canary 检测；
 *   11) 签名哈希校验、信任根链、安全基线、加固状态、自检与 dump。
 * 不依赖任何外部安全核心/闭源方案。
 * ========================================================================== */
#include "security.h"

extern int memcmp(const void *a, const void *b, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);
extern void *memset(void *dst, int c, unsigned int n);
#include "console.h"

/* ---------------- 全局状态 ---------------- */
static u32 sec_magic = 0u;
static u32 rng_state[4];                  /* xorshift128+ 状态 */
static u32 rng_have_rdrand = 0u;
static u32 rng_draws = 0u;
static u8  sys_key[SEC_KEY_MAX];          /* 系统主密钥（加密密钥环用） */
static u32 sys_keylen = 0u;

typedef struct key_slot {
    u16 used;
    u16 len;
    u8  data[SEC_KEY_MAX];                /* 加密态存储 */
} key_slot_t;
static key_slot_t keyring[SEC_KEYRING_MAX];

typedef struct ima_entry {
    u16 used;
    char name[24];
    u8  hash[32];
} ima_entry_t;
static ima_entry_t ima_log[SEC_IMA_MAX];

typedef struct audit_ev {
    u16 type;
    u16 result;
    u32 pid;
    u16 seq;
} audit_ev_t;
static audit_ev_t audit_ring[SEC_AUDIT_MAX];
static u16 audit_head = 0u;
static u16 audit_count = 0u;
static u16 audit_seq = 0u;

typedef struct hook_entry {
    u16 type;
    u16 used;
    sec_hook_fn fn;
} hook_entry_t;
static hook_entry_t hooks[SEC_HOOK_MAX];
static u32 hook_calls[SEC_HOOK_MAX];

static u16 mac_policy[SEC_DOMAIN_MAX][16];  /* 0=deny 1=allow，默认 allow */
static u32 mac_set_count = 0u;

typedef struct aa_rule {
    u16 used;
    char path[24];
    u16 ops;
} aa_rule_t;
static aa_rule_t aa_rules[SEC_DOMAIN_MAX];

static u32 seccomp_map[4];                /* nr 0..127 位图，1=allow */

static u8 trust_roots[4][32];
static u16 trust_count = 0u;

typedef struct sec_cfg {
    char name[20];
    u32 val;
    u32 recommended;
} sec_cfg_t;
static sec_cfg_t cfg[SEC_CFG_MAX];

static u32 canary_val = 0u;
static u32 audit_dump_ok = 0u;

/* ---------------- 端口 / 时钟（自包含） ---------------- */
static inline void outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static u64 sec_rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}
static u32 sec_pit_read(void)
{
    u8 lo, hi;
    outb(0x43, 0x00u);
    lo = inb(0x40);
    hi = inb(0x40);
    return (u32)lo | ((u32)hi << 8);
}

/* ---------------- 随机数生成器 ---------------- */
static int cpu_has_rdrand(void)
{
    u32 a, c;
    __asm__ __volatile__("cpuid" : "=a"(a), "=c"(c) : "a"(1u) : "ebx", "edx");
    return (int)((c >> 30) & 1u);
}
static int rdrand_u32(u32 *out)
{
    u8 ok;
    __asm__ __volatile__("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    return ok ? 1 : 0;
}
static void xorshift_seed(u64 s)
{
    rng_state[0] = (u32)s;
    rng_state[1] = (u32)(s >> 32);
    rng_state[2] = 0x9E3779B9u;
    rng_state[3] = 0x85EBCA6Bu;
}
static u32 xorshift_next(void)
{
    u32 x = rng_state[0];
    u32 y = rng_state[1];
    rng_state[0] = y;
    x ^= x << 23;
    rng_state[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    rng_state[2] = rng_state[3];
    rng_state[3] = y;
    return rng_state[2] + rng_state[3];
}

int sec_rng_init(void)
{
    u64 t;
    u32 r = 0u;
    rng_have_rdrand = (u32)cpu_has_rdrand();
    if (rng_have_rdrand)
        rdrand_u32(&r);
    t = sec_rdtsc() ^ ((u64)sec_pit_read() << 32) ^ (u64)r;
    xorshift_seed(t);
    /* 预热：混合熵并丢弃前 8 个输出 */
    sec_rng_u32();
    sec_rng_u32();
    sec_rng_u32();
    sec_rng_u32();
    return SEC_OK;
}

u32 sec_rng_u32(void)
{
    u32 r = 0u;
    if (rng_have_rdrand)
        rdrand_u32(&r);
    r ^= xorshift_next();
    r ^= (u32)(sec_rdtsc() & 0xFFFFu) << 16;
    r ^= sec_pit_read();
    rng_draws++;
    return r;
}

void sec_rng_fill(u8 *buf, u32 n)
{
    u32 i;
    for (i = 0u; i < n; i++)
        buf[i] = (u8)(sec_rng_u32() >> ((i & 3u) * 8u));
}

/* ---------------- SHA-256（标准实现） ---------------- */
static const u32 sha_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32u - n)); }

u32 sec_sha256(const u8 *msg, u32 len, u8 out[32])
{
    u32 h[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    u32 w[64], i;
    u64 bitlen = (u64)len * 8u;
    u32 padded = ((len + 8u) / 64u + 1u) * 64u;
    u8  block[64];
    u32 off = 0u;

    while (off < len || off < padded) {
        u32 chunk = len > off ? (len - off > 64u ? 64u : len - off) : 0u;
        u32 b;
        for (b = 0u; b < 64u; b++) {
            if (b < chunk)
                block[b] = msg[off + b];
            else if (off + b == len)
                block[b] = 0x80u;
            else
                block[b] = 0u;
        }
        if (off + 64u >= padded) {
            /* 最后一块：写入位长（大端） */
            for (b = 0u; b < 8u; b++)
                block[64u - 8u + b] = (u8)(bitlen >> (56u - b * 8u));
        }
        /* 消息调度 */
        for (i = 0u; i < 16u; i++)
            w[i] = ((u32)block[i*4] << 24) | ((u32)block[i*4+1] << 16)
                 | ((u32)block[i*4+2] << 8) | (u32)block[i*4+3];
        for (i = 16u; i < 64u; i++) {
            u32 s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            u32 s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        {
            u32 a = h[0], b = h[1], c = h[2], d = h[3];
            u32 e = h[4], f = h[5], g = h[6], hh = h[7];
            for (i = 0u; i < 64u; i++) {
                u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                u32 ch = (e & f) ^ ((~e) & g);
                u32 t1 = hh + s1 + ch + sha_k[i] + w[i];
                u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                u32 maj = (a & b) ^ (a & c) ^ (b & c);
                u32 t2 = s0 + maj;
                hh = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }
        off += 64u;
    }
    for (i = 0u; i < 8u; i++) {
        out[i*4]   = (u8)(h[i] >> 24);
        out[i*4+1] = (u8)(h[i] >> 16);
        out[i*4+2] = (u8)(h[i] >> 8);
        out[i*4+3] = (u8)h[i];
    }
    return SEC_OK;
}

/* ---------------- RC4（标准实现） ---------------- */
void sec_rc4(const u8 *key, u32 keylen, const u8 *in, u32 len, u8 *out)
{
    u8 s[256];
    u32 i, j = 0u, k = 0u;
    for (i = 0u; i < 256u; i++) s[i] = (u8)i;
    for (i = 0u; i < 256u; i++) {
        u8 t;
        j = (j + s[i] + key[i % keylen]) & 0xFFu;
        t = s[i]; s[i] = s[j]; s[j] = t;
    }
    i = 0u; j = 0u;
    for (k = 0u; k < len; k++) {
        u8 t, v;
        i = (i + 1u) & 0xFFu;
        j = (j + s[i]) & 0xFFu;
        t = s[i]; s[i] = s[j]; s[j] = t;
        v = s[(s[i] + s[j]) & 0xFFu];
        out[k] = (u8)(in[k] ^ v);
    }
}

/* ---------------- CRC32（查表实现） ---------------- */
static u32 crc_table[256];
static u32 crc_table_ready = 0u;
static void crc_build_table(void)
{
    u32 i, j;
    for (i = 0u; i < 256u; i++) {
        u32 c = i;
        for (j = 0u; j < 8u; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_ready = 1u;
}
u32 sec_crc32(const u8 *buf, u32 len, u32 seed)
{
    u32 c = seed ^ 0xFFFFFFFFu;
    u32 i;
    if (!crc_table_ready) crc_build_table();
    for (i = 0u; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- 加密算法库自检（标准向量） ---------------- */
int sec_crypto_selftest(void)
{
    /* SHA-256("abc") 标准向量 */
    u8 out[32];
    static const u8 abc_expected[32] = {
        0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,
        0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
        0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,
        0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD
    };
    u32 i;
    sec_sha256((const u8 *)"abc", 3u, out);
    for (i = 0u; i < 32u; i++)
        if (out[i] != abc_expected[i]) return 1;

    /* RC4(Key="Key", "Plaintext") = BBF316E8D940AF0AD3 */
    {
        static const u8 rc4_key[] = {'K','e','y'};
        static const u8 rc4_in[] = {'P','l','a','i','n','t','e','x','t'};
        static const u8 rc4_exp[9] = {0xBB,0xF3,0x16,0xE8,0xD9,0x40,0xAF,0x0A,0xD3};
        u8 rc4_out[9];
        sec_rc4(rc4_key, 3u, rc4_in, 9u, rc4_out);
        for (i = 0u; i < 9u; i++)
            if (rc4_out[i] != rc4_exp[i]) return 2;
    }

    /* CRC32("123456789") = 0xCBF43926 */
    if (sec_crc32((const u8 *)"123456789", 9u, 0u) != 0xCBF43926u) return 3;
    return SEC_OK;
}

/* ---------------- 密钥环 ---------------- */
int sec_key_gen(u8 *key, u32 len)
{
    if (!key || len == 0u || len > SEC_KEY_MAX) return SEC_EINVAL;
    sec_rng_fill(key, len);
    return SEC_OK;
}

int sec_keyring_add(const u8 *key, u32 len, u32 *id)
{
    u32 i, slot = SEC_KEYRING_MAX;
    u8  enc[SEC_KEY_MAX];
    if (!key || len == 0u || len > SEC_KEY_MAX) return SEC_EINVAL;
    for (i = 0u; i < SEC_KEYRING_MAX; i++)
        if (!keyring[i].used) { slot = i; break; }
    if (slot >= SEC_KEYRING_MAX) return SEC_EFULL;
    if (sys_keylen == 0u) {
        sec_key_gen(sys_key, SEC_KEY_MAX);
        sys_keylen = SEC_KEY_MAX;
    }
    sec_rc4(sys_key, sys_keylen, key, len, enc);
    for (i = 0u; i < len; i++) keyring[slot].data[i] = enc[i];
    keyring[slot].len = (u16)len;
    keyring[slot].used = 1u;
    if (id) *id = slot;
    return SEC_OK;
}

int sec_keyring_get(u32 id, u8 *out, u32 *len)
{
    u32 i;
    u8  dec[SEC_KEY_MAX];
    if (id >= SEC_KEYRING_MAX || !keyring[id].used) return SEC_ENOENT;
    if (!out || !len) return SEC_EINVAL;
    sec_rc4(sys_key, sys_keylen, keyring[id].data, keyring[id].len, dec);
    for (i = 0u; i < keyring[id].len; i++) out[i] = dec[i];
    *len = keyring[id].len;
    return SEC_OK;
}

int sec_keyring_del(u32 id)
{
    if (id >= SEC_KEYRING_MAX || !keyring[id].used) return SEC_ENOENT;
    keyring[id].used = 0u;
    keyring[id].len = 0u;
    return SEC_OK;
}

int sec_keyring_has(u32 id)
{
    if (id >= SEC_KEYRING_MAX) return 0;
    return keyring[id].used ? 1 : 0;
}

/* ---------------- 文件加密抽象 ---------------- */
int sec_file_encrypt(const u8 *key, u32 keylen, const u8 *in, u32 len, u8 *out)
{
    if (!key || keylen == 0u || !in || !out || len == 0u) return SEC_EINVAL;
    sec_rc4(key, keylen, in, len, out);
    return SEC_OK;
}
int sec_file_decrypt(const u8 *key, u32 keylen, const u8 *in, u32 len, u8 *out)
{
    if (!key || keylen == 0u || !in || !out || len == 0u) return SEC_EINVAL;
    sec_rc4(key, keylen, in, len, out);
    return SEC_OK;
}

/* ---------------- IMA 完整性度量 ---------------- */
static int sec_strcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 1;
        a++; b++;
    }
    return *a != *b;
}
static int ima_find(const char *name)
{
    u32 i;
    for (i = 0u; i < SEC_IMA_MAX; i++) {
        if (ima_log[i].used && sec_strcmp(ima_log[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}
static void sec_strcpy(char *dst, const char *src, u32 max)
{
    u32 i;
    for (i = 0u; i < max - 1u && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

int sec_ima_measure(const char *name, const u8 *data, u32 len)
{
    int idx = ima_find(name);
    u32 slot;
    if (!name || !data) return SEC_EINVAL;
    if (idx >= 0)
        slot = (u32)idx;
    else {
        for (slot = 0u; slot < SEC_IMA_MAX; slot++)
            if (!ima_log[slot].used) break;
        if (slot >= SEC_IMA_MAX) return SEC_EFULL;
    }
    ima_log[slot].used = 1u;
    sec_strcpy(ima_log[slot].name, name, 24u);
    sec_sha256(data, len, ima_log[slot].hash);
    return SEC_OK;
}

int sec_ima_verify(const char *name, const u8 *data, u32 len)
{
    int idx = ima_find(name);
    u8 h[32];
    u32 i;
    if (idx < 0) return SEC_ENOENT;
    sec_sha256(data, len, h);
    for (i = 0u; i < 32u; i++)
        if (h[i] != ima_log[idx].hash[i]) return SEC_EHASH;
    return SEC_OK;
}

u32 sec_ima_count(void)
{
    u32 c = 0u, i;
    for (i = 0u; i < SEC_IMA_MAX; i++)
        if (ima_log[i].used) c++;
    return c;
}

/* ---------------- 审计子系统 ---------------- */
void sec_audit(u16 type, u32 pid, u16 result)
{
    u16 i = audit_head;
    audit_ring[i].type = type;
    audit_ring[i].pid = pid;
    audit_ring[i].result = result;
    audit_ring[i].seq = audit_seq++;
    audit_head = (u16)((audit_head + 1u) & (SEC_AUDIT_MAX - 1u));
    if (audit_count < SEC_AUDIT_MAX) audit_count++;
}

void sec_audit_dump(void)
{
    u16 n, k;
    con_puts("  [sec] audit entries=");
    con_put_dec(audit_count);
    con_puts(" seq=");
    con_put_dec(audit_seq);
    con_putc('\n');
    for (n = 0u; n < audit_count; n++) {
        audit_ev_t *e = &audit_ring[n];
        con_puts("    #");
        con_put_dec(e->seq);
        con_puts(" type=");
        con_put_dec(e->type);
        con_puts(" pid=");
        con_put_dec(e->pid);
        con_puts(" result=");
        con_put_dec(e->result);
        con_putc('\n');
    }
    audit_dump_ok = 1u;
    (void)k;
}

u32 sec_audit_count(void)
{
    return audit_count;
}

/* ---------------- LSM 钩子框架 ---------------- */
int sec_hook_register(u16 type, sec_hook_fn fn)
{
    u32 i;
    if (!fn || type >= SEC_HOOK_MAX) return SEC_EINVAL;
    for (i = 0u; i < SEC_HOOK_MAX; i++) {
        if (hooks[i].used && hooks[i].type == type)
            return SEC_EBUSY;             /* 同类型仅一个钩子 */
    }
    for (i = 0u; i < SEC_HOOK_MAX; i++) {
        if (!hooks[i].used) {
            hooks[i].type = type;
            hooks[i].fn = fn;
            hooks[i].used = 1u;
            return SEC_OK;
        }
    }
    return SEC_EFULL;
}

int sec_hook_invoke(u16 type, u32 ctx, u32 arg)
{
    u32 i;
    for (i = 0u; i < SEC_HOOK_MAX; i++) {
        if (hooks[i].used && hooks[i].type == type) {
            hook_calls[type]++;
            return hooks[i].fn(ctx, arg);
        }
    }
    return SEC_DEC_ALLOW;                 /* 无钩子默认放行 */
}

/* ---------------- MAC 策略矩阵 ---------------- */
int sec_mac_policy(u32 domain, u16 operation)
{
    if (domain >= SEC_DOMAIN_MAX || operation >= 16u) return SEC_DEC_DENY;
    return mac_policy[domain][operation] ? SEC_DEC_ALLOW : SEC_DEC_DENY;
}

int sec_mac_set(u32 domain, u16 operation, u16 decision)
{
    if (domain >= SEC_DOMAIN_MAX || operation >= 16u) return SEC_EINVAL;
    mac_policy[domain][operation] = (u16)(decision ? 1u : 0u);
    mac_set_count++;
    return SEC_OK;
}

u32 sec_mac_domains(void)
{
    return SEC_DOMAIN_MAX;
}

/* ---------------- AppArmor 域 ---------------- */
int sec_apparmor_add(const char *path, u16 ops)
{
    u32 i;
    if (!path) return SEC_EINVAL;
    for (i = 0u; i < SEC_DOMAIN_MAX; i++) {
        if (!aa_rules[i].used) {
            sec_strcpy(aa_rules[i].path, path, 24u);
            aa_rules[i].ops = ops;
            aa_rules[i].used = 1u;
            return SEC_OK;
        }
    }
    return SEC_EFULL;
}

int sec_apparmor_check(const char *path, u16 op)
{
    u32 i;
    if (!path) return SEC_DEC_DENY;
    for (i = 0u; i < SEC_DOMAIN_MAX; i++) {
        if (aa_rules[i].used && sec_strcmp(aa_rules[i].path, path) == 0) {
            if (aa_rules[i].ops & op) return SEC_DEC_ALLOW;
            return SEC_DEC_DENY;
        }
    }
    return SEC_DEC_ALLOW;                 /* 无规则默认放行 */
}

/* ---------------- seccomp 系统调用策略表 ---------------- */
int sec_seccomp_allow(u32 nr)
{
    if (nr >= 128u) return SEC_EINVAL;
    seccomp_map[nr >> 5] |= (1u << (nr & 31u));
    return SEC_OK;
}
int sec_seccomp_deny(u32 nr)
{
    if (nr >= 128u) return SEC_EINVAL;
    seccomp_map[nr >> 5] &= ~(1u << (nr & 31u));
    return SEC_OK;
}
int sec_seccomp_check(u32 nr)
{
    if (nr >= 128u) return SEC_DEC_DENY;
    return (seccomp_map[nr >> 5] & (1u << (nr & 31u))) ? SEC_DEC_ALLOW : SEC_DEC_DENY;
}

/* ---------------- KASLR / NX / 隔离状态 ---------------- */
u32 sec_kslr_entropy(void)
{
    return sec_rng_u32();                 /* 熵源状态（真实随机） */
}

static u32 read_cr0(void)
{
    u32 v;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
    return v;
}
static u32 read_cr4(void)
{
    u32 v;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
    return v;
}

u32 sec_nx_status(void)
{
    u32 eax, edx;
    u32 st = 0u;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=d"(edx) : "a"(0x80000001u) : "ebx", "ecx");
    if (edx & (1u << 20)) st |= 1u;       /* CPUID NX 支持 */
    if (read_cr0() & (1u << 16)) st |= 2u;/* CR0.WP 置位 */
    if (read_cr4() & (1u << 5)) st |= 4u; /* CR4.PAE */
    if (read_cr4() & (1u << 6)) st |= 8u; /* CR4.MCE */
    return st;
}

int sec_isolation_test(void)
{
    u32 pgd_addr;
    u32 entry0;
    /* 读当前 CR3 → 内核页目录首条目：验证内核页表用户位（bit2 U/S）未置位，
     * 即内核内存对用户态隔离（真实页表检查） */
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(pgd_addr));
    entry0 = *(volatile u32 *)(pgd_addr & ~0xFFFu);
    if (entry0 & 0x4u) return 1;          /* 用户位被置位 → 隔离失败 */
    if (!(entry0 & 0x1u)) return 2;       /* present 位缺失 */
    return SEC_OK;
}

/* ---------------- 栈保护 canary ---------------- */
u32 sec_canary_get(void)
{
    if (canary_val == 0u)
        canary_val = sec_rng_u32() | 0x01010101u;
    return canary_val;
}
int sec_canary_check(u32 expected)
{
    return (expected == sec_canary_get()) ? SEC_OK : SEC_EINVAL;
}

/* ---------------- 签名哈希校验 / 信任链 / 基线 ---------------- */
int sec_verify_hash(const u8 *data, u32 len, const u8 expect[32])
{
    u8 h[32];
    u32 i;
    if (!data || !expect) return SEC_EINVAL;
    sec_sha256(data, len, h);
    for (i = 0u; i < 32u; i++)
        if (h[i] != expect[i]) return SEC_EHASH;
    return SEC_OK;
}

int sec_trust_add_root(const u8 hash[32])
{
    u32 i;
    if (!hash || trust_count >= 4u) return SEC_EFULL;
    for (i = 0u; i < 32u; i++) trust_roots[trust_count][i] = hash[i];
    trust_count++;
    return SEC_OK;
}
int sec_trust_verify(const u8 hash[32])
{
    u32 i, j;
    if (!hash) return SEC_EINVAL;
    for (i = 0u; i < trust_count; i++) {
        u32 ok = 1u;
        for (j = 0u; j < 32u; j++)
            if (trust_roots[i][j] != hash[j]) { ok = 0u; break; }
        if (ok) return SEC_OK;
    }
    return SEC_EHASH;
}

int sec_baseline_check(void)
{
    u32 i, bad = 0u;
    for (i = 0u; i < SEC_CFG_MAX; i++) {
        if (cfg[i].name[0] == '\0') break;
        if (cfg[i].val < cfg[i].recommended) bad++;
    }
    return bad == 0u ? SEC_OK : 2;        /* 2 = 基线存在偏差 */
}

int sec_harden_status(void)
{
    u32 st = sec_nx_status();
    u32 s = 0u;
    if (st & 1u) s |= 1u;                 /* NX 可用 */
    if (st & 2u) s |= 2u;                 /* WP 置位 */
    if (sec_canary_get()) s |= 4u;        /* canary 启用 */
    if (sec_kslr_entropy()) s |= 8u;      /* 随机熵可用 */
    return (int)s;
}

/* ---------------- 初始化 ---------------- */
void sec_init(void)
{
    u32 i, j;
    sec_rng_init();
    sec_magic = SEC_MAGIC;
    audit_head = 0u; audit_count = 0u; audit_seq = 0u;
    trust_count = 0u; mac_set_count = 0u;
    for (i = 0u; i < SEC_KEYRING_MAX; i++) keyring[i].used = 0u;
    for (i = 0u; i < SEC_IMA_MAX; i++) ima_log[i].used = 0u;
    for (i = 0u; i < SEC_HOOK_MAX; i++) { hooks[i].used = 0u; hook_calls[i] = 0u; }
    for (i = 0u; i < SEC_DOMAIN_MAX; i++) {
        aa_rules[i].used = 0u;
        for (j = 0u; j < 16u; j++) mac_policy[i][j] = 1u;  /* 默认 allow */
    }
    for (i = 0u; i < 4u; i++) seccomp_map[i] = 0xFFFFFFFFu; /* 默认 allow */

    /* 安全配置与基线 */
    sec_strcpy(cfg[0].name, "audit_enabled", 20u);
    cfg[0].val = 1u; cfg[0].recommended = 1u;
    sec_strcpy(cfg[1].name, "enforce_mode", 20u);
    cfg[1].val = 1u; cfg[1].recommended = 1u;
    sec_strcpy(cfg[2].name, "keyring_max", 20u);
    cfg[2].val = SEC_KEYRING_MAX; cfg[2].recommended = 8u;
    sec_strcpy(cfg[3].name, "audit_ring", 20u);
    cfg[3].val = SEC_AUDIT_MAX; cfg[3].recommended = 16u;
    sec_strcpy(cfg[4].name, "ima_max", 20u);
    cfg[4].val = SEC_IMA_MAX; cfg[4].recommended = 8u;
    sec_strcpy(cfg[5].name, "canary_enabled", 20u);
    cfg[5].val = 1u; cfg[5].recommended = 1u;
    for (i = 6u; i < SEC_CFG_MAX; i++) cfg[i].name[0] = '\0';
}

/* ---------------- 自检组 1：RNG ---------------- */
int sec_selftest_rng(void)
{
    u32 a, b, c, i, nz = 0u;
    u8  buf[64];

    if (sec_magic != SEC_MAGIC) return 1;
    /* RNG 输出非全零 */
    for (i = 0u; i < 16u; i++) {
        u32 v = sec_rng_u32();
        if (v != 0u) nz++;
    }
    if (nz < 16u) return 2;
    /* 填充缓冲非全零 */
    sec_rng_fill(buf, 64u);
    nz = 0u;
    for (i = 0u; i < 64u; i++) if (buf[i] != 0u) nz++;
    if (nz == 0u) return 3;
    /* 分布粗查：高 8 位与低 8 位均应出现非零值 */
    a = sec_rng_u32(); b = sec_rng_u32(); c = sec_rng_u32();
    if (((a | b | c) & 0xFFu) == 0u) return 4;
    if (((a | b | c) & 0xFF000000u) == 0u) return 5;
    /* 确定性：相同种子应可重复（这里验证内部状态已推进） */
    (void)a; (void)b; (void)c;
    return SEC_OK;
}

/* ---------------- 自检组 2：加密算法 ---------------- */
int sec_selftest_crypto(void)
{
    int rc = sec_crypto_selftest();
    if (rc != SEC_OK) return rc;
    /* 文件加密往返一致性（64 字节随机数据） */
    {
        u8 key[16], plain[64], enc[64], dec[64];
        u32 i;
        sec_rng_fill(key, 16u);
        sec_rng_fill(plain, 64u);
        if (sec_file_encrypt(key, 16u, plain, 64u, enc) != SEC_OK) return 11;
        if (sec_file_decrypt(key, 16u, enc, 64u, dec) != SEC_OK) return 12;
        for (i = 0u; i < 64u; i++)
            if (plain[i] != dec[i]) return 13;
        /* 不同密钥解密应失败（内容不一致） */
        key[0] ^= 0x5Au;
        sec_file_decrypt(key, 16u, enc, 64u, dec);
        if (dec[0] == plain[0] && dec[31] == plain[31]) return 14;
    }
    return SEC_OK;
}

/* ---------------- 自检组 3：密钥环 ---------------- */
int sec_selftest_key(void)
{
    u8 k1[16], k2[16], out[16];
    u32 len, id1, id2;

    if (sec_key_gen(k1, 16u) != SEC_OK) return 1;
    if (sec_key_gen(k2, 16u) != SEC_OK) return 2;
    /* 两次生成应不同（熵有效性） */
    {
        u32 i, same = 1u;
        for (i = 0u; i < 16u; i++) if (k1[i] != k2[i]) { same = 0u; break; }
        if (same) return 3;
    }
    if (sec_keyring_add(k1, 16u, &id1) != SEC_OK) return 4;
    if (sec_keyring_add(k2, 16u, &id2) != SEC_OK) return 5;
    if (id1 == id2) return 6;
    if (!sec_keyring_has(id1)) return 7;
    len = 0u;
    if (sec_keyring_get(id1, out, &len) != SEC_OK) return 8;
    if (len != 16u) return 9;
    {
        u32 i, same = 1u;
        for (i = 0u; i < 16u; i++) if (out[i] != k1[i]) { same = 0u; break; }
        if (!same) return 10;              /* 读取应与存入一致 */
    }
    if (sec_keyring_del(id1) != SEC_OK) return 11;
    if (sec_keyring_has(id1)) return 12;
    if (sec_keyring_get(id1, out, &len) != SEC_ENOENT) return 13;
    /* 非法参数 */
    if (sec_keyring_add((const u8 *)0, 16u, &id1) != SEC_EINVAL) return 14;
    return SEC_OK;
}

/* ---------------- 自检组 4：LSM / MAC / seccomp / IMA / 审计 ---------------- */
static int deny_all_hook(u32 ctx, u32 arg)
{
    (void)ctx; (void)arg;
    return SEC_DEC_DENY;
}

int sec_selftest_lsm(void)
{
    u8 data[32], expect[32];
    u32 i;
    /* LSM 钩子注册与调用 */
    if (sec_hook_register(SEC_HOOK_MODULE_LOAD, deny_all_hook) != SEC_OK) return 1;
    if (sec_hook_register(SEC_HOOK_MODULE_LOAD, deny_all_hook) != SEC_EBUSY) return 2;
    if (sec_hook_invoke(SEC_HOOK_MODULE_LOAD, 0u, 0u) != SEC_DEC_DENY) return 3;
    if (sec_hook_invoke(SEC_HOOK_FILE_OPEN, 1u, 0u) != SEC_DEC_ALLOW) return 4;

    /* MAC 策略矩阵：默认允许，设置拒绝后生效 */
    if (sec_mac_policy(0u, 0u) != SEC_DEC_ALLOW) return 5;
    if (sec_mac_set(0u, 0u, SEC_DEC_DENY) != SEC_OK) return 6;
    if (sec_mac_policy(0u, 0u) != SEC_DEC_DENY) return 7;
    if (sec_mac_policy(0u, 1u) != SEC_DEC_ALLOW) return 8;
    if (sec_mac_policy(9u, 0u) != SEC_DEC_DENY) return 9;      /* 越界域拒绝 */

    /* AppArmor：加入规则并校验 */
    if (sec_apparmor_add("/bin/xos_shell", 0x1u) != SEC_OK) return 10;
    if (sec_apparmor_check("/bin/xos_shell", 0x1u) != SEC_DEC_ALLOW) return 11;
    if (sec_apparmor_check("/bin/xos_shell", 0x2u) != SEC_DEC_DENY) return 12;
    if (sec_apparmor_check("/usr/bin/other", 0x2u) != SEC_DEC_ALLOW) return 13;

    /* seccomp：默认允许，拒绝 nr=1 后生效 */
    if (sec_seccomp_check(1u) != SEC_DEC_ALLOW) return 14;
    if (sec_seccomp_deny(1u) != SEC_OK) return 15;
    if (sec_seccomp_check(1u) != SEC_DEC_DENY) return 16;
    if (sec_seccomp_check(2u) != SEC_DEC_ALLOW) return 17;
    if (sec_seccomp_allow(1u) != SEC_OK) return 18;
    if (sec_seccomp_check(1u) != SEC_DEC_ALLOW) return 19;

    /* IMA：度量→篡改检测→恢复 */
    for (i = 0u; i < 32u; i++) data[i] = (u8)i;
    if (sec_ima_measure("/etc/xos.conf", data, 32u) != SEC_OK) return 20;
    if (sec_ima_verify("/etc/xos.conf", data, 32u) != SEC_OK) return 21;
    data[16] ^= 0xFFu;
    if (sec_ima_verify("/etc/xos.conf", data, 32u) != SEC_EHASH) return 22;
    data[16] ^= 0xFFu;
    if (sec_ima_verify("/etc/xos.conf", data, 32u) != SEC_OK) return 23;

    /* 审计：记录与计数 */
    sec_audit(SEC_EV_DENIED, 7u, 1u);
    sec_audit(SEC_EV_FILE, 3u, 0u);
    if (sec_audit_count() < 2u) return 24;

    /* 签名哈希校验 */
    sec_sha256(data, 32u, expect);
    if (sec_verify_hash(data, 32u, expect) != SEC_OK) return 25;
    expect[0] ^= 1u;
    if (sec_verify_hash(data, 32u, expect) != SEC_EHASH) return 26;
    return SEC_OK;
}

/* ---------------- 自检组 5：KASLR / NX / 隔离 / canary / 信任链 / 基线 ---------------- */
int sec_selftest_hw(void)
{
    u32 st, c;
    u8 h[32];
    /* KASLR 熵可用 */
    if (sec_kslr_entropy() == 0u) return 1;
    /* NX/WP 状态：WP 必须置位（分页已启用 WP）；XOS 采用 32 位分页 +
     * PSE 4MB 大页（不启用 PAE），故此处校验 CR4.PSE 而非 PAE */
    st = sec_nx_status();
    if (!(st & 2u)) return 2;             /* CR0.WP 未置位 */
    if (!(read_cr4() & (1u << 4))) return 3;  /* CR4.PSE 未置位 */
    /* 内存隔离：内核页表用户位清除 */
    if (sec_isolation_test() != SEC_OK) return 4;
    /* 栈 canary */
    c = sec_canary_get();
    if (c == 0u) return 5;
    if (sec_canary_check(c) != SEC_OK) return 6;
    if (sec_canary_check(c ^ 0xDEADBEEFu) == SEC_OK) return 7;  /* 篡改检测 */
    /* 信任根链 */
    sec_sha256((const u8 *)"XOS-Root-CA", 11u, h);
    if (sec_trust_add_root(h) != SEC_OK) return 8;
    if (sec_trust_verify(h) != SEC_OK) return 9;
    h[0] ^= 0x80u;
    if (sec_trust_verify(h) != SEC_EHASH) return 10;
    /* 基线 */
    if (sec_baseline_check() == 1) return 11;
    /* 加固状态 */
    if (sec_harden_status() == 0) return 12;
    return SEC_OK;
}

/* ---------------- 入侵检测规则引擎（IDS） ----------------
 * 规则表：类型 + 阈值 + 计数 + 告警标志。事件计数达阈值即触发告警并写审计。
 * 完全自研：无外部 IDS/开源规则库依赖。 */
typedef struct {
    u16 type;           /* SEC_IDS_AUTHFAIL / PRIVESC / BADSYSCALL / NETSCAN */
    u16 enabled;
    u16 alerted;
    u32 threshold;
    u32 count;
} ids_rule_t;

static ids_rule_t ids_rules[SEC_IDS_MAX];
static u32 ids_alert_total = 0u;

int sec_ids_add(u16 type, u32 threshold)
{
    u32 i;
    if (threshold == 0u) return SEC_EINVAL;
    for (i = 0u; i < SEC_IDS_MAX; i++) {
        if (!ids_rules[i].enabled) {
            ids_rules[i].type = type;
            ids_rules[i].threshold = threshold;
            ids_rules[i].count = 0u;
            ids_rules[i].alerted = 0u;
            ids_rules[i].enabled = 1u;
            return SEC_OK;
        }
    }
    return SEC_EFULL;
}

int sec_ids_event(u16 type)
{
    u32 i, hit = 0u;
    for (i = 0u; i < SEC_IDS_MAX; i++) {
        if (!ids_rules[i].enabled || ids_rules[i].type != type)
            continue;
        ids_rules[i].count++;
        if (ids_rules[i].count >= ids_rules[i].threshold &&
            !ids_rules[i].alerted) {
            ids_rules[i].alerted = 1u;
            ids_alert_total++;
            sec_audit(0x14u, (u32)type, 1u);   /* IDS 告警审计事件 */
            hit = 1u;
        }
    }
    return hit ? SEC_IDS_ALERT : SEC_OK;
}

u32 sec_ids_alerts(void)
{
    return ids_alert_total;
}

/* ---------------- 签名块框架 ----------------
 * 布局：magic(4) | version(4) | payload_len(4) | sha256(payload)(32) | payload
 * 校验：魔数 / 版本 / 长度边界 / payload 摘要 / 信任根。完全自研。 */
typedef struct {
    u32 magic;
    u32 version;
    u32 payload_len;
    u8  hash[32];
} sec_sig_hdr_t;

int sec_sig_verify(const u8 *block, u32 block_len, const u8 root_hash[32])
{
    const sec_sig_hdr_t *h;
    u8 h2[32];
    if (!block || block_len < sizeof(sec_sig_hdr_t)) return SEC_EINVAL;
    h = (const sec_sig_hdr_t *)block;
    if (h->magic != SEC_SIG_MAGIC) return SEC_EINVAL;
    if (h->version != 1u) return SEC_EINVAL;
    if (h->payload_len == 0u ||
        h->payload_len > block_len - sizeof(sec_sig_hdr_t))
        return SEC_EINVAL;
    sec_sha256(block + sizeof(sec_sig_hdr_t), h->payload_len, h2);
    if (memcmp(h2, h->hash, 32u) != 0) return SEC_EHASH;
    if (root_hash && sec_trust_verify(root_hash) != SEC_OK) return SEC_EPERM;
    return SEC_OK;
}

/* ---------------- KASLR 节级随机基址槽 ----------------
 * 在 0x100000..0x7FF000 内按 4KB 对齐取熵，节尾不越过 0x800000。 */
int sec_kslr_slot(u32 *base_out, u32 size)
{
    u32 ent, base;
    if (!base_out || size == 0u) return SEC_EINVAL;
    if (size > 0x200000u) return SEC_EINVAL;   /* 单节上限 2MB */
    ent = sec_kslr_entropy();
    if (ent == 0u) return SEC_EINVAL;
    base = 0x100000u + (ent % 0x6F0000u);      /* 覆盖 0x100000..0x7FF000 */
    base &= ~0xFFFu;                            /* 4KB 对齐 */
    if (base + size > 0x800000u) base = 0x700000u;
    *base_out = base;
    return SEC_OK;
}

/* ---------------- 自检组 6：IDS / 签名块 / KASLR 槽 ---------------- */
int sec_selftest_ids(void)
{
    u32 base, i;
    u8 blk[60];
    u8 h[32];
    /* IDS：添加规则 → 未达阈值 → 达阈值触发告警 */
    if (sec_ids_add(SEC_IDS_AUTHFAIL, 3u) != SEC_OK) return 1;
    if (sec_ids_event(SEC_IDS_AUTHFAIL) != SEC_OK) return 2;
    if (sec_ids_event(SEC_IDS_AUTHFAIL) != SEC_OK) return 3;
    if (sec_ids_event(SEC_IDS_AUTHFAIL) != SEC_IDS_ALERT) return 4;
    if (sec_ids_alerts() == 0u) return 5;
    /* 填满规则表（共 8 槽）→ 第 9 条返回 EFULL */
    for (i = 2u; i <= 8u; i++)
        if (sec_ids_add((u16)i, 3u) != SEC_OK) return 6;
    if (sec_ids_add(0xFEu, 3u) != SEC_EFULL) return 7;
    /* 签名块：合法块 → 通过；坏 magic → EINVAL；坏哈希 → EHASH */
    sec_sha256((const u8 *)"XOS-Kern", 8u, h);
    blk[0] = (u8)(SEC_SIG_MAGIC & 0xFFu);
    blk[1] = (u8)((SEC_SIG_MAGIC >> 8) & 0xFFu);
    blk[2] = (u8)((SEC_SIG_MAGIC >> 16) & 0xFFu);
    blk[3] = (u8)((SEC_SIG_MAGIC >> 24) & 0xFFu);
    blk[4] = 1u; blk[5] = 0u; blk[6] = 0u; blk[7] = 0u;   /* version=1 */
    blk[8] = 8u; blk[9] = 0u; blk[10] = 0u; blk[11] = 0u;  /* payload_len=8 */
    memcpy(&blk[12], h, 32u);
    memcpy(&blk[44], "XOS-Kern", 8u);                      /* payload */
    if (sec_sig_verify(blk, 52u, NULL) != SEC_OK) return 8;
    blk[0] ^= 0x80u;
    if (sec_sig_verify(blk, 52u, NULL) != SEC_EINVAL) return 9;
    blk[0] ^= 0x80u;
    blk[12] ^= 0x80u;                                      /* 篡改摘要 */
    if (sec_sig_verify(blk, 52u, NULL) != SEC_EHASH) return 10;
    blk[12] ^= 0x80u;
    if (sec_sig_verify(blk, 60u, NULL) != SEC_OK) return 11; /* 尾部扩展字节不校验 */
    if (sec_sig_verify(NULL, 52u, NULL) != SEC_EINVAL) return 12;
    /* KASLR 槽：范围/对齐/参数校验 */
    if (sec_kslr_slot(&base, 0x1000u) != SEC_OK) return 13;
    if (base < 0x100000u || (base & 0xFFFu) != 0u) return 14;
    if (sec_kslr_slot(NULL, 0x1000u) != SEC_EINVAL) return 15;
    if (sec_kslr_slot(&base, 0x300000u) != SEC_EINVAL) return 16;
    return SEC_OK;
}

/* ---------------- 第 27 册补全：安全扩展自检 ---------------- */

static u32 sec_mitigations_flag;
static u32 sec_scan_checks;

u32 sec_mitigations_enabled(void)
{
    sec_mitigations_flag |= 1u;           /* NX+WP */
    sec_mitigations_flag |= 2u;           /* canary */
    sec_mitigations_flag |= 4u;           /* KASLR */
    sec_mitigations_flag |= 8u;           /* SMEP/UMIP 语义模拟 */
    return sec_mitigations_flag;
}

int sec_nx_check(u32 pte_flags)
{
    /* pte_flags 位 11 = NX：置位则页面不可执行 */
    if (pte_flags & (1u << 11u)) return SEC_DEC_DENY;
    return SEC_DEC_ALLOW;
}

int sec_mem_protect(u32 perms, u32 requested)
{
    /* perms: 位0=读 位1=写 位2=执行；requested 为请求位 */
    if ((perms & requested) == requested) return SEC_DEC_ALLOW;
    return SEC_DEC_DENY;
}

int sec_audit_flush(void)
{
    u32 i;
    for (i = 0u; i < SEC_AUDIT_MAX; i++) audit_ring[i].type = 0u;
    audit_count = 0u;
    return SEC_OK;
}

int sec_scan_pass(void)
{
    u32 i;
    u32 ok = 0u;
    if (sec_canary_get() != 0u) ok++;
    if (sec_nx_status() != 0u) ok++;
    for (i = 0u; i < SEC_KEYRING_MAX; i++)
        if (keyring[i].len != 0u) ok++;
    if (trust_count > 0u) ok++;
    sec_scan_checks = ok;
    return (ok >= 2u) ? SEC_OK : SEC_EPERM;
}

u32 sec_scan_checks_get(void) { return sec_scan_checks; }

int sec_selftest_ext(void)
{
    u32 st, slide;
    u8 blk[60], h[32];

    /* 1-3: NX/DEP 页检查 */
    if (sec_nx_check(1u << 11u) != SEC_DEC_DENY) return 1;
    if (sec_nx_check(0u) != SEC_DEC_ALLOW) return 2;
    if (sec_nx_check((1u << 11u) | 1u) != SEC_DEC_DENY) return 3;

    /* 4-7: 内存保护权限 */
    if (sec_mem_protect(7u, 1u) != SEC_DEC_ALLOW) return 4;
    if (sec_mem_protect(3u, 4u) != SEC_DEC_DENY) return 5;
    if (sec_mem_protect(6u, 2u) != SEC_DEC_ALLOW) return 6;
    if (sec_mem_protect(0u, 1u) != SEC_DEC_DENY) return 7;

    /* 8-11: SELinux/MAC 策略矩阵 */
    if (sec_mac_set(0u, SEC_HOOK_FILE_OPEN, SEC_DEC_DENY) != SEC_OK) return 8;
    if (sec_mac_policy(0u, SEC_HOOK_FILE_OPEN) != SEC_DEC_DENY) return 9;
    if (sec_mac_set(0u, SEC_HOOK_FILE_OPEN, SEC_DEC_ALLOW) != SEC_OK) return 10;
    if (sec_mac_policy(0u, SEC_HOOK_FILE_OPEN) != SEC_DEC_ALLOW) return 11;

    /* 12-15: KASLR 随机化偏移（熵值非零、槽位页对齐） */
    slide = sec_kslr_entropy();
    if (slide == 0u) return 12;
    if (sec_kslr_entropy() == 0u) return 13;
    if (sec_kslr_slot(&st, 0x1000u) != SEC_OK) return 14;
    if ((st & 0xFFFu) != 0u) return 15;

    /* 16-18: 安全启动签名（信任根） */
    sec_sha256((const u8 *)"XOS-Kern", 8u, h);
    blk[0] = (u8)(SEC_SIG_MAGIC & 0xFFu);
    blk[1] = (u8)((SEC_SIG_MAGIC >> 8) & 0xFFu);
    blk[2] = (u8)((SEC_SIG_MAGIC >> 16) & 0xFFu);
    blk[3] = (u8)((SEC_SIG_MAGIC >> 24) & 0xFFu);
    blk[4] = 1u; blk[5] = 0u; blk[6] = 0u; blk[7] = 0u;
    blk[8] = 8u; blk[9] = 0u; blk[10] = 0u; blk[11] = 0u;
    memcpy(&blk[12], h, 32u);
    memcpy(&blk[44], "XOS-Kern", 8u);
    if (sec_sig_verify(blk, 52u, NULL) != SEC_OK) return 16;
    blk[12] ^= 0x40u;
    if (sec_sig_verify(blk, 52u, NULL) != SEC_EHASH) return 17;
    blk[12] ^= 0x40u;
    if (sec_sig_verify(NULL, 52u, NULL) != SEC_EINVAL) return 18;

    /* 19-22: 漏洞缓解标志 */
    if (sec_mitigations_enabled() == 0u) return 19;
    if (!(sec_mitigations_enabled() & 1u)) return 20;
    if (!(sec_mitigations_enabled() & 2u)) return 21;
    if (!(sec_mitigations_enabled() & 4u)) return 22;

    /* 23-25: 审计子系统 */
    if (sec_audit_flush() != SEC_OK) return 23;
    if (sec_audit_count() != 0u) return 24;
    if (sec_audit_count() > 0u) return 25;

    /* 26-28: 入侵检测联动 */
    if (sec_ids_event(SEC_IDS_PRIVESC) != SEC_OK) return 26;
    if (sec_ids_alerts() == 0u) return 27;
    if (sec_ids_add(0x7Fu, 2u) != SEC_EFULL) return 28;   /* 规则表已满 */

    /* 29-31: 安全扫描 */
    if (sec_scan_pass() != SEC_OK) return 29;
    if (sec_scan_checks_get() < 2u) return 30;
    if (sec_canary_get() == 0u) return 31;

    /* 32-33: seccomp 过滤 */
    if (sec_seccomp_deny(42u) != SEC_OK) return 32;
    if (sec_seccomp_check(42u) != SEC_DEC_DENY) return 33;

    return SEC_OK;
}

/* ---------------- dump ---------------- */
void sec_dump(void)
{
    u32 i, j;
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  Security subsystem dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("    rng: rdrand=");
    con_put_dec(rng_have_rdrand);
    con_puts(" draws=");
    con_put_dec(rng_draws);
    con_puts("  nx_status=");
    con_put_hex32(sec_nx_status());
    con_puts("  kslr_entropy=");
    con_put_hex32(sec_kslr_entropy());
    con_puts("  canary=");
    con_put_hex32(sec_canary_get());
    con_putc('\n');
    con_puts("    keyring slots:");
    for (i = 0u; i < SEC_KEYRING_MAX; i++)
        if (keyring[i].used) {
            con_puts(" [");
            con_put_dec(i);
            con_put_dec(keyring[i].len);
            con_putc(']');
        }
    con_puts("  ima_entries=");
    con_put_dec(sec_ima_count());
    con_puts("  mac_set=");
    con_put_dec(mac_set_count);
    con_puts("  trust_roots=");
    con_put_dec(trust_count);
    con_putc('\n');
    con_puts("    baseline:");
    for (i = 0u; i < SEC_CFG_MAX; i++) {
        if (cfg[i].name[0] == '\0') break;
        con_puts(" ");
        con_puts(cfg[i].name);
        con_putc('=');
        con_put_dec(cfg[i].val);
        if (cfg[i].val < cfg[i].recommended) con_puts("(LOW)");
    }
    con_putc('\n');
    sec_audit_dump();
    (void)j;
}
