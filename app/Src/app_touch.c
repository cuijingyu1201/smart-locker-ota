#include "app_touch.h"
#include "app_lcd.h"      /* g_lcd_mutex_handle / App_Lcd_Lock / Unlock */
#include "app_uart.h"     /* uart_printf_mutex */
#include "touch.h"
#include "lcd.h"
#include <stdio.h>

/* 初始化触摸（GPIO + 默认校准）。必须在 lcd_init 之后调用（tp_init 读取 lcddev.dir） */
int32_t App_Touch_Init(void)
{
    tp_init();
    uart_printf_mutex("[TOUCH] XPT2046 init OK (dir=%s). 引脚: PEN=PF10 CS=PF11 MISO=PB2 MOSI=PF9 CLK=PB1\r\n",
                      (tp_dev.touchtype & 1U) ? "横屏" : "竖屏");
    return 0;
}

/* 触摸测试任务：手指/触笔在屏幕滑动即画红色轨迹；点屏幕左上角 24x24 区域清屏 */
void TaskTouch(void *argument)
{
    (void)argument;
    uint8_t  pressed_cnt = 0;
    uint8_t  last_state  = 0;

    /* 初始提示界面（加锁，和 TaskLcdTest 共享 FSMC） */
    App_Lcd_Lock();
    {
        lcd_show_string(10, 300, 240, 16, 12, (char *)"Touch Test: draw on screen", MAGENTA);
    }
    App_Lcd_Unlock();

    for (;;) {
        uint8_t pressed = tp_scan(0);   /* 读 AD + 转屏幕坐标（软件SPI，不持锁） */

        if (pressed) {
            if (last_state == 0) {
                uart_printf_mutex("[TOUCH] DOWN  AD(x=%u,y=%u) -> px(%u,%u)\r\n",
                                  tp_dev.x_ad, tp_dev.y_ad, tp_dev.x, tp_dev.y);
            }
            pressed_cnt++;

            /* 限频打印（每 8 次打印 1 次），避免串口刷屏 */
            if ((pressed_cnt & 0x07U) == 0U) {
                uart_printf_mutex("[TOUCH] MOVE  AD(x=%u,y=%u) -> px(%u,%u)\r\n",
                                  tp_dev.x_ad, tp_dev.y_ad, tp_dev.x, tp_dev.y);
            }

            App_Lcd_Lock();
            {
                if (tp_dev.x < 24U && tp_dev.y < 24U) {
                    /* 左上角 = 清屏键 */
                    lcd_clear(WHITE);
                    lcd_show_string(10, 300, 240, 16, 12, (char *)"Touch Test: draw on screen", MAGENTA);
                } else {
                    /* 画 3x3 红色轨迹点 */
                    uint16_t x = tp_dev.x, y = tp_dev.y;
                    lcd_fill(x - 1, y - 1, x + 1, y + 1, RED);
                }
            }
            App_Lcd_Unlock();
        } else {
            if (last_state) {
                uart_printf_mutex("[TOUCH] UP    last px(%u,%u)\r\n", tp_dev.x, tp_dev.y);
            }
            pressed_cnt = 0;
        }

        last_state = pressed;
        osDelay(20);   /* 20ms 扫描周期 ≈ 50Hz */
    }
}