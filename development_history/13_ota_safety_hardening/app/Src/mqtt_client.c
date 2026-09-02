/* ================================================================
 * mqtt_client.c
 *   MQTT 3.1.1 协议层：报文构造 + 回复解析
 *
 *   不依赖任何硬件，只操作字节数组，方便单元测试和移植。
 *   发送动作由上层 TaskESP8266 调 ESP8266_SendRaw 完成。
 * ================================================================ */

#include "mqtt_client.h"
#include "app_uart.h"
#include <string.h>   /* memcpy, strlen */
#include <stdio.h> 

/* ----------------------------------------------------------------
 * MQTT 报文类型枚举（固定头 Byte0 的高 4 位）
 *   低 4 位是标志位（DUP/QoS/RETAIN），不同报文类型含义不同
 *   这里直接写完整的 Byte0 值，省去移位运算，更直观
 * ---------------------------------------------------------------- */
#define MQTT_TYPE_CONNECT    0x10   /* 0001 0000：CONNECT，固定头低4位保留=0 */
#define MQTT_TYPE_CONNACK    0x20   /* 0010 0000：CONNACK */
#define MQTT_TYPE_PUBLISH    0x30   /* 0011 0000：PUBLISH QoS0（DUP=0, QoS=0, RETAIN=0）*/
#define MQTT_TYPE_SUBSCRIBE  0x82   /* 1000 0010：SUBSCRIBE，低4位固定=0010（协议强制）*/
#define MQTT_TYPE_PINGREQ    0xC0   /* 1100 0000：PINGREQ */
#define MQTT_TYPE_PINGRESP   0xD0   /* 1101 0000：PINGRESP */

/* ================================================================
 *   MQTT_BuildConnect
 *   拼装 CONNECT 报文，用于客户端首次连接 Broker
 *
 *   报文结构（共 2 + 10 + 2 + cid_len 字节）：
 *     固定头(2B)：  0x10 + 剩余长度
 *     可变头(10B)： 协议名长度(2) + "MQTT"(4) + 协议级别(1)
 *                  + 连接标志(1) + KeepAlive(2)
 *     载荷：        ClientID长度(2) + ClientID字符串
 *
 *   参数：
 *     buf       : 输出缓冲区，调用者分配
 *     buf_size  : buf 的最大容量，防止越界
 *     client_id : 客户端唯一标识，如 "stm32_dev138"
 *   返回值：
 *     >0 = 报文总字节数（成功）
 *     <0 = buf 太小（失败）
 * ================================================================ */
int MQTT_BuildConnect(uint8_t *buf, int buf_size, const char *client_id)
{
    int cid_len;        /* Client ID 字符串长度 */
    int remaining_len;  /* 剩余长度 = 可变头 + 载荷 */
    int i = 0;          /* buf 写入位置游标 */

    /* ---- 1. 计算各部分长度 ---- */
    cid_len = (int)strlen(client_id);
    /* 可变头固定 10 字节 + 载荷(2字节长度 + cid字符串) */
    remaining_len = 10 + 2 + cid_len;

    /* ---- 2. 安全检查：buf 必须能装下 固定头(2) + 剩余内容 ---- */
    if (buf_size < (2 + remaining_len)) {
        return -1;   /* buf 太小，拒绝拼装 */
    }

    /* ---- 3. 写固定头（2 字节）---- */
    buf[i++] = MQTT_TYPE_CONNECT;            /* Byte0: 0x10 (CONNECT) */
    buf[i++] = (uint8_t)remaining_len;       /* Byte1: 剩余长度
                                              D6 报文很短(<127字节)
                                              单字节编码即可 */

    /* ---- 4. 写可变头（10 字节，完全固定）---- */
    buf[i++] = 0x00;  /* 协议名长度 MSB（高字节）*/
    buf[i++] = 0x04;  /* 协议名长度 LSB = 4 */
    buf[i++] = 'M';   /* 协议名 "MQTT" 4 个字符 */
    buf[i++] = 'Q';
    buf[i++] = 'T';
    buf[i++] = 'T';
    buf[i++] = 0x04;  /* 协议级别：4 = MQTT 3.1.1 */
    buf[i++] = 0x02;  /* 连接标志：0x02 = 仅 CleanSession=1
                       * 其余位(遗嘱/用户名/密码)=0，匿名连接 emqx */
    buf[i++] = 0x00;  /* KeepAlive MSB */
    buf[i++] = 0x1E;  /* KeepAlive LSB = 0x1E = 30 秒
                       * D6 要求 30 秒心跳间隔 */

    /* ---- 5. 写载荷：Client ID ---- */
    /* 先写 2 字节长度（大端序：MSB 在前，LSB 在后）*/
    buf[i++] = (uint8_t)((cid_len >> 8) & 0xFF);  /* 长度高字节 */
    buf[i++] = (uint8_t)(cid_len & 0xFF);          /* 长度低字节 */
    /* 再写 Client ID 字符串本身 */
    memcpy(&buf[i], client_id, cid_len);
    i += cid_len;

    /* ---- 6. 返回报文总字节数 ---- */
    return i;
}

