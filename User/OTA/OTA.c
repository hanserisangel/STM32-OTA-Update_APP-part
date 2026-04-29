#include "OTA.h"
#include "ESP32AT.h"
#include "main.h"

static char tx_buffer[512]; // 发送缓冲
OTA_Info_t OTA_Info;
OTA_State_t OTA_State;
static SemaphoreHandle_t Send_Handle; // 用于DMA发送完成的信号量

#define OTA_PROGRESS_SAVE_EVERY_CHUNKS 2U		// 每接收完2个数据块就保存一次进度，平衡保存频率和Flash磨损
#define OTA_RETRY_BACKOFF_MS 3000U

static bool g_ota_session_open = false;

static void OTA_EraseStagingTail(uint32_t payload_offset)
{
	if(payload_offset >= OTA_STAGING_SIZE)
		return;

	uint32_t raw_start = OTA_STAGING_ADDR + payload_offset;
	uint32_t sector_start = (raw_start / W25Q64_SECTOR_SIZE) * W25Q64_SECTOR_SIZE;
	uint32_t keep_len = raw_start - sector_start;
	static uint8_t keep_buf[W25Q64_SECTOR_SIZE];

	// 断点落在扇区中间时，先保留断点前数据，避免续传后前缀被擦成 0xFF。
	if(keep_len > 0U)
	{
		W25Q64_ReadBytes(sector_start, keep_buf, keep_len);
		W25Q64_EraseSector(sector_start / W25Q64_SECTOR_SIZE);
		W25Q64_WriteBytes(sector_start, keep_buf, keep_len);
		sector_start += W25Q64_SECTOR_SIZE;
	}

	for(uint32_t addr = sector_start; addr < (OTA_STAGING_ADDR + OTA_STAGING_SIZE); addr += W25Q64_SECTOR_SIZE)
	{
		W25Q64_EraseSector(addr / W25Q64_SECTOR_SIZE);
	}
}

static bool OTA_ParseHeader(const uint8_t *buf, OTA_Header_t *hdr);

typedef struct {		// OTA包接收上下文
	uint32_t total_received;		// 已接收的总字节数
	uint32_t hdr_received;
	uint32_t meta_received;
	uint32_t payload_received;		// 已接收的固件数据字节数
	uint32_t sig_received;
	OTA_Header_t hdr;
	uint8_t hdr_buf[OTA_HDR_SIZE];
	uint8_t meta_buf[OTA_META_LEN];
	bool header_ready;
} OTA_PkgRxCtx_t;

