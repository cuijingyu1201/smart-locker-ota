/**
 * @file    app_lcd.c
 * @brief   D17 新组件：LCD 初始化封装 + FSMC 互斥锁 + TaskLcdTest 最小验证
 *          大变量注意：都放全局 static / BSS 段，防栈溢出（项目硬约束）
 */
#include "app_lcd.h"
#include "app_uart.h"     /* 用于 uart_printf_mutex 打印启动日志 */
#include <stdio.h>

/* ============================================================
 * 全局互斥锁句柄定义（与 app_ipc.c 里其他互斥锁同风格）
 * ============================================================ */
osMutexId_t g_lcd_mutex_handle = NULL;

/* ============================================================
 * LCD 互斥锁 + IPC 初始化
 *  （在 App_IPC_Init 中调用，保证 App_Lcd_Lock() 在任务创建前就可用）
 * ============================================================ */
void App_Lcd_IPC_Init(void)
{
    g_lcd_mutex_handle = osMutexNew(NULL);
    if (g_lcd_mutex_handle == NULL) {
        uart_printf_mutex("[LCD] Mutex create FAIL! (多任务写屏将无保护)\r\n");
    } else {
        uart_printf_mutex("[LCD] Mutex created OK.\r\n");
    }
}

/* ============================================================
 * LCD FSMC 总线锁/解锁（封装给所有任务调用，防并发花屏）
 * ============================================================ */
void App_Lcd_Lock(void)
{
    if (g_lcd_mutex_handle == NULL) {
        return;   /* 互斥锁还没创建（调度器前调用时），直接过 */
    }
    /* 200ms 超时拿不到锁就放弃，防止死锁；不直接 osOK 死等是项目 D16 加固思路 */
    (void)osMutexAcquire(g_lcd_mutex_handle, 200U);
}

void App_Lcd_Unlock(void)
{
    if (g_lcd_mutex_handle == NULL) {
        return;
    }
    /* D16 加固：只有成功 Acquire 才能 Release，防止"未获取就释放" */
    (void)osMutexRelease(g_lcd_mutex_handle);
}

/* ============================================================
 * LCD 初始化封装（调度器起之前调用，FreeRTOS 不打断 FSMC 时序）
 *  功能：点亮背光 + FSMC GPIO 时钟开 + 调正点原子 lcd_init()（内部重配 FSMC 时序+LCD IC寄存器）
 * ============================================================ */
int32_t App_Lcd_Init(void)
{
    /* 1. 先强制点亮背光（PB0），调试时先看背光亮没，快速排查电源/接线问题 */
    __HAL_RCC_GPIOB_CLK_ENABLE();                /* 确保 PB0 时钟已开（CubeMX还没开FSMC之前也能用） */
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = LCD_BL_GPIO_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LCD_BL_GPIO_PORT, &GPIO_InitStruct);
    LCD_BL(1);   /* 背光 = 高电平点亮 */
    uart_printf_mutex("[LCD] BL(PB0)=HIGH set OK. [v2-test]\r\n");

    /* 2. 延时 10ms 让 LCD 电源和背光上电稳定（HAL_Delay 依赖 SysTick，CubeMX 启动时已经配好 SysTick=1ms 了） */
    HAL_Delay(10);
	

    /* 3. 调正点原子 lcd_init()：内部做 FSMC GPIO + FSMC 控制器 + 读LCD ID + 对应IC寄存器序列初始化 */
    lcd_init();

    /* 4. 初始化完成，打印 LCD ID 和分辨率（串口可查，快速判断 lcd_init 走到哪一步了） */
    uart_printf_mutex("[LCD] Init done: ID=0x%04X W=%u H=%u\r\n",
                      (unsigned)lcddev.id,
                      (unsigned)lcddev.width,
                      (unsigned)lcddev.height);

    return 0;
}

/* ============================================================
 * TaskLcdTest：最小显示验证任务（今天验证用，D9可删换为TaskLcdUI）
 *  流程：调度器启动后才跑到这里 → 锁LCD → 蓝底白字标题 + 绿字 PASS + 500ms 红/蓝方块闪烁
 * ============================================================ */
void TaskLcdTest(void *argument)
{
    (void)argument;

    /* 进入任务先拿锁，画初始页面 */
    App_Lcd_Lock();
    {
        lcd_clear(BLUE);   /* 整屏蓝色背景 */
        /* 顶部两行文字（16号字，白色标题 + 绿色状态） */
        lcd_show_string(10, 10, 240, 16, 16, "D17: Locker CTRL v1.0.0", WHITE);
        lcd_show_string(10, 32, 240, 16, 16, "LCD Bringup  : PASS",   GREEN);

        /* 打印分辨率信息（12号字，黄色小字） */
        char info_buf[64];
        snprintf(info_buf, sizeof(info_buf), "ID:0x%04X  %ux%u",
                 (unsigned)lcddev.id,
                 (unsigned)lcddev.width,
                 (unsigned)lcddev.height);
        lcd_show_string(10, 60, 240, 12, 12, info_buf, YELLOW);
    }
    App_Lcd_Unlock();

    uart_printf_mutex("[LCD] TaskLcdTest start: UI drawn, entering blink loop.\r\n");

    /* 主循环：500ms 切换一次方块颜色，验证 LCD 写屏在 FreeRTOS 调度下不卡不花 */
    uint8_t blink_state = 0;
    for (;;)
    {
        blink_state ^= 1;   /* 翻转 0/1 */

        App_Lcd_Lock();
        {
            /* 在 (10, 90) 位置画一个 40×40 方块，状态0=红 状态1=蓝(和背景同色=看起来擦除) */
            if (blink_state) {
                lcd_fill(10, 90, 10 + 40, 90 + 40, RED);
            } else {
                lcd_fill(10, 90, 10 + 40, 90 + 40, BLUE);
            }
        }
        App_Lcd_Unlock();

        osDelay(500);   /* FreeRTOS 阻塞延时，释放 CPU 给其他任务 */
    }
}