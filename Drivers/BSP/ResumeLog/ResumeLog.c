#include "ResumeLog.h"
#include "W25Q64.h"
#include "main.h"
#include <stddef.h>
#include <string.h>

#define RESUME_LOG_MAGIC          0xA5A5A5A5U
#define RESUME_LOG_COMMIT         0x5A5A5A5AU
#define RESUME_LOG_TYPE_PROGRESS  0x0001U

#define RESUME_LOG_START_ADDR     OTA_HEAT_ADDR
#define RESUME_LOG_END_ADDR       OTA_STAGING_ADDR
#define RESUME_LOG_SIZE           (RESUME_LOG_END_ADDR - RESUME_LOG_START_ADDR)
#define RESUME_LOG_SECTOR_COUNT   (RESUME_LOG_SIZE / W25Q64_SECTOR_SIZE)

#define RESUME_LOG_MIN_DELTA      1024U

#define RESUME_SECTOR_MAGIC       0x4C475353U
#define RESUME_SECTOR_COMMIT      0x53A55A3CU

#if (RESUME_LOG_START_ADDR >= RESUME_LOG_END_ADDR)
#error "ResumeLog region invalid: start address must be less than end address"
#endif

#if (RESUME_LOG_END_ADDR > W25Q64_TOTAL_SIZE)
#error "ResumeLog region invalid: end address out of W25Q64 range"
#endif

#if (RESUME_LOG_SIZE < W25Q64_SECTOR_SIZE)
#error "ResumeLog region invalid: size must be at least one sector"
#endif

#if ((RESUME_LOG_SIZE % W25Q64_SECTOR_SIZE) != 0U)
#error "ResumeLog region invalid: size must be sector-aligned"
#endif

#pragma pack(push, 1)
typedef struct      // 日志记录结构体
{
    uint32_t magic;
    uint16_t type;
    uint16_t len;       // 记录数据部分的长度（字节）
    uint32_t seq;       // 全局递增的序列号，用于区分新旧记录和恢复进度

    uint32_t value;     // 记录的值
    uint32_t task_tag;  // 固件标签
    uint32_t total_size; // 总大小

    uint32_t crc;
    uint32_t commit;    // 0x5A5A5A5A 表示已提交，0xFFFFFFFF 表示未提交
    uint32_t reserved;
} ResumeLog_Record_t;

typedef struct      // 扇区头结构体
{
    uint32_t magic;
    uint32_t generation;    // 代际号，每次擦除扇区时递增，用于区分新旧扇区
    uint32_t crc;
    uint32_t commit;        // 0x53A55A3C 表示已提交，0xFFFFFFFF 表示未提交
    uint32_t reserved;
} ResumeLog_SectorHeader_t;
#pragma pack(pop)

typedef struct      // 内部状态结构体
{
    uint32_t write_addr;
    uint32_t latest_addr;

    uint32_t latest_seq;
    uint32_t latest_value;
    uint32_t latest_task_tag;
    uint32_t latest_total_size;

    uint32_t max_generation;

    int32_t current_sector_id;      // 当前正在写入的扇区 ID，-1 表示尚未打开任何扇区
    uint16_t ordered_count;         // ordered_sector_ids 数组中有效的扇区数量
    uint16_t ordered_sector_ids[RESUME_LOG_SECTOR_COUNT];   // 按照 generation 从小到大排序的扇区 ID 数组，便于快速找到最旧的扇区进行回收

    uint32_t sector_generation[RESUME_LOG_SECTOR_COUNT];    // 每个扇区的 generation，冗余存储以避免频繁读取扇区头
    uint32_t sector_write_addr[RESUME_LOG_SECTOR_COUNT];    // 每个扇区当前的写入地址，初始值为扇区头结束地址，递增写入记录
    bool sector_present[RESUME_LOG_SECTOR_COUNT];       // 每个扇区是否存在有效数据，初始值为 false，扫描时更新
    bool sector_has_space[RESUME_LOG_SECTOR_COUNT];     // 每个扇区是否有足够空间写入新的记录，初始值为 false，扫描时更新

    bool latest_valid;
    bool initialized;
} ResumeLog_State_t;

static ResumeLog_State_t g_state = {0};

