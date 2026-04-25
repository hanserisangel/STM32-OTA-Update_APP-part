#ifndef __OTA_H__
#define __OTA_H__

#include <stdint.h>
#include <stdbool.h>

#define MCU_FLASH_BASE_ADDRESS      0x08000000U
#define MCU_FLASH_TOTAL_SIZE       1024U         // 总共 1MB 的Flash
#define MCU_FLASH_B_PAGE_SIZE      32U         // flash 的B区大小为 32KB
#define MCU_FLASH_A_PAGE_SIZE      (MCU_FLASH_TOTAL_SIZE - MCU_FLASH_B_PAGE_SIZE) // 剩下的都是A区
#define MCU_FLASH_A_START_PAGE    2U      // A区从第2页开始
#define MCU_FLASH_A_START_ADDRESS  (MCU_FLASH_BASE_ADDRESS + MCU_FLASH_B_PAGE_SIZE * 1024U) // A区起始地址

#define iot_url "183.230.40.33"
#define Product_ID "qvt81NOIxc"
#define Device_ID "microwave"
#define Token "version=2018-10-31&res=products%2Fqvt81NOIxc%2Fdevices%2Fmicrowave&et=1802007194&method=md5&sign=cI8Xo%2BL9Ef3vNNTRtYNufQ%3D%3D"
#define SSID "yousa"
#define PASSWORD "yanxbxpwxz32qkj"
#define OTA_FLAG                  0xAABB1122

#define MCU_FLASH_APP_A_SLOT        0U
#define MCU_FLASH_APP_B_SLOT        1U
#define MCU_FLASH_APP_A_ADDR        (FLASH_BASE + 0x20000U) // A区起始地址
#define MCU_FLASH_APP_B_ADDR        (FLASH_BASE + 0x80000U) // B区起始地址
#define MCU_FLASH_APP_A_SECTOR      5U
#define MCU_FLASH_APP_A_COUNT       3U
#define MCU_FLASH_APP_B_SECTOR      8U
#define MCU_FLASH_APP_B_COUNT       3U
#define MCU_FLASH_SLOT_SIZE         (384U * 1024U)

#define OTA_HDR_MAGIC             0x4F544148U
#define OTA_HDR_SIZE              20U
#define OTA_SIG_MAX               96U
#define OTA_PUBKEY_LEN            91U    // P-256 公钥长度为 64 字节，但可能会有额外的头部信息，预留 91 字节
#define OTA_PKG_TYPE_FULL         0U
#define OTA_PKG_TYPE_DELTA        1U

#define OTA_ECDH_PUB_LEN           65U
#define OTA_SALT_LEN               16U
#define OTA_IV_LEN                 16U
#define OTA_META_LEN               (OTA_ECDH_PUB_LEN + OTA_SALT_LEN + OTA_IV_LEN)
#define OTA_VERSION_MAX_LEN         12

// W25Q64 分区表
/* 第一扇区 0~4KB */
#define OTA_PUBKEY_ADDR           0U
/* 第二扇区 4KB~8KB */
#define OTA_INFO_ADDR             (OTA_PUBKEY_ADDR + 4096U)
/* 第三扇区 8KB~12KB */
#define OTA_META_ADDR             (OTA_INFO_ADDR + 4096U)
/* 第四扇区 12KB~16KB */
#define OTA_HDR_ADDR              (OTA_META_ADDR + 4096U)
#define OTA_SIG_ADDR              (OTA_HDR_ADDR + OTA_HDR_SIZE) 
/* 第五扇区到第二块 16KB~64KB */
#define OTA_HEAT_ADDR             (OTA_HDR_ADDR + 4096U) 

/* 第二块*/
#define OTA_STAGING_ADDR          0x100000U
#define OTA_STAGING_SIZE          (384U * 1024U)
#define OTA_TUZ_DICT_MAX          (2U * 1024U)
#define OTA_TUZ_CACHE_SIZE        1024U

// OTA header 结构体定义
typedef struct{
  uint32_t magic;       // OTA_HDR_MAGIC
  uint32_t header_size; // OTA_HDR_SIZE
  uint32_t pkg_type;    // 0: full, 1: delta
  uint32_t fw_size;     // firmware size in bytes
  uint32_t sig_len;     // signature length in bytes
}OTA_Header_t;

typedef enum {
  UPDATE = 0,      // OTA包已接收但未验证
  NORMAL,
  FAIL
}OTA_status_t;

typedef struct{
  uint32_t OTA_Flag;
  uint32_t FileSize;        // 服务器下发的整个应用程序的大小（字节）
  uint8_t OTA_version[OTA_VERSION_MAX_LEN];  // OTA 版本号，字符串数组，格式: version-1.0
  uint8_t OTA_area;          // 0 表示 A 区，1 表示 B 区
  uint8_t OTA_type;          // 0 表示全量更新，1 表示增量更新
  OTA_status_t OTA_status;        // OTA 状态，用来自动回滚
}OTA_Info_t;
extern OTA_Info_t OTA_Info;

#define PackageSize 512
// OTA状态机
typedef enum{
  OTA_START,    // 开始OTA流程
  POST_VERSION, // 上报当前版本号
  GET_UPDATE,   // 检测是否有升级任务
  GET_DOWNLOAD, // 分片下载新用户程序
  OTA_END,      // OTA流程结束，成功或失败
}OTA_State_t;
extern OTA_State_t OTA_State;

bool OTA_Init(void);
void OTA_Process(void);
bool OTA_ConfirmBootSuccess(void);

#endif /* __OTA_H__ */
