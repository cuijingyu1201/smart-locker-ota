#ifndef __APP_ESP8266_H
#define __APP_ESP8266_H

#include "main.h"

/* ==================== WiFi/TCP 状态机状态枚举 ==================== */
typedef enum {
    ESP_STATE_INIT = 0,       /* 刚上电：测试 AT 指令通不通 */
    ESP_STATE_AT_TEST,        /* 发 AT，等 OK */
    ESP_STATE_SET_STA,        /* 设 CWMODE=1，等 OK */
    ESP_STATE_JOIN_WIFI,      /* 发 CWJAP，等 WIFI GOT IP */
    ESP_STATE_CONN_TCP,       /* 发 CIPSTART 连 TCP Server */
    ESP_STATE_MQTT_CONNECT,   /* 发 MQTT CONNECT，等 CONNACK */
    ESP_STATE_MQTT_SUBSCRIBE, /* 发 MQTT SUBSCRIBE */
    ESP_STATE_MQTT_WORKING,   /* 5秒PUBLISH + 30秒PINGREQ */
    ESP_STATE_RECONNECT,      /* 超时/掉线：延迟一下回 INIT 重来 */
} ESP8266_State_t;


/* ==================== 对外 API ==================== */
/*
 * TaskESP8266：FreeRTOS 任务函数
 *   状态机驱动 AT 指令流程：AT→CWMODE→CWJAP→CIPSTART→CIPSEND上报JSON
 *   掉线自动重连
 */
void TaskESP8266(void *argument);

#endif /* __APP_ESP8266_H */
