/**
 * @file    app_task.c
 * @brief   D2: FreeRTOS 业务任务实现（LED/Print/IdleHook）
 */
#include "app_task.h"
#include <stdio.h>

/* ======================== TaskLED ======================== */
//实现LED翻转
void TaskLED(void *argument)
{
    (void)argument;
    for (;;)
    {
        HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
        HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
        printf("[TASK_LED ] rtos_tick=%lu\r\n",
               (unsigned long)osKernelGetTickCount());
        osDelay(500);
    }
}

/* ======================== TaskPrint ======================== */
//调试验证，栈溢出，系统时间是否正常，任务数量是否异常
void TaskPrint(void *argument)
{
    (void)argument;
    for (;;)
    {
        osDelay(1000);
        printf("---------- RTOS STATS ----------\r\n");
        printf("uptime     = %lu s\r\n",
               (unsigned long)(osKernelGetTickCount() / 1000));
        printf("rtos_tick  = %lu\r\n",
               (unsigned long)osKernelGetTickCount());
        printf("hal_tick   = %lu\r\n",
               (unsigned long)HAL_GetTick());
        printf("n_tasks    = %u\r\n",
               (unsigned)uxTaskGetNumberOfTasks());
        printf("TaskPrint_stack_free = %u words\r\n",
               (unsigned)uxTaskGetStackHighWaterMark(NULL));
        printf("--------------------------------\r\n");
    }
}

/* ======================== Idle Hook (所有任务阻塞时才运行) ======================== */
void vApplicationIdleHook(void)
{
    static uint32_t idle_cnt = 0;
    idle_cnt++;
    if (idle_cnt >= 500000U)
    {
        printf("[IDLE_HOOK] running, loop count=%lu\r\n",
               (unsigned long)idle_cnt);
        idle_cnt = 0;
    }
    __WFI();   /* ARM 睡眠指令：WFI = Wait For Interrupt，省电用 */
}

/* ======================== Malloc Failed Hook (堆不够会进这里) ======================== */
void vApplicationMallocFailedHook(void)
{
    printf("\r\n[ERROR] pvPortMalloc() FAILED! FreeRTOS Heap exhausted.\r\n");
    printf("        -> 请在 CubeMX 中增大 TOTAL_HEAP_SIZE (当前 10240 Bytes)\r\n");
    for (;;) { osDelay(1000); }
}

/* ======================== Stack Overflow Hook (栈溢出会进这里) ======================== */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("\r\n[ERROR] STACK OVERFLOW in task '%s' !\r\n", pcTaskName);
    printf("        -> 请增大该任务的 STACK_SIZE 参数\r\n");
    for (;;) { osDelay(1000); }
}
