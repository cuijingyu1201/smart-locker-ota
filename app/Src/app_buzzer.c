#include "app_buzzer.h"
#include "app_uart.h"

/* 初始化：确保蜂鸣器关闭状态（防上电误响） */
void App_Buzzer_Init(void)
{
    BUZZER_OFF();
}

/* 阻塞式短响（仅用于初始化自检，任务里不要用，会卡调度） */
void App_Buzzer_Beep_Blocking(uint32_t duration_ms)
{
    BUZZER_ON();
    HAL_Delay(duration_ms);
    BUZZER_OFF();
}

