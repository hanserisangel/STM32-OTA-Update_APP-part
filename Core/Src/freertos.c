#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#define START_TASK_STACK_SIZE 256
#define START_TASK_PRIO 2

#define OTA_TASK_STACK_SIZE 1024
#define OTA_TASK_PRIO 3

TaskHandle_t start_taskHandle;
TaskHandle_t OTA_taskHandle;

static void StartTask(void *argument);
static void OTA_Task(void *argument);

void MX_FREERTOS_Init(void)
{
  xTaskCreate(StartTask, "startTask", START_TASK_STACK_SIZE, NULL, START_TASK_PRIO, &start_taskHandle);
}

static void StartTask(void *argument)
{
  OTA_Init();
  xTaskCreate(OTA_Task, "OTA_Task", OTA_TASK_STACK_SIZE, NULL, OTA_TASK_PRIO, &OTA_taskHandle);
  vTaskDelete(start_taskHandle);
}

static void OTA_Task(void *argument)
{
  while(1)
  {
    OTA_Process();
  }
}
