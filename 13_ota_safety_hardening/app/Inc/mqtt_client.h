#ifndef __MQTT_CLIENT_H
#define __MQTT_CLIENT_H

#include <stdint.h>

/* ================================================================
 * MQTT 3.1.1 报文构造与解析函数（纯协议层，不依赖任何硬件）
 *
 * 设计原则：
 *   1. 每个 BuildXXX 函数只做"拼字节到 buf"，返回总长度
 *      实际发送由调用者（TaskESP8266）用 ESP8266_SendRaw 完成
 *   2. 所有函数都不 malloc，只用调用者传入的 buf
 *   3. 返回值 >0 = 成功（=报文字节数），<0 = 失败（buf 太小）
 * ================================================================ */

/* 1. CONNECT：客户端→Broker，请求建立 MQTT 连接
 *    client_id：设备唯一标识，不能和别人重名（建议加学号后缀）
 *    返回值：报文总字节数，<0 = buf 太小 */
int MQTT_BuildConnect(uint8_t *buf, int buf_size, const char *client_id);

/* 2. PUBLISH (QoS0)：客户端→Broker，发布一条消息
 *    topic：主题字符串，如 "iot/dev001/sensor"
 *    payload：消息内容（JSON 字节流）
 *    payload_len：payload 的字节数（用 snprintf 返回值，不要写死）
 *    返回值：报文总字节数，<0 = buf 太小 */
int MQTT_BuildPublish(uint8_t *buf, int buf_size, const char *topic,
                      const uint8_t *payload, int payload_len);

/* 3. SUBSCRIBE：客户端→Broker，订阅一个主题
 *    topic：要订阅的主题，如 "iot/dev001/ota"
 *    qos：服务质量等级，D6 统一用 0
 *    返回值：报文总字节数，<0 = buf 太小 */
int MQTT_BuildSubscribe(uint8_t *buf, int buf_size, const char *topic, uint8_t qos);

/* 4. PINGREQ：客户端→Broker，30 秒心跳保活
 *    固定 2 字节，没有参数
 *    返回值：2，<0 = buf 太小 */
int MQTT_BuildPingreq(uint8_t *buf, int buf_size);

/* 5. 解析 CONNACK：判断 Broker 回复的连接确认是否成功
 *    data：从 ESP8266 环形缓冲读出的一行数据
 *    len：数据长度
 *    返回值：1 = 连接成功，0 = 不是 CONNACK 或连接失败 */
int MQTT_IsConnackSuccess(const uint8_t *data, int len);

/*  构造 CleanSession=False 的 CONNECT（重连后恢复离线消息）*/
int MQTT_BuildConnect_Resume(uint8_t *buf, int buf_size, const char *client_id);

/*  解析 PUBLISH 报文，提取主题长度/主题指针/payload长度/payload指针
 *   data     : +IPD 后面的完整 MQTT 字节流（不含 +IPD,len: 前缀）
 *   mqtt_len : MQTT 字节流长度
 *   topic_out: 输出 - 主题字符串指针（指向 data 内部，不要 free）
 *   topic_len_out: 输出 - 主题字节数
 *   payload_out: 输出 - payload 字节指针
 *   payload_len_out: 输出 - payload 字节数
 *   返回值: 0=成功, <0=解析失败
 */
int MQTT_ParsePublish(const uint8_t *data, int mqtt_len,
                      const char **topic_out, int *topic_len_out,
                      const uint8_t **payload_out, int *payload_len_out);

/*  搜索缓冲区里的 PUBLISH 帧头(0x30/0x31/0x32)，返回偏移量
 *   返回 -1 表示没找到
 */
int MQTT_FindPublishFrame(const uint8_t *data, int len);

#endif /* __MQTT_CLIENT_H */

