#ifndef __MAIN_H
#define __MAIN_H

#include "stm32f4xx_hal.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

#include "Log.h"
#include "Flash.h"
#include "Uart.h"
#include "W25Q64.h"
#include "ESP32AT.h"
#include "OTA.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "ResumeLog.h"

void Error_Handler(void);

#define SPI_RST_Pin GPIO_PIN_2
#define SPI_RST_GPIO_Port GPIOG
#define SPI_CS_Pin GPIO_PIN_3
#define SPI_CS_GPIO_Port GPIOG
#define SPI_DC_Pin GPIO_PIN_4
#define SPI_DC_GPIO_Port GPIOG

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
