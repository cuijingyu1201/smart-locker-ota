#include "app_esp8266.h"
#include "cmsis_os2.h"
#include "usart.h"
#include "app_ipc.h"
#include "app_dht11.h"
#include <stdio.h>
#include <string.h>

/* ==================== 可配置参数 ==================== */
#define ESP_WIFI_SSID       "cai"        /*  2.4G WiFi 名（不能是 5G！） */
#define ESP_WIFI_PASSWORD   "ccc1234567"     /*  WiFi 密码 */
#define ESP_TCP_SERVER_IP   "10.127.21.17"         /* 改成电脑在同一个 WiFi 的 IPv4 地址 */
#define ESP_TCP_SERVER_PORT 9000                      /* NetAssist 开的 TCP Server 端口 */

/* ==================== 内部辅助函数声明 ==================== */
static int  ESP8266_SendRaw(const char *data, uint16_t len);
static int  ESP8266_SendAT(const char *cmd, const char *expect_ack, uint32_t timeout_ms);
static void ESP8266_SendJSON_Report(void);

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

            osDelay(300);                                            // ? 新增：先等 300ms，让 ESP8266 完整回复

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
                    printf("[ESP8266] AT OK! 进入 SET_STA\r\n");
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
                    printf("[ESP8266] CWMODE=1 OK! 进入 JOIN_WIFI\r\n");
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
            printf("[ESP8266] 进入 CWJAP %s, timeout 15s\r\n", ESP_WIFI_SSID);

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
                printf("[ESP8266] WiFi CONNECTED! 进入 CONN_TCP\r\n");
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
            printf("[ESP8266] 进入 CIPSTART %s:%d\r\n", ESP_TCP_SERVER_IP, ESP_TCP_SERVER_PORT);

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
                state = ESP_STATE_WORKING;
                retry_cnt = 0;
                enter_state_tick = now;
                last_send_tick = now;
                printf("[ESP8266] TCP CONNECTED! 进入 WORKING (report every 5s)\r\n");
            } else if (retry_cnt != 99) {
                state = ESP_STATE_RECONNECT;
                enter_state_tick = now;
            }
            break;
        }
        

        /* ---------------------------------------------------------------------
         * 状态 6：WORKING → 每 5 秒发一次温湿度 JSON
         * --------------------------------------------------------------------- */
        case ESP_STATE_WORKING: {
            /* 收到一条就打印，检测 TCP 被踢 / WIFI 掉线 */
            if (g_esp_rx_sem_handle != NULL &&
                osSemaphoreAcquire(g_esp_rx_sem_handle, 50) == osOK)
            {
                if (ESP8266_GetLine(line, sizeof(line)) > 0) {
                    printf("[ESP8266] RX: %s", line);
                    if (strstr(line, "WIFI DISCONNECT") != NULL ||
                        strstr(line, "CLOSED") != NULL) {
                        state = ESP_STATE_RECONNECT; enter_state_tick = now; break;
                    }
                }
            }
            /* 到 5 秒了就发一条温湿度 JSON */
            if ((now - last_send_tick) >= 5000) {
                ESP8266_SendJSON_Report();
                last_send_tick = now;
            }
            break;
        }

        /* ---------------------------------------------------------------------
         * 状态 7：RECONNECT → 等 3 秒从头再来
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

        /* 每个循环让出 10ms，不占满 CPU */
        osDelay(10);
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

/* （ESP8266_SendAT 这个函数后面调试时会用，状态机现在直接用 SendRaw 简化逻辑，保留占位） */
__attribute__((unused)) static int ESP8266_SendAT(const char *cmd, const char *expect_ack, uint32_t timeout_ms)
{
    (void)cmd; (void)expect_ack; (void)timeout_ms;
    return 0;
}
