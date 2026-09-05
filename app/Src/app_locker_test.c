#include "app_locker_test.h"
#include "app_servo.h"
#include "app_sensor.h"
#include "app_uart.h"

/* D3 ��С���ԣ�����״̬����״̬���� D4����
 *  - ÿ 2 ���� ���������������� ѭ��
 *  - ÿ 20ms ɨ�����/���⣨ȥ������״̬�仯�� sensor ������ӡ
 *  - ÿ 2 ���ӡһ��ԭʼ��ƽ������ȷ�Ϻ��⼫�� */
void TaskLockerTest(void *argument)
{
    (void)argument;
    uint8_t unlock = 0;
    uint16_t tick = 0;

    for (;;) {
        App_Sensor_Scan();              /* 20ms ����ȥ��ɨ�� */
        tick++;

        if (tick >= 100U) {             /* 100×20ms = 2s */
            tick = 0;
            unlock ^= 1U;
            if (unlock) {
                App_Servo_Unlock();     /* 1.5ms 开锁 */
            } else {
                App_Servo_Lock();       /* 0.5ms 关锁 */
            }
        }
        osDelay(20);
    }
}

