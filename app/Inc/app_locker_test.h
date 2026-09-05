#ifndef APP_LOCKER_TEST_H
#define APP_LOCKER_TEST_H
#include "cmsis_os2.h"

/* D18(D3) 最小验证任务：舵机开/关锁循环 + 霍尔/红外电平监控 */
#define TASK_LOCKER_STACK_SIZE_BYTES   (512U * 4U)   /* 2048B，和方案一致 */
#define TASK_LOCKER_PRIORITY           (osPriorityNormal)

void TaskLockerTest(void *argument);

#endif

