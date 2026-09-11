#include "code_check.h"
#include "app_buzzer.h"
#include "app_uart.h"
#include "cabinet_fsm.h"
#include "flash_param.h"
#include <string.h>

/* ============================================================
 *  static 变量（全部放 BSS 段，防栈溢出）
 * ============================================================ */
static char     s_input_buf[CODE_LEN];      /* 用户输入缓冲，'0'~'9' */
static uint8_t  s_cursor = 0;               /* 光标位置 0~5 */
static uint8_t  s_err_cnt = 0;              /* 错误次数 */
static code_state_e s_state = CODE_STATE_INPUT;  /* 当前状态 */
static uint32_t s_lock_start_tick = 0;      /* 锁定起始 tick */
static char     s_correct_code[CODE_LEN];   /* 从 Flash 读的正确码 */

/* ============================================================
 *  初始化
 * ============================================================ */
void CodeCheck_Init(void)
{
    memset(s_input_buf, '0', CODE_LEN);   /* 输入缓冲初始化为 "000000" */
    s_cursor = 0;
    s_err_cnt = 0;
    s_state = CODE_STATE_INPUT;
    s_lock_start_tick = 0;
    FlashParam_GetCurrentCode(s_correct_code, CODE_LEN);  /* 从 Flash 读正确码 */
    uart_printf_mutex("[CODE] Init. correct code loaded from Flash.\r\n");
}

/* ============================================================
 *  KEY0 短按：光标后移一位
 * ============================================================ */
void CodeCheck_OnKey0(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) return;  /* 锁定期间忽略 */
    s_cursor = (uint8_t)((s_cursor + 1U) % CODE_LEN);       /* 0→1→2→3→4→5→0 循环 */
    uart_printf_mutex("[CODE] KEY0: cursor=%u\r\n", s_cursor);
}

/* ============================================================
 *  KEY1 短按：当前位数字 +1（0→9 循环）
 * ============================================================ */
void CodeCheck_OnKey1(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) return;
    if (s_input_buf[s_cursor] < '9') {
        s_input_buf[s_cursor]++;
    } else {
        s_input_buf[s_cursor] = '0';
    }
    uart_printf_mutex("[CODE] KEY1: buf[%u]=%c\r\n", s_cursor, s_input_buf[s_cursor]);
}

/* ============================================================
 *  确认：校验 6 位
 *  正确 → 开柜 + 蜂鸣短响 + 清空输入
 *  错误 → 蜂鸣长响 + 清空输入 + 错误计数 +1
 *  10 次错误 → 锁定 30 秒
 * ============================================================ */
void CodeCheck_OnConfirm(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) {
        uart_printf_mutex("[CODE] LOCKED, ignore confirm.\r\n");
        return;
    }

    /* 对比输入和正确码 */
    if (memcmp(s_input_buf, s_correct_code, CODE_LEN) == 0) {
        /* ====== 正确 ====== */
        uart_printf_mutex("[CODE] CONFIRM OK! Unlocking cabinet.\r\n");
        Cabinet_FSM_OpenRequest();                    /* 触发开柜 */
        s_err_cnt = 0;                                /* 清错误计数 */
        memset(s_input_buf, '0', CODE_LEN);           /* 清输入 */
        s_cursor = 0;
        /* 蜂鸣短响（osDelay 不卡调度，让出 CPU 给其他任务）*/
        BUZZER_ON();
        osDelay(CODE_BEEP_OK_MS);
        BUZZER_OFF();
    } else {
        /* ====== 错误 ====== */
        s_err_cnt++;
        uart_printf_mutex("[CODE] CONFIRM FAIL! err_cnt=%u/%u\r\n",
                          s_err_cnt, CODE_MAX_ERR);
        memset(s_input_buf, '0', CODE_LEN);           /* 清输入 */
        s_cursor = 0;
        /* 蜂鸣长响 */
        BUZZER_ON();
        osDelay(CODE_BEEP_ERR_MS);
        BUZZER_OFF();

        /* 10 次错误 → 锁定 30 秒 */
        if (s_err_cnt >= CODE_MAX_ERR) {
            s_state = CODE_STATE_LOCKED;
            s_lock_start_tick = osKernelGetTickCount();
            uart_printf_mutex("[CODE] LOCKED 30s (err_cnt=%u reached limit).\r\n",
                             s_err_cnt);
        }
    }
}

/* ============================================================
 *  获取当前状态（自动检查锁定是否到期）
 * ============================================================ */
code_state_e CodeCheck_GetState(void)
{
    if (s_state == CODE_STATE_LOCKED) {
        uint32_t elapsed = osKernelGetTickCount() - s_lock_start_tick;
        if (elapsed >= CODE_LOCK_MS) {
            /* 锁定期满，自动恢复输入态 */
            s_state = CODE_STATE_INPUT;
            s_err_cnt = 0;
            memset(s_input_buf, '0', CODE_LEN);
            s_cursor = 0;
            uart_printf_mutex("[CODE] Lock expired, resume input.\r\n");
        }
    }
    return s_state;
}

uint8_t CodeCheck_GetCursor(void) { return s_cursor; }
uint8_t CodeCheck_GetErrCnt(void) { return s_err_cnt; }

uint32_t CodeCheck_GetRemainLockMs(void)
{
    if (CodeCheck_GetState() != CODE_STATE_LOCKED) return 0U;
    uint32_t elapsed = osKernelGetTickCount() - s_lock_start_tick;
    if (elapsed >= CODE_LOCK_MS) return 0U;
    return CODE_LOCK_MS - elapsed;
}

void CodeCheck_GetInputBuf(char *out, uint8_t len)
{
    if (out == NULL || len < (uint8_t)(CODE_LEN + 1U)) return;
    memcpy(out, s_input_buf, CODE_LEN);
    out[CODE_LEN] = '\0';
}

