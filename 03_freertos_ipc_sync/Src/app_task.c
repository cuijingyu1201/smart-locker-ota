/**
 * @file    app_task.c
 * @brief   D2: FreeRTOS 业务任务实现（LED/Print/IdleHook）
 */
#include "app_task.h"
#include <stdio.h>
#include "app_ipc.h"
#include "app_uart.h"

uint32_t g_irq_cnt = 0;   /* 中断计数，TaskSemHandle 写，TaskPrint 读 */

/* ======================== TaskLED ======================== */
void TaskLED(void *argument)
{
    (void)argument;
    led_msg_t msg;
    uint32_t delay_ms;
    for (;;)
    {
        HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
        HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);

        msg.rtos_tick  = osKernelGetTickCount();
        msg.led0_state = (uint8_t)HAL_GPIO_ReadPin(LED0_GPIO_Port, LED0_Pin);
        osMessageQueuePut(g_led_queue_handle, &msg, 0, 0);

        printf("[TASK_LED ] rtos_tick=%lu\r\n", (unsigned long)msg.rtos_tick);

        uint32_t flags = osEventFlagsWait(g_event_group_handle, BIT_KEY_DOWN,
                                          osFlagsWaitAny, 0);
        if ((int32_t)flags > 0 && (flags & BIT_KEY_DOWN)) {
            delay_ms = 100;
        } else {
            delay_ms = 500;
        }
        osDelay(delay_ms);
    }
}
/* ======================== TaskPrint ======================== */
void TaskPrint(void *argument)
{
    (void)argument;
    led_msg_t rx_msg;
    uint32_t msg_cnt;
    for (;;)
    {
        osDelay(1000);

        uart_printf_mutex("---------- RTOS STATS ----------\r\n");

        /* 从队列把所有积压的 LED 消息读出来打印 */
        msg_cnt = 0;
        while (osMessageQueueGet(g_led_queue_handle, &rx_msg, NULL, 0) == osOK)
        {
            printf("  [Q] tick=%lu led0=%u\r\n",
                   (unsigned long)rx_msg.rtos_tick, (unsigned)rx_msg.led0_state);
            msg_cnt++;
        }
        printf("  queue_msg_count = %lu\r\n", (unsigned long)msg_cnt);

        printf("uptime     = %lu s\r\n", (unsigned long)(osKernelGetTickCount() / 1000));
        printf("rtos_tick  = %lu\r\n", (unsigned long)osKernelGetTickCount());
        printf("hal_tick   = %lu\r\n", (unsigned long)HAL_GetTick());
        printf("n_tasks    = %u\r\n", (unsigned)uxTaskGetNumberOfTasks());
        printf("TaskPrint_stack_free = %u words\r\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        printf("--------------------------------\r\n");
				printf("irq_cnt    = %lu\r\n", (unsigned long)g_irq_cnt);
    }
}
/* ===================== TaskKeyPoll：轮询 PA0 按键状态 ===================== */
void TaskKeyPoll(void *argument)
{
    (void)argument;
    for (;;)
    {
        /* 精英板 WK_UP：按下=高电平(SET)，松开=低电平(RESET) */
        GPIO_PinState st = HAL_GPIO_ReadPin(KEY_WKUP_GPIO_Port, KEY_WKUP_Pin);
        if (st == GPIO_PIN_SET)
        {
            osEventFlagsSet(g_event_group_handle, BIT_KEY_DOWN);
        }
        else
        {
            osEventFlagsClear(g_event_group_handle, BIT_KEY_DOWN);
        }
        osDelay(20);
    }
}
/* ================ TaskSemHandle：中断延迟处理任务 ================ */
void TaskSemHandle(void *argument)
{
    (void)argument;
    static uint32_t last_tick = 0;
    for (;;)
    {
        if (osSemaphoreAcquire(g_irq_sem_handle, osWaitForever) == osOK)
        {
            uint32_t now = osKernelGetTickCount();
            if ((now - last_tick) < 50) continue;
            last_tick = now;
            g_irq_cnt++;   /* 只计数，不打印 */
        }
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
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
    (void)xTask;
    printf("\r\n[ERROR] STACK OVERFLOW in task '%s' !\r\n", pcTaskName);
    printf("        -> 请增大该任务的 STACK_SIZE 参数\r\n");
    for (;;) { osDelay(1000); }
}