static bool ResumeLog_IsBlank(const uint8_t *data, uint32_t len);
static uint32_t ResumeLog_Crc32(const uint8_t *data, uint32_t len);
static uint32_t ResumeLog_CalcRecordCrc(const ResumeLog_Record_t *rec);
static uint32_t ResumeLog_CalcSectorHeaderCrc(const ResumeLog_SectorHeader_t *hdr);

static bool ResumeLog_IsValidRecord(const ResumeLog_Record_t *rec);
static bool ResumeLog_IsValidSectorHeader(const ResumeLog_SectorHeader_t *hdr);

static uint32_t ResumeLog_SectorBaseAddr(uint32_t sector_id);
static uint32_t ResumeLog_SectorRecordStart(uint32_t sector_id);
static uint32_t ResumeLog_SectorEndAddr(uint32_t sector_id);

static void ResumeLog_ResetState(void);

static uint16_t ResumeLog_BisectRightByGeneration(uint32_t generation);

static void ResumeLog_InsertOrderedSector(uint32_t sector_id);
static void ResumeLog_RemoveOrderedSector(uint32_t sector_id);

static void ResumeLog_ScanSectorRecords(uint32_t sector_id);
static void ResumeLog_Scan(void);

static bool ResumeLog_OpenSector(uint32_t sector_id, uint32_t generation);
static bool ResumeLog_EnsureWritableSector(void);
static bool ResumeLog_ReclaimOldestSector(void);

static bool ResumeLog_WriteRecordToCurrent(uint32_t value, uint32_t seq, uint32_t task_tag, uint32_t total_size);

static uint32_t ResumeLog_SectorBaseAddr(uint32_t sector_id)
{
    return RESUME_LOG_START_ADDR + sector_id * W25Q64_SECTOR_SIZE;
}

static uint32_t ResumeLog_SectorRecordStart(uint32_t sector_id)
{
    return ResumeLog_SectorBaseAddr(sector_id) + (uint32_t)sizeof(ResumeLog_SectorHeader_t);
}

static uint32_t ResumeLog_SectorEndAddr(uint32_t sector_id)
{
    return ResumeLog_SectorBaseAddr(sector_id) + W25Q64_SECTOR_SIZE;
}

static void ResumeLog_ResetState(void)
{
    g_state.write_addr = RESUME_LOG_END_ADDR;
    g_state.latest_addr = 0xFFFFFFFFU;
    g_state.latest_seq = 0U;
    g_state.latest_value = 0U;
    g_state.latest_task_tag = 0U;
    g_state.latest_total_size = 0U;
    g_state.max_generation = 0U;
    g_state.current_sector_id = -1;
    g_state.ordered_count = 0U;
    g_state.latest_valid = false;

    for (uint32_t i = 0; i < RESUME_LOG_SECTOR_COUNT; ++i)
    {
        g_state.ordered_sector_ids[i] = 0U;
        g_state.sector_generation[i] = 0U;
        g_state.sector_write_addr[i] = ResumeLog_SectorRecordStart(i);
        g_state.sector_present[i] = false;
        g_state.sector_has_space[i] = false;
    }
}

/**
 * @brief  在 ordered_sector_ids 数组中通过 generation 二分查找，找到第一个 generation 大于给定值的位置
 * @param  generation: 给定的 generation 值
 * @retval 位置索引，范围 [0, ordered_count]，如果所有 generation 都小于等于给定值，则返回 ordered_count
 * @note   该函数实现了一个标准的二分查找算法，时间复杂度为 O(log n)，其中 n 是 ordered_sector_ids 数组中有效的扇区数量。返回的索引可以用于在 ordered_sector_ids 数组中插入新的扇区 ID，以保持数组按照 generation 从小到大排序。
 */
static uint16_t ResumeLog_BisectRightByGeneration(uint32_t generation)
{
    uint16_t low = 0U;
    uint16_t high = g_state.ordered_count;

    while (low < high)
    {
        uint16_t mid = (uint16_t)(low + ((high - low) >> 1U));
        uint16_t sid = g_state.ordered_sector_ids[mid];     // 根据索引获取扇区 ID
        uint32_t mid_gen = g_state.sector_generation[sid];  // 根据扇区 ID 获取 generation

        if (mid_gen <= generation)
        {
            low = (uint16_t)(mid + 1U);
        }
        else
        {
            high = mid;
        }
    }

    return low;
}

