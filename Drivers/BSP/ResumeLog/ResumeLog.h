#ifndef __RESUME_LOG_H__
#define __RESUME_LOG_H__

#include <stdint.h>
#include <stdbool.h>

void ResumeLog_Init(void);
// bool ResumeLog_ReadLatest(uint32_t *value, uint32_t *seq);
bool ResumeLog_ReadLatestForTask(uint32_t task_tag, uint32_t total_size, uint32_t *value, uint32_t *seq);
// 统一写入接口：force=false 为普通节流写入，force=true 为关键点强制落盘。
bool ResumeLog_WriteProgress(uint32_t task_tag, uint32_t total_size, uint32_t value, bool force);
void ResumeLog_EraseAll(void);

#endif // __RESUME_LOG_H__
