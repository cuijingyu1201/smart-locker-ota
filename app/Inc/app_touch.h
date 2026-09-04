#ifndef APP_TOUCH_H
#define APP_TOUCH_H

#include "cmsis_os2.h"

#define TASK_TOUCH_STACK_SIZE_BYTES    (512U * 4U)
#define TASK_TOUCH_PRIORITY            (osPriorityNormal)

int32_t App_Touch_Init(void);      /* 触摸初始化（在 App_Lcd_Init 之后调用） */
void    TaskTouch(void *argument); /* 触摸测试任务 */

void App_Touch_Calibrate(void);   /* 屏幕2点触摸校准（按住WK_UP上电触发） */

#endif /* APP_TOUCH_H */
