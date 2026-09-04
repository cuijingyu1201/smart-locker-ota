#include "app_ipc.h"
#include "app_uart.h"
#include <stdio.h>
#include "app_lcd.h"

/* ===== 4 大 IPC 全局句柄定义 ===== */
osMessageQueueId_t g_led_queue_handle;
osMutexId_t        g_uart_mutex_handle;
osEventFlagsId_t   g_event_group_handle;
osSemaphoreId_t    g_irq_sem_handle;
osMutexId_t g_dht11_mutex_handle = NULL;
osSemaphoreId_t g_esp_rx_sem_handle = NULL;
osMutexId_t g_mqtt_mutex_handle = NULL; 

/* ===== 4 大 IPC 初始化函数 ===== */
void App_IPC_Init(void)
{
    /* 1. 消息队列：深度=10 条，每条=led_msg_t 大小 */
    g_led_queue_handle = osMessageQueueNew(10, sizeof(led_msg_t), NULL);
    if (g_led_queue_handle == NULL) {
        uart_printf_mutex("[ERROR] osMessageQueueNew(g_led_queue) failed!\r\n");
    }

    /* 2. 互斥锁：保护串口 printf，NULL=默认属性 */
    g_uart_mutex_handle = osMutexNew(NULL);
    if (g_uart_mutex_handle == NULL) {
        uart_printf_mutex("[ERROR] osMutexNew(g_uart_mutex) failed!\r\n");
    }
		//MQTT 发送互斥锁，防止多个任务并发调 AT+CIPSEND 冲突
		g_mqtt_mutex_handle = osMutexNew(NULL);
		if (g_mqtt_mutex_handle == NULL) {
				uart_printf_mutex("[IPC] MQTT Mutex create FAIL!\r\n");
		}

    /* 3. 事件组：32 个标志位，NULL=默认属性 */
    g_event_group_handle = osEventFlagsNew(NULL);
    if (g_event_group_handle == NULL) {
        uart_printf_mutex("[ERROR] osEventFlagsNew(g_event_group) failed!\r\n");
    }

    /* 4. 二值信号量：max_count=1，initial_count=0（初始无令牌，任务阻塞等中断 Release） */
    g_irq_sem_handle = osSemaphoreNew(1, 0, NULL);
		g_dht11_mutex_handle = osMutexNew(NULL);  /* DHT11 数据互斥锁 */
		g_esp_rx_sem_handle = osSemaphoreNew(1, 0, NULL);/*（max=1，initial=0 → 收到一行 Give 一次，任务里 Acquire 就被唤醒）*/
    if (g_irq_sem_handle == NULL) {
        uart_printf_mutex("[ERROR] osSemaphoreNew(g_irq_sem) failed!\r\n");
    }

    uart_printf_mutex("[IPC] App_IPC_Init done: Queue+Mutex+EventGroup+BinSem created.\r\n");
		    
    App_Lcd_IPC_Init(); /*  LCD FSMC 互斥锁初始化 */
}