/* ================================================================
 *   MQTT_BuildPublish
 *   拼装 PUBLISH 报文(QoS0)，用于发布温湿度 JSON
 *
 *   报文结构：
 *     固定头(2B)：  0x30 + 剩余长度
 *     可变头：      主题长度(2) + 主题字符串
 *     载荷：        JSON 字节流（没有长度字段，靠剩余长度推算）
 *
 *   注意：
 *     QoS0 模式下可变头没有"报文标识符"(2字节)，比 QoS1/2 简单
 *     payload_len 必须用 snprintf 的返回值，不能写死！
 * ================================================================ */
int MQTT_BuildPublish(uint8_t *buf, int buf_size, const char *topic,
                      const uint8_t *payload, int payload_len)
{
    int topic_len;      /* 主题字符串长度 */
    int remaining_len;   /* 剩余长度 = 可变头 + 载荷 */
    int i = 0;

    /* ---- 1. 计算长度 ---- */
    topic_len = (int)strlen(topic);
    /* 可变头(2 + topic_len) + 载荷(payload_len) */
    remaining_len = (2 + topic_len) + payload_len;

    /* ---- 2. 安全检查 ---- */
    if (buf_size < (2 + remaining_len)) {
        return -1;
    }

    /* ---- 3. 写固定头 ---- */
    buf[i++] = MQTT_TYPE_PUBLISH;       /* Byte0: 0x30 (PUBLISH QoS0) */
    buf[i++] = (uint8_t)remaining_len;  /* Byte1: 剩余长度 */

    /* ---- 4. 写可变头：主题 ---- */
    buf[i++] = (uint8_t)((topic_len >> 8) & 0xFF);  /* 主题长度 MSB */
    buf[i++] = (uint8_t)(topic_len & 0xFF);          /* 主题长度 LSB */
    memcpy(&buf[i], topic, topic_len);               /* 主题字符串 */
    i += topic_len;

    /* ---- 5. 写载荷：JSON（直接 memcpy，不加长度字段）---- */
    /* MQTT PUBLISH 的 payload 没有显式长度字段
     * 接收方靠"剩余长度 - 可变头长度"来推算 payload 长度
     * 所以 payload_len 必须精确，多1字节少1字节都会导致解析错乱 */
    memcpy(&buf[i], payload, payload_len);
    i += payload_len;

    /* ---- 6. 返回总字节数 ---- */
    return i;
}

/* ================================================================
 *   MQTT_BuildSubscribe
 *   拼装 SUBSCRIBE 报文，用于订阅主题（为 D11 OTA 下行做准备）
 *
 *   报文结构：
 *     固定头(2B)：  0x82 + 剩余长度
 *     可变头：      报文标识符(2B)，随便取个非0值
 *     载荷：        主题长度(2) + 主题 + QoS(1)
 *
 *   注意：
 *     SUBSCRIBE 的固定头低4位必须是 0010（协议强制），所以是 0x82
 * ================================================================ */
