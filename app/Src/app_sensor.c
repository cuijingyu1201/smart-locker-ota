#include "app_sensor.h"
#include "app_uart.h"

/* PE0 / PC0 在 CubeMX 里已配成上拉输入，这里直接读 */
#define HALL_READ()   HAL_GPIO_ReadPin(HALL_DOOR_GPIO_Port, HALL_DOOR_Pin)
#define IR_READ()     HAL_GPIO_ReadPin(IR_ITEM_GPIO_Port,   IR_ITEM_Pin)

#define DEBOUNCE_N    5U    /* 连续 5 次一致(5×20ms=100ms)才确认 */

static door_state_e s_door = DOOR_UNKNOWN;
static item_state_e s_item = ITEM_UNKNOWN;

void App_Sensor_Init(void)
{
    s_door = DOOR_UNKNOWN;
    s_item = ITEM_UNKNOWN;
    uart_printf_mutex("[SENSOR] Hall(PE0)=%u  IR(PC0)=%u (raw, 上电)\r\n",
                      (unsigned)HALL_READ(), (unsigned)IR_READ());
}

/* 周期调用。去抖：连续 DEBOUNCE_N 次相同才更新状态；状态变化时打印。 */
void App_Sensor_Scan(void)
{
    static uint8_t hall_cnt = 0, ir_cnt = 0;
    static uint8_t hall_last = 0xFF, ir_last = 0xFF;
    uint8_t h = HALL_READ();
    uint8_t r = IR_READ();

    /* ---- 霍尔去抖 ---- */
    if (h == hall_last) {
        if (hall_cnt < DEBOUNCE_N) {
            hall_cnt++;
            if (hall_cnt == DEBOUNCE_N) {
                door_state_e ns = (h == 0) ? DOOR_CLOSED : DOOR_OPEN; /* A3144: 靠近=0 */
                if (ns != s_door) {
                    s_door = ns;
                    uart_printf_mutex("[DOOR] %s (hall=%u)\r\n",
                                      (ns == DOOR_CLOSED) ? "CLOSED 门关" : "OPEN   门开", h);
                }
            }
        }
    } else {
        hall_cnt = 0;
        hall_last = h;
    }

    /* ---- 红外去抖（极性待定：先打印，挡光时看电平是 0 还是 1）---- */
    if (r == ir_last) {
        if (ir_cnt < DEBOUNCE_N) {
            ir_cnt++;
            if (ir_cnt == DEBOUNCE_N) {
                /* 暂定：挡光=0=有物（多数数字对管模块如此）。实测若反了，交换下面两行 */
                item_state_e ns = (r == 0) ? ITEM_PRESENT : ITEM_ABSENT;
                if (ns != s_item) {
                    s_item = ns;
                    uart_printf_mutex("[ITEM] %s (ir=%u)\r\n",
                                      (ns == ITEM_PRESENT) ? "PRESENT 有物挡光" : "ABSENT  无物", r);
                }
            }
        }
    } else {
        ir_cnt = 0;
        ir_last = r;
    }
}

door_state_e App_Sensor_GetDoor(void) { return s_door; }
item_state_e App_Sensor_GetItem(void) { return s_item; }
uint8_t App_Sensor_RawHall(void) { return HALL_READ(); }
uint8_t App_Sensor_RawIR(void)   { return IR_READ(); }

