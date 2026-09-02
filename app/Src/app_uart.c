#include "app_uart.h"
#include "app_ipc.h"
#include "main.h"
#include "usart.h"
#include <stdarg.h>
#include <string.h>

#define UART_LOG_BUFFER_SIZE 256U

static char g_uart_log_buffer[UART_LOG_BUFFER_SIZE];

void uart_printf_mutex(const char *fmt, ...)
{
    osStatus_t lock_status = osError;
    va_list args;
    int len;

    if (fmt == NULL) {
        return;
    }

    /* Before the scheduler starts there is no concurrency and no mutex yet. */
    if (g_uart_mutex_handle != NULL) {
        lock_status = osMutexAcquire(g_uart_mutex_handle, 100U);
        if (lock_status != osOK) {
            return;
        }
    }

    va_start(args, fmt);
    len = vsnprintf(g_uart_log_buffer, sizeof(g_uart_log_buffer), fmt, args);
    va_end(args);

    if (len > 0) {
        if (len >= (int)sizeof(g_uart_log_buffer)) {
            len = (int)sizeof(g_uart_log_buffer) - 1;
        }
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)g_uart_log_buffer,
                                (uint16_t)len, 100U);  /* 100ms 超时，防 TX 故障死锁 */
    }

    if (lock_status == osOK) {
        osMutexRelease(g_uart_mutex_handle);
    }
}

static void uart_panic_write_text(const char *text, uint32_t max_len)
{
    uint32_t count = 0U;

    if ((text == NULL) || (huart1.Instance == NULL)) {
        return;
    }

    while ((*text != '\0') && (count < max_len)) {
        while ((huart1.Instance->SR & USART_SR_TXE) == 0U) {
        }
        huart1.Instance->DR = (uint16_t)(uint8_t)*text;
        text++;
        count++;
    }
}

void uart_panic_write(const char *reason, const char *detail)
{
    uart_panic_write_text("\r\n[FATAL] ", 11U);
    uart_panic_write_text(reason, 64U);
    if (detail != NULL) {
        uart_panic_write_text(": ", 2U);
        uart_panic_write_text(detail, 16U);
    }
    uart_panic_write_text("\r\nSystem halted.\r\n", 18U);

    if (huart1.Instance != NULL) {
        while ((huart1.Instance->SR & USART_SR_TC) == 0U) {
        }
    }
}
