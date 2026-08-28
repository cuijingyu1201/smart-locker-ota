#ifndef APP_UART_H
#define APP_UART_H

#include <stdio.h>

/*
 * Formats into a shared BSS buffer and transmits one complete log record while
 * holding the UART mutex. Callers do not need a large local buffer and records
 * from different tasks cannot be interleaved byte by byte.
 */
void uart_printf_mutex(const char *fmt, ...);

/* Minimal polled output for fatal hooks. It does not use libc or RTOS APIs. */
void uart_panic_write(const char *reason, const char *detail);

#endif /* APP_UART_H */
