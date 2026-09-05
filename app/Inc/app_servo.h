#ifndef APP_SERVO_H
#define APP_SERVO_H

#include "main.h"

/* SG90 舵机（电子锁）驱动 —— TIM3_CH1 @ PA6, 50Hz
 * 脉宽：500us≈0°(关锁位) / 1500us≈90°(开锁位) / 2500us≈180° */
#define SERVO_PWM_LOCK_US     500    /* 关锁位置 0.5ms */
#define SERVO_PWM_UNLOCK_US   1500   /* 开锁位置 1.5ms */

void    App_Servo_Init(void);              /* 启动 PWM，默认转到关锁位 */
void    App_Servo_SetPulseUs(uint16_t us); /* 直接设脉宽(us) */
void    App_Servo_Lock(void);              /* 关锁 */
void    App_Servo_Unlock(void);            /* 开锁 */

#endif

