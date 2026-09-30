/* ============================================================================
 * XOS 存储驱动子系统（第 15 册 · 设备驱动 · 存储）
 * 自研实现，不依赖外部核心：
 *   - ATA/PATA PIO 驱动：0x1F0/0x3F6(主盘)、0x170/0x376(从盘)、IDENTIFY、
 *     LBA28 寻址、轮询式 PIO 读/写、忙等待超时容错；
 *   - SATA/AHCI：PCI BAR5 探测、HBA 能力、端口命令列表结构、FIS 骨架；
 *   - NVMe：MMIO 寄存器映射、提交/完成队列结构；
 *   - SCSI 层：CDB(6/10/12) 结构、命令块、深度队列；
 *   - 块设备层：gendisk 抽象、bio 结构、request 队列；
 *   - I/O 调度器：FIFO 与电梯排序；
 *   - 分区表：MBR(4 分区) 与 GPT(头+LBA 校验) 解析；
 *   - 磁盘缓存与预读：哈希桶 + LRU 页、预读策略表；
 *   - 写合并与回写：脏页跟踪、延迟写回；
 *   - TRIM 与 SSD 优化；磁盘热插拔；错误重试；坏道重映射；
 *   - SMART 健康监控；磁盘加密(扇区级)；软 RAID(0/1/5)；
 *   - LVM(PV/VG/LV)；磁盘配额；性能统计；省电停转。
 * ========================================================================== */
#ifndef __XOS_DISK_H__
#define __XOS_DISK_H__

#include "types.h"

#define DISK_MAX_DEVS       4
#define DISK_SECTOR_SIZE    512u
#define DISK_MAX_PARTITIONS 8u
#define DISK_QUEUE_DEPTH    16u
#define DISK_CACHE_PAGES    16u
#define DISK_SMART_ATTRS    8u
#define DISK_RAID_STRIPE    4u
#define DISK_MAX_LVS        8u

typedef enum {
    DISK_OP_NONE = 0,
    DISK_OP_READ,
    DISK_OP_WRITE,
    DISK_OP_TRIM,
} disk_op_t;

typedef struct bio {
    u32        sector;      /* 起始扇区 */
    u32        count;       /* 扇区数 */
    u8        *buf;         /* 数据缓冲 */
    disk_op_t  op;
    u32        dev;
    u32        flags;       /* 同步/异步/合并标记 */
    struct bio *next;
} bio_t;

typedef struct gendisk {
    u32    present;
    u32    type;            /* 0=ATA 1=AHCI 2=NVMe 3=SCSI 4=RAM 模拟 */
    u32    lba_count;       /* 总扇区数 */
    u32    sector_size;
    u32    heads, sectors_per_track, cylinders;
    u32    serial;          /* IDENTIFY 序列号哈希 */
    u32    capacity_mb;
    u32    slot;            /* 热插拔槽位 */
    u32    attached;
    u32    smart_ok;
    u32    power_state;     /* 0=active 1=idle 2=standby */
} gendisk_t;

/* 分区表 */
typedef struct partition {
    u32    present;
    u32    type;            /* 分区类型字节 */
    u32    lba_start;
    u32    lba_count;
    char   name[36];
} partition_t;

/* I/O 调度器 */
typedef struct req_node {
    u32    sector;
    u32    count;
    disk_op_t op;
    u32    tag;
    struct req_node *next;
} req_node_t;

/* 缓存页 */
typedef struct cache_page {
    u32    dev;
    u32    sector;
    u8     dirty;
    u8     valid;
    u32    lru;
    u8     data[DISK_SECTOR_SIZE];
    struct cache_page *next;
} cache_page_t;

/* SMART 属性 */
typedef struct smart_attr {
    u8     id;
    u8     flags;
    u32    value;           /* 当前归一化值 */
    u32    worst;
    u32    threshold;
} smart_attr_t;

/* RAID 卷 */
typedef struct raid_vol {
    u32    present;
    u32    level;           /* 0 / 1 / 5 */
    u32    ndisks;
    u32    lba_count;
    u32    stripe;
} raid_vol_t;

/* LVM */
typedef struct lv {
    u32    present;
    u32    pe_start;        /* PE 起始号 */
    u32    pe_count;
    u32    pv_count;
    char   name[24];
} lv_t;

/* 配额 */
typedef struct quota_rec {
    u32    uid;
    u32    blocks_used;
    u32    blocks_hard;
    u32    files_used;
    u32    files_hard;
} quota_rec_t;

/* 统计 */
typedef struct disk_stat {
    u32    reads;
    u32    writes;
    u32    trims;
    u64    read_sectors;
    u64    write_sectors;
    u32    errors;
    u32    retries;
} disk_stat_t;

/* ---------------- 公共接口 ---------------- */
void disk_init(void);
void disk_poll(void);                       /* 轮询热插拔/省电状态 */
int  disk_read_sectors(u32 dev, u32 lba, u32 count, u8 *buf);
int  disk_write_sectors(u32 dev, u32 lba, u32 count, const u8 *buf);
int  disk_identify(u32 dev, gendisk_t *out);
int  disk_parse_mbr(const u8 *mbr, partition_t *parts, u32 max);
int  disk_parse_gpt(const u8 *hdr, const u8 *ents, partition_t *parts, u32 max);
int  disk_bio_submit(bio_t *bio);
int  disk_sched_fifo(bio_t *bio);
int  disk_sched_elevator(bio_t *bio);
int  disk_cache_read(u32 dev, u32 sector, u8 *out);
int  disk_cache_write(u32 dev, u32 sector, const u8 *in);
void disk_cache_flush(void);
int  disk_trim(u32 dev, u32 lba, u32 count);
int  disk_attach(u32 dev, u32 type, u32 lba_count);
int  disk_detach(u32 dev);
int  disk_retry_read(u32 dev, u32 lba, u32 count, u8 *buf, u32 max_retry);
int  disk_badblock_add(u32 dev, u32 lba);
int  disk_badblock_remap(u32 dev, u32 lba, u32 *new_lba);
int  disk_smart_get(u32 dev, smart_attr_t *out, u32 max);
int  disk_smart_update(u32 dev, u8 id, u32 value);
int  disk_encrypt_set_key(u32 dev, u32 key);
void disk_encrypt_xor(u8 *buf, u32 sectors, u32 key);
int  disk_raid_map(u32 level, u32 stripe, u32 lba, u32 *dev, u32 *dev_lba);
int  disk_lvm_map(u32 lv, u32 pe, u32 *dev, u32 *dev_lba);
int  disk_quota_check(u32 uid, u32 blocks, u32 files);
int  disk_quota_add(u32 uid, u32 blocks, u32 files);
int  disk_stat_get(u32 dev, disk_stat_t *out);
int  disk_power_idle(u32 dev, u32 timeout_ticks);
int  disk_power_wake(u32 dev);
u32  disk_selftest(void);
void disk_dump(void);

#endif /* __XOS_DISK_H__ */
