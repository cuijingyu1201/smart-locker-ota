#include "code_check.h"
#include "app_buzzer.h"
#include "app_uart.h"
#include "cabinet_fsm.h"
#include "flash_param.h"
#include <string.h>

static char     s_input_buf[CODE_LEN];      
static uint8_t  s_cursor = 0;               
static uint8_t  s_input_len = 0;            
static uint8_t  s_err_cnt = 0;              
static code_state_e s_state = CODE_STATE_INPUT; 
static uint32_t s_lock_start_tick = 0;     
static uint32_t s_beep_until_tick = 0;   
static char     s_correct_code[CODE_LEN + 1];   

void CodeCheck_Init(void)
{
    memset(s_input_buf, '0', CODE_LEN);   
    s_cursor = 0;
    s_input_len = 0;
    s_err_cnt = 0;
    s_state = CODE_STATE_INPUT;
    s_lock_start_tick = 0;
    if (FlashParam_GetCurrentCode(s_correct_code, (uint8_t)(CODE_LEN + 1U)) != 0) {
        memcpy(s_correct_code, "123456", CODE_LEN);
        s_correct_code[CODE_LEN] = '\0';
        uart_printf_mutex("[CODE] WARN: use default code 123456.\r\n");
    } else {
        uart_printf_mutex("[CODE] Init. correct code=%s loaded from Flash.\r\n", s_correct_code);
    }
}

void CodeCheck_OnKey0(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) return;  
    s_cursor = (uint8_t)((s_cursor + 1U) % CODE_LEN);       
    uart_printf_mutex("[CODE] KEY0: cursor=%u\r\n", s_cursor);
}


void CodeCheck_OnKey1(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) return;
    if (s_input_buf[s_cursor] < '9') {
        s_input_buf[s_cursor]++;
    } else {
        s_input_buf[s_cursor] = '0';
    }
    uart_printf_mutex("[CODE] KEY1: buf[%u]=%c\r\n", s_cursor, s_input_buf[s_cursor]);
    if ((uint8_t)(s_cursor + 1U) > s_input_len) s_input_len = (uint8_t)(s_cursor + 1U);
}

void CodeCheck_OnConfirm(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) {
        uart_printf_mutex("[CODE] LOCKED, ignore confirm.\r\n");
        return;
    }

    if (s_input_len < CODE_LEN) {
        uart_printf_mutex("[CODE] Need %u more digit(s).\r\n",
                         (uint8_t)(CODE_LEN - s_input_len));
        return;
    }

    if (memcmp(s_input_buf, s_correct_code, CODE_LEN) == 0) {
        uart_printf_mutex("[CODE] CONFIRM OK! Unlocking cabinet.\r\n");
        Cabinet_FSM_OpenRequest();                   
        s_err_cnt = 0;                               
        memset(s_input_buf, '0', CODE_LEN);           
        s_cursor = 0;
        s_input_len = 0;
        BUZZER_ON();
        s_beep_until_tick = osKernelGetTickCount() + CODE_BEEP_OK_MS;   
    } else {
        s_err_cnt++;
        uart_printf_mutex("[CODE] CONFIRM FAIL! err_cnt=%u/%u\r\n",
                          s_err_cnt, CODE_MAX_ERR);
        memset(s_input_buf, '0', CODE_LEN);          
        s_cursor = 0;
        s_input_len = 0;
        BUZZER_ON();
        s_beep_until_tick = osKernelGetTickCount() + CODE_BEEP_ERR_MS;  

        if (s_err_cnt >= CODE_MAX_ERR) {
            s_state = CODE_STATE_LOCKED;
            s_lock_start_tick = osKernelGetTickCount();
            uart_printf_mutex("[CODE] LOCKED 30s (err_cnt=%u reached limit).\r\n",
                             s_err_cnt);
        }
    }
}

code_state_e CodeCheck_GetState(void)
{
    if (s_state == CODE_STATE_LOCKED) {
        uint32_t elapsed = osKernelGetTickCount() - s_lock_start_tick;
        if (elapsed >= CODE_LOCK_MS) {
            s_state = CODE_STATE_INPUT;
            s_err_cnt = 0;
            memset(s_input_buf, '0', CODE_LEN);
            s_cursor = 0;
            s_input_len = 0;
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

void CodeCheck_OnDigit(uint8_t digit)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) return;
    if (digit > 9U) return;   /* out of range */
	  if (s_input_len >= CODE_LEN) return;   /* 6 digits full: wait for OK, ignore more digits */
    s_input_buf[s_cursor] = (char)('0' + digit);
    uart_printf_mutex("[CODE] DIGIT: buf[%u]=%c\r\n", s_cursor, s_input_buf[s_cursor]);
    s_cursor = (uint8_t)((s_cursor + 1U) % CODE_LEN);
    if (s_input_len < CODE_LEN) s_input_len++;   /* auto advance */
}

void CodeCheck_OnClear(void)
{
    if (CodeCheck_GetState() == CODE_STATE_LOCKED) return;
    memset(s_input_buf, '0', CODE_LEN);
    s_cursor = 0;
    s_input_len = 0;
    uart_printf_mutex("[CODE] CLEAR: input reset.\r\n");
}

uint8_t CodeCheck_GetInputLen(void) { return s_input_len; }

/* ============================================================
 *  Non-blocking beep poll: call from TaskLcdUI 20ms loop.
 *  Turns off buzzer when beep duration expires.
 * ============================================================ */
void CodeCheck_BeepPoll(void)
{
    if (s_beep_until_tick != 0U && osKernelGetTickCount() >= s_beep_until_tick) {
        BUZZER_OFF();
        s_beep_until_tick = 0U;
    }
}
