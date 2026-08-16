#ifndef __APP_DHT11_H
#define __APP_DHT11_H

#include "main.h"
#include "cmsis_os2.h"

/* ==================== DHT11 数据结构体 ==================== */
/* DHT11 一次传 40 bit = 5 字节：
 *   湿度高8位 + 湿度低8位 + 温度高8位 + 温度低8位 + 校验和
 * DHT11（不是 DHT22）整数部分在高字节，低字节恒为 0 */
typedef struct {
    uint8_t humi_int;   /* 湿度整数部分（0~100%） */
    uint8_t humi_dec;   /* 湿度小数部分（DHT11 一般为 0） */
    uint8_t temp_int;   /* 温度整数部分（0~50℃） */
    uint8_t temp_dec;   /* 温度小数部分（DHT11 一般为 0） */
    uint8_t check_sum;   /* 校验和 = 前4字节之和低8位 */
} DHT11_Data_t;

/* ==================== 全局变量 ==================== */
extern volatile DHT11_Data_t g_dht11_data;  /* 最新一次温湿度数据 */
extern osMutexId_t g_dht11_mutex_handle;    /* 保护 g_dht11_data 的互斥锁 */

/* ==================== 对外 API ==================== */
/*
 * DHT11_Init：初始化 DHT11 DATA 引脚 + 微秒延时模块
 * 返回：0=成功，-1=失败
 */
int DHT11_Init(void);

/*
 * DHT11_Read：读一次温湿度
 * 参数 out：读到的数据写入这个结构体
 * 返回：0=成功，-1=校验失败/超时/没应答
 */
int DHT11_Read(DHT11_Data_t *out);

/*
 * TaskDHT11：FreeRTOS 任务函数，每 2 秒读一次 DHT11
 * 参数 argument：osThreadNew 传入的参数（NULL）
 */
void TaskDHT11(void *argument);

#endif /* __APP_DHT11_H */
