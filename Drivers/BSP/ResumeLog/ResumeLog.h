#ifndef __RESUME_LOG_H__
#define __RESUME_LOG_H__

#include <stdint.h>
#include <stdbool.h>

void ResumeLog_Init(void);
bool ResumeLog_ReadLatestForTask(uint32_t task_tag, uint32_t total_size, uint32_t *value, uint32_t *seq);
bool ResumeLog_WriteProgressForTask(uint32_t task_tag, uint32_t total_size, uint32_t value);
bool ResumeLog_WriteProgressForceForTask(uint32_t task_tag, uint32_t total_size, uint32_t value);
void ResumeLog_EraseAll(void);

#endif // __RESUME_LOG_H__
