#ifndef CABINET_FSM_H
#define CABINET_FSM_H

#include "main.h"

/* ============================================================
 *  D4: 1 柜状态机（6 态）
 *  CLOSED → OPENING → WAIT_PICKUP → TIMEOUT_CLOSING → CLOSED
 *                       ↓ 超时/异常
 *                     FAULT
 * ============================================================ */

/* ---------- 状态枚举 ---------- */
typedef enum {
    CAB_STATE_CLOSED = 0,       /* 门关，待机 */
    CAB_STATE_OPENING,           /* 舵机已开锁，等门被推开 */
    CAB_STATE_WAIT_PICKUP,       /* 门已开，等用户取件 */
    CAB_STATE_TIMEOUT_CLOSING,   /* 取件超时，自动关门中 */
    CAB_STATE_FAULT              /* 故障态（锁卡住/传感器异常），需管理员清除 */
} cabinet_state_e;

/* ---------- 故障原因 ---------- */
typedef enum {
    FAULT_NONE = 0,
    FAULT_OPENING_TIMEOUT,       /* 开锁后门没开（霍尔没到位） */
    FAULT_PICKUP_TIMEOUT,        /* 开门后没人取件超时 */
    FAULT_CLOSING_TIMEOUT,       /* 关门后门没关上（霍尔没到位） */
    FAULT_SENSOR_ABNORMAL        /* 传感器读数异常 */
} fault_reason_e;

/* ---------- 超时配置（毫秒） ---------- */
#define OPENING_TIMEOUT_MS      5000U   /* 开锁后 5 秒内门必须推开 */
#define PICKUP_TIMEOUT_MS       5000U  /* 开门后 10 秒内必须取件 */
#define CLOSING_TIMEOUT_MS      5000U   /* 关门指令后 5 秒内门必须关到位 */
#define KEY_FAULT_CLEAR_MS      3000U   /* 长按 KEY1 3 秒清故障 */

/* ---------- API ---------- */
void  Cabinet_FSM_Init(void);                    /* 上电初始化状态机 */
void  Cabinet_FSM_Task(void *argument);          /* 状态机主任务（FreeRTOS 任务函数） */
void  Cabinet_FSM_OpenRequest(void);             /* 外部触发开柜（MQTT/按键/LCD按钮调用） */
cabinet_state_e Cabinet_FSM_GetState(void);      /* 读当前状态（给 LCD UI 显示用） */
fault_reason_e  Cabinet_FSM_GetFaultReason(void); /* 读故障原因 */
const char*     Cabinet_FSM_StateStr(cabinet_state_e s);  /* 状态转字符串（打印用） */

#endif /* CABINET_FSM_H */

