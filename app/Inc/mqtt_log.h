#ifndef __MQTT_LOG_H
#define __MQTT_LOG_H

#include "main.h"

/* ============================================================
 *  D10: MQTT 日志环形缓冲（6 行 × 48 字节）
 *
 *  收到 MQTT 消息时调 MqttLog_Append，自动通知 TaskLcdUI 刷新
 *  不在 MQTT 任务里直接写屏（防 FSMC 总线冲突）
 * ============================================================ */

#define MQTT_LOG_LINES    6U     /* 6 行日志 */
#define MQTT_LOG_LEN      48U    /* 每行最大 48 字符 */

typedef enum {
    MQTT_LOG_TX = 0,     /* 上行（发布），蓝色 */
    MQTT_LOG_RX,         /* 下行（命令），绿色 */
    MQTT_LOG_ALERT       /* 告警，红色 */
} mqtt_log_type_e;

/* API */
void MqttLog_Init(void);                                    /* 初始化 */
void MqttLog_Append(mqtt_log_type_e type, const char *msg); /* 追加一行 + EventGroup 通知 */
void MqttLog_GetLine(uint8_t idx, char *out, uint8_t len);  /* 读第 idx 行（idx=0 最新）*/
mqtt_log_type_e MqttLog_GetType(uint8_t idx);               /* 读第 idx 行类型 */

#endif /* __MQTT_LOG_H */

