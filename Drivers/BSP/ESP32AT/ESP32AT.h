#ifndef __ESP32AT_H__
#define __ESP32AT_H__

/**
 * @file    ESP32AT.h
 * @brief   ESP32 AT 指令模组通用驱动接口
 * @note    本模块只负责底层的 AT 指令收发和基础网络连接，不包含任何具体的业务逻辑。
 * @date    2026-1-15
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define BUFFER_SIZE 1024

extern char rx_buffer[BUFFER_SIZE];
extern char data[BUFFER_SIZE];

typedef enum
{
    AT_ACK_NONE,        // 未收到任何响应
    AT_ACK_OK,          // 收到 "OK\r\n"
    AT_ACK_ERROR,       // 收到 "ERROR\r\n"
    AT_ACK_BUSY,        // 收到 "busy p...\r\n"
    AT_ACK_READY,       // 收到 "ready\r\n"，通常表示模块重启后准备就绪
} ack_t;

typedef struct
{
    ack_t ack;
    const char *string;
} ack_string_t;     // 用于将 AT 指令响应字符串与枚举值进行匹配

typedef struct
{
	char ssid[64];
	char bssid[18];
	int channel;        // WiFi 信道
	int rssi;           // 信号强度，单位 dBm
	bool connected;     // 是否已连接
} wifi_info_t;

/**
 * @brief  初始化 ESP32 模块驱动
 * @note   负责初始化对应的 UART 硬件，并清空接收缓冲区。
 * @retval None
 */
bool ESP32AT_Init(void);
bool ESP32AT_ForceRecoverLink(void);

/**
 * @brief  发送 AT 指令并等待预期响应 
 * @note   发送指令后，会持续轮询接收缓冲区
 * @param  cmd:         要发送的 AT 指令字符串 (如 "AT", "AT+CIPMODE=1")
 * @param  timeout_ms:  单次等待超时时间 (单位: ms)。建议普通指令 1000ms，连接指令 15000ms。
 * @retval true:  在超时前成功匹配到预期响应
 * @retval false: 所有重试次数用尽后仍失败，或收到 "ERROR"
 */
bool ESP32AT_SendCommand(const char *cmd, uint32_t timeout_ms);

/**
 * @brief  连接 WiFi 热点 (Station 模式)
 * @note   内部会自动执行 AT+CWMODE=1 和 AT+CWJAP 指令。
 *         这是一个耗时操作，通常需要 5-10 秒。
 * @param  ssid:  WiFi 名称 (字符串)
 * @param  pwd:   WiFi 密码 (字符串)
 * @retval true:  成功连接并获取 IP
 * @retval false: 连接失败或超时
 */
bool ESP32AT_WiFiConnect(const char *ssid, const char *password);

/**
 * @brief  检查当前 WiFi 连接状态并获取连接信息
 * @note   内部会自动执行 AT+CWSTATE? 和 AT+CWJAP? 指令。
 * @param  info: 指向 wifi_info_t 结构体的指针，用于存储连接信息
 * @retval true:  成功获取连接信息
 * @retval false: 获取失败或未连接
 */
bool ESP32AT_WiFiCheckConnection(wifi_info_t *info);

bool ESP32AT_SNTP_GetTime(void);
bool OTA_ReceivedataEx(uint32_t timeout_ms, uint16_t *out_len);

#endif /* __ESP32AT_H__ */
