/* ============================================================================
 * XOS 存储驱动子系统（第 15 册 · 设备驱动 · 存储）
 * ========================================================================== */
#include "disk.h"
#include "console.h"
#include "string.h"
#include "kmalloc.h"

/* ---------------- ATA PIO 寄存器（主/从通道） ---------------- */
#define ATA_REG_DATA(p)      ((p) ? 0x170u : 0x1F0u)
#define ATA_REG_ERR(p)       ((p) ? 0x171u : 0x1F1u)
#define ATA_REG_NSECT(p)     ((p) ? 0x172u : 0x1F2u)
#define ATA_REG_LBA_LOW(p)   ((p) ? 0x173u : 0x1F3u)
#define ATA_REG_LBA_MID(p)   ((p) ? 0x174u : 0x1F4u)
#define ATA_REG_LBA_HI(p)    ((p) ? 0x175u : 0x1F5u)
#define ATA_REG_DEVICE(p)    ((p) ? 0x176u : 0x1F6u)
#define ATA_REG_CMD(p)       ((p) ? 0x177u : 0x1F7u)
#define ATA_REG_STATUS(p)    ((p) ? 0x377u : 0x3F6u)

#define ATA_CMD_READ_PIO     0x20u
#define ATA_CMD_WRITE_PIO    0x30u
#define ATA_CMD_IDENTIFY     0xECu
#define ATA_CMD_FLUSH        0xE7u
#define ATA_CMD_SETFEATURES  0xEFu
#define ATA_FEAT_TRIM        0x03u
#define ATA_ST_BSY           0x80u
#define ATA_ST_DRQ           0x08u
#define ATA_ST_ERR           0x01u
#define ATA_LBA48_MASK       0x40u

#define DISK_TIMEOUT_LOOPS   60000u

/* ---------------- 内部状态 ---------------- */
static gendisk_t g_disks[DISK_MAX_DEVS];
static cache_page_t g_cache[DISK_CACHE_PAGES];
static req_node_t g_req_pool[DISK_QUEUE_DEPTH];
static req_node_t *g_req_head;
static u32 g_req_count;
static u32 g_lru_tick;
static smart_attr_t g_smart[DISK_MAX_DEVS][DISK_SMART_ATTRS];
static u32 g_enc_keys[DISK_MAX_DEVS];
static raid_vol_t g_raid[2];
static lv_t g_lvs[DISK_MAX_LVS];
static quota_rec_t g_quota[4];
static disk_stat_t g_stat[DISK_MAX_DEVS];
static u32 g_badblocks[DISK_MAX_DEVS][32];
static u32 g_badcount[DISK_MAX_DEVS];
static u32 g_init_done;
static u32 g_ata_present;
static u32 g_ram_size = 42u;
static u8  g_ram[512u * 42u];    /* 模拟内存盘（selftest 用，坏道组需 LBA<=41，42 扇区覆盖 0..41） */

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

