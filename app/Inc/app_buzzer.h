#ifndef __APP_BUZZER_H
#define __APP_BUZZER_H

#include "main.h"

/* ============================================================
 *  蜂鸣器全局开关
 *  0 = 静音（所有 BUZZER_ON/OFF 变空操作，功能代码保留）
 *  1 = 启用蜂鸣器
 *  整体调试时改回 1 即可
 * ============================================================ */
#define APP_BUZZER_ENABLED  0

#if APP_BUZZER_ENABLED
  #define BUZZER_ON()   HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET)
  #define BUZZER_OFF()  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET)
#else
  #define BUZZER_ON()   ((void)0)   /* 静音：空操作 */
  #define BUZZER_OFF()  ((void)0)   /* 静音：空操作 */
#endif
	
void App_Buzzer_Init(void);                    /* 初始化（确保关闭状态） */
void App_Buzzer_Beep_Blocking(uint32_t duration_ms);  /* 阻塞式短响（仅自检用）*/
	
#endif /* __APP_BUZZER_H */

