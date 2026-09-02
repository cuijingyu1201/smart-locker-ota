#include "app_uart.h"
#include "app_ipc.h"
#include "main.h"
#include "usart.h"
#include <stdarg.h>
#include <string.h>

void uart_printf_mutex(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);

    va_end(args);

    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;

    /* 拿互斥锁：拿不到最多等 100ms，跳过本次打印（不死等） */
    if (osMutexAcquire(g_uart_mutex_handle, 100) == osOK)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 0xFFFF);
        osMutexRelease(g_uart_mutex_handle);
    }
}