/**
 * @brief  将扇区 ID 插入到 ordered_sector_ids 数组中，保持数组按照 generation 从小到大排序
 * @param  sector_id: 要插入的扇区 ID
 * @retval None
 */
static void ResumeLog_InsertOrderedSector(uint32_t sector_id)
{
    uint16_t pos = ResumeLog_BisectRightByGeneration(g_state.sector_generation[sector_id]);

    // 在 pos 位置插入 sector_id，后续元素依次后移一位
    for (uint16_t i = g_state.ordered_count; i > pos; --i)
    {
        g_state.ordered_sector_ids[i] = g_state.ordered_sector_ids[i - 1U];
    }

    g_state.ordered_sector_ids[pos] = (uint16_t)sector_id;
    g_state.ordered_count++;
}

static void ResumeLog_RemoveOrderedSector(uint32_t sector_id)
{
    for (uint16_t i = 0U; i < g_state.ordered_count; ++i)
    {
        if (g_state.ordered_sector_ids[i] == sector_id)
        {
            for (uint16_t j = i + 1U; j < g_state.ordered_count; ++j)
            {
                g_state.ordered_sector_ids[j - 1U] = g_state.ordered_sector_ids[j];
            }
            g_state.ordered_count--;
            return;
        }
    }
}

static uint32_t ResumeLog_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (uint32_t j = 0; j < 8; ++j)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t ResumeLog_CalcRecordCrc(const ResumeLog_Record_t *rec)
{
    return ResumeLog_Crc32((const uint8_t *)rec, (uint32_t)offsetof(ResumeLog_Record_t, crc));
}

static uint32_t ResumeLog_CalcSectorHeaderCrc(const ResumeLog_SectorHeader_t *hdr)
{
    return ResumeLog_Crc32((const uint8_t *)hdr, (uint32_t)offsetof(ResumeLog_SectorHeader_t, crc));
}

static bool ResumeLog_IsBlank(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i)
    {
        if (data[i] != 0xFFU)
        {
            return false;
        }
    }
    return true;
}

static bool ResumeLog_IsValidSectorHeader(const ResumeLog_SectorHeader_t *hdr)
{
    if (hdr->magic != RESUME_SECTOR_MAGIC)
    {
        return false;
    }
    if (hdr->commit != RESUME_SECTOR_COMMIT)
    {
        return false;
    }
    return (hdr->crc == ResumeLog_CalcSectorHeaderCrc(hdr));
}

static bool ResumeLog_IsValidRecord(const ResumeLog_Record_t *rec)
{
    if (rec->magic != RESUME_LOG_MAGIC)
    {
        return false;
    }
    if (rec->commit != RESUME_LOG_COMMIT)
    {
        return false;
    }
    if (rec->type != RESUME_LOG_TYPE_PROGRESS)
    {
        return false;
    }
    if (rec->len != (uint16_t)(sizeof(uint32_t) * 3U))
    {
        return false;
    }
    return (rec->crc == ResumeLog_CalcRecordCrc(rec));
}

/**
 * @brief  扫描指定扇区的所有记录，更新全局最新记录状态，并找到当前扇区的写入地址和空间状态
 * @param  sector_id: 要扫描的扇区 ID
 * @retval None
 */
static void ResumeLog_ScanSectorRecords(uint32_t sector_id)
{
    uint32_t start = ResumeLog_SectorRecordStart(sector_id);
    uint32_t end = ResumeLog_SectorEndAddr(sector_id);
    uint32_t write_addr = end;
    bool has_space = false;

    for (uint32_t addr = start; addr + sizeof(ResumeLog_Record_t) <= end; addr += sizeof(ResumeLog_Record_t))
    {
        ResumeLog_Record_t rec;
        W25Q64_ReadBytes(addr, (uint8_t *)&rec, sizeof(rec));

        if (ResumeLog_IsBlank((const uint8_t *)&rec, sizeof(rec)))
        {
            write_addr = addr;
            has_space = true;
            break;
        }

        if (ResumeLog_IsValidRecord(&rec))
        {
            if (!g_state.latest_valid || rec.seq > g_state.latest_seq)
            {
                g_state.latest_valid = true;
                g_state.latest_seq = rec.seq;
                g_state.latest_value = rec.value;
                g_state.latest_task_tag = rec.task_tag;
                g_state.latest_total_size = rec.total_size;
                g_state.latest_addr = addr;
            }
        }
    }

    g_state.sector_write_addr[sector_id] = write_addr;
    g_state.sector_has_space[sector_id] = has_space;
}

