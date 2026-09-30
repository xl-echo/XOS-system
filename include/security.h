/* ============================================================================
 * XOS 安全机制子系统核心头文件（第 27 册 · 安全机制）
 * 完全自研：随机数生成器（RDRAND 探测 + 混合熵）、SHA-256 / RC4 / CRC32
 * 加密算法库、密钥环、文件加密抽象、完整性度量 IMA、审计子系统、
 * LSM 安全钩子框架、MAC 策略矩阵（SELinux 语义简化）、AppArmor 域、
 * seccomp 系统调用策略表、KASLR 状态、NX/页表隔离验证、栈 canary、
 * 签名哈希校验、信任根链、安全基线与加固配置、自检。
 * 不依赖任何外部安全核心/闭源方案。
 * ========================================================================== */
#ifndef XOS_SECURITY_H
#define XOS_SECURITY_H

#include "types.h"

#define SEC_MAGIC        0x58534543u    /* "XSEC" */
#define SEC_KEYRING_MAX  8              /* 密钥环槽位数 */
#define SEC_KEY_MAX      32             /* 密钥最大字节 */
#define SEC_AUDIT_MAX    16             /* 审计环形事件数 */
#define SEC_IMA_MAX      8              /* 完整性度量日志条数 */
#define SEC_HOOK_MAX     8              /* LSM 钩子表项数 */
#define SEC_DOMAIN_MAX   4              /* MAC 域数量 */
#define SEC_CFG_MAX      12             /* 安全配置项数 */

/* 安全钩子类型 */
#define SEC_HOOK_FILE_OPEN  0u
#define SEC_HOOK_TASK_CREATE 1u
#define SEC_HOOK_MODULE_LOAD 2u
#define SEC_HOOK_MEM_MAP    3u
#define SEC_HOOK_NET_SEND   4u

/* MAC 决策 */
#define SEC_DEC_ALLOW   1
#define SEC_DEC_DENY    0

/* 审计事件类型 */
#define SEC_EV_FILE      1u
#define SEC_EV_TASK      2u
#define SEC_EV_NET       3u
#define SEC_EV_CRYPTO    4u
#define SEC_EV_MODULE    5u
#define SEC_EV_DENIED    6u

/* 错误码 */
#define SEC_OK           0
#define SEC_EINVAL       (-1)
#define SEC_EFULL        (-2)
#define SEC_ENOENT       (-3)
#define SEC_EBADKEY      (-4)
#define SEC_EPERM        (-5)
#define SEC_EBUSY        (-6)
#define SEC_EHASH        (-7)
#define SEC_IDS_ALERT    1              /* 入侵检测规则触发（区别于错误码） */
#define SEC_IDS_MAX      8              /* 入侵检测规则表项数 */
#define SEC_SIG_MAGIC    0x584F5349u    /* 'XOSI'：签名块魔数 */

/* 入侵检测规则类型 */
#define SEC_IDS_AUTHFAIL  1u            /* 认证失败 */
#define SEC_IDS_PRIVESC   2u            /* 越权访问 */
#define SEC_IDS_BADSYSCALL 3u           /* 异常系统调用序列 */
#define SEC_IDS_NETSCAN   4u            /* 非法网络访问 */

/* ===== 随机数生成器 ===== */
int      sec_rng_init(void);              /* 探测 RDRAND + 熵混合 */
u32      sec_rng_u32(void);               /* 输出 32 位随机数 */
void     sec_rng_fill(u8 *buf, u32 n);

/* ===== 加密算法库（标准向量可验证） ===== */
u32      sec_sha256(const u8 *msg, u32 len, u8 out[32]);
void     sec_rc4(const u8 *key, u32 keylen, const u8 *in, u32 len, u8 *out);
u32      sec_crc32(const u8 *buf, u32 len, u32 seed);
int      sec_crypto_selftest(void);       /* SHA-256/RC4/CRC32 标准向量 */