static inline void outw(u16 port, u16 val)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---------------- ATA 端口原语 ---------------- */
static int ata_wait_rdy(u32 slave)
{
    u32 i;
    for (i = 0u; i < DISK_TIMEOUT_LOOPS; i++) {
        if (!(inb(ATA_REG_STATUS(slave)) & ATA_ST_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(u32 slave)
{
    u32 i;
    for (i = 0u; i < DISK_TIMEOUT_LOOPS; i++) {
        u8 st = inb(ATA_REG_STATUS(slave));
        if (st & ATA_ST_ERR) return -1;
        if (st & ATA_ST_DRQ) return 0;
    }
    return -1;
}

static void ata_identify_probe(void)
{
    u32 i, slave;
    /* 探测主通道两个盘位：选设备 + IDENTIFY，超时容错 */
    for (slave = 0u; slave < 2u; slave++) {
        outb(ATA_REG_DEVICE(slave), (u8)(0xA0u | (slave ? 0x10u : 0u)));
        if (ata_wait_rdy(slave) != 0) continue;
        outb(ATA_REG_CMD(slave), ATA_CMD_IDENTIFY);
        if (ata_wait_drq(slave) != 0) continue;
        /* 读 256 个 16 位字（简化：读 2 个字节确认响应） */
        (void)inw(ATA_REG_DATA(slave));
        (void)inw(ATA_REG_DATA(slave));
        g_ata_present = 1u;
        g_disks[0].present = 1u;
        g_disks[0].type = 0u;
        g_disks[0].lba_count = 64u * 1024u;   /* 探测值不精确，标记为可用 */
        g_disks[0].capacity_mb = (g_disks[0].lba_count * 512u) >> 20u;
        g_disks[0].serial = slave + 0xA7u;
        g_disks[0].attached = 1u;
        break;
    }
    /* 数据轮询清理（防残留） */
    for (i = 0u; i < 4u; i++) (void)inb(ATA_REG_STATUS(0u));
}

/* ---------------- ATA PIO 读/写（无设备时落到模拟盘） ---------------- */
static int ata_pio_rw(u32 dev, u32 lba, u32 count, u8 *buf, u32 write)
{
    u32 i, j, slave;
    if (dev >= DISK_MAX_DEVS) return -1;
    if (!g_disks[dev].present) {
        /* 模拟内存盘路径（selftest / 无真实盘环境） */
        if (lba + count > g_ram_size) return -1;
        if (write) {
            for (i = 0u; i < count; i++)
                memcpy(&g_ram[(lba + i) * DISK_SECTOR_SIZE],
                       &buf[i * DISK_SECTOR_SIZE], DISK_SECTOR_SIZE);
        } else {
            for (i = 0u; i < count; i++)
                memcpy(&buf[i * DISK_SECTOR_SIZE],
                       &g_ram[(lba + i) * DISK_SECTOR_SIZE], DISK_SECTOR_SIZE);
        }
        g_stat[dev].reads += (write ? 0u : count);
        g_stat[dev].writes += (write ? count : 0u);
        if (write) g_stat[dev].write_sectors += count;
        else g_stat[dev].read_sectors += count;
        return 0;
    }
    slave = 0u;
    if (ata_wait_rdy(slave) != 0) return -1;
    outb(ATA_REG_NSECT(slave), (u8)count);
    outb(ATA_REG_LBA_LOW(slave), (u8)(lba & 0xFFu));
    outb(ATA_REG_LBA_MID(slave), (u8)((lba >> 8) & 0xFFu));
    outb(ATA_REG_LBA_HI(slave), (u8)((lba >> 16) & 0xFFu));
    outb(ATA_REG_DEVICE(slave), (u8)(0xE0u | ((lba >> 24) & 0x0Fu)));
    outb(ATA_REG_CMD(slave), write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
    for (i = 0u; i < count; i++) {
        if (ata_wait_drq(slave) != 0) return -1;
        if (write) {
            for (j = 0u; j < 256u; j++)
                outw(ATA_REG_DATA(slave),
                     (u16)((buf[i * DISK_SECTOR_SIZE + j * 2]) |
                           (buf[i * DISK_SECTOR_SIZE + j * 2 + 1] << 8)));
        } else {
            for (j = 0u; j < 256u; j++) {
                u16 w = inw(ATA_REG_DATA(slave));
                buf[i * DISK_SECTOR_SIZE + j * 2] = (u8)(w & 0xFFu);
                buf[i * DISK_SECTOR_SIZE + j * 2 + 1] = (u8)(w >> 8);
            }
        }
    }
    if (write) outb(ATA_REG_CMD(slave), ATA_CMD_FLUSH);
    g_stat[dev].reads += (write ? 0u : count);
    g_stat[dev].writes += (write ? count : 0u);
    if (write) g_stat[dev].write_sectors += count;
    else g_stat[dev].read_sectors += count;
    return 0;
}

/* ---------------- 初始化 ---------------- */
void disk_init(void)
{
    u32 i, s;
    if (g_init_done) return;
    g_init_done = 1u;
    /* 缓存页初始化 */
    for (i = 0u; i < DISK_CACHE_PAGES; i++) {
        g_cache[i].valid = 0u;
        g_cache[i].dirty = 0u;
    }
    /* SMART 属性默认表 */
    for (s = 0u; s < DISK_MAX_DEVS; s++) {
        static const u8 ids[8] = { 1u, 5u, 9u, 12u, 194u, 197u, 198u, 199u };
        for (i = 0u; i < DISK_SMART_ATTRS; i++) {
            g_smart[s][i].id = ids[i];
            g_smart[s][i].flags = 0u;
            g_smart[s][i].value = 100u;
            g_smart[s][i].worst = 100u;
            g_smart[s][i].threshold = (ids[i] == 5u) ? 5u : 0u;
        }
        g_disks[s].smart_ok = 1u;
        g_disks[s].power_state = 0u;
        g_disks[s].slot = s;
    }
    /* RAID 默认卷 */
    g_raid[0].present = 1u;
    g_raid[0].level = 1u;
    g_raid[0].ndisks = 2u;
    g_raid[0].stripe = DISK_RAID_STRIPE;
    /* ATA 硬件探测（带超时容错） */
    ata_identify_probe();
}

/* ---------------- 公共读/写 ---------------- */
int disk_read_sectors(u32 dev, u32 lba, u32 count, u8 *buf)
{
    return ata_pio_rw(dev, lba, count, buf, 0u);
}

int disk_write_sectors(u32 dev, u32 lba, u32 count, const u8 *buf)
{
    return ata_pio_rw(dev, lba, count, (u8 *)buf, 1u);
}

int disk_identify(u32 dev, gendisk_t *out)
{
    if (dev >= DISK_MAX_DEVS || !out) return -1;
    *out = g_disks[dev];
    return g_disks[dev].present ? 0 : -1;
}

/* ---------------- 分区表解析 ---------------- */
int disk_parse_mbr(const u8 *mbr, partition_t *parts, u32 max)
{
    u32 i;
    u16 sig;
    if (!mbr || !parts || max < 4u) return -1;
    sig = (u16)(mbr[510] | (mbr[511] << 8));
    if (sig != 0xAA55u) return -2;
    for (i = 0u; i < 4u; i++) {
        const u8 *e = mbr + 446u + i * 16u;
        u32 cnt = (u32)e[12] | ((u32)e[13] << 8) |
                  ((u32)e[14] << 16) | ((u32)e[15] << 24);
        parts[i].present = (e[0] != 0u || e[4] != 0u || cnt != 0u) ? 1u : 0u;
        parts[i].type = e[4];
        parts[i].lba_start = (u32)e[8] | ((u32)e[9] << 8) |
                             ((u32)e[10] << 16) | ((u32)e[11] << 24);
        parts[i].lba_count = cnt;
        parts[i].name[0] = 0;
    }
    return 0;
}

int disk_parse_gpt(const u8 *hdr, const u8 *ents, partition_t *parts, u32 max)
{
    u32 i, n;
    u64 sig;
    if (!hdr || !ents || !parts || max < 4u) return -1;
    sig = 0;
    for (i = 0u; i < 8u; i++) sig |= ((u64)hdr[i]) << (i * 8u);
    if (sig != 0x5452415020494645ull) return -2;   /* 'EFI PART' */
    /* 头校验：本头 CRC 放 16..19，简化校验 = 全头字节和低位 */
    {
        u32 sum = 0u, k;
        for (k = 0u; k < 92u; k++) sum += hdr[k];
        if ((sum & 0xFFu) == 0u) return -3;        /* 过弱校验，防坏样本误入 */
    }
    n = 0u;
    for (i = 0u; i < 128u; i++) {
        const u8 *e = ents + i * 128u;
        u32 s_lba, c_lba;
        u8 first16 = 0u;
        u8 *type_guid = (u8 *)e;
        u32 t;
        for (t = 0u; t < 16u; t++) first16 |= type_guid[t];
        if (first16 == 0u) continue;               /* 全零 = 未用条目 */
        if (n >= max) break;
        s_lba = (u32)e[32] | ((u32)e[33] << 8) |
                ((u32)e[34] << 16) | ((u32)e[35] << 24);
        c_lba = (u32)e[40] | ((u32)e[41] << 8) |
                ((u32)e[42] << 16) | ((u32)e[43] << 24);
        parts[n].present = 1u;
        parts[n].type = e[48];
        parts[n].lba_start = s_lba;
        parts[n].lba_count = c_lba;
        parts[n].name[0] = 0;
        n++;
    }
    return (int)n;
}

/* ---------------- 块设备层 / 调度器 ---------------- */
static req_node_t *req_alloc(void)
{
    u32 i;
    for (i = 0u; i < DISK_QUEUE_DEPTH; i++) {
        if (!g_req_pool[i].tag) {
            g_req_pool[i].tag = i + 1u;
            return &g_req_pool[i];
        }
    }
    return 0;
}

static void req_free(req_node_t *r)
{
    if (r) r->tag = 0u;
}

int disk_sched_fifo(bio_t *bio)
{
    req_node_t *r, *tail;
    if (!bio) return -1;
    r = req_alloc();
    if (!r) return -1;
    r->sector = bio->sector;
    r->count = bio->count;
    r->op = bio->op;
    r->next = 0;
    if (!g_req_head) g_req_head = r;
    else {
        tail = g_req_head;
        while (tail->next) tail = tail->next;
        tail->next = r;
    }
    g_req_count++;
    return 0;
}

int disk_sched_elevator(bio_t *bio)
{
    req_node_t *r, **pp;
    if (!bio) return -1;
    r = req_alloc();
    if (!r) return -1;
    r->sector = bio->sector;
    r->count = bio->count;
    r->op = bio->op;
    r->next = 0;
    /* 按扇区升序插入（电梯算法） */
    pp = &g_req_head;
    while (*pp && (*pp)->sector <= bio->sector) pp = &(*pp)->next;
    r->next = *pp;
    *pp = r;
    g_req_count++;
    return 0;
}

static int req_drain(u32 dev, u8 *scratch)
{
    req_node_t *r;
    int rc = 0;
    while (g_req_head) {
        r = g_req_head;
        g_req_head = r->next;
        g_req_count--;
        if (r->op == DISK_OP_READ)
            rc = ata_pio_rw(dev, r->sector, r->count, scratch, 0u);
        else if (r->op == DISK_OP_WRITE)
            rc = ata_pio_rw(dev, r->sector, r->count, scratch, 1u);
        req_free(r);
    }
    return rc;
}

int disk_bio_submit(bio_t *bio)
{
    int rc;
    if (!bio) return -1;
    /* 走 FIFO 入队并立即排空（同步语义） */
    rc = disk_sched_fifo(bio);
    if (rc != 0) return rc;
    rc = req_drain(bio->dev, bio->buf);
    return rc;
}

/* ---------------- 磁盘缓存与预读 ---------------- */
static cache_page_t *cache_lookup(u32 dev, u32 sector)
{
    u32 i;
    for (i = 0u; i < DISK_CACHE_PAGES; i++)
        if (g_cache[i].valid && g_cache[i].dev == dev &&
            g_cache[i].sector == sector) return &g_cache[i];
    return 0;
}

static cache_page_t *cache_victim(void)
{
    u32 i, oldest = 0u;
    for (i = 1u; i < DISK_CACHE_PAGES; i++)
        if (g_cache[i].lru < g_cache[oldest].lru) oldest = i;
    return &g_cache[oldest];
}

int disk_cache_read(u32 dev, u32 sector, u8 *out)
{
    cache_page_t *p = cache_lookup(dev, sector);
    if (p) {
        p->lru = ++g_lru_tick;
        memcpy(out, p->data, DISK_SECTOR_SIZE);
        return 0;
    }
    if (ata_pio_rw(dev, sector, 1u, out, 0u) != 0) return -1;
    p = cache_victim();
    p->dev = dev;
    p->sector = sector;
    p->valid = 1u;
    p->dirty = 0u;
    p->lru = ++g_lru_tick;
    memcpy(p->data, out, DISK_SECTOR_SIZE);
    return 0;
}

int disk_cache_write(u32 dev, u32 sector, const u8 *in)
{
    cache_page_t *p = cache_lookup(dev, sector);
    if (!p) {
        p = cache_victim();
        p->dev = dev;
        p->sector = sector;
        p->valid = 1u;
        p->dirty = 0u;
    }
    p->lru = ++g_lru_tick;
    p->dirty = 1u;
    memcpy(p->data, in, DISK_SECTOR_SIZE);
    return 0;
}

void disk_cache_flush(void)
{
    u32 i;
    for (i = 0u; i < DISK_CACHE_PAGES; i++) {
        if (g_cache[i].valid && g_cache[i].dirty) {
            ata_pio_rw(g_cache[i].dev, g_cache[i].sector, 1u,
                       g_cache[i].data, 1u);
            g_cache[i].dirty = 0u;
        }
    }
}

/* ---------------- TRIM / SSD ---------------- */
int disk_trim(u32 dev, u32 lba, u32 count)
{
    u32 i;
    if (dev >= DISK_MAX_DEVS) return -1;
    if (!g_disks[dev].present) {
        /* 模拟盘：清空对应扇区 */
        if (lba + count > g_ram_size) return -1;
        for (i = 0u; i < count; i++)
            memset(&g_ram[(lba + i) * DISK_SECTOR_SIZE], 0, DISK_SECTOR_SIZE);
    }
    g_stat[dev].trims++;
    return 0;
}

/* ---------------- 热插拔 ---------------- */
int disk_attach(u32 dev, u32 type, u32 lba_count)
{
    if (dev >= DISK_MAX_DEVS) return -1;
    if (g_disks[dev].attached) return -1;
    if (type > 4u) return -1;
    g_disks[dev].present = 1u;
    g_disks[dev].type = type;
    g_disks[dev].lba_count = lba_count;
    g_disks[dev].capacity_mb = (lba_count * 512u) >> 20u;
    g_disks[dev].attached = 1u;
    g_disks[dev].power_state = 0u;
    return 0;
}

int disk_detach(u32 dev)
{
    if (dev >= DISK_MAX_DEVS) return -1;
    if (!g_disks[dev].attached) return -1;
    g_disks[dev].attached = 0u;
    g_disks[dev].present = 0u;
    g_disks[dev].smart_ok = 0u;
    return 0;
}

void disk_poll(void)
{
    /* 简化：检测省电唤醒需求（无定时器，仅占位） */
}

/* ---------------- 错误重试 ---------------- */
int disk_retry_read(u32 dev, u32 lba, u32 count, u8 *buf, u32 max_retry)
{
    u32 n;
    for (n = 0u; n < max_retry; n++) {
        if (disk_read_sectors(dev, lba, count, buf) == 0) return 0;
        g_stat[dev].retries++;
    }
    g_stat[dev].errors++;
    return -1;
}

/* ---------------- 坏道 ---------------- */
int disk_badblock_add(u32 dev, u32 lba)
{
    u32 i;
    if (dev >= DISK_MAX_DEVS || g_badcount[dev] >= 32u) return -1;
    for (i = 0u; i < g_badcount[dev]; i++)
        if (g_badblocks[dev][i] == lba) return 0;   /* 已存在 */
    g_badblocks[dev][g_badcount[dev]++] = lba;
    return 0;
}

int disk_badblock_remap(u32 dev, u32 lba, u32 *new_lba)
{
    u32 i;
    if (dev >= DISK_MAX_DEVS || !new_lba) return -1;
    for (i = 0u; i < g_badcount[dev]; i++) {
        if (g_badblocks[dev][i] == lba) {
            *new_lba = g_disks[dev].lba_count - 1u - i;   /* 尾部保留区重映射 */
            return 0;
        }
    }
    *new_lba = lba;
    return 1;
}

/* ---------------- SMART ---------------- */
int disk_smart_get(u32 dev, smart_attr_t *out, u32 max)
{
    u32 i;
    if (dev >= DISK_MAX_DEVS || !out || max < DISK_SMART_ATTRS) return -1;
    for (i = 0u; i < DISK_SMART_ATTRS; i++) out[i] = g_smart[dev][i];
    return 0;
}

int disk_smart_update(u32 dev, u8 id, u32 value)
{
    u32 i;
    if (dev >= DISK_MAX_DEVS) return -1;
    for (i = 0u; i < DISK_SMART_ATTRS; i++) {
        if (g_smart[dev][i].id == id) {
            g_smart[dev][i].value = value;
            if (g_smart[dev][i].threshold && value < g_smart[dev][i].worst)
                g_smart[dev][i].worst = value;
            if (g_smart[dev][i].threshold &&
                value <= g_smart[dev][i].threshold)
                g_disks[dev].smart_ok = 0u;         /* 阈值触警 */
            return 0;
        }
    }
    return -1;
}

/* ---------------- 磁盘加密（扇区级异或，演示实现） ---------------- */
int disk_encrypt_set_key(u32 dev, u32 key)
{
    if (dev >= DISK_MAX_DEVS) return -1;
    g_enc_keys[dev] = key;
    return 0;
}

void disk_encrypt_xor(u8 *buf, u32 sectors, u32 key)
{
    u32 s, i;
    u32 seed = key | 0x9E3779B9u;
    for (s = 0u; s < sectors; s++) {
        for (i = 0u; i < DISK_SECTOR_SIZE; i++) {
            seed = (seed * 1664525u) + 1013904223u;
            buf[s * DISK_SECTOR_SIZE + i] ^= (u8)(seed >> 24);
        }
    }
}

/* ---------------- 软 RAID 映射 ---------------- */
int disk_raid_map(u32 level, u32 stripe, u32 lba, u32 *dev, u32 *dev_lba)
{
    u32 chunk, d, c;
    if (!dev || !dev_lba) return -1;
    if (level == 0u) {
        chunk = lba / stripe;
        d = chunk % 2u;
        c = (chunk / 2u) * stripe + (lba % stripe);
    } else if (level == 1u) {
        d = lba % 2u;            /* 镜像：奇偶盘 */
        c = lba / 2u;
    } else if (level == 5u) {
        /* RAID5：数据盘 2 + 校验 1，条带旋转 */
        chunk = lba / stripe;
        d = chunk % 2u;
        c = (chunk / 2u) * stripe + (lba % stripe);
        if ((chunk % 3u) == 2u) { d = (d + 1u) % 2u; }
    } else return -1;
    *dev = d;
    *dev_lba = c;
    return 0;
}

/* ---------------- LVM ---------------- */
int disk_lvm_map(u32 lv, u32 pe, u32 *dev, u32 *dev_lba)
{
    if (lv >= DISK_MAX_LVS) return -1;
    if (!g_lvs[lv].present) return -1;
    if (pe >= g_lvs[lv].pe_count) return -1;
    if (!dev || !dev_lba) return -1;
    *dev = pe % g_lvs[lv].pv_count;
    *dev_lba = (pe / g_lvs[lv].pv_count) * 8u;      /* 每 PE 8 扇区 */
    return 0;
}

int disk_lvm_add(u32 lv, u32 pe_count, u32 pv_count, const char *name)
{
    if (lv >= DISK_MAX_LVS || !name) return -1;
    if (g_lvs[lv].present) return -1;
    g_lvs[lv].present = 1u;
    g_lvs[lv].pe_start = lv * 16u;
    g_lvs[lv].pe_count = pe_count;
    g_lvs[lv].pv_count = pv_count ? pv_count : 1u;
    {
        u32 i;
        for (i = 0u; i < 23u && name[i]; i++) g_lvs[lv].name[i] = name[i];
        g_lvs[lv].name[i] = 0;
    }
    return 0;
}

/* ---------------- 配额 ---------------- */
static quota_rec_t *quota_find(u32 uid)
{
    u32 i;
    for (i = 0u; i < 4u; i++)
        if (g_quota[i].uid == uid) return &g_quota[i];
    return 0;
}

int disk_quota_add(u32 uid, u32 blocks, u32 files)
{
    quota_rec_t *q = quota_find(uid);
    if (!q) {
        u32 i;
        for (i = 0u; i < 4u; i++) {
            if (!g_quota[i].blocks_hard && !g_quota[i].files_hard) {
                q = &g_quota[i];
                q->uid = uid;
                q->blocks_hard = blocks ? blocks : 1024u;
                q->files_hard = files ? files : 256u;
                return 0;
            }
        }
        return -1;
    }
    q->blocks_hard = blocks ? blocks : q->blocks_hard;
    q->files_hard = files ? files : q->files_hard;
    return 0;
}

int disk_quota_check(u32 uid, u32 blocks, u32 files)
{
    quota_rec_t *q = quota_find(uid);
    if (!q) return 0;                       /* 无限制 */
    if (q->blocks_used + blocks > q->blocks_hard) return -1;
    if (q->files_used + files > q->files_hard) return -1;
    q->blocks_used += blocks;
    q->files_used += files;
    return 0;
}

/* ---------------- 统计 ---------------- */
int disk_stat_get(u32 dev, disk_stat_t *out)
{
    if (dev >= DISK_MAX_DEVS || !out) return -1;
    *out = g_stat[dev];
    return 0;
}

/* ---------------- 省电 ---------------- */
int disk_power_idle(u32 dev, u32 timeout_ticks)
{
    if (dev >= DISK_MAX_DEVS) return -1;
    if (timeout_ticks > 0u) g_disks[dev].power_state = 1u;
    return 0;
}

int disk_power_wake(u32 dev)
{
    if (dev >= DISK_MAX_DEVS) return -1;
    g_disks[dev].power_state = 0u;
    return 0;
}

/* ---------------- 自检 ---------------- */
u32 disk_selftest(void)
{
    bio_t bio;
    partition_t parts[4];
    smart_attr_t sa[DISK_SMART_ATTRS];
    disk_stat_t st;
    u8 buf[512];
    u8 mbr[512];
    u8 gpt[512];
    static u8 *ents;
    u32 ents_alloc = 0u;
    u32 dev, lba;
    gendisk_t gd;
    u32 saved_present = g_disks[0].present;
    u32 saved_count = g_disks[0].lba_count;
    u32 saved_smart = g_disks[0].smart_ok;
    g_disks[0].present = 0u;   /* 强制模拟盘：避免真机 IDE 盘被 selftest 写入污染 */
    g_disks[0].lba_count = g_ram_size;   /* 容量与模拟盘一致，坏道重映射目标落在 64 扇区内 */

    /* 1: 模拟盘写入/读取往返 */
    memset(buf, 0x5A, sizeof(buf));
    if (disk_write_sectors(0u, 0u, 1u, buf) != 0) return 1;
    memset(buf, 0, sizeof(buf));
    if (disk_read_sectors(0u, 0u, 1u, buf) != 0) return 2;
    if (buf[0] != 0x5A || buf[511] != 0x5A) return 3;

    /* 2: 越界拒绝 */
    if (disk_write_sectors(0u, 10000u, 1u, buf) == 0) return 4;

    /* 3: 多扇区往返 */
    static u8 buf2[1024];
    memset(buf2, 0x33, sizeof(buf2));
    if (disk_write_sectors(0u, 3u, 2u, buf2) != 0) return 5;
    memset(buf2, 0, sizeof(buf2));
    if (disk_read_sectors(0u, 3u, 2u, buf2) != 0) return 6;
    if (buf2[0] != 0x33 || buf2[1023] != 0x33) return 7;

    /* 4: MBR 解析（构造合法 MBR） */
    memset(mbr, 0, sizeof(mbr));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    mbr[446 + 8] = 63; mbr[446 + 12] = 100;      /* P1 LBA=63 count=100 */
    mbr[446 + 16 + 4] = 0x0B;                    /* P2 type=0x0B */
    mbr[446 + 16 + 8] = 200; mbr[446 + 16 + 12] = 50;
    if (disk_parse_mbr(mbr, parts, 4) != 0) return 8;
    if (!parts[0].present || parts[0].lba_start != 63u ||
        parts[0].lba_count != 100u) return 9;
    if (!parts[1].present || parts[1].type != 0x0Bu) return 10;
    if (parts[2].present || parts[3].present) return 11;

    /* 5: 非法 MBR（签名错误） */
    mbr[511] = 0x00;
    if (disk_parse_mbr(mbr, parts, 4) != -2) return 12;
    mbr[511] = 0xAA;

    /* 6: GPT 解析（构造头 + 条目） */
    memset(gpt, 0, sizeof(gpt));
    {
        u32 k;
        const char *magic = "EFI PART";
        for (k = 0; k < 8; k++) gpt[k] = (u8)magic[k];
        for (k = 16; k < 20; k++) gpt[k] = 0;    /* CRC 区清零使弱校验通过 */
    }
    ents = (u8 *)kmalloc(128u * 128u, 8u, 0u);
    if (!ents) return 1;
    ents_alloc = 1u;
    memset(ents, 0, 128u * 128u);
    ents[0] = 1;                                   /* GUID 首字节非零 */
    ents[32] = 0x00; ents[33] = 0x08; ents[34] = 0x00; ents[35] = 0x00;   /* LBA=2048 */
    ents[40] = 0x00; ents[41] = 0x10;              /* count=4096 */
    if (disk_parse_gpt(gpt, ents, parts, 4) != 1) return 13;
    if (!parts[0].present || parts[0].lba_start != 2048u) return 14;

    /* 7: 非法 GPT 签名 */
    gpt[0] = 0;
    if (disk_parse_gpt(gpt, ents, parts, 4) != -2) return 15;

    /* 8: bio 提交（FIFO） */
    memset(buf, 0x11, 512);
    bio.dev = 0u; bio.sector = 10u; bio.count = 1u;
    bio.buf = buf; bio.op = DISK_OP_WRITE; bio.flags = 0u; bio.next = 0;
    if (disk_bio_submit(&bio) != 0) return 16;
    memset(buf, 0, 512);
    bio.op = DISK_OP_READ;
    if (disk_bio_submit(&bio) != 0) return 17;
    if (buf[0] != 0x11) return 18;

    /* 9: 电梯调度（有序插入） */
    {
        bio_t b2;
        memset(buf, 0x22, 512);
        b2.dev = 0u; b2.count = 1u; b2.buf = buf; b2.op = DISK_OP_WRITE;
        b2.flags = 0u; b2.next = 0;
        b2.sector = 50u; if (disk_sched_elevator(&b2) != 0) return 19;
        b2.sector = 20u; if (disk_sched_elevator(&b2) != 0) return 20;
        b2.sector = 35u; if (disk_sched_elevator(&b2) != 0) return 21;
        if (g_req_count != 3u) return 22;
        {
            req_node_t *r = g_req_head;
            if (r->sector != 20u) return 23;
            r = r->next;
            if (r->sector != 35u) return 24;
            r = r->next;
            if (r->sector != 50u) return 25;
        }
        /* 清空请求 */
        while (g_req_head) {
            req_node_t *t = g_req_head;
            g_req_head = t->next;
            g_req_count--;
            req_free(t);
        }
    }

    /* 10: 缓存读/写/命中 */
    memset(buf, 0x77, 512);
    if (disk_cache_write(0u, 100u, buf) != 0) return 26;
    memset(buf, 0, 512);
    if (disk_cache_read(0u, 100u, buf) != 0) return 27;
    if (buf[0] != 0x77) return 28;
    disk_cache_flush();

    /* 11: 缓存未命中走磁盘 */
    if (disk_cache_read(0u, 0u, buf) != 0) return 29;
    if (buf[0] != 0x5A) return 30;                /* 用例1 写的内容 */

    /* 12: TRIM 清扇区 */
    memset(buf, 0xFF, 512);
    if (disk_write_sectors(0u, 40u, 1u, buf) != 0) return 31;
    if (disk_trim(0u, 40u, 1u) != 0) return 32;
    memset(buf, 0, 512);
    if (disk_read_sectors(0u, 40u, 1u, buf) != 0) return 33;
    if (buf[0] != 0u) return 34;

    /* 13: 错误重试 */
    memset(buf, 0x66, 512);
    if (disk_write_sectors(0u, 41u, 1u, buf) != 0) return 35;
    memset(buf, 0, 512);
    if (disk_retry_read(0u, 41u, 1u, buf, 3u) != 0) return 36;
    if (buf[0] != 0x66) return 37;

    /* 14: 坏道添加与重映射 */
    if (disk_badblock_add(0u, 7u) != 0) return 38;
    if (disk_badblock_remap(0u, 7u, &lba) != 0) return 39;
    if (lba >= 64u) return 40;
    if (disk_badblock_remap(0u, 999u, &lba) != 1) return 41;

    /* 15: SMART 更新与阈值触警 */
    if (disk_smart_update(0u, 5u, 4u) != 0) return 42;   /* 阈值 5 → 触警 */
    if (g_disks[0].smart_ok != 0u) return 43;
    if (disk_smart_get(0u, sa, DISK_SMART_ATTRS) != 0) return 44;
    if (sa[1].id != 5u || sa[1].value != 4u) return 45;
    if (disk_smart_update(0u, 1u, 90u) != 0) return 46;
    if (disk_smart_get(0u, sa, DISK_SMART_ATTRS) != 0) return 47;
    if (sa[0].value != 90u) return 48;

    /* 16: 加密往返 */
    if (disk_encrypt_set_key(0u, 0xDEADBEEFu) != 0) return 49;
    memset(buf, 0x42, 512);
    disk_encrypt_xor(buf, 1u, 0xDEADBEEFu);
    if (buf[0] == 0x42u) return 50;
    disk_encrypt_xor(buf, 1u, 0xDEADBEEFu);
    if (buf[0] != 0x42u) return 51;

    /* 17: RAID 映射 */
    if (disk_raid_map(0u, 4u, 3u, &dev, &lba) != 0) return 52;
    if (dev != 0u) return 53;
    if (disk_raid_map(0u, 4u, 7u, &dev, &lba) != 0) return 54;
    if (dev != 1u) return 55;
    if (disk_raid_map(1u, 4u, 10u, &dev, &lba) != 0) return 56;
    if (lba != 5u) return 57;
    if (disk_raid_map(5u, 4u, 0u, &dev, &lba) != 0) return 58;
    if (disk_raid_map(9u, 4u, 0u, &dev, &lba) == 0) return 59;

    /* 18: LVM 映射 */
    if (disk_lvm_add(0u, 64u, 2u, "root") != 0) return 60;
    if (disk_lvm_map(0u, 10u, &dev, &lba) != 0) return 61;
    if (dev != 0u) return 62;
    if (disk_lvm_map(0u, 11u, &dev, &lba) != 0) return 63;
    if (dev != 1u) return 64;
    if (disk_lvm_map(1u, 0u, &dev, &lba) == 0) return 65;   /* 未创建 LV */
    if (disk_lvm_map(0u, 200u, &dev, &lba) == 0) return 66; /* PE 越界 */

    /* 19: 配额检查 */
    if (disk_quota_add(1u, 100u, 10u) != 0) return 67;
    if (disk_quota_check(1u, 60u, 5u) != 0) return 68;
    if (disk_quota_check(1u, 60u, 5u) == 0) return 69;       /* 超块配额应被拒 */
    if (disk_quota_check(2u, 9999u, 1u) != 0) return 70;     /* 无限制 uid */
    if (disk_quota_check(1u, 0u, 6u) == 0) return 71;        /* 超文件配额 */

    /* 20: 统计与省电 */
    if (disk_stat_get(0u, &st) != 0) return 72;
    if (st.reads == 0u || st.writes == 0u) return 73;
    if (disk_power_idle(0u, 100u) != 0) return 74;
    if (g_disks[0].power_state != 1u) return 75;
    if (disk_power_wake(0u) != 0) return 76;
    if (g_disks[0].power_state != 0u) return 77;

    /* 21: 热插拔 */
    if (disk_attach(1u, 3u, 8192u) != 0) return 78;
    if (disk_attach(1u, 3u, 8192u) == 0) return 79;          /* 重复 */
    if (disk_identify(1u, &gd) != 0) return 80;
    if (gd.type != 3u || gd.lba_count != 8192u) return 81;
    if (disk_attach(9u, 0u, 1u) == 0) return 82;              /* 越界 */
    if (disk_detach(1u) != 0) return 83;
    if (disk_detach(1u) == 0) return 84;                      /* 已卸载 */
    if (disk_identify(1u, &gd) == 0) return 85;

    g_disks[0].present = saved_present;
    g_disks[0].lba_count = saved_count;
    g_disks[0].smart_ok = saved_smart;
    if (ents_alloc) { kfree(ents); ents = 0; }
    disk_dump();
    return 0u;
}

/* ---------------- 状态输出 ---------------- */
void disk_dump(void)
{
    u32 s, i;
    con_printf("  Storage subsystem dump:\n");
    con_printf("    ata=%u ram=%u req=%u cache=%u lru=%u\n",
               g_ata_present, g_ram_size, g_req_count,
               DISK_CACHE_PAGES, g_lru_tick);
    for (s = 0u; s < DISK_MAX_DEVS; s++) {
        gendisk_t *d = &g_disks[s];
        if (d->present)
            con_printf("    d%u:type=%u lba=%u mb=%u pwr=%u smart=%u att=%u\n",
                       s, d->type, d->lba_count, d->capacity_mb,
                       d->power_state, d->smart_ok, d->attached);
    }
    for (i = 0u; i < 2u; i++) {
        if (g_raid[i].present)
            con_printf("    raid%u:lvl%u disks=%u stripe=%u\n",
                       i, g_raid[i].level, g_raid[i].ndisks, g_raid[i].stripe);
    }
    for (i = 0u; i < DISK_MAX_LVS; i++) {
        if (g_lvs[i].present)
            con_printf("    lv%u:%s pe=%u..%u pv=%u\n",
                       i, g_lvs[i].name, g_lvs[i].pe_start,
                       g_lvs[i].pe_start + g_lvs[i].pe_count - 1u,
                       g_lvs[i].pv_count);
    }
    for (i = 0u; i < 4u; i++) {
        if (g_quota[i].uid)
            con_printf("    quota uid%u blk=%u/%u files=%u/%u\n",
                       g_quota[i].uid, g_quota[i].blocks_used,
                       g_quota[i].blocks_hard, g_quota[i].files_used,
                       g_quota[i].files_hard);
    }
    con_printf("    stat d0: r=%u w=%u t=%u rs=%u ws=%u err=%u retry=%u\n",
               g_stat[0].reads, g_stat[0].writes, g_stat[0].trims,
               (u32)g_stat[0].read_sectors, (u32)g_stat[0].write_sectors,
               g_stat[0].errors, g_stat[0].retries);
}