static void OTA_PkgRxReset(OTA_PkgRxCtx_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

/**
 * @brief  计算版本号字符串的 FNV-1a 哈希值，作为任务标识
 * @param  version: 指向版本号字符串的指针，字符串格式为 "version-x.y"
 * @retval 计算得到的 FNV-1a 哈希值
 */
static uint32_t OTA_VersionTagHash(const uint8_t *version)
{
	uint32_t hash = 2166136261U; // FNV-1a

	for(uint32_t i = 0; i < OTA_VERSION_MAX_LEN && version[i] != '\0'; i++)
	{
		hash ^= version[i];
		hash *= 16777619U;
	}

	return hash;
}

/**
 * @brief  从指定偏移恢复 OTA 包接收上下文，支持续传功能
 * @param  ctx: 指向 OTA 包接收上下文结构体的指针
 * @param  offset: 要恢复的偏移量，单位为字节，表示已经成功接收的数据量
 * @retval true 恢复成功；false 恢复失败
 */
static bool OTA_RestoreRxCtxFromOffset(OTA_PkgRxCtx_t *ctx, uint32_t offset)
{
	OTA_PkgRxReset(ctx);

	if(offset == 0U)
		return true;

	if(offset > OTA_Info.FileSize)
		return false;

	if(offset <= OTA_HDR_SIZE)
	{
		ctx->hdr_received = offset;
		return true;
	}

	W25Q64_ReadBytes(OTA_HDR_ADDR, ctx->hdr_buf, OTA_HDR_SIZE);
	if(!OTA_ParseHeader(ctx->hdr_buf, &ctx->hdr))
		return false;

	if((OTA_HDR_SIZE + OTA_META_LEN + ctx->hdr.fw_size + ctx->hdr.sig_len) != OTA_Info.FileSize)
		return false;

	ctx->hdr_received = OTA_HDR_SIZE;
	ctx->header_ready = true;

	{
		uint32_t remain = offset - OTA_HDR_SIZE;
		uint32_t n;

		n = (remain < OTA_META_LEN) ? remain : OTA_META_LEN;
		ctx->meta_received = n;
		remain -= n;

		n = (remain < ctx->hdr.fw_size) ? remain : ctx->hdr.fw_size;
		ctx->payload_received = n;
		remain -= n;

		n = (remain < ctx->hdr.sig_len) ? remain : ctx->hdr.sig_len;
		ctx->sig_received = n;
		remain -= n;

		if(remain != 0U)
			return false;
	}

	return true;
}

static bool OTA_ParseHeader(const uint8_t *buf, OTA_Header_t *hdr)
{
	hdr->magic = (uint32_t)buf[0] |
		((uint32_t)buf[1] << 8) |
		((uint32_t)buf[2] << 16) |
		((uint32_t)buf[3] << 24);
	hdr->header_size = (uint32_t)buf[4] |
		((uint32_t)buf[5] << 8) |
		((uint32_t)buf[6] << 16) |
		((uint32_t)buf[7] << 24);
	hdr->pkg_type = (uint32_t)buf[8] |
		((uint32_t)buf[9] << 8) |
		((uint32_t)buf[10] << 16) |
		((uint32_t)buf[11] << 24);
	hdr->fw_size = (uint32_t)buf[12] |
		((uint32_t)buf[13] << 8) |
		((uint32_t)buf[14] << 16) |
		((uint32_t)buf[15] << 24);
	hdr->sig_len = (uint32_t)buf[16] |
		((uint32_t)buf[17] << 8) |
		((uint32_t)buf[18] << 16) |
		((uint32_t)buf[19] << 24);

	if(hdr->magic != OTA_HDR_MAGIC || hdr->header_size != OTA_HDR_SIZE)
		return false;
	if(hdr->pkg_type != OTA_PKG_TYPE_FULL && hdr->pkg_type != OTA_PKG_TYPE_DELTA)
		return false;
	if(hdr->fw_size == 0U || hdr->fw_size > OTA_STAGING_SIZE)
		return false;
	if(hdr->sig_len == 0U || hdr->sig_len > OTA_SIG_MAX)
		return false;

	return true;
}

static bool OTA_PkgRouteWrite(OTA_PkgRxCtx_t *ctx, const uint8_t *src, uint32_t len)
{
	while(len > 0U)
	{
		if(ctx->hdr_received < OTA_HDR_SIZE)
		{
			uint32_t left = OTA_HDR_SIZE - ctx->hdr_received;
			uint32_t n = (len < left) ? len : left;
			memcpy(&ctx->hdr_buf[ctx->hdr_received], src, n);
			ctx->hdr_received += n;
			src += n;
			len -= n;

			if(ctx->hdr_received == OTA_HDR_SIZE)
			{
				if(!OTA_ParseHeader(ctx->hdr_buf, &ctx->hdr))
					return false;
				if((OTA_HDR_SIZE + OTA_META_LEN + ctx->hdr.fw_size + ctx->hdr.sig_len) != OTA_Info.FileSize)
					return false;

				W25Q64_WriteBytes(OTA_HDR_ADDR, ctx->hdr_buf, OTA_HDR_SIZE);
				OTA_Info.OTA_type = (uint8_t)ctx->hdr.pkg_type;
				ctx->header_ready = true;
			}
			continue;
		}

		if(ctx->meta_received < OTA_META_LEN)
		{
			uint32_t left = OTA_META_LEN - ctx->meta_received;
			uint32_t n = (len < left) ? len : left;
			memcpy(&ctx->meta_buf[ctx->meta_received], src, n);
			ctx->meta_received += n;
			src += n;
			len -= n;

			if(ctx->meta_received == OTA_META_LEN)
				W25Q64_WriteBytes(OTA_META_ADDR, ctx->meta_buf, OTA_META_LEN);
			continue;
		}

		if(!ctx->header_ready)
			return false;

		if(ctx->payload_received < ctx->hdr.fw_size)
		{
			uint32_t left = ctx->hdr.fw_size - ctx->payload_received;
			uint32_t n = (len < left) ? len : left;
			W25Q64_WriteBytes(OTA_STAGING_ADDR + ctx->payload_received, src, n);
			ctx->payload_received += n;
			src += n;
			len -= n;
			continue;
		}

		if(ctx->sig_received < ctx->hdr.sig_len)
		{
			uint32_t left = ctx->hdr.sig_len - ctx->sig_received;
			uint32_t n = (len < left) ? len : left;
			W25Q64_WriteBytes(OTA_SIG_ADDR + ctx->sig_received, src, n);
			ctx->sig_received += n;
			src += n;
			len -= n;
			continue;
		}

		return false;
	}

	return true;
}

static bool OTA_PkgIsComplete(const OTA_PkgRxCtx_t *ctx)
{
	if(!ctx->header_ready)
		return false;
	if(ctx->meta_received != OTA_META_LEN)
		return false;
	if(ctx->payload_received != ctx->hdr.fw_size)
		return false;
	if(ctx->sig_received != ctx->hdr.sig_len)
		return false;

	return true;
}

static uint8_t OTA_GetCurrentSlotByVTOR(void)
{
	uint32_t vtor = SCB->VTOR;	// 读取当前 VTOR 寄存器的值，获取当前运行的固件所在的地址

	if((vtor >= MCU_FLASH_APP_A_ADDR) && (vtor < (MCU_FLASH_APP_A_ADDR + MCU_FLASH_SLOT_SIZE)))
		return MCU_FLASH_APP_A_SLOT;

	if((vtor >= MCU_FLASH_APP_B_ADDR) && (vtor < (MCU_FLASH_APP_B_ADDR + MCU_FLASH_SLOT_SIZE)))
		return MCU_FLASH_APP_B_SLOT;

	if(OTA_Info.OTA_area == MCU_FLASH_APP_B_SLOT)
		return MCU_FLASH_APP_B_SLOT;

	return MCU_FLASH_APP_A_SLOT;
}

bool OTA_ConfirmBootSuccess(void)
{
	if(OTA_Info.OTA_status != UPDATE)
		return false;

	OTA_Info.OTA_area = OTA_GetCurrentSlotByVTOR();
	OTA_Info.OTA_status = NORMAL;
	OTA_Info.OTA_Flag = 0U;
	W25Q64_WriteOTAInfo();

	LOG_I("Confirm new firmware success, slot=%c",
		(OTA_Info.OTA_area == MCU_FLASH_APP_B_SLOT) ? 'B' : 'A');
	return true;
}

bool OTA_Init(void)
{
	W25Q64_ReadOTAInfo();		// 从 W25Q64 读取 OTA 信息
	OTA_ConfirmBootSuccess(); // 新固件启动后尽快确认，避免被 Bootloader 误回滚

	if(!ESP32AT_Init())
		return false;
	
	if(!ESP32AT_WiFiConnect(SSID, PASSWORD))
		return false;
	
	OTA_State = OTA_START;
	return true;
}

static bool OTA_Start(const char *url)
{
    // 创建二值信号量
	if(Send_Handle == NULL)
	{
		Send_Handle = xSemaphoreCreateBinary();
	}
	// 时间同步
	if(!ESP32AT_SNTP_GetTime())
		return false;
	
	// TCP 连接
	char *tx_buffer = rx_buffer;
	snprintf(tx_buffer, sizeof(rx_buffer), "AT+CIPSTART=\"TCP\",\"%s\",80", url);
	if(!ESP32AT_SendCommand(tx_buffer, 2000))
		return false;
	
	// 开透传
	if(!ESP32AT_SendCommand("AT+CIPMODE=1", 2000))
		return false;
	
	// 开启数据发送模式
	if(!ESP32AT_SendCommand("AT+CIPSEND", 2000))
		return false;

	g_ota_session_open = true;
	
	return true;
}

/**
 * @brief  解析 POST 响应
 * @note   函数通过简单的字符串查找和解析来提取响应中的 "code" 字段，
 * 			并判断其值是否为 0 来确定 POST 请求是否成功。
 * @param  json: 服务器返回的 JSON 格式响应字符串
 * @retval bool: true 表示 POST 请求成功，false 表示失败
 */
static bool ESP32AT_ParsePOST(const char* json)
{
	// 简单的字符串查找和解析
	const char *code_ptr = strstr(json, "\"code\":");

	if(code_ptr)
	{
		int code;
		sscanf(code_ptr, "\"code\":%d", &code);

		if(code == 0) return true;
	}

	return false;
}

/**
 * @brief  解析 GET 响应
 * @note   函数通过简单的字符串查找和解析来提取响应中的 "target" 字段，
 * 			并判断其值是否与当前版本号相同，来确定 GET 请求是否成功。
 * @param  json: 服务器返回的 JSON 格式响应字符串
 * @param  tid: 输出参数，用于存储解析出的 tid 值
 * @retval bool: true 表示 GET 请求成功，false 表示失败
 */
static bool ESP32AT_ParseGET(const char* json, char tid[])
{
	// 简单的字符串查找和解析
	const char *version_ptr = strstr(json, "\"target\":");	// 版本号
	const char *tid_ptr = strstr(json, "\"tid\":");		// 升级任务ID
	const char *size_ptr = strstr(json, "\"size\":");	// 待升级文件大小
	
	// 注意不要栈溢出
	if(tid_ptr && size_ptr && version_ptr)
	{
		char temp_version[32]= {0};
		sscanf(version_ptr, "\"target\":\"%30[^\"]", temp_version);
		sscanf(tid_ptr, "\"tid\":%7[^,]", tid);
		sscanf(size_ptr, "\"size\":%u", &OTA_Info.FileSize);
		
		if(strcmp(temp_version, (char*)OTA_Info.OTA_version) != 0)
		{
			// 版本不同，说明有升级任务
			memset(OTA_Info.OTA_version, 0, OTA_VERSION_MAX_LEN);
			strncpy((char*)OTA_Info.OTA_version, temp_version, OTA_VERSION_MAX_LEN - 1);
			return true;
		}
	}

	return false;
}

/**
 * @brief  处理 OTA 流程（主要）
 * @note   函数实现了一个简单的状态机来处理 OTA 流程，包括建立 TCP 连接、上报版本号、
 * 			检测升级任务、分片下载新用户程序等步骤。每个状态对应一个具体的操作，
 * 			并根据操作结果来决定下一步的状态转换。
 * @param  None
 */
void OTA_Process(void)
{
	char tid[10];
	static char small_buf[128];
	int body_len = 0;
	// bool OTA_Timer = false;

	switch(OTA_State)
	{
		case OTA_START:
			// 建立 TCP 连接
			if(OTA_Start(iot_url))
			{
				OTA_State = POST_VERSION;		// 连接成功，进入下一个状态
			}
			else{
				OTA_State = OTA_END;			// 连接失败，进入结束状态
			}
			vTaskDelay(pdMS_TO_TICKS(100));		// 延时，避免过快进入下一个状态
			break;

		case POST_VERSION:
			// 上报初始版本号
			memset(small_buf, 0, sizeof(small_buf));
			// 版本号
			snprintf(small_buf, sizeof(small_buf), "{\"s_version\":\"%s\",\"f_version\":\"%s\"}", 
				(char*)OTA_Info.OTA_version, (char*)OTA_Info.OTA_version);
			
			body_len = strlen(small_buf);

			// 构建完整 HTTP 请求
			snprintf(tx_buffer, sizeof(tx_buffer),
				"POST /fuse-ota/%s/%s/version HTTP/1.1\r\n"
				"Content-Type:application/json\r\n"
				"Authorization:%s\r\n"
				"Host:iot-api.heclouds.com\r\n"
				"Content-Length:%d\r\n"
				"\r\n"
				"%s"
				"\r\n",
				Product_ID, Device_ID, Token, body_len, small_buf);
			
			LOG_I("%s", tx_buffer);

			HAL_UART_Transmit_DMA(&huart1, (uint8_t*)tx_buffer, strlen(tx_buffer));
            xSemaphoreTake(Send_Handle, portMAX_DELAY);

			if(OTA_ReceivedataEx(8000, NULL) && ESP32AT_ParsePOST(data)) // 接收数据到缓冲区
			{
				OTA_State = GET_UPDATE;		// 上报成功，进入下一个状态
			}
			else{
				OTA_State = OTA_END;		// 上报失败，进入结束状态
			}
			vTaskDelay(pdMS_TO_TICKS(100));
			break;

		case GET_UPDATE:
			// 检测升级任务
			vTaskDelay(pdMS_TO_TICKS(1000));
			memset(tx_buffer, 0, sizeof(tx_buffer));

			snprintf(tx_buffer, sizeof(tx_buffer),
				"GET /fuse-ota/%s/%s/check?type=2&version=%s HTTP/1.1\r\n"
				"Content-Type:application/json\r\n"
				"Authorization:%s\r\n"
				"Host:iot-api.heclouds.com\r\n"
				"\r\n",
				Product_ID, Device_ID, (char*)OTA_Info.OTA_version, Token);

			LOG_I("%s", tx_buffer);
			HAL_UART_Transmit_DMA(&huart1, (uint8_t*)tx_buffer, strlen(tx_buffer));
            xSemaphoreTake(Send_Handle, portMAX_DELAY);

			if(OTA_ReceivedataEx(8000, NULL) && ESP32AT_ParseGET(data, tid))  // 接收数据到缓冲区
			{
				OTA_State = GET_DOWNLOAD;
			}
			else{
				OTA_State = GET_UPDATE; // 循环检测升级任务
			}
			break;

		case GET_DOWNLOAD:	// 分片下载新用户程序
			memset(tx_buffer, 0, sizeof(tx_buffer));
			memset(small_buf, 0, sizeof(small_buf));
			{
				OTA_PkgRxCtx_t rx_ctx;
				uint16_t body_len = 0;
				uint32_t task_tag = OTA_VersionTagHash(OTA_Info.OTA_version);	// 计算当前版本的任务标识，用于断点续传日志
				uint32_t resume_offset = 0;			// 续传偏移量，表示已经成功接收并写入的字节数
				uint32_t seq = 0;					// 续传日志序列号，用于区分不同的写入记录，确保日志的正确性
				uint32_t chunk_count = 0;
				bool hdr_forced = false;			// 是否已经强制保存过头部接收进度，确保至少保存一次，即使下载过程中没有达到定期保存的条件
				bool meta_forced = false;			// 是否已经强制保存过元数据接收进度，确保至少保存一次，即使下载过程中没有达到定期保存的条件

				ResumeLog_Init();
				// 读取到有效的续传记录，获取上次的接收进度和序列号
				if(ResumeLog_ReadLatestForTask(task_tag, OTA_Info.FileSize, &resume_offset, &seq))
				{
					// 如果续传偏移量超过了文件大小，说明记录无效，重置续传状态
					if(resume_offset >= OTA_Info.FileSize)	
					{
						resume_offset = 0U;
						ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, 0U, true);
					}
				}
				else
				{
					resume_offset = 0U;
				}
				
				// 如果续传偏移量为0，说明没有有效的续传记录，需要擦除 OTA 相关的存储区域，为新的 OTA 流程做好准备
				if(resume_offset == 0U)
				{
					for(uint32_t addr = OTA_STAGING_ADDR; addr < (OTA_STAGING_ADDR + OTA_STAGING_SIZE); addr += W25Q64_SECTOR_SIZE)
					{
						W25Q64_EraseSector(addr / W25Q64_SECTOR_SIZE);
					}
					W25Q64_EraseSector(OTA_META_ADDR / W25Q64_SECTOR_SIZE);
					W25Q64_EraseSector(OTA_HDR_ADDR / W25Q64_SECTOR_SIZE);
					ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, 0U, true);
				}

				// 尝试从续传偏移量恢复 OTA 包接收上下文，如果恢复失败，说明续传记录无效，需要擦除 OTA 相关的存储区域，并重置续传状态
				if(!OTA_RestoreRxCtxFromOffset(&rx_ctx, resume_offset))
				{
					LOG_W("Resume context invalid, fallback full download");
					for(uint32_t addr = OTA_STAGING_ADDR; addr < (OTA_STAGING_ADDR + OTA_STAGING_SIZE); addr += W25Q64_SECTOR_SIZE)
					{
						W25Q64_EraseSector(addr / W25Q64_SECTOR_SIZE);
					}
					W25Q64_EraseSector(OTA_META_ADDR / W25Q64_SECTOR_SIZE);
					W25Q64_EraseSector(OTA_HDR_ADDR / W25Q64_SECTOR_SIZE);
					ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, 0U, true);
					resume_offset = 0U;
					OTA_PkgRxReset(&rx_ctx);
				}

				// 防止“日志回退而尾部旧数据未清理”导致的哈希不一致。
				// 对于续传场景，先擦除 payload 断点之后的尾部区域，再继续写入。
				if(rx_ctx.payload_received < OTA_STAGING_SIZE)
				{
					OTA_EraseStagingTail(rx_ctx.payload_received);
					LOG_D("Erased staging tail from offset %lu", (unsigned long)rx_ctx.payload_received);
				}

				rx_ctx.total_received = resume_offset;

				for(uint32_t start = resume_offset; start < OTA_Info.FileSize; start += body_len)
				{
					uint32_t end = start + PackageSize - 1U;
					uint32_t expect_len;

					if(end >= OTA_Info.FileSize)
						end = OTA_Info.FileSize - 1U;
					expect_len = end - start + 1U;

					snprintf(small_buf, sizeof(small_buf), "%lu-%lu", (unsigned long)start, (unsigned long)end);
					snprintf(tx_buffer, sizeof(tx_buffer),
						"GET /fuse-ota/%s/%s/%s/download HTTP/1.1\r\n"
						"Content-Type:application/json\r\n"
						"Authorization:%s\r\n"
						"Host:iot-api.heclouds.com\r\n"
						"Range:bytes=%s\r\n"
						"\r\n",
						Product_ID, Device_ID, tid, Token, small_buf);

					LOG_I("OTA recv %lu/%lu", (unsigned long)start, (unsigned long)OTA_Info.FileSize);

					HAL_UART_Transmit_DMA(&huart1, (uint8_t*)tx_buffer, strlen(tx_buffer));
					xSemaphoreTake(Send_Handle, portMAX_DELAY);

					if(OTA_ReceivedataEx(8000, &body_len))
					{
						if(body_len != expect_len)
						{
							LOG_E("OTA body len mismatch: exp=%lu got=%u", (unsigned long)expect_len, body_len);
							OTA_State = OTA_END;
							break;
						}

						if(!OTA_PkgRouteWrite(&rx_ctx, (const uint8_t*)data, body_len))
						{
							LOG_E("OTA package route write failed");
							OTA_State = OTA_END;
							ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, rx_ctx.total_received, true);
							break;
						}

						rx_ctx.total_received += body_len;
						chunk_count++;

						if(!hdr_forced && rx_ctx.hdr_received == OTA_HDR_SIZE)
						{
							ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, rx_ctx.total_received, true);
							hdr_forced = true;
						}

						if(!meta_forced && rx_ctx.meta_received == OTA_META_LEN)
						{
							ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, rx_ctx.total_received, true);
							meta_forced = true;
						}

						if((chunk_count % OTA_PROGRESS_SAVE_EVERY_CHUNKS) == 0U)
						{
							ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, rx_ctx.total_received, false);
						}
					}
					else
					{
						OTA_State = OTA_END;
						ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, rx_ctx.total_received, true);
						break;
					}
				}

				if(OTA_State == OTA_END)
					break;

				if(rx_ctx.total_received != OTA_Info.FileSize || !OTA_PkgIsComplete(&rx_ctx))
				{
					LOG_E("OTA package incomplete");
					OTA_State = OTA_END;
					break;
				}

				OTA_Info.OTA_Flag = OTA_FLAG;
				W25Q64_WriteOTAInfo();
				ResumeLog_WriteProgress(task_tag, OTA_Info.FileSize, OTA_Info.FileSize, true);

				vTaskDelay(pdMS_TO_TICKS(500));
				NVIC_SystemReset();
			}

			break;

		case OTA_END:
			if(g_ota_session_open)
			{
				HAL_UART_Transmit(&huart1, "+++", 3, HAL_MAX_DELAY);
				ESP32AT_SendCommand("", 200);
				ESP32AT_SendCommand("AT+CIPMODE=0", 200);
				ESP32AT_SendCommand("AT+CIPCLOSE",200);
				g_ota_session_open = false;
			}

			// 网络异常时避免 OTA_START/OTA_END 高频抖动。
			vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_BACKOFF_MS));
			OTA_State = OTA_START;
			break;
	}
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART1)
	{
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;

		// 释放信号量
		xSemaphoreGiveFromISR(Send_Handle, &xHigherPriorityTaskWoken);

		// 任务切换
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}
