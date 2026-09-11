#ifndef __CODE_CHECK_H
#define __CODE_CHECK_H

#include "main.h"
#include "cmsis_os2.h"

/* ============================================================
 *  D8: 6 位取件码校验（1 柜简化版）
 *
 *  操作方式（板载按键）：
 *    KEY0 短按 = 光标移到下一位（0→1→2→3→4→5→0 循环）
 *    KEY1 短按 = 当前位数字 +1（0→9 循环）
 *    确认键   = 校验 6 位（KEY_WKUP 短按，第 3 步接入）
 *
 *  校验规则：
 *    正确 → 触发开柜 + 蜂鸣短响 100ms + 清空输入
 *    错误 → 蜂鸣长响 1 秒 + 清空输入 + 错误计数 +1
 *    10 次错误 → 锁定 30 秒，期间所有按键无效
 * ============================================================ */

#define CODE_LEN           6U          /* 6 位取件码 */
#define CODE_MAX_ERR       10U         /* 10 次错误锁定 */
#define CODE_LOCK_MS       30000U      /* 锁定 30 秒 */
#define CODE_BEEP_OK_MS    100U        /* 正确：短响 100ms */
#define CODE_BEEP_ERR_MS   1000U       /* 错误：长响 1 秒 */

typedef enum {
    CODE_STATE_INPUT = 0,    /* 输入中 */
    CODE_STATE_LOCKED        /* 锁定中（30 秒倒计时） */
} code_state_e;

/* ---------- API ---------- */
void CodeCheck_Init(void);                    /* 初始化，从 Flash 读 current_code */
void CodeCheck_OnKey0(void);                  /* KEY0 短按：光标移到下一位 */
void CodeCheck_OnKey1(void);                  /* KEY1 短按：当前位 +1 */
void CodeCheck_OnConfirm(void);               /* 确认：校验 6 位 */
code_state_e CodeCheck_GetState(void);        /* 获取当前状态（自动检查锁定是否到期）*/
uint8_t CodeCheck_GetCursor(void);            /* 获取光标位置 0~5 */
uint8_t CodeCheck_GetErrCnt(void);            /* 获取错误次数 */
uint32_t CodeCheck_GetRemainLockMs(void);     /* 获取剩余锁定时间 ms */
void CodeCheck_GetInputBuf(char *out, uint8_t len);  /* 读输入缓冲（LCD 显示用）*/

#endif /* __CODE_CHECK_H */