int MQTT_BuildSubscribe(uint8_t *buf, int buf_size, const char *topic, uint8_t qos)
{
    int topic_len;
    int remaining_len;
    int i = 0;

    /* ---- 1. 计算长度 ---- */
    topic_len = (int)strlen(topic);
    /* 可变头(2) + 载荷(2 + topic_len + 1) */
    remaining_len = 2 + (2 + topic_len + 1);

    /* ---- 2. 安全检查 ---- */
    if (buf_size < (2 + remaining_len)) {
        return -1;
    }

    /* ---- 3. 写固定头 ---- */
    buf[i++] = MQTT_TYPE_SUBSCRIBE;      /* Byte0: 0x82 */
    buf[i++] = (uint8_t)remaining_len;   /* Byte1: 剩余长度 */

    /* ---- 4. 写可变头：报文标识符（D6 用 QoS0，随便取 1）---- */
    buf[i++] = 0x00;  /* 报文标识符 MSB */
    buf[i++] = 0x01;  /* 报文标识符 LSB = 1 */

    /* ---- 5. 写载荷：主题 + QoS ---- */
    buf[i++] = (uint8_t)((topic_len >> 8) & 0xFF);  /* 主题长度 MSB */
    buf[i++] = (uint8_t)(topic_len & 0xFF);          /* 主题长度 LSB */
    memcpy(&buf[i], topic, topic_len);               /* 主题字符串 */
    i += topic_len;
    buf[i++] = qos;  /* QoS 字节：D6 用 0 */

    /* ---- 6. 返回总字节数 ---- */
    return i;
}

/* ================================================================
 *   MQTT_BuildPingreq
 *   拼装 PINGREQ 心跳报文，每 30 秒发一次，防止 Broker 踢下线
 *
 *   报文结构：固定 2 字节，没有可变头和载荷
 *     Byte0: 0xC0
 *     Byte1: 0x00 (剩余长度 = 0)
 *
 *   这是 MQTT 里最简单的报文，死记就行。
 * ================================================================ */
int MQTT_BuildPingreq(uint8_t *buf, int buf_size)
{
    if (buf_size < 2) {
        return -1;   /* 连 2 字节都装不下 */
    }

    buf[0] = MQTT_TYPE_PINGREQ;   /* 0xC0 */
    buf[1] = 0x00;                 /* 剩余长度 = 0 */

    return 2;   /* 永远返回 2 */
}

/* ================================================================
 *   MQTT_IsConnackSuccess
 *   解析 Broker 回复的 CONNACK，判断连接是否成功
 *
 *   CONNACK 固定 4 字节：
 *     Byte0: 0x20 (报文类型 = CONNACK)
 *     Byte1: 0x02 (剩余长度 = 2)
 *     Byte2: 0x00 (Session Present，CleanSession=1 时固定 0)
 *     Byte3: 返回码
 *       0x00 = 连接成功 ?
 *       0x01 = 协议版本不支持
 *       0x02 = Client ID 不合法
 *       0x03 = Broker 离线/不可用
 *       0x04 = 用户名密码错误
 *       0x05 = 未授权
 *
 *   参数：
 *     data : 从 ESP8266 环形缓冲读出的字节流
 *     len  : 数据长度
 *   返回值：
 *     1 = 是 CONNACK 且返回码 = 0x00（成功）
 *     0 = 不是 CONNACK 或返回码非 0
 *
 *   注意：
 *     ESP8266 回复的内容可能包含回显 "AT+CIPSEND=..\r\n" 等
 *     前缀垃圾数据，所以不能只看 data[0]，要搜索 0x20 标志。
 *     但 D6 阶段先用简单判断（看前4字节），如果调试发现
 *     回显干扰了判断，再改成搜索模式。
 * ================================================================ */
int MQTT_IsConnackSuccess(const uint8_t *data, int len)
{
    int i;

    if (data == NULL || len < 4) {
        return 0;   /* 数据太短，肯定不是 CONNACK */
    }

    /* 搜索 CONNACK 标志字节 0x20
     * 因为 ESP8266 可能在 CONNACK 前面混入回显或其他 URC */
    for (i = 0; i <= (len - 4); i++) {
        if (data[i] == 0x20 &&
            data[i + 1] == 0x02) {
            /* 找到 CONNACK 固定头，检查返回码 */
            if (data[i + 3] == 0x00) {
                return 1;   /* 返回码 0x00 = 成功 */
            } else {
                return 0;   /* 返回码非 0 = 失败 */
            }
        }
    }

    return 0;   /* 没找到 CONNACK 标志 */
}

/* ================================================================
 *   MQTT_BuildConnect_Resume
 *   和原来的 BuildConnect 几乎一样，唯一区别：
 *   连接标志字节 = 0x00（CleanSession=False）
 *   重连时 Broker 会把离线期间的消息补发回来
 * ================================================================ */
