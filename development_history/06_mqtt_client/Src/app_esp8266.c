#include "app_esp8266.h"
#include "cmsis_os2.h"
#include "usart.h"
#include "app_ipc.h"
#include "app_dht11.h"
#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"   /*  MQTT 协议层 */

/* ==================== 可配置参数 ==================== */
#define ESP_WIFI_SSID       "cai"        /*  2.4G WiFi 名（不能是 5G！） */
#define ESP_WIFI_PASSWORD   "ccc1234567"     /*  WiFi 密码 */
#define ESP_TCP_SERVER_IP   "broker.emqx.io"         /* 改成电脑在同一个 WiFi 的 IPv4 地址 */
#define ESP_TCP_SERVER_PORT 1883                      /* NetAssist 开的 TCP Server 端口 */
#define MQTT_CLIENT_ID      "stm32_dev138"       /* Client ID，必须全局唯一！ */
#define MQTT_TOPIC_SENSOR   "iot/dev001/sensor"  /* 上报温湿度的主题 */
#define MQTT_TOPIC_OTA      "iot/dev001/ota"     /* 订阅OTA下行命令*/

/* ==================== 内部辅助函数声明 ==================== */
static int  ESP8266_SendRaw(const char *data, uint16_t len);
static int  ESP8266_SendAT(const char *cmd, const char *expect_ack, uint32_t timeout_ms);
static void ESP8266_SendJSON_Report(void);
static void MQTT_PublishSensor(void);   /* MQTT PUBLISH 发温湿度 */

/* ================================================================
 * TaskESP8266：WiFi + TCP 状态机任务（优先级 24，栈 512 words）
 * ================================================================ */
void TaskESP8266(void *argument)
{
    (void)argument;
    static ESP8266_State_t state = ESP_STATE_INIT;
    static uint8_t retry_cnt = 0;              /* 单状态重试次数 */
    static uint32_t last_send_tick = 0;        /* 上次 TCP 上报时间戳（WORKING 用） */
    static uint32_t enter_state_tick = 0;      /* 进入当前状态的时间戳（超时判断） */
		static uint32_t last_ping_tick = 0;        /* 上次发PINGREQ的时间戳 */
    char line[256];                             /* 接收行缓冲 */

		/* 等待 ESP8266-12F 模块启动完成（冷启动需要 2-3 秒） */
    printf("[ESP8266] Waiting for module boot (3s)...\r\n");
    osDelay(5000);
	
    /* 启动 USART2 中断接收（IPC 创建好才能启动，因为信号量句柄已经初始化） */
    ESP8266_StartReceiveIT();
    printf("[ESP8266] USART2 RX IT started\r\n");

    enter_state_tick = osKernelGetTickCount();

    for (;;)
    {
        uint32_t now = osKernelGetTickCount();
//        uint32_t state_elapsed = now - enter_state_tick;
//        int at_ok = -1;

        switch (state)
        {
        /* ---------------------------------------------------------------------
         * 状态 1：INIT → 清缓冲 → 切 AT_TEST
         * --------------------------------------------------------------------- */
        case ESP_STATE_INIT:
            ESP8266_ClearRxBuf();
            retry_cnt = 0;
            state = ESP_STATE_AT_TEST;
            enter_state_tick = now;
            printf("[ESP8266] → AT_TEST (retry=%u)\r\n", retry_cnt);
            break;

        /* ---------------------------------------------------------------------
         * 状态 2：AT_TEST → 发 "AT\r\n"，100ms 内收到 "OK" 就过
         * --------------------------------------------------------------------- */
        case ESP_STATE_AT_TEST:
            ESP8266_ClearRxBuf();                                    // 每次清空接收缓冲
            ESP8266_SendRaw("AT\r\n", 4);                            // 每次重发 AT 指令
            retry_cnt++;

            osDelay(300);                                            // 新增：先等 300ms，让 ESP8266 完整回复

            // 等待 1 秒，循环读取所有行，直到找到 "OK"
				     {
								uint32_t wait_start = osKernelGetTickCount();
								int found_ok = 0;
								while ((osKernelGetTickCount() - wait_start) < 1000)
								{
										osDelay(50);
										if (ESP8266_GetLine(line, sizeof(line)) > 0) {
												printf("[ESP8266] RX: %s", line);
												if (strstr(line, "OK") != NULL) {
														found_ok = 1;
														break;
												}
												// 新增：遇到 busy 就多等一会儿，不要急着重试
												if (strstr(line, "busy") != NULL) {
														osDelay(500);  // busy 了就多等 500ms
												}
										}                
								}
								if (found_ok) {
										state = ESP_STATE_SET_STA;
										retry_cnt = 0;
										enter_state_tick = now;
										printf("[ESP8266] AT OK!  SET_STA\r\n");
										break;
								}
							}
						if (retry_cnt >= 5) {
                state = ESP_STATE_RECONNECT;
                enter_state_tick = now;
            }
            break;
        /* ---------------------------------------------------------------------
         * 状态 3：SET_STA - 发 AT+CWMODE=1 (Station 模式)，等 OK
         * --------------------------------------------------------------------- */
        case ESP_STATE_SET_STA:
            ESP8266_ClearRxBuf();                                    // 每次清空
            ESP8266_SendRaw("AT+CWMODE=1\r\n", 13);                  // 每次重发
            retry_cnt++;

						osDelay(300);                                            // 新增：先等 300ms

            {
                uint32_t wait_start = osKernelGetTickCount();
                int found_ok = 0;
                while ((osKernelGetTickCount() - wait_start) < 2000)
                {
                    osDelay(50);
                    if (ESP8266_GetLine(line, sizeof(line)) > 0) {
                        printf("[ESP8266] RX: %s", line);
                        if (strstr(line, "OK") != NULL) {
                            found_ok = 1;
                            break;
                        }
                        if (strstr(line, "busy") != NULL) {          // 新增
                            osDelay(500);
                        }
                    }
                }
                if (found_ok) {
                    state = ESP_STATE_JOIN_WIFI;
                    retry_cnt = 0;
                    enter_state_tick = now;
                    printf("[ESP8266] CWMODE=1 OK!  JOIN_WIFI\r\n");
                    break;
                }
            }
            // 超时 3 次后进入重连
            if (retry_cnt >= 3) { state = ESP_STATE_RECONNECT; enter_state_tick = now; }
            break;

        /* ---------------------------------------------------------------------
         * 状态 4：JOIN_WIFI - 发 AT+CWJAP="SSID","PASS"，等 WIFI GOT IP
         * --------------------------------------------------------------------- */
						case ESP_STATE_JOIN_WIFI: {
            char cmd[192];
            snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n",
                             ESP_WIFI_SSID, ESP_WIFI_PASSWORD);
            ESP8266_ClearRxBuf();                                    // 每次清空
            ESP8266_SendRaw(cmd, strlen(cmd));                        // 每次重发
            retry_cnt++;
            printf("[ESP8266]  CWJAP %s, timeout 15s\r\n", ESP_WIFI_SSID);

            // 等待 15 秒（CWJAP 最长需要这么久：DHCP + 握手）
            uint32_t wait_start = osKernelGetTickCount();
            int found_ip = 0;
            while ((osKernelGetTickCount() - wait_start) < 15000)
            {
                osDelay(100);                                      // 每 100ms 轮询一次
                if (ESP8266_GetLine(line, sizeof(line)) > 0) {
                    printf("[ESP8266] RX: %s", line);
                    if (strstr(line, "WIFI GOT IP") != NULL) {
                        found_ip = 1;
                        break;
                    }
                    if (strstr(line, "FAIL") != NULL ||
                        strstr(line, "+CWJAP:1") != NULL ||   // 密码错误
                        strstr(line, "+CWJAP:2") != NULL ||   // 找不到 AP
                        strstr(line, "+CWJAP:3") != NULL ||   // 连接失败
                        strstr(line, "ERROR") != NULL) {
                        printf("[ESP8266] WiFi join FAIL!\r\n");
                        state = ESP_STATE_RECONNECT;
                        enter_state_tick = now;
                        retry_cnt = 99;
                        break;
                    }
                }
            }

            if (found_ip) {
                state = ESP_STATE_CONN_TCP;
                retry_cnt = 0;
                enter_state_tick = now;
                printf("[ESP8266] WiFi CONNECTED!  CONN_TCP\r\n");
            } else if (retry_cnt != 99) {
                // 超时未获取到 IP
                state = ESP_STATE_RECONNECT;
                enter_state_tick = now;
            }
            break;
        }

        /* ---------------------------------------------------------------------
         * 状态 5：CONN_TCP - 发 AT+CIPSTART="TCP","IP",PORT，等 CONNECT
         * --------------------------------------------------------------------- */
						case ESP_STATE_CONN_TCP: {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n",
                             ESP_TCP_SERVER_IP, ESP_TCP_SERVER_PORT);
            ESP8266_ClearRxBuf();                                    // 每次清空
            ESP8266_SendRaw(cmd, strlen(cmd));                        // 每次重发
            retry_cnt++;
            printf("[ESP8266]  CIPSTART %s:%d\r\n", ESP_TCP_SERVER_IP, ESP_TCP_SERVER_PORT);

            // 等待 5 秒，轮询直到找到 "CONNECT"
            uint32_t wait_start = osKernelGetTickCount();
            int found_connect = 0;
            while ((osKernelGetTickCount() - wait_start) < 5000)
            {
                osDelay(50);                                        // 每 50ms 轮询一次
                if (ESP8266_GetLine(line, sizeof(line)) > 0) {
                    printf("[ESP8266] RX: %s", line);
                    if (strstr(line, "CONNECT") != NULL) {
                        found_connect = 1;
                        break;
                    }
                    if (strstr(line, "CLOSED") != NULL || strstr(line, "ERROR") != NULL) {
                        printf("[ESP8266] TCP connect FAIL!\r\n");
                        state = ESP_STATE_RECONNECT; enter_state_tick = now;
                        retry_cnt = 99;
                        break;
                    }
                }
            }

             if (found_connect) {
                state = ESP_STATE_MQTT_CONNECT;   /* TCP通了进MQTT连接 */
                retry_cnt = 0;
                enter_state_tick = now;
                last_send_tick = now;
                last_ping_tick = now;
                printf("[ESP8266] TCP CONNECTED!  MQTT_CONNECT\r\n");
            } else if (retry_cnt != 99) {
                state = ESP_STATE_RECONNECT;
                enter_state_tick = now;
            }
            break;
        }
        

                /* ---------------------------------------------------------------------
         * 状态 6：MQTT_CONNECT → 发 MQTT CONNECT 报文，等 CONNACK
         * --------------------------------------------------------------------- */
        case ESP_STATE_MQTT_CONNECT: {
            uint8_t mqtt_buf[128];
            int pkt_len;

            ESP8266_ClearRxBuf();

            /* 1. 拼 CONNECT 报文 */
            pkt_len = MQTT_BuildConnect(mqtt_buf, sizeof(mqtt_buf), MQTT_CLIENT_ID);
            if (pkt_len < 0) {
                printf("[MQTT] BuildConnect FAIL!\r\n");
                state = ESP_STATE_RECONNECT;
                enter_state_tick = now;
                break;
            }

            /* 2. 用 AT+CIPSEND 把 MQTT 报文通过 TCP 发给 Broker */
            {
                char at_cmd[32];
                snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", pkt_len);
                ESP8266_SendRaw(at_cmd, strlen(at_cmd));
                osDelay(200);   /* 等 > 提示符 */
                ESP8266_SendRaw((char *)mqtt_buf, (uint16_t)pkt_len);
            }

            printf("[MQTT] TX CONNECT (%d bytes)\r\n", pkt_len);
            retry_cnt++;

            /* 3. 等 CONNACK，3 秒超时 */
            {
                uint32_t wait_start = osKernelGetTickCount();
                int found_ack = 0;
                while ((osKernelGetTickCount() - wait_start) < 3000) {
                    osDelay(50);
                    int rlen = ESP8266_GetLine(line, sizeof(line));
                    if (rlen > 0) {
                        /* 二进制数据不能用 %s 打印，用十六进制 */
                        printf("[MQTT] RX (%d bytes):", rlen);
                        for (int j = 0; j < rlen && j < 32; j++) {
                            printf(" %02X", (uint8_t)line[j]);
                        }
                        printf("\r\n");
                        if (MQTT_IsConnackSuccess((uint8_t *)line, rlen)) {
                            found_ack = 1;
                            break;
                        }
                    }
                }
                if (found_ack) {
                    state = ESP_STATE_MQTT_SUBSCRIBE;
                    retry_cnt = 0;
                    enter_state_tick = now;
                    printf("[MQTT] CONNACK success!  MQTT_SUBSCRIBE\r\n");
                } else if (retry_cnt >= 3) {
                    printf("[MQTT] CONNACK timeout!  RECONNECT\r\n");
                    state = ESP_STATE_RECONNECT;
                    enter_state_tick = now;
                }
            }
            break;
        }

        /* ---------------------------------------------------------------------
         * 状态 7：MQTT_SUBSCRIBE → 发 SUBSCRIBE 报文，订阅 OTA 主题
         * --------------------------------------------------------------------- */
        case ESP_STATE_MQTT_SUBSCRIBE: {
            uint8_t mqtt_buf[128];
            int pkt_len;

            ESP8266_ClearRxBuf();

            /* 1. 拼 SUBSCRIBE 报文 */
            pkt_len = MQTT_BuildSubscribe(mqtt_buf, sizeof(mqtt_buf),
                                          MQTT_TOPIC_OTA, 0);
            if (pkt_len < 0) {
                printf("[MQTT] BuildSubscribe FAIL!\r\n");
                state = ESP_STATE_RECONNECT;
                enter_state_tick = now;
                break;
            }

            /* 2. 发出去 */
            {
                char at_cmd[32];
                snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", pkt_len);
                ESP8266_SendRaw(at_cmd, strlen(at_cmd));
                osDelay(200);
                ESP8266_SendRaw((char *)mqtt_buf, (uint16_t)pkt_len);
            }

            printf("[MQTT] TX SUBSCRIBE %s (%d bytes)\r\n", MQTT_TOPIC_OTA, pkt_len);

            /* 3. 等 2 秒让 SUBACK 回来（D6 不严格解析 SUBACK，发了就行） */
            osDelay(2000);

            /* 4. 进入 MQTT_WORKING，初始化两个时间戳 */
            state = ESP_STATE_MQTT_WORKING;
            retry_cnt = 0;
            enter_state_tick = osKernelGetTickCount();
            last_send_tick = osKernelGetTickCount();
            last_ping_tick = osKernelGetTickCount();
            printf("[MQTT] Enter MQTT_WORKING (publish 5s / ping %ds)\r\n", 30);
            break;
        }

        /* ---------------------------------------------------------------------
         * 状态 8：MQTT_WORKING → 5秒PUBLISH + 30秒PINGREQ + 检测掉线
         * --------------------------------------------------------------------- */
        case ESP_STATE_MQTT_WORKING: {
            /* ---- 第一件事：检查下行数据 / 掉线事件（每 50ms）---- */
            if (g_esp_rx_sem_handle != NULL &&
                osSemaphoreAcquire(g_esp_rx_sem_handle, 50) == osOK)
            {
                if (ESP8266_GetLine(line, sizeof(line)) > 0) {
                    printf("[MQTT] RX: %s", line);
                    if (strstr(line, "WIFI DISCONNECT") != NULL ||
                        strstr(line, "CLOSED") != NULL) {
                        printf("[MQTT] Connection lost!  RECONNECT\r\n");
                        state = ESP_STATE_RECONNECT;
                        enter_state_tick = now;
                        break;
                    }
                }
            }

            /* ---- 第二件事：每 5 秒发一次温湿度 PUBLISH ---- */
            if ((now - last_send_tick) >= 5000) {
                MQTT_PublishSensor();
                last_send_tick = now;
            }

            /* ---- 第三件事：每 30 秒发一次 PINGREQ 心跳 ---- */
            if ((now - last_ping_tick) >= 30000) {
                uint8_t ping_buf[4];
                int ping_len = MQTT_BuildPingreq(ping_buf, sizeof(ping_buf));
                char at_cmd[32];
                snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", ping_len);
                ESP8266_SendRaw(at_cmd, strlen(at_cmd));
                osDelay(200);
                ESP8266_SendRaw((char *)ping_buf, (uint16_t)ping_len);
                printf("[MQTT] TX PINGREQ (%d bytes)\r\n", ping_len);
                last_ping_tick = now;
            }
            break;
        }

        /* ---------------------------------------------------------------------
         * 状态 9：RECONNECT → 等 3 秒从头再来
         * --------------------------------------------------------------------- */
        case ESP_STATE_RECONNECT:
            printf("[ESP8266] ! RECONNECT in 3s...\r\n");
            osDelay(3000);
            state = ESP_STATE_INIT;
            enter_state_tick = osKernelGetTickCount();
            break;

        default:
            state = ESP_STATE_INIT;
            break;
        }
		}
	}