/* ===== 密钥环 ===== */
int      sec_key_gen(u8 *key, u32 len);   /* 用 RNG 生成密钥 */
int      sec_keyring_add(const u8 *key, u32 len, u32 *id);
int      sec_keyring_get(u32 id, u8 *out, u32 *len);     /* 仅导出加密态 */
int      sec_keyring_del(u32 id);
int      sec_keyring_has(u32 id);

/* ===== 文件加密抽象 ===== */
int      sec_file_encrypt(const u8 *key, u32 keylen,
                          const u8 *in, u32 len, u8 *out);
int      sec_file_decrypt(const u8 *key, u32 keylen,
                          const u8 *in, u32 len, u8 *out);

/* ===== 完整性度量 IMA ===== */
int      sec_ima_measure(const char *name, const u8 *data, u32 len);
int      sec_ima_verify(const char *name, const u8 *data, u32 len);
u32      sec_ima_count(void);

/* ===== 审计子系统 ===== */
void     sec_audit(u16 type, u32 pid, u16 result);
void     sec_audit_dump(void);
u32      sec_audit_count(void);

/* ===== LSM 钩子框架 ===== */
typedef int (*sec_hook_fn)(u32 ctx, u32 arg);
int      sec_hook_register(u16 type, sec_hook_fn fn);
int      sec_hook_invoke(u16 type, u32 ctx, u32 arg);

/* ===== MAC 策略矩阵（SELinux 语义简化：域 × 操作） ===== */
int      sec_mac_policy(u32 domain, u16 operation);
int      sec_mac_set(u32 domain, u16 operation, u16 decision);
u32      sec_mac_domains(void);

/* ===== AppArmor 域（程序路径 → 允许操作集） ===== */
int      sec_apparmor_add(const char *path, u16 ops);
int      sec_apparmor_check(const char *path, u16 op);

/* ===== seccomp 系统调用策略表 ===== */
int      sec_seccomp_allow(u32 nr);
int      sec_seccomp_deny(u32 nr);
int      sec_seccomp_check(u32 nr);

/* ===== KASLR / NX / 内存隔离状态 ===== */
u32      sec_kslr_entropy(void);          /* 当前 ASLR 熵 */
u32      sec_nx_status(void);             /* CR0.WP / CR4.PAE / NX 能力 */
int      sec_isolation_test(void);        /* 页表写保护拒绝验证 */
int      sec_kslr_slot(u32 *base_out, u32 size);  /* KASLR 节级随机基址槽 */

/* ===== 入侵检测与防护（IDS 规则引擎） ===== */
int      sec_ids_add(u16 type, u32 threshold);    /* 添加规则（类型+阈值） */
int      sec_ids_event(u16 type);                 /* 事件计数；达阈值返回 SEC_IDS_ALERT */
u32      sec_ids_alerts(void);                    /* 累计告警数 */

/* ===== 签名块框架（magic+version+len+SHA256+信任根） ===== */
int      sec_sig_verify(const u8 *block, u32 block_len, const u8 root_hash[32]);

/* ===== 栈保护 canary ===== */
u32      sec_canary_get(void);
int      sec_canary_check(u32 expected);  /* 篡改检测 */

/* ===== 签名哈希校验 / 信任链 / 基线 ===== */
int      sec_verify_hash(const u8 *data, u32 len, const u8 expect[32]);
int      sec_trust_add_root(const u8 hash[32]);
int      sec_trust_verify(const u8 hash[32]);
int      sec_baseline_check(void);
int      sec_harden_status(void);

/* ===== 子系统初始化和自检 ===== */
void     sec_init(void);
int      sec_selftest_rng(void);
int      sec_selftest_crypto(void);
int      sec_selftest_key(void);
int      sec_selftest_lsm(void);
int      sec_selftest_hw(void);
int      sec_selftest_ids(void);
int      sec_selftest_ext(void);   /* 第 27 册补全：SELinux/NX/内存保护/缓解/安全启动/审计/扫描 */         /* IDS / 签名块 / KASLR 槽 */
void     sec_dump(void);

#endif /* XOS_SECURITY_H */
