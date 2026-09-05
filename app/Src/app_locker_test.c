#include "app_locker_test.h"
#include "app_servo.h"
#include "app_sensor.h"
#include "app_uart.h"

/* D3 最小测试（不做状态机，状态机是 D4）：
 *  - 每 2 秒舵机 关锁→开锁→关锁 循环
 *  - 每 20ms 扫描霍尔/红外（去抖），状态变化由 sensor 驱动打印
 *  - 每 2 秒打印一次原始电平，方便确认红外极性 */
void TaskLockerTest(void *argument)
{
    (void)argument;
    uint8_t unlock = 0;
    uint16_t tick = 0;

    for (;;) {
        App_Sensor_Scan();              /* 20ms 周期去抖扫描 */
        tick++;

        if (tick >= 100U) {             /* 100×20ms = 2s */
            tick = 0;
            unlock ^= 1U;
            if (unlock) {
                App_Servo_Unlock();     /* 1.5ms 开锁 */
            } else {
                App_Servo_Lock();       /* 0.5ms 关锁 */
            }
            /* 周期打印原始电平，用来核对红外/霍尔逻辑 */
            uart_printf_mutex("[RAW] hall(PE0)=%u  ir(PC0)=%u  door=%d item=%d\r\n",
                              (unsigned)App_Sensor_RawHall(),
                              (unsigned)App_Sensor_RawIR(),
                              (int)App_Sensor_GetDoor(),
                              (int)App_Sensor_GetItem());
        }
        osDelay(20);
    }
}

