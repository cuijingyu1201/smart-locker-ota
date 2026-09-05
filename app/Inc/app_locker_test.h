#ifndef APP_LOCKER_TEST_H
#define APP_LOCKER_TEST_H
#include "cmsis_os2.h"

/*  柜子状态机任务（替换原 TaskLockerTest） */
#define TASK_LOCKER_STACK_SIZE_BYTES   (512U * 4U)   /* 2048B */
#define TASK_LOCKER_PRIORITY           (osPriorityAboveNormal)  /* 比传感器高，及时响应 */

void TaskLocker(void *argument);   /* 状态机主任务 */

#endif

