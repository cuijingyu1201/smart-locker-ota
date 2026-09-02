/**
 * @file    app_task.c
 * @brief   D2: FreeRTOS 业务任务实现（LED/Print/IdleHook）
 */
#include "app_task.h"
#include <stdio.h>
#include "app_ipc.h"
#include "app_uart.h"

uint32_t g_irq_cnt = 0;   /* 中断计数，TaskSemHandle 写，TaskPrint 读 */

#define STACK_MONITOR_MAX_TASKS       7U
#define STACK_WARN_MIN_FREE_BYTES     128U
#define STACK_REPORT_PERIOD_TICKS     pdMS_TO_TICKS(30000U)

typedef struct {
    osThreadId_t thread_id;
    const char *name;
    uint32_t stack_size_bytes;
} stack_monitor_entry_t;

static stack_monitor_entry_t g_stack_monitors[STACK_MONITOR_MAX_TASKS];
static uint32_t g_stack_monitor_count;

void App_StackMonitorRegister(osThreadId_t thread_id, const char *name,
                              uint32_t stack_size_bytes)
{
    if ((thread_id == NULL) || (name == NULL) ||
        (g_stack_monitor_count >= STACK_MONITOR_MAX_TASKS)) {
        return;
    }

    g_stack_monitors[g_stack_monitor_count].thread_id = thread_id;
    g_stack_monitors[g_stack_monitor_count].name = name;
    g_stack_monitors[g_stack_monitor_count].stack_size_bytes = stack_size_bytes;
    g_stack_monitor_count++;
}

void App_StackMonitorCheck(uint8_t force_report)
{
    uint32_t i;

    for (i = 0U; i < g_stack_monitor_count; i++) {
        uint32_t free_bytes = osThreadGetStackSpace(g_stack_monitors[i].thread_id);
        uint32_t warn_bytes = g_stack_monitors[i].stack_size_bytes / 4U;

        if (warn_bytes < STACK_WARN_MIN_FREE_BYTES) {
            warn_bytes = STACK_WARN_MIN_FREE_BYTES;
        }

        if ((force_report != 0U) || (free_bytes <= warn_bytes)) {
            uart_printf_mutex("[STACK]%s %-13s free=%luB total=%luB\r\n",
                              (free_bytes <= warn_bytes) ? " WARN" : "",
                              g_stack_monitors[i].name,
                              (unsigned long)free_bytes,
                              (unsigned long)g_stack_monitors[i].stack_size_bytes);
        }
    }
}

/* ======================== TaskLED ======================== */
void TaskLED(void *argument)
{
    (void)argument;

    /* Flash parameter maintenance runs before the scheduler in main(). */
    led_msg_t msg;
    uint32_t delay_ms;
    for (;;)
    {
//        HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
        HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);

        msg.rtos_tick  = osKernelGetTickCount();
        msg.led0_state = (uint8_t)HAL_GPIO_ReadPin(LED0_GPIO_Port, LED0_Pin);
        osMessageQueuePut(g_led_queue_handle, &msg, 0, 0);

        uart_printf_mutex("[TASK_LED ] rtos_tick=%lu\r\n", (unsigned long)msg.rtos_tick);

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
    uint32_t last_stack_report_tick = 0U;
    for (;;)
    {
        osDelay(1000);

        uart_printf_mutex("---------- RTOS STATS ----------\r\n");

        /* 从队列把所有积压的 LED 消息读出来打印 */
        msg_cnt = 0;
        while (osMessageQueueGet(g_led_queue_handle, &rx_msg, NULL, 0) == osOK)
        {
            uart_printf_mutex("  [Q] tick=%lu led0=%u\r\n",
                   (unsigned long)rx_msg.rtos_tick, (unsigned)rx_msg.led0_state);
            msg_cnt++;
        }
        uart_printf_mutex("  queue_msg_count = %lu\r\n", (unsigned long)msg_cnt);

        uart_printf_mutex("uptime     = %lu s\r\n", (unsigned long)(osKernelGetTickCount() / 1000));
        uart_printf_mutex("rtos_tick  = %lu\r\n", (unsigned long)osKernelGetTickCount());
        uart_printf_mutex("hal_tick   = %lu\r\n", (unsigned long)HAL_GetTick());
        uart_printf_mutex("n_tasks    = %u\r\n", (unsigned)uxTaskGetNumberOfTasks());
        {
            uint32_t now = osKernelGetTickCount();
            uint8_t force_report = ((now - last_stack_report_tick) >= STACK_REPORT_PERIOD_TICKS) ||
                                   (last_stack_report_tick == 0U);
            App_StackMonitorCheck(force_report);
            if (force_report != 0U) {
                last_stack_report_tick = now;
            }
        }
        uart_printf_mutex("--------------------------------\r\n");
				uart_printf_mutex("irq_cnt    = %lu\r\n", (unsigned long)g_irq_cnt);
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
    __WFI();
}

/* ======================== Malloc Failed Hook (堆不够会进这里) ======================== */
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    uart_panic_write("FreeRTOS heap exhausted", NULL);
    for (;;) {
    }
}

/* ======================== Stack Overflow Hook (栈溢出会进这里) ======================== */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    uart_panic_write("FreeRTOS stack overflow", (const char *)pcTaskName);
    for (;;) {
    }
}
