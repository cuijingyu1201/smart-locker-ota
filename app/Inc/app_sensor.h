#ifndef APP_SENSOR_H
#define APP_SENSOR_H

#include "main.h"

/* 双传感器：霍尔 A3144 门检(PE0) + 红外对管物品检测(PC0)
 * 均为内部上拉数字输入，软件连续 N 次一致才确认（去抖）。 */

/* 门状态（霍尔）：A3144 磁铁靠近=低=门关，远离=高=门开 */
typedef enum {
    DOOR_UNKNOWN = 0,
    DOOR_CLOSED,      /* 霍尔=0，磁铁正对，门关 */
    DOOR_OPEN         /* 霍尔=1，门开 */
} door_state_e;

/* 物品状态（红外）：极性先按原始电平打印实测，再定 */
typedef enum {
    ITEM_UNKNOWN = 0,
    ITEM_PRESENT,     /* 光路被挡 */
    ITEM_ABSENT       /* 光路畅通 */
} item_state_e;

void          App_Sensor_Init(void);
void          App_Sensor_Scan(void);          /* 周期调用(建议20ms)，内部去抖 */
door_state_e  App_Sensor_GetDoor(void);
item_state_e  App_Sensor_GetItem(void);
uint8_t       App_Sensor_RawHall(void);       /* 调试：原始电平 */
uint8_t       App_Sensor_RawIR(void);         /* 调试：原始电平 */

#endif