/* ================================================================
 * 辅助函数：ESP8266_SendRaw
 *   通过 USART2 发一段原始字节
 * ================================================================ */
static int ESP8266_SendRaw(const char *data, uint16_t len)
{
    /* 互斥锁？暂时不用互斥锁保护 huart2：
     *   因为只有 TaskESP8266 一个任务会发 AT 指令到 huart2，不会和别人抢。
     *   以后要加 ISR 发数据或多任务发的时候再加。*/
    if (HAL_UART_Transmit(&huart2, (uint8_t *)data, len, 2000) != HAL_OK) {
        return -1;
    }
    return 0;
}

/* ================================================================
 * 辅助函数：ESP8266_SendJSON_Report
 *   读取 g_dht11_data → 拼 JSON → CIPSEND 报长 → 发 payload
 * ================================================================ */
static void ESP8266_SendJSON_Report(void)
{
    char json_buf[128];
    char at_cmd[64];
    int json_len;
    DHT11_Data_t local;

    /* 1. 拿互斥锁拷出 DHT11 数据（防止 TaskDHT11 正在写） */
    if (g_dht11_mutex_handle != NULL) {
        osMutexAcquire(g_dht11_mutex_handle, 100);
    }
    local = g_dht11_data;
    if (g_dht11_mutex_handle != NULL) {
        osMutexRelease(g_dht11_mutex_handle);
    }

    /* 2. 拼 JSON 字符串（加个 tick，方便你看是不是新数据） */
    json_len = snprintf(json_buf, sizeof(json_buf),
                        "{\"temp\":%u,\"humi\":%u,\"tick\":%lu,\"uptime\":%lu}\r\n",
                        (unsigned)local.temp_int,
                        (unsigned)local.humi_int,
                        osKernelGetTickCount(),
                        osKernelGetTickCount() / 1000);
    if (json_len <= 0 || json_len >= (int)sizeof(json_buf)) return;

    printf("[ESP8266] TX JSON: %s", json_buf);

    /* 3. 先告诉 ESP8266 要发多少字节：AT+CIPSEND=长度 */
    snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", json_len);
    ESP8266_SendRaw(at_cmd, strlen(at_cmd));

    /* 4. 等 ">" 提示符（ESP8266 回复 > 就可以发 payload），这里简单用 200ms 延时兜底
     *    （更严谨做法是等信号量收到">"再发，初期演示用延时够了） */
    osDelay(200);

    /* 5. 发 JSON payload */
    ESP8266_SendRaw(json_buf, (uint16_t)json_len);

    /* 6. 发完后等 "SEND OK" 确认的逻辑暂时不加，后面 D5 OTA 再严谨 */
}