/**
 * @brief  扫描所有扇区，构建内存中扇区状态和最新记录状态
 * @retval None
 */
static void ResumeLog_Scan(void)
{
    ResumeLog_ResetState();

    for (uint32_t sector_id = 0; sector_id < RESUME_LOG_SECTOR_COUNT; ++sector_id)
    {
        ResumeLog_SectorHeader_t hdr;
        uint32_t base = ResumeLog_SectorBaseAddr(sector_id);
        W25Q64_ReadBytes(base, (uint8_t *)&hdr, sizeof(hdr));

        if (ResumeLog_IsBlank((const uint8_t *)&hdr, sizeof(hdr)))
        {
            continue;
        }

        if (!ResumeLog_IsValidSectorHeader(&hdr))
        {
            continue;
        }

        g_state.sector_present[sector_id] = true;
        g_state.sector_generation[sector_id] = hdr.generation;
        // 更新全局最大代际号，便于后续打开新扇区时使用
        if (hdr.generation > g_state.max_generation)
        {
            g_state.max_generation = hdr.generation;
        }

        ResumeLog_InsertOrderedSector(sector_id);
        ResumeLog_ScanSectorRecords(sector_id);
    }

    // 根据扫描结果设置当前写入扇区和地址，如果没有任何有效扇区，则 current_sector_id 保持为 -1，表示需要打开新扇区
    g_state.current_sector_id = -1;
    for (int32_t i = (int32_t)g_state.ordered_count - 1; i >= 0; --i)
    {
        uint32_t sid = g_state.ordered_sector_ids[i];
        if (g_state.sector_has_space[sid])
        {
            g_state.current_sector_id = (int32_t)sid;
            g_state.write_addr = g_state.sector_write_addr[sid];
            break;
        }
    }
}

/**
 * @brief  打开一个新的扇区进行写入，如果当前没有可用的扇区，则尝试回收最旧的扇区。成功打开后会更新全局状态以指向新的写入扇区和地址。
 * @param  sector_id: 要打开的扇区 ID
 * @param  generation: 扇区的 generation 值，通常为当前全局最大 generation + 1
 * @retval None
 */
static bool ResumeLog_OpenSector(uint32_t sector_id, uint32_t generation)
{
    ResumeLog_SectorHeader_t hdr;
    uint32_t base = ResumeLog_SectorBaseAddr(sector_id);

    memset(&hdr, 0xFF, sizeof(hdr));
    hdr.magic = RESUME_SECTOR_MAGIC;
    hdr.generation = generation;
    hdr.crc = ResumeLog_CalcSectorHeaderCrc(&hdr);
    hdr.commit = 0xFFFFFFFFU;
    hdr.reserved = 0xFFFFFFFFU;

    W25Q64_WriteBytes(base, (const uint8_t *)&hdr, sizeof(hdr));
    {
        uint32_t commit = RESUME_SECTOR_COMMIT;
        W25Q64_WriteBytes(base + offsetof(ResumeLog_SectorHeader_t, commit), (const uint8_t *)&commit, sizeof(commit));
    }

    g_state.sector_present[sector_id] = true;
    g_state.sector_generation[sector_id] = generation;
    g_state.sector_write_addr[sector_id] = ResumeLog_SectorRecordStart(sector_id);
    g_state.sector_has_space[sector_id] = true;
    g_state.current_sector_id = (int32_t)sector_id;
    g_state.write_addr = g_state.sector_write_addr[sector_id];
    g_state.max_generation = generation;
    ResumeLog_InsertOrderedSector(sector_id);
    return true;
}

/**
 * @brief  回收最旧的扇区以供写入使用，回收过程包括擦除扇区、更新扇区头、如果需要则保留最新记录并写入新扇区。成功回收后会更新全局状态以指向新的写入扇区和地址。
 * @retval None
 */
