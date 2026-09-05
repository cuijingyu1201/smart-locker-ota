#include "app_locker_test.h"
#include "cabinet_fsm.h"
#include "app_uart.h"

/*   柜子状态机任务封装
 * - 调用 Cabinet_FSM_Init 初始化
 * - 调用 Cabinet_FSM_Task 运行状态机
 */
void TaskLocker(void *argument)
{
    (void)argument;
    Cabinet_FSM_Init();
    Cabinet_FSM_Task(argument);
}

