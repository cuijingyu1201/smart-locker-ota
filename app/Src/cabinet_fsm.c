#include "cabinet_fsm.h"
#include "app_servo.h"
#include "app_sensor.h"
#include "app_uart.h"
#include "cmsis_os2.h"
#include "app_ipc.h"
#include "flash_param.h" 

/* ============================================================
 *  D4 配置：是否启用红外取件检测
 *  0 = 不接红外（只靠霍尔 + 超时，用于 D4 初步测试）
 *  1 = 接红外（完整双传感器融合，后续模块到货后打开）
 * ============================================================ */
#define CABINET_USE_IR   0

/* ============================================================
 *  状态机内部状态（static 保留，任务级生命周期）
 * ============================================================ */
static cabinet_state_e s_state = CAB_STATE_CLOSED;
static fault_reason_e  s_fault = FAULT_NONE;
static uint32_t        s_state_enter_tick = 0;   /* 进入当前态的 tick */

/* 开柜请求标志（外部置 1，状态机消费后置 0） */
static volatile uint8_t s_open_request = 0;

/* KEY1 长按计时 */
static uint32_t s_key1_press_tick = 0;
static uint8_t  s_key1_was_pressed = 0;

/* ============================================================
 *  辅助：判断 KEY1（PE3）是否按下（低电平有效）
 * ============================================================ */
static uint8_t key1_is_pressed(void)
{
    /* 按下=低电平，松开=高电平（内部上拉） */
    return (HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin) == GPIO_PIN_RESET) ? 1U : 0U;
}

/* ============================================================
 *  外部触发开柜
 * ============================================================ */
void Cabinet_FSM_OpenRequest(void)
{
    if (s_state == CAB_STATE_CLOSED) {
        s_open_request = 1;
        uart_printf_mutex("[CAB] Open request received.\r\n");
    } else {
        uart_printf_mutex("[CAB] Open request IGNORED (state=%d).\r\n", s_state);
    }
}

/* ============================================================
 *  读取当前状态（给 LCD/MQTT 用）
 * ============================================================ */
cabinet_state_e Cabinet_FSM_GetState(void) { return s_state; }
fault_reason_e  Cabinet_FSM_GetFaultReason(void) { return s_fault; }

const char* Cabinet_FSM_StateStr(cabinet_state_e s)
{
    switch (s) {
        case CAB_STATE_CLOSED:          return "CLOSED";
        case CAB_STATE_OPENING:         return "OPENING";
        case CAB_STATE_WAIT_PICKUP:     return "WAIT_PICKUP";
        case CAB_STATE_TIMEOUT_CLOSING: return "TIMEOUT_CLOSING";
        case CAB_STATE_FAULT:           return "FAULT";
        default:                        return "UNKNOWN";
    }
}

/* ============================================================
 *  状态切换（统一入口，打印日志 + 记录进入时间）
 * ============================================================ */
static void fsm_transition(cabinet_state_e new_state)
{
    if (new_state == s_state) return;
    uart_printf_mutex("[CAB] %s -> %s\r\n",
                      Cabinet_FSM_StateStr(s_state),
                      Cabinet_FSM_StateStr(new_state));
    s_state = new_state;
    s_state_enter_tick = osKernelGetTickCount();
		/* 稳态持久化 —— 进 CLOSED 或 FAULT 时写 Flash */
    if (new_state == CAB_STATE_CLOSED) {
        FlashParam_SetDoorState(0U);   /* CLOSED */
    } else if (new_state == CAB_STATE_FAULT) {
        FlashParam_SetDoorState(2U);   /* FAULT */
    }
    /* OPENING/WAIT_PICKUP/TIMEOUT_CLOSING 是过渡态，不写 Flash */
}

/* ============================================================
 *  进入 FAULT 态
 * ============================================================ */
static void fsm_enter_fault(fault_reason_e reason)
{
    s_fault = reason;
    App_Servo_Lock();   /* 故障时强制关锁，防止柜门大开 */
    uart_printf_mutex("[CAB] FAULT! reason=%d\r\n", reason);
    fsm_transition(CAB_STATE_FAULT);
}

/* ============================================================
 *  初始化
 * ============================================================ */
void Cabinet_FSM_Init(void)
{
    s_state = CAB_STATE_CLOSED;
    s_fault = FAULT_NONE;
    s_open_request = 0;
    s_state_enter_tick = osKernelGetTickCount();
    uart_printf_mutex("[CAB] FSM init: state=CLOSED\r\n");
}

