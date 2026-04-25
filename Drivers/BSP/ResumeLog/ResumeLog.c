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

// 为了降低 Flash 磨损，仅当进度至少前进该步长时才落盘。
// 如需每次都写，设置为 1。
#define RESUME_LOG_MIN_DELTA      1024U

#if (RESUME_LOG_START_ADDR >= RESUME_LOG_END_ADDR)
#error "ResumeLog region invalid: start address must be less than end address"
#endif

#if (RESUME_LOG_END_ADDR > W25Q64_TOTAL_SIZE)
#error "ResumeLog region invalid: end address out of W25Q64 range"
#endif

#pragma pack(push, 1)   // 确保结构体按 1 字节对齐，避免编译器添加填充字节
typedef struct  // 日志记录结构体
{
    uint32_t magic;     // 固定值 0xA5A5A5A5U，用于快速判断记录的有效性
    uint16_t type;      // 记录类型
    uint16_t len;       // 记录长度
    uint32_t seq;       // 序列号
    uint32_t value;     // 值
    uint32_t task_tag;  // 任务标识（当前使用版本号哈希）
    uint32_t total_size;// 任务总大小
    uint32_t crc;       // 校验和
    uint32_t commit;    // 提交标志，0x5A5A5A5AU 表示记录已提交，0xFFFFFFFFU 表示未提交
    uint32_t reserved;  // 保留字段
} ResumeLog_Record_t;
#pragma pack(pop)   // 恢复默认对齐方式

typedef struct  // 全局状态变量：最新有效记录
{
    uint32_t write_addr;        // 下一条记录的写入地址
    uint32_t latest_addr;       // 地址

    uint32_t latest_seq;        // 序列号
    uint32_t latest_value;      // 值
    uint32_t latest_task_tag;   // 所属任务标识
    uint32_t latest_total_size; // 所属任务总大小

    bool latest_valid;          // 是否存在
    bool initialized;           // 是否已经初始化
} ResumeLog_State_t;

static ResumeLog_State_t g_state = {0};

static bool ResumeLog_WriteRecord(uint32_t value, uint32_t seq, uint32_t task_tag, uint32_t total_size);
static void ResumeLog_RecycleAll(void);
static bool ResumeLog_IsBlank(const uint8_t *data, uint32_t len);

static uint32_t ResumeLog_GetSlotCount(void)
{
    return RESUME_LOG_SIZE / (uint32_t)sizeof(ResumeLog_Record_t);
}

static uint32_t ResumeLog_SlotToAddr(uint32_t slot)
{
    return RESUME_LOG_START_ADDR + slot * (uint32_t)sizeof(ResumeLog_Record_t);
}

static bool ResumeLog_IsBlankSlot(uint32_t slot)
{
    ResumeLog_Record_t rec;
    uint32_t addr = ResumeLog_SlotToAddr(slot);
    W25Q64_ReadBytes(addr, (uint8_t *)&rec, sizeof(rec));
    return ResumeLog_IsBlank((const uint8_t *)&rec, sizeof(rec));
}

// 在追加写模型下，空槽满足单调性：前面非空，后面全空。
// 使用二分查找定位首个空槽，若不存在空槽则返回 slot_count。
static uint32_t ResumeLog_FindFirstBlankSlot(void)
{
    uint32_t slot_count = ResumeLog_GetSlotCount();
    uint32_t low = 0;
    uint32_t high = slot_count;

    while (low < high)
    {
        uint32_t mid = low + ((high - low) >> 1U); // 避免溢出，向上取中
        if (ResumeLog_IsBlankSlot(mid))
        {
            high = mid;
        }
        else
        {
            low = mid + 1U;
        }
    }

    return low;
}

/**
 * @brief  写入进度记录的内部函数
 * @param  value: 要写入的进度值
 * @param  force: 是否强制写入，即使进度没有明显前进
 * @param  task_tag: 任务标识，用于区分不同的任务
 * @param  total_size: 任务总大小，用于日志记录和验证
 * @retval true: 写入成功；false: 写入失败
 */
