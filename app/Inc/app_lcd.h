/**
 * @file    app_lcd.h
 * @brief   D17 新组件：LCD 初始化封装 + FSMC 总线互斥锁 + LCD 测试任务定义
 *          基于正点原子 TFTLCD(MCU屏) 驱动（FSMC NE4 + A10 + PB0背光）
 *          调用规则：
 *              1. App_Lcd_Init() 必须在 FreeRTOS osKernelStart() 之前调用（调度器起之前）
 *              2. 任务里调 LCD API 前必须 App_Lcd_Lock()，用完 App_Lcd_Unlock()（防多任务 FSMC 冲突）
 */
#ifndef APP_LCD_H
#define APP_LCD_H

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "lcd.h"   /* 正点原子 LCD 驱动 API：lcd_init / lcd_clear / lcd_show_string / ... */

/* ============================================================
 * TaskLcdTest 任务参数（用于今天最小验证，D2后可升级为 TaskLcdUI 1024 Words）
 * ============================================================ */
#define TASK_LCD_TEST_STACK_SIZE_BYTES    (512U * 4U)   /* 2048 Bytes = 512 Words；画字符+填充矩形足够 */
#define TASK_LCD_TEST_PRIORITY            (osPriorityNormal)   /* 24，和 TaskLED/TaskDHT11 同，不抢高优先级 */

/* ============================================================
 * LCD FSMC 总线互斥锁（防止多任务同时写屏花屏）句柄 声明
 * ============================================================ */
extern osMutexId_t g_lcd_mutex_handle;

/* ============================================================
 * 对外 API
 * ============================================================ */

/**
 * @brief  初始化 LCD（FSMC GPIO + FSMC 控制器时序 + LCD IC 寄存器 + 背光点亮）
 *         必须在 FreeRTOS 调度器启动之前调用！
 * @param  无
 * @retval 0 = 初始化完成；其他值保留（正点原子 lcd_init 是 void，我们封装后也返回 0）
 */
int32_t App_Lcd_Init(void);

/**
 * @brief  LCD 互斥锁 + 测试任务 初始化（创建互斥锁，在 App_IPC_Init 里调用）
 * @param  无
 * @retval 无
 */
void App_Lcd_IPC_Init(void);

/**
 * @brief  获取 LCD FSMC 总线（死等拿到锁，不可在中断里调用）
 */
void App_Lcd_Lock(void);

/**
 * @brief  释放 LCD FSMC 总线
 */
void App_Lcd_Unlock(void);

/**
 * @brief  FreeRTOS 任务：LCD 最小显示测试（蓝底+白字+闪烁矩形）
 *         今天验证用，D9 可替换为 TaskLcdUI
 * @param  argument 未使用
 */
void TaskLcdTest(void *argument);

#endif /* APP_LCD_H */