/* ============================================================
 *  状态机主任务（每 20ms 跑一次）
 * ============================================================ */
void Cabinet_FSM_Task(void *argument)
{
    (void)argument;

    for (;;) {
        /* 先扫描传感器（去抖在 App_Sensor_Scan 内部完成） */
        App_Sensor_Scan();

        uint32_t now = osKernelGetTickCount();
        uint32_t elapsed = now - s_state_enter_tick;

        /* ====== KEY1 长按 3 秒清 FAULT ====== */
        if (key1_is_pressed()) {
            if (!s_key1_was_pressed) {
                s_key1_press_tick = now;
                s_key1_was_pressed = 1;
            } else {
                if ((now - s_key1_press_tick) >= KEY_FAULT_CLEAR_MS) {
                    if (s_state == CAB_STATE_FAULT) {
                        s_fault = FAULT_NONE;
                        fsm_transition(CAB_STATE_CLOSED);
                        uart_printf_mutex("[CAB] KEY1 long-press: FAULT cleared.\r\n");
                    }
                    s_key1_press_tick = now;  /* 防止重复触发 */
                }
            }
        } else {
            s_key1_was_pressed = 0;
        }

        /* ====== 状态处理 ====== */
        switch (s_state) {

        /* ---------- CLOSED：门关待机，等开柜请求 ---------- */
        case CAB_STATE_CLOSED: {
            if (s_open_request) {
                s_open_request = 0;
                App_Servo_Unlock();          /* 舵机开锁 */
								FlashParam_RecordOpen();     /* 记录开柜（写 door_state=1 + 时间戳） */
                fsm_transition(CAB_STATE_OPENING);
            }
            break;
        }

        /* ---------- OPENING：舵机已开，等门被推开 ---------- */
        case CAB_STATE_OPENING: {
            door_state_e door = App_Sensor_GetDoor();
            item_state_e item = App_Sensor_GetItem();

            /* 条件：门开了(霍尔=1) 且 红外状态变化过(有人伸手) */
           #if CABINET_USE_IR
							/* 完整模式：门开了 且 红外检测到手伸进去 */
							if (door == DOOR_OPEN && item == ITEM_PRESENT) {
									fsm_transition(CAB_STATE_WAIT_PICKUP);
							}
					#else
							/* 简化模式：门开了就进 WAIT_PICKUP（不判红外） */
							if (door == DOOR_OPEN) {
									fsm_transition(CAB_STATE_WAIT_PICKUP);
							}
					#endif
						else if (elapsed >= OPENING_TIMEOUT_MS) {
                /* 5 秒后门没推开 → 锁卡住或用户没开门 */
                fsm_enter_fault(FAULT_OPENING_TIMEOUT);
            }
            break;
        }

        /* ---------- WAIT_PICKUP：门开着，等用户取件 ---------- */
        case CAB_STATE_WAIT_PICKUP: {
            item_state_e item = App_Sensor_GetItem();

            /* 红外从"有物挡光"变回"无物" → 用户取走了物品 */
            #if CABINET_USE_IR
								/* 完整模式：红外检测到手拿走 → 立即关锁 */
								if (item == ITEM_ABSENT) {
										App_Servo_Lock();
										fsm_transition(CAB_STATE_TIMEOUT_CLOSING);
								} else
						#endif
								if (elapsed >= PICKUP_TIMEOUT_MS) {
										/* 超时自动关锁（不接红外时只能走这条路） */
										uart_printf_mutex("[CAB] Pickup timeout, auto closing.\r\n");
										App_Servo_Lock();
										fsm_transition(CAB_STATE_TIMEOUT_CLOSING);
								}
            break;
        }

        /* ---------- TIMEOUT_CLOSING：关锁中，等门关上 ---------- */
        case CAB_STATE_TIMEOUT_CLOSING: {
            door_state_e door = App_Sensor_GetDoor();

            if (door == DOOR_CLOSED) {
                fsm_transition(CAB_STATE_CLOSED);   /* 门关到位 → 回待机 */
            } else if (elapsed >= CLOSING_TIMEOUT_MS) {
                /* 5 秒后门没关上 → 故障 */
                fsm_enter_fault(FAULT_CLOSING_TIMEOUT);
            }
            break;
        }

        /* ---------- FAULT：故障态，只等 KEY1 清除 ---------- */
        case CAB_STATE_FAULT: {
            /* 什么都不做，等 KEY1 长按清除（在上面处理） */
            break;
        }

        } /* switch end */

        osDelay(20);   /* 20ms 扫描一次 = 50Hz */
    }
}