static bool ResumeLog_ReclaimOldestSector(void)
{
    uint32_t victim;
    uint32_t victim_base;
    uint32_t victim_end;
    bool keep_latest = false;
    uint32_t keep_value = 0U;
    uint32_t keep_seq = 0U;
    uint32_t keep_task_tag = 0U;
    uint32_t keep_total_size = 0U;

    if (g_state.ordered_count == 0U)
    {
        return false;
    }

    victim = g_state.ordered_sector_ids[0]; // 最旧的扇区在 ordered_sector_ids 数组的第一个位置
    victim_base = ResumeLog_SectorBaseAddr(victim);
    victim_end = ResumeLog_SectorEndAddr(victim);

    if (g_state.latest_valid && g_state.latest_addr >= victim_base && g_state.latest_addr < victim_end)
    {
        keep_latest = true;
        keep_value = g_state.latest_value;
        keep_seq = g_state.latest_seq;
        keep_task_tag = g_state.latest_task_tag;
        keep_total_size = g_state.latest_total_size;
    }

    W25Q64_EraseSector(victim_base / W25Q64_SECTOR_SIZE);

    g_state.sector_present[victim] = false;
    g_state.sector_generation[victim] = 0U;
    g_state.sector_write_addr[victim] = ResumeLog_SectorRecordStart(victim);
    g_state.sector_has_space[victim] = false;
    ResumeLog_RemoveOrderedSector(victim);

    // 打开新扇区时使用的 generation 是当前全局最大 generation + 1
    if (!ResumeLog_OpenSector(victim, g_state.max_generation + 1U))
    {
        return false;
    }

    // 如果被回收的扇区中包含全局最新的记录，则将该记录写入新的扇区，以避免数据丢失
    if (keep_latest)
    {
        if (!ResumeLog_WriteRecordToCurrent(keep_value, keep_seq + 1U, keep_task_tag, keep_total_size))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief  确保当前有一个可写的扇区，如果没有则尝试打开一个新的扇区，如果所有扇区都已满则尝试回收最旧的扇区。
 * @retval true 成功确保有可写扇区；false 无法确保有可写扇区
 */
static bool ResumeLog_EnsureWritableSector(void)
{
    // 如果当前扇区可用且有空间，直接使用
    if (g_state.current_sector_id >= 0)
    {
        uint32_t sid = (uint32_t)g_state.current_sector_id;
        if (g_state.sector_present[sid] && g_state.sector_has_space[sid])
        {
            g_state.write_addr = g_state.sector_write_addr[sid];
            return true;
        }
    }

    // 尝试打开一个新的扇区，优先使用没有数据的空扇区，如果没有空扇区则回收最旧的扇区
    for (uint32_t sid = 0; sid < RESUME_LOG_SECTOR_COUNT; ++sid)
    {
        if (!g_state.sector_present[sid])
        {
            uint32_t base = ResumeLog_SectorBaseAddr(sid);
            W25Q64_EraseSector(base / W25Q64_SECTOR_SIZE);
            return ResumeLog_OpenSector(sid, g_state.max_generation + 1U);
        }
    }

    return ResumeLog_ReclaimOldestSector();
}

static bool ResumeLog_WriteRecordToCurrent(uint32_t value, uint32_t seq, uint32_t task_tag, uint32_t total_size)
{
    ResumeLog_Record_t rec;
    uint32_t sid;
    uint32_t end;
    uint32_t addr;

    if (g_state.current_sector_id < 0)
    {
        return false;
    }

    sid = (uint32_t)g_state.current_sector_id;
    end = ResumeLog_SectorEndAddr(sid);
    addr = g_state.sector_write_addr[sid];

    if (addr + sizeof(ResumeLog_Record_t) > end)
    {
        g_state.sector_has_space[sid] = false;
        return false;
    }

    memset(&rec, 0xFF, sizeof(rec));
    rec.magic = RESUME_LOG_MAGIC;
    rec.type = RESUME_LOG_TYPE_PROGRESS;
    rec.len = (uint16_t)(sizeof(uint32_t) * 3U);
    rec.seq = seq;
    rec.value = value;
    rec.task_tag = task_tag;
    rec.total_size = total_size;
    rec.crc = ResumeLog_CalcRecordCrc(&rec);
    rec.commit = 0xFFFFFFFFU;
    rec.reserved = 0xFFFFFFFFU;

    W25Q64_WriteBytes(addr, (const uint8_t *)&rec, sizeof(rec));
    {
        uint32_t commit = RESUME_LOG_COMMIT;
        W25Q64_WriteBytes(addr + offsetof(ResumeLog_Record_t, commit), (const uint8_t *)&commit, sizeof(commit));
    }

    addr += sizeof(ResumeLog_Record_t);
    g_state.sector_write_addr[sid] = addr;
    g_state.write_addr = addr;
    g_state.sector_has_space[sid] = (addr + sizeof(ResumeLog_Record_t) <= end);

    g_state.latest_valid = true;
    g_state.latest_seq = seq;
    g_state.latest_value = value;
    g_state.latest_task_tag = task_tag;
    g_state.latest_total_size = total_size;
    g_state.latest_addr = addr - sizeof(ResumeLog_Record_t);

    return true;
}

void ResumeLog_Init(void)
{
    ResumeLog_Scan();
    g_state.initialized = true;
}

bool ResumeLog_ReadLatestForTask(uint32_t task_tag, uint32_t total_size, uint32_t *value, uint32_t *seq)
{
    int32_t start_idx;

    if (!g_state.initialized)
    {
        ResumeLog_Init();
    }

    start_idx = (int32_t)ResumeLog_BisectRightByGeneration(g_state.max_generation) - 1;
    if (start_idx < 0)
    {
        return false;
    }

    for (int32_t oi = start_idx; oi >= 0; --oi)
    {
        uint32_t sid = g_state.ordered_sector_ids[oi];
        uint32_t start = ResumeLog_SectorRecordStart(sid);
        uint32_t end = g_state.sector_has_space[sid]
            ? g_state.sector_write_addr[sid]
            : ResumeLog_SectorEndAddr(sid);

        if (end <= start)
        {
            continue;
        }

        for (uint32_t addr = end - sizeof(ResumeLog_Record_t); addr >= start; addr -= sizeof(ResumeLog_Record_t))
        {
            ResumeLog_Record_t rec;
            W25Q64_ReadBytes(addr, (uint8_t *)&rec, sizeof(rec));

            if (ResumeLog_IsValidRecord(&rec)
                && rec.task_tag == task_tag
                && rec.total_size == total_size)
            {
                if (value != NULL)
                {
                    *value = rec.value;
                }
                if (seq != NULL)
                {
                    *seq = rec.seq;
                }
                return true;
            }

            if (addr == start)
            {
                break;
            }
        }
    }

    return false;
}

bool ResumeLog_WriteProgress(uint32_t task_tag, uint32_t total_size, uint32_t value, bool force)
{
    bool same_task = false;

    if (!g_state.initialized)
    {
        ResumeLog_Init();
    }

    if (g_state.latest_valid)
    {
        same_task = (g_state.latest_task_tag == task_tag) && (g_state.latest_total_size == total_size);

        if (same_task && value <= g_state.latest_value && !force)
        {
            return true;
        }

        if (same_task && !force)
        {
            uint32_t delta = value - g_state.latest_value;
            if (delta < RESUME_LOG_MIN_DELTA)
            {
                return true;
            }
        }
    }

    if (!ResumeLog_EnsureWritableSector())
    {
        return false;
    }

    return ResumeLog_WriteRecordToCurrent(
        value,
        g_state.latest_valid ? (g_state.latest_seq + 1U) : 0U,
        task_tag,
        total_size);
}

static void ResumeLog_EraseRegion(void)
{
    for (uint32_t addr = RESUME_LOG_START_ADDR; addr < RESUME_LOG_END_ADDR; addr += W25Q64_SECTOR_SIZE)
    {
        uint32_t sector = addr / W25Q64_SECTOR_SIZE;
        W25Q64_EraseSector(sector);
    }
}

void ResumeLog_EraseAll(void)
{
    ResumeLog_EraseRegion();
    ResumeLog_ResetState();
    g_state.initialized = true;
}