static bool ResumeLog_WriteProgressInternal(uint32_t value, bool force, uint32_t task_tag, uint32_t total_size)
{
    uint32_t next_seq = 0;
    bool same_task = false;

    if (!g_state.initialized)
    {
        ResumeLog_Init();
    }

    if (g_state.latest_valid)
    {
        same_task = (g_state.latest_task_tag == task_tag) && (g_state.latest_total_size == total_size);

        // 如果是同一任务，并且新值没有前进，且没有强制写入，则直接返回成功，避免不必要的写入操作
        if (same_task && value <= g_state.latest_value && !force)
        {
            return true;
        }

        // 如果是同一任务，并且新值前进但没有达到最小步长，避免不必要的写入操作
        if (same_task && !force)
        {
            uint32_t delta = value - g_state.latest_value;
            if (delta < RESUME_LOG_MIN_DELTA)
            {
                return true;
            }
        }

        next_seq = g_state.latest_seq + 1U;
    }

    // 如果当前写入地址已经接近日志区域的末尾，无法容纳下一条记录，则回收日志区域。
    if (g_state.write_addr + sizeof(ResumeLog_Record_t) > RESUME_LOG_END_ADDR)
    {
        ResumeLog_RecycleAll();
        next_seq = g_state.latest_valid ? (g_state.latest_seq + 1U) : 0U;
    }

    return ResumeLog_WriteRecord(value, next_seq, task_tag, total_size);
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

/**
 * @brief  计算记录的 CRC32 校验值
 * @param  rec: 指向要计算 CRC 的记录结构体的指针
 * @retval 计算得到的 CRC32 校验值
 */
static uint32_t ResumeLog_CalcRecordCrc(const ResumeLog_Record_t *rec)
{
    return ResumeLog_Crc32((const uint8_t *)rec, (uint32_t)offsetof(ResumeLog_Record_t, crc));
}   // offsetof(ResumeLog_Record_t, crc)，确保计算 CRC 时不包含 crc 字段本身

/**
 * @brief  检查数据是否全为 0xFF，判断是否为擦除状态
 * @param  data: 指向要检查的数据的指针
 * @param  len: 数据的长度（以字节为单位）
 * @retval true 表示擦除状态；false 表示不是擦除状态
 */
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

/**
 * @brief  检查记录的有效性
 * @param  rec: 指向要验证的记录结构体的指针
 * @retval true 记录有效；false 记录无效
 */
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
 * @brief  扫描日志区域，找到最新有效的记录并更新全局状态
 * @retval None
 */
static void ResumeLog_Scan(void)
{
    uint32_t first_blank_slot = ResumeLog_FindFirstBlankSlot();
    uint32_t first_blank_addr = (first_blank_slot < ResumeLog_GetSlotCount())
        ? ResumeLog_SlotToAddr(first_blank_slot)
        : RESUME_LOG_END_ADDR;

    g_state.latest_valid = false;
    g_state.latest_seq = 0;
    g_state.latest_value = 0;
    g_state.latest_task_tag = 0;
    g_state.latest_total_size = 0;
    g_state.latest_addr = 0xFFFFFFFFU;
    
    // 只遍历 [start, first_blank) 区间，减少冷启动扫描时间。
    for (uint32_t addr = RESUME_LOG_START_ADDR;
         addr < first_blank_addr;
         addr += sizeof(ResumeLog_Record_t))
    {
        ResumeLog_Record_t rec;
        W25Q64_ReadBytes(addr, (uint8_t *)&rec, sizeof(rec));

        // 检查记录的有效性
        if (ResumeLog_IsValidRecord(&rec))
        {
            // 如果记录有效，并且序列号比当前最新的还大，更新全局状态为最新记录的信息
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

    // 设置下一条记录的写入地址，如果找到空白地址则使用空白地址，否则从日志区域末尾开始覆盖旧记录
    if (first_blank_addr != RESUME_LOG_END_ADDR)
    {
        g_state.write_addr = first_blank_addr;
    }
    else
    {
        g_state.write_addr = RESUME_LOG_END_ADDR;
    }
}

/**
 * @brief  擦除日志区域，清除所有记录
 * @retval None
 */
static void ResumeLog_EraseRegion(void)
{
    for (uint32_t addr = RESUME_LOG_START_ADDR; addr < RESUME_LOG_END_ADDR; addr += W25Q64_SECTOR_SIZE)
    {
        uint32_t sector = addr / W25Q64_SECTOR_SIZE;
        W25Q64_EraseSector(sector);
    }
}

/**
 * @brief  回收日志区域，擦除所有记录并保留最新有效记录（如果存在）
 * @retval None
 */
static void ResumeLog_RecycleAll(void)
{
    bool keep_latest = g_state.latest_valid;
    uint32_t keep_value = g_state.latest_value;
    uint32_t keep_seq = g_state.latest_seq;
    uint32_t keep_task_tag = g_state.latest_task_tag;
    uint32_t keep_total_size = g_state.latest_total_size;

    ResumeLog_EraseRegion();

    g_state.latest_valid = false;
    g_state.latest_seq = 0;
    g_state.latest_value = 0;
    g_state.latest_task_tag = 0;
    g_state.latest_total_size = 0;
    g_state.latest_addr = 0xFFFFFFFFU;
    g_state.write_addr = RESUME_LOG_START_ADDR;

    if (keep_latest)
    {
        (void)ResumeLog_WriteRecord(keep_value, keep_seq + 1U, keep_task_tag, keep_total_size);
    }
}

/**
 * @brief  写入一条记录到日志区域
 * @retval true 写入成功；false 写入失败
 */
static bool ResumeLog_WriteRecord(uint32_t value, uint32_t seq, uint32_t task_tag, uint32_t total_size)
{
    ResumeLog_Record_t rec;

    memset(&rec, 0xFF, sizeof(rec));
    rec.magic = RESUME_LOG_MAGIC;
    rec.type = RESUME_LOG_TYPE_PROGRESS;
    rec.len = (uint16_t)(sizeof(uint32_t) * 3U);    // 记录长度为 value、task_tag 和 total_size 三个字段的大小之和
    rec.seq = seq;

    rec.value = value;
    rec.task_tag = task_tag;
    rec.total_size = total_size;

    rec.crc = ResumeLog_CalcRecordCrc(&rec);
    rec.commit = 0xFFFFFFFFU;       // 先写入未提交状态
    rec.reserved = 0xFFFFFFFFU;

    // 将记录写入 Flash，先写入未提交状态，等数据写入完成后再更新为已提交状态，确保记录的原子性
    W25Q64_WriteBytes(g_state.write_addr, (const uint8_t *)&rec, sizeof(rec));
    {
        uint32_t commit = RESUME_LOG_COMMIT;
        W25Q64_WriteBytes(g_state.write_addr + offsetof(ResumeLog_Record_t, commit),
                          (const uint8_t *)&commit,
                          sizeof(commit));
    }

    // 更新全局状态为最新记录的信息，以便后续读取和写入操作使用
    g_state.latest_valid = true;
    g_state.latest_seq = seq;
    g_state.latest_value = value;
    g_state.latest_task_tag = task_tag;
    g_state.latest_total_size = total_size;
    g_state.latest_addr = g_state.write_addr;
    g_state.write_addr += sizeof(ResumeLog_Record_t);

    return true;
}

/**
 * @brief  日志系统初始化函数，扫描日志区域以找到最新有效的记录，并更新全局状态（掉电恢复）
 * @retval None
 */
void ResumeLog_Init(void)
{
    ResumeLog_Scan();
    g_state.initialized = true;
}

/**
 * @brief  读取特定任务的最新有效记录的值和序列号
 * @param  task_tag: 任务标识，用于区分不同的任务，当前使用版本号的哈希值作为标识
 * @param  total_size: 任务的总大小，用于验证记录的有效性
 * @param  value: 要写入的进度值
 * @param  seq: 要写入的序列号
 * @retval true 写入成功；false 写入失败
 */
bool ResumeLog_ReadLatestForTask(uint32_t task_tag, uint32_t total_size, uint32_t *value, uint32_t *seq)
{
    if (!g_state.initialized)
    {
        ResumeLog_Init();
    }

    if (!g_state.latest_valid)
    {
        return false;
    }

    if (g_state.latest_task_tag != task_tag || g_state.latest_total_size != total_size)
    {
        return false;
    }

    // 如果调用者提供了 value 和 seq 的指针，则将最新记录的值和序列号写入对应的变量中，供调用者使用
    if (value != NULL)
    {
        *value = g_state.latest_value;
    }
    if (seq != NULL)
    {
        *seq = g_state.latest_seq;
    }
    return true;
}

/**
 * @brief  普通写入进度记录到日志区域（主要接口）
 * @param  task_tag: 任务标识，用于区分不同的任务，当前使用版本号的哈希值作为标识
 * @param  total_size: 任务的总大小，用于验证记录的有效性
 * @param  value: 要写入的进度值
 * @retval true 写入成功；false 写入失败
 */
bool ResumeLog_WriteProgressForTask(uint32_t task_tag, uint32_t total_size, uint32_t value)
{
    return ResumeLog_WriteProgressInternal(value, false, task_tag, total_size);
}

/**
 * @brief  强制写入进度记录到日志区域（主要接口）
 * @param  task_tag: 任务标识，用于区分不同的任务，当前使用版本号的哈希值作为标识
 * @param  total_size: 任务的总大小，用于验证记录的有效性
 * @param  value: 要写入的进度值
 * @retval true 写入成功；false 写入失败
 */
bool ResumeLog_WriteProgressForceForTask(uint32_t task_tag, uint32_t total_size, uint32_t value)
{
    return ResumeLog_WriteProgressInternal(value, true, task_tag, total_size);
}

/**
 * @brief  擦除日志区域，清除所有记录
 * @note    通过擦除日志区域的所有扇区，清除所有记录，并重置全局状态以反映没有有效记录的状态
 * @retval None
 */
void ResumeLog_EraseAll(void)
{
    ResumeLog_EraseRegion();

    g_state.latest_valid = false;
    g_state.latest_seq = 0;
    g_state.latest_value = 0;
    g_state.latest_task_tag = 0;
    g_state.latest_total_size = 0;
    g_state.latest_addr = 0xFFFFFFFFU;
    g_state.write_addr = RESUME_LOG_START_ADDR;
    g_state.initialized = true;
}