int MQTT_BuildConnect_Resume(uint8_t *buf, int buf_size, const char *client_id)
{
    int cid_len = (int)strlen(client_id);
    int remaining_len = 10 + 2 + cid_len;
    int i = 0;

    if (buf_size < (2 + remaining_len)) return -1;

    buf[i++] = 0x10;                      /* CONNECT 类型 */
    buf[i++] = (uint8_t)remaining_len;    /* 剩余长度 */

    buf[i++] = 0x00; buf[i++] = 0x04;     /* 协议名长度=4 */
    buf[i++] = 'M';  buf[i++] = 'Q';
    buf[i++] = 'T';  buf[i++] = 'T';
    buf[i++] = 0x04;                      /* 协议级别=4 (MQTT 3.1.1) */
    buf[i++] = 0x00;                      /* D7改：CleanSession=False(0x00)
                                           * 原来是 0x02 (CleanSession=True) */
    buf[i++] = 0x00; buf[i++] = 0x1E;     /* KeepAlive=30s */

    buf[i++] = (uint8_t)((cid_len >> 8) & 0xFF);
    buf[i++] = (uint8_t)(cid_len & 0xFF);
    memcpy(&buf[i], client_id, cid_len);
    i += cid_len;

    return i;
}

/* ================================================================
 *   MQTT_FindPublishFrame
 *   在数据里搜索 PUBLISH 帧头（0x30~0x3F，即报文类型=3的所有 QoS/DUP/Retain 组合）
 *   找到后返回帧头偏移位置，找不到返回 -1
 * ================================================================ */
int MQTT_FindPublishFrame(const uint8_t *data, int len)
{
    int i;
    if (data == NULL || len < 4) return -1;

    for (i = 0; i < len; i++) {
        /* 高4位 = 0011 = 3 = PUBLISH 类型，低4位是 DUP/QoS/Retain 标志 */
        if ((data[i] & 0xF0) == 0x30) {
            return i;   /* 找到 PUBLISH 帧头 */
        }
    }
    return -1;
}

/* ================================================================
 *   MQTT_ParsePublish
 *   解析一个完整的 PUBLISH 报文(QoS0)，提取主题和 payload
 *
 *   QoS0 PUBLISH 报文结构：
 *   Byte0      : 0x30~0x3F (PUBLISH 帧头)
 *   Byte1      : 剩余长度 Remaining Length（单字节编码，<128）
 *   Byte2~3    : 主题长度 Topic Length（MSB+LSB）
 *   Byte4~N    : 主题字符串（Topic Length 字节）
 *   ByteN+1~End: Payload（= 剩余长度 - 2 - 主题长度）
 *
 *   入参：data 必须是帧头起始位置（先用 FindPublishFrame 找到偏移）
 * ================================================================ */
int MQTT_ParsePublish(const uint8_t *data, int mqtt_len,
                      const char **topic_out, int *topic_len_out,
                      const uint8_t **payload_out, int *payload_len_out)
{
	
    /* 添加调试打印 */
    uart_printf_mutex("[DEBUG] ParsePublish: len=%d, first_byte=0x%02X\r\n", mqtt_len, data[0]);
		
    int remaining_len;
    int topic_len;
    int offset;

    /* 1. 入参合法性检查 */
    if (data == NULL || mqtt_len < 4) return -1;
    if ((data[0] & 0xF0) != 0x30)      return -2;   /* 不是 PUBLISH 帧头 */

    /* 2. 读剩余长度（<128 时单字节，足够 D7 使用）*/
    remaining_len = data[1];
    if (remaining_len < 4) return -3;                /* 至少 2+2=4 字节 */

    /* 3. 读主题长度（大端序：MSB 在前）*/
    topic_len = (data[2] << 8) | data[3];
    if (topic_len <= 0 || topic_len > (remaining_len - 2)) return -4;

    /* 4. 主题偏移 = 帧头(2) + 主题长度字段(2) = 4 */
    offset = 4;
    *topic_out = (const char *)&data[offset];
    *topic_len_out = topic_len;

    /* 5. payload 偏移 = 4 + 主题长度 */
    offset += topic_len;
    *payload_len_out = remaining_len - 2 - topic_len;  /* 剩余长度 - 主题字段长度(2+len) */
    if (*payload_len_out < 0) return -5;
    *payload_out = (const uint8_t *)&data[offset];

    return 0; /* 解析成功 */
}
