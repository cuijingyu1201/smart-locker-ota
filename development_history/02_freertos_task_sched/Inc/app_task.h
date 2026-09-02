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

/* ======================== 任务参数宏 ======================== */

/* TaskLED: 500ms 翻转 LED，优先级 Normal(24) */
#define TASK_LED_STACK_SIZE      (256U)
#define TASK_LED_PRIORITY        (osPriorityNormal)

/* TaskPrint: 1s 打印 RTOS 统计，优先级 BelowNormal(16) */
#define TASK_PRINT_STACK_SIZE    (512U)
#define TASK_PRINT_PRIORITY      (osPriorityBelowNormal)

/* ======================== 任务函数原型 ======================== */
void TaskLED   (void *argument);
void TaskPrint (void *argument);

/* ======================== 钩子函数原型 ======================== */
void vApplicationIdleHook             (void);
void vApplicationMallocFailedHook     (void);
void vApplicationStackOverflowHook    (TaskHandle_t xTask, char *pcTaskName);

#endif /* APP_TASK_H */
