/**
 * @file    app_task.h
 * @brief   D2: FreeRTOS 业务任务定义（LED/Print/IdleHook）
 */
#ifndef APP_TASK_H
#define APP_TASK_H

/* FreeRTOS 严格包含顺序：FreeRTOS.h 必须在最前，然后才是 task.h */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"

extern uint32_t g_irq_cnt;

/* ======================== 任务参数宏 ======================== */

/* TaskLED: 500ms 翻转 LED，优先级 Normal(24) */
#define TASK_LED_STACK_SIZE_BYTES      (384U)
#define TASK_LED_PRIORITY        (osPriorityNormal)

/* TaskPrint: 1s 打印 RTOS 统计，优先级 BelowNormal(16) */
#define TASK_PRINT_STACK_SIZE_BYTES    (768U)
#define TASK_PRINT_PRIORITY      (osPriorityBelowNormal)

/* ======================== TaskKeyPoll 参数 ======================== */
#define TASK_KEY_POLL_STACK_SIZE_BYTES   (256U)
#define TASK_KEY_POLL_PRIORITY     (osPriorityAboveNormal)

/* ======================== TaskSemHandle 参数 ======================== */
#define TASK_SEM_HANDLE_STACK_SIZE_BYTES  (256U)
#define TASK_SEM_HANDLE_PRIORITY    (osPriorityHigh)

#define TASK_DHT11_STACK_SIZE_BYTES       (384U * 4U)
#define TASK_ESP8266_STACK_SIZE_BYTES     (512U * 4U)

/* ======================== 任务函数原型 ======================== */
void TaskLED   (void *argument);
void TaskPrint (void *argument);
void TaskKeyPoll(void *argument);
void TaskSemHandle(void *argument);

void App_StackMonitorRegister(osThreadId_t thread_id, const char *name,
                              uint32_t stack_size_bytes);
void App_StackMonitorCheck(uint8_t force_report);

/* ======================== 钩子函数原型 ======================== */
void vApplicationIdleHook             (void);
void vApplicationMallocFailedHook     (void);
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);


#endif /* APP_TASK_H */
