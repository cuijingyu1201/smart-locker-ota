#ifndef APP_IPC_H
#define APP_IPC_H

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"

/* ===== 事件位宏（EventGroup 的 32 位标志位，每位代表一个事件） ===== */
#define BIT_KEY_DOWN   (1U << 0U)   /* BIT0 = 1 表示 WK_UP 按键被按住 */

/* ===== 队列消息结构体（TaskLED 生产 → TaskPrint 消费） ===== */
typedef struct {
    uint32_t rtos_tick;    /* 翻转时的 FreeRTOS tick 值 */
    uint8_t  led0_state;   /* LED0 当前电平状态 0/1 */
} led_msg_t;

/* ===== 4 大 IPC 全局句柄声明 ===== */
extern osMessageQueueId_t g_led_queue_handle;     /* 消息队列：LED 翻转信息 */
extern osMutexId_t        g_uart_mutex_handle;    /* 互斥锁：串口 printf 保护 */
extern osEventFlagsId_t   g_event_group_handle;   /* 事件组：按键状态标志 */
extern osSemaphoreId_t    g_irq_sem_handle;       /* 二值信号量：中断→任务同步 */
extern osMutexId_t g_dht11_mutex_handle;          /* DHT11 数据互斥锁 */
extern osSemaphoreId_t g_esp_rx_sem_handle;       /* ESP8266 收到一行的信号量 */
extern osMutexId_t g_mqtt_mutex_handle;           /* MQTT发送互斥锁 */
extern osMutexId_t g_lcd_mutex_handle;            /* LCD互斥锁句柄声明（防止多任务同时写屏FSMC冲突花屏） */


/* ===== 初始化函数 ===== */
void App_IPC_Init(void);
void App_Lcd_IPC_Init(void);   /* LCD互斥锁初始化，在 App_IPC_Init 末尾调用 */

#endif /* APP_IPC_H */
