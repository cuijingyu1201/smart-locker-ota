#include "app_servo.h"
#include "tim.h"
#include "app_uart.h"

extern TIM_HandleTypeDef htim3;   /* CubeMX 在 tim.c 生成 */

void App_Servo_SetPulseUs(uint16_t us)
{
    /* CCR 单位 = 1us（PSC=71 → 1MHz）。钳位防越界 */
    if (us < 500U)  us = 500U;
    if (us > 2500U) us = 2500U;
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, us);
}

void App_Servo_Lock(void)
{
    App_Servo_SetPulseUs(SERVO_PWM_LOCK_US);
    uart_printf_mutex("[SERVO] LOCK  (pulse=%dus)\r\n", SERVO_PWM_LOCK_US);
}

void App_Servo_Unlock(void)
{
    App_Servo_SetPulseUs(SERVO_PWM_UNLOCK_US);
    uart_printf_mutex("[SERVO] UNLOCK(pulse=%dus)\r\n", SERVO_PWM_UNLOCK_US);
}

void App_Servo_Init(void)
{
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);   /* 启动 PA6 PWM 输出 */
    App_Servo_SetPulseUs(SERVO_PWM_LOCK_US);     /* 上电先关锁 */
    uart_printf_mutex("[SERVO] TIM3_CH1 PWM start (50Hz), init=LOCK\r\n");
}