/* ================================================================
 *   函数：MQTT_PublishSensor
 *   读 DHT11 → 拼 JSON → 拼成 MQTT PUBLISH 报文 → AT+CIPSEND 发出
 *
 *   和 D5 的 ESP8266_SendJSON_Report 区别：
 *     D5：AT+CIPSEND 发的是裸 JSON 字符串
 *     D6：AT+CIPSEND 发的是完整 MQTT PUBLISH 报文（固定头+主题+JSON）
 *         Broker 收到后能按 MQTT 协议解析，转发给所有订阅者
 * ================================================================ */
static void MQTT_PublishSensor(void)
{
    uint8_t mqtt_buf[256];       /* MQTT 完整报文缓冲 */
    char json_buf[128];          /* JSON 载荷缓冲 */
    char at_cmd[32];             /* AT+CIPSEND 指令缓冲 */
    int json_len, mqtt_len;
    DHT11_Data_t local;

    /* 1. 拿互斥锁拷出 DHT11 数据（和 D5 一样的逻辑） */
    if (g_dht11_mutex_handle != NULL) {
        osMutexAcquire(g_dht11_mutex_handle, 100);
    }
    local = g_dht11_data;
    if (g_dht11_mutex_handle != NULL) {
        osMutexRelease(g_dht11_mutex_handle);
    }

    /* 2. 拼 JSON（注意：D6 不加 \r\n！MQTT payload 长度必须精确） */
    json_len = snprintf(json_buf, sizeof(json_buf),
                        "{\"temp\":%u,\"humi\":%u,\"tick\":%lu,\"uptime\":%lu}",
                        (unsigned)local.temp_int,
                        (unsigned)local.humi_int,
                        osKernelGetTickCount(),
                        osKernelGetTickCount() / 1000);
    if (json_len <= 0 || json_len >= (int)sizeof(json_buf)) return;

    /* 3. 把 JSON 包装成完整的 MQTT PUBLISH 报文 */
    mqtt_len = MQTT_BuildPublish(mqtt_buf, sizeof(mqtt_buf),
                                 MQTT_TOPIC_SENSOR,
                                 (uint8_t *)json_buf, json_len);
    if (mqtt_len < 0) return;

    printf("[MQTT] TX PUBLISH %s (%d bytes)\r\n", MQTT_TOPIC_SENSOR, mqtt_len);

    /* 4. 用 AT+CIPSEND 把 MQTT 报文通过 TCP 发给 Broker */
    snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", mqtt_len);
    ESP8266_SendRaw(at_cmd, strlen(at_cmd));
    osDelay(200);   /* 等 > 提示符 */
    ESP8266_SendRaw((char *)mqtt_buf, (uint16_t)mqtt_len);
}

/* （ESP8266_SendAT 这个函数后面调试时会用，状态机现在直接用 SendRaw 简化逻辑，保留占位） */
__attribute__((unused)) static int ESP8266_SendAT(const char *cmd, const char *expect_ack, uint32_t timeout_ms)
{
    (void)cmd; (void)expect_ack; (void)timeout_ms;
    return 0;
}

