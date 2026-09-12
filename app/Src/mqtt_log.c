#include "mqtt_log.h"
#include "app_ipc.h"
#include "app_uart.h"
#include <string.h>

/* ============================================================
 *  static 变量（全放 BSS 段，防栈溢出）
 * ============================================================ */
static char             s_log_buf[MQTT_LOG_LINES][MQTT_LOG_LEN];  /* 环形缓冲 */
static mqtt_log_type_e s_log_type[MQTT_LOG_LINES];                /* 每行类型 */
static uint8_t          s_head = 0;     /* 写入位置（最新行的下一行）*/
static uint8_t          s_count = 0;   /* 已存行数（0~6）*/

/* ============================================================
 *  初始化
 * ============================================================ */
void MqttLog_Init(void)
{
    memset(s_log_buf, 0, sizeof(s_log_buf));
    memset(s_log_type, 0, sizeof(s_log_type));
    s_head = 0;
    s_count = 0;
    uart_printf_mutex("[MQTT-LOG] Init. ring buffer %u x %u\r\n",
                      MQTT_LOG_LINES, MQTT_LOG_LEN);
}

/* ============================================================
 *  追加一行 + EventGroup 通知 TaskLcdUI 刷新
 * ============================================================ */
void MqttLog_Append(mqtt_log_type_e type, const char *msg)
{
    if (msg == NULL) return;

    /* 写入环形缓冲 */
    uint8_t idx = s_head;
    strncpy(s_log_buf[idx], msg, MQTT_LOG_LEN - 1U);
    s_log_buf[idx][MQTT_LOG_LEN - 1U] = '\0';
    s_log_type[idx] = type;

    /* 环形前进 */
    s_head = (uint8_t)((s_head + 1U) % MQTT_LOG_LINES);
    if (s_count < MQTT_LOG_LINES) s_count++;

    /* 通知 TaskLcdUI 刷新日志区 */
    if (g_event_group_handle != NULL) {
        osEventFlagsSet(g_event_group_handle, BIT_MQTT_LOG_REFRESH);
    }
}

/* ============================================================
 *  读第 idx 行（idx=0 = 最新，idx=1 = 次新，...）
 * ============================================================ */
void MqttLog_GetLine(uint8_t idx, char *out, uint8_t len)
{
    if (out == NULL || len == 0U || idx >= s_count) {
        if (out != NULL && len > 0U) out[0] = '\0';
        return;
    }
    /* 最新行 = s_head - 1，次新 = s_head - 2，... */
    uint8_t src = (uint8_t)((s_head + MQTT_LOG_LINES - 1U - idx) % MQTT_LOG_LINES);
    strncpy(out, s_log_buf[src], (size_t)len - 1U);
    out[len - 1U] = '\0';
}

mqtt_log_type_e MqttLog_GetType(uint8_t idx)
{
    if (idx >= s_count) return MQTT_LOG_TX;
    uint8_t src = (uint8_t)((s_head + MQTT_LOG_LINES - 1U - idx) % MQTT_LOG_LINES);
    return s_log_type[src];
}

