
#include "app_lcd.h"
#include "app_uart.h"     
#include <stdio.h>

osMutexId_t g_lcd_mutex_handle = NULL;

void App_Lcd_IPC_Init(void)
{
    g_lcd_mutex_handle = osMutexNew(NULL);
    if (g_lcd_mutex_handle == NULL) {
        uart_printf_mutex("[LCD] Mutex create FAIL! (������д�����ޱ���)\r\n");
    } else {
        uart_printf_mutex("[LCD] Mutex created OK.\r\n");
    }
}

void App_Lcd_Lock(void)
{
    if (g_lcd_mutex_handle == NULL) {
        return;  
    }
    (void)osMutexAcquire(g_lcd_mutex_handle, 200U);
}

void App_Lcd_Unlock(void)
{
    if (g_lcd_mutex_handle == NULL) {
        return;
    }
    (void)osMutexRelease(g_lcd_mutex_handle);
}

int32_t App_Lcd_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();               
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = LCD_BL_GPIO_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LCD_BL_GPIO_PORT, &GPIO_InitStruct);
    LCD_BL(1);   
    uart_printf_mutex("[LCD] BL(PB0)=HIGH set OK.\r\n");

    HAL_Delay(10);
	

    lcd_init();

    uart_printf_mutex("[LCD] Init done: ID=0x%04X W=%u H=%u\r\n",
                      (unsigned)lcddev.id,
                      (unsigned)lcddev.width,
                      (unsigned)lcddev.height);

    return 0;
}

void TaskLcdTest(void *argument)
{
    (void)argument;

    App_Lcd_Lock();
    {
        lcd_clear(BLUE);  
        lcd_show_string(10, 10, 240, 16, 16, "D17: Locker CTRL v1.0.0", WHITE);
        lcd_show_string(10, 32, 240, 16, 16, "LCD Bringup  : PASS",   GREEN);

        char info_buf[64];
        snprintf(info_buf, sizeof(info_buf), "ID:0x%04X  %ux%u",
                 (unsigned)lcddev.id,
                 (unsigned)lcddev.width,
                 (unsigned)lcddev.height);
        lcd_show_string(10, 60, 240, 12, 12, info_buf, YELLOW);
    }
    App_Lcd_Unlock();

    uart_printf_mutex("[LCD] TaskLcdTest start: UI drawn, entering blink loop.\r\n");

    uint8_t blink_state = 0;
    for (;;)
    {
        blink_state ^= 1;  

        App_Lcd_Lock();
        {
            if (blink_state) {
                lcd_fill(10, 90, 10 + 40, 90 + 40, RED);
            } else {
                lcd_fill(10, 90, 10 + 40, 90 + 40, BLUE);
            }
        }
        App_Lcd_Unlock();

        osDelay(500);  
    }
}

