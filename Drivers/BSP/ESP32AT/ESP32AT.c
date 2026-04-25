#include "usart.h"
#include "ESP32AT.h"
#include "main.h"

char rx_buffer[BUFFER_SIZE];
char data[BUFFER_SIZE];

static ack_t ESP32AT_ReceiveAck(uint32_t timeout_ms);

static const ack_string_t at_ack_matches[] = 
{
    {AT_ACK_OK, "OK\r\n"},
    {AT_ACK_ERROR, "ERROR\r\n"},
    {AT_ACK_BUSY, "busy p…\r\n"},
    {AT_ACK_READY, "ready\r\n"},
};

bool ESP32AT_ForceRecoverLink(void)
{
	// 断电/复位后，ESP32 可能仍停留在透传模式。
	// 这里用“裸 +++”尝试退出透传，然后关闭透传和 TCP 连接。
	HAL_Delay(1000);
	HAL_UART_Transmit(&huart1, (uint8_t *)"+++", 3, HAL_MAX_DELAY);
	HAL_Delay(1000);

	(void)ESP32AT_SendCommand("AT+CIPMODE=0", 500);
	(void)ESP32AT_SendCommand("AT+CIPCLOSE", 500);
	(void)ESP32AT_SendCommand("AT", 200);

	return true;
}

bool ESP32AT_Init(void)
{
	ESP32AT_ForceRecoverLink();

	ESP32AT_SendCommand("AT", 100); 		// 跳过模块不稳定期
	if(!ESP32AT_SendCommand("AT", 100))
		return false;
	
	// if(!ESP32AT_SendCommand("AT+RESTORE", 5000)) // 恢复出厂设置
	// 	return false;
	
	// if(ESP32AT_ReceiveAck(5000) != AT_ACK_READY) // 等待模块准备就绪
	// 	return false;
	
	LOG_I("[ESP32] AT Init OK.");
	return true;
}

bool ESP32AT_SendCommand(const char *cmd, uint32_t timeout_ms)
{
	LOG_D("[ESP32] Sending Command: %s", cmd);
	
	HAL_UART_Transmit(&huart1, (uint8_t *)cmd, strlen(cmd), HAL_MAX_DELAY);
	HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n", 2, HAL_MAX_DELAY);

	ack_t ack = ESP32AT_ReceiveAck(timeout_ms);

	LOG_D("[ESP32] Received: %s", rx_buffer);

	return (ack == AT_ACK_OK);
}

/**
 * @brief  接收 AT 指令响应并匹配预定义的 ACK 类型
 * @param  timeout_ms:  单次等待超时时间 (单位: ms)。
 * @retval ack_t: 匹配到的 ACK 类型，或 AT_ACK_NONE 表示未匹配到任何预定义响应
 * @note   该函数会持续轮询接收缓冲区，直到匹配到预定义的响应字符串或超时。
 * 			它会自动处理换行符，并将每行数据与预定义的响应进行比较。
 */
static ack_t ESP32AT_ReceiveAck(uint32_t timeout_ms)
{
	uint32_t start_tick = xTaskGetTickCount();
	uint32_t rx_len = 0;
	rx_buffer[0] = '\0'; // 防止未初始化使用
	// const char *line = rx_buffer;
	// uint32_t last_len = 0;

	while((xTaskGetTickCount() - start_tick) < timeout_ms)
	{
		uint16_t size = RingBuffer_Get(uart_rb, (uint8_t*)rx_buffer);
		if(size)
		{
			rx_len = 0;
			// last_len = 0;
			while(rx_len < size)
			{
				if(rx_buffer[rx_len ++] == '\n')
				{
					// 每次接收到一行（以 \n 结尾），就将其与预定义的响应进行比较
					for (uint8_t i = 0; i < sizeof(at_ack_matches) / sizeof(at_ack_matches[0]); i ++)
					{
						size_t ack_len = strlen(at_ack_matches[i].string);
						if (rx_len >= ack_len && memcmp(rx_buffer + rx_len - ack_len, at_ack_matches[i].string, ack_len) == 0)
							return at_ack_matches[i].ack;
					}

					// data[rx_len] = '\0';
					// strncpy(data, line, rx_len - last_len);
					// for (uint8_t i = 0; i < sizeof(at_ack_matches) / sizeof(at_ack_matches[0]); i ++)
					// {
					// 	if (strncmp(data, at_ack_matches[i].string, rx_len - last_len) == 0)
					// 		return at_ack_matches[i].ack;
					// }
					// last_len = rx_len;
					// line = rx_buffer + rx_len; // Reset buffer for next line
				}
			}
		}
	}
	
	return AT_ACK_NONE;
}

bool OTA_ReceivedataEx(uint32_t timeout_ms, uint16_t *out_len)
{
	uint32_t start_tick = xTaskGetTickCount();
	const char * content_ptr = rx_buffer;
	const char * content_size_ptr = rx_buffer;
	uint16_t body_size = 0;

	if(out_len != NULL)
		*out_len = 0;

	while ((xTaskGetTickCount() - start_tick) < timeout_ms)
	{
		uint16_t size = RingBuffer_Get(uart_rb, (uint8_t*)rx_buffer);
		
		if(size)
		{
			content_size_ptr = strstr(rx_buffer, "Content-Length:");

			if(content_size_ptr)
			{
				sscanf(content_size_ptr, "Content-Length: %hu", &body_size);
			}

			content_ptr = strstr(rx_buffer, "\r\n\r\n");
			if(content_ptr)
			{
				if(body_size > BUFFER_SIZE)
					body_size = BUFFER_SIZE;

				memset(data, 0, sizeof(data));
				memcpy(data, content_ptr + 4, sizeof(char) * body_size);

				if(out_len != NULL)
					*out_len = body_size;

				return true;
			}
		}
	}

	return false;
}

bool ESP32AT_WiFiConnect(const char *ssid, const char *password)
{
	char cmd_buffer[128];
	
	// Set WiFi mode to Station
	if(!ESP32AT_SendCommand("AT+CWMODE=1", 2000))
		return false;

	// Connect to WiFi network
	snprintf(cmd_buffer, sizeof(cmd_buffer), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
	
	if(!ESP32AT_SendCommand(cmd_buffer, 5000))
		return false;
	
	LOG_I("[ESP32] WiFi Connected.\r\n");
	
	return true;
}

bool ESP32AT_WiFiCheckConnection(wifi_info_t *info)
{
	// 1. 检查当前WiFi连接状态
	if(!ESP32AT_SendCommand("AT+CWSTATE?", 2000))
		return false;
	
	// 解析 CWSTATE 返回结果
	char *response = strstr(rx_buffer, "+CWSTATE:"); // 查找rx_buffer中的+CWSTATE:的位置
	int state = 0;

	if (response == NULL)
		return false;
	
	if(sscanf(response, "+CWSTATE:%d,\"%63[^\"]", &state, info->ssid) != 2)
		return false; // 从响应中读取状态和SSID, %63[^\"] 表示读取最多63个非引号字符

	info->connected = (state == 2); // 2表示已连接

	// 2. 获取当前连接的AP信息
	if (!ESP32AT_SendCommand("AT+CWJAP?", 2000))
        return false;

	// 解析 CWJAP 返回结果
	response = strstr(rx_buffer, "+CWJAP:"); // 查找rx_buffer中的+CWJAP:的位置
	if (response == NULL)
		return false;
	
	if (sscanf(response, "+CWJAP:\"%63[^\"]\",\"%17[^\"]\",%d,%d", info->ssid, info->bssid, &info->channel, &info->rssi) != 4)
		return false; // 从响应中读取AP信息
	
	return info->connected;
}

bool ESP32AT_SNTP_GetTime(void)
{
	// 1. 设置时区为北京时间 (UTC+8)
	if (!ESP32AT_SendCommand("AT+CIPSNTPCFG=1,8", 2000))
		return false;
	
	// 2. 获取当前时间
	if (!ESP32AT_SendCommand("AT+CIPSNTPTIME?", 2000))
		return false;
	
	// 解析 CIPSNTPTIME 返回结果
	char *response = strstr(rx_buffer, "+CIPSNTPTIME:"); // 查找rx_buffer中的+CIPSNTPTIME:的位置
	if (response == NULL)
		return false;

	LOG_I("[ESP32] SNTP Time OK");

	return true;
}
