#include "app_dht11.h"
#include "gpio.h"
#include "usart.h"
#include <stdio.h>

/* ==================== 全局变量定义 ==================== */
volatile DHT11_Data_t g_dht11_data = {0};

/* ==================== 微秒延时函数（DWT 计数器法） ==================== */
/*
 * DWT（Data Watchpoint and Trace）是 Cortex-M3 的调试单元，
 * 有一个 CYCCNT 寄存器，每个时钟周期自动 +1（72MHz 下 1μs = 72 个周期）。
 * 比 HAL_Delay 精度高 1000 倍，而且不占用 SysTick/tim。
 */
#define DWT_CR      *(volatile uint32_t *)0xE0001000  /* DWT Control Register */
#define DWT_CYCCNT  *(volatile uint32_t *)0xE0001004  /* DWT Cycle Counter */
#define DEM_CR      *(volatile uint32_t *)0xE000EDFC  /* Demcr Register */
#define DEM_CR_TRCENA (1 << 24)                        /* Trace Enable bit */

static void DWT_Delay_us(uint32_t us)
{
    uint32_t start = DWT_CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000);  /* us 换算成时钟周期数 */
    while ((DWT_CYCCNT - start) < ticks);               /* 忙等直到过了指定周期 */
}

static void DWT_Init(void)
{
    DEM_CR |= DEM_CR_TRCENA;  /* 使能 DWT 调试单元 */
    DWT_CYCCNT = 0;           /* 计数器清零 */
    DWT_CR |= 1;              /* 使能 CYCCNT 计数 */
}

/* ==================== DHT11 引脚操作宏 ==================== */
/*
 * PE6 配置为 Output Open Drain + Pull-up：
 *   写 0 → 引脚被拉低（低电平）
 *   写 1 → 引脚浮空（高阻态），被上拉拉到高电平
 * 读引脚电平用 HAL_GPIO_ReadPin（读输入寄存器 IDR，不管 MODER 是什么模式都能读）
 */
#define DHT11_DQ_LOW()   HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_RESET)
#define DHT11_DQ_HIGH()  HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_SET)
#define DHT11_DQ_READ()  HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin)

/* ==================== DHT11_Init ==================== */
int DHT11_Init(void)
{
    DWT_Init();        /* 初始化微秒延时模块 */
    DHT11_DQ_HIGH();   /* 释放总线（空闲高电平） */
    osDelay(1000);     /* 等 DHT11 上电稳定（DHT11 上电后需 1~2 秒才正常工作） */
    return 0;
}

/* ==================== DHT11_Read ==================== */
/*
 * DHT11 单总线时序：
 *   1. MCU 拉低 ≥18ms → 释放拉高 20~40μs（复位脉冲）
 *   2. DHT11 应答：拉低 80μs → 拉高 80μs
 *   3. DHT11 发 40 bit 数据：
 *      每一位先 50μs 低电平起始 → 再拉高：
 *        高 26~28μs = bit0
 *        高 70μs   = bit1
 *   4. 最后 DHT11 拉低 50μs → 释放 → 总线回到高电平
 */
int DHT11_Read(DHT11_Data_t *out)
{
    uint8_t data[5] = {0};  /* 5 字节缓冲：湿度高+湿度低+温度高+温度低+校验 */
    uint8_t i, j;
    uint32_t timeout;

    /* ---- 第 1 步：MCU 发复位脉冲 ---- */
    DHT11_DQ_LOW();          /* 拉低 */
    osDelay(20);             /* 等 ≥18ms，用 osDelay 不卡 CPU（其他任务可以跑） */
    DHT11_DQ_HIGH();         /* 释放拉高 */
    DWT_Delay_us(30);        /* 等 20~40μs */

    /* ---- 第 2 步：等 DHT11 应答（拉低 80μs）---- */
    timeout = 0;
    while (DHT11_DQ_READ() == GPIO_PIN_SET) {  /* 等引脚变低 */
        DWT_Delay_us(1);
        if (++timeout > 100) return -1;        /* 100μs 超时 = DHT11 没接/没应答 */
    }

    /* ---- 第 3 步：等 DHT11 拉低 80μs + 拉高 80μs（应答信号）---- */
    timeout = 0;
    while (DHT11_DQ_READ() == GPIO_PIN_RESET) { /* 等 80μs 低电平结束 */
        DWT_Delay_us(1);
        if (++timeout > 100) return -1;
    }
    timeout = 0;
    while (DHT11_DQ_READ() == GPIO_PIN_SET) {   /* 等 80μs 高电平结束 */
        DWT_Delay_us(1);
        if (++timeout > 100) return -1;
    }

    /* ---- 第 4 步：读 40 bit 数据（5 字节 × 8 位）---- */
    for (j = 0; j < 5; j++) {        /* 5 字节 */
        for (i = 0; i < 8; i++) {    /* 每字节 8 位，MSB 先到 */
            /* 等 50μs 低电平起始结束 */
            timeout = 0;
            while (DHT11_DQ_READ() == GPIO_PIN_RESET) {
                DWT_Delay_us(1);
                if (++timeout > 100) return -1;
            }
            /* 延时 40μs 后看引脚：
             *   如果还是高 → bit1（高电平持续 70μs，40μs 时还没结束）
             *   如果已变低 → bit0（高电平只持续 26~28μs，40μs 时已结束） */
            DWT_Delay_us(40);
            data[j] <<= 1;  /* 先左移，为新位腾位置 */
            if (DHT11_DQ_READ() == GPIO_PIN_SET) {
                data[j] |= 1;  /* 高 → bit1 */
                /* 等 bit1 的高电平结束（70-40=30μs 剩余） */
                timeout = 0;
                while (DHT11_DQ_READ() == GPIO_PIN_SET) {
                    DWT_Delay_us(1);
                    if (++timeout > 100) break;
                }
            }
            /* 如果是 bit0，26~28μs 高电平已经结束，不用额外等 */
        }
    }

    /* ---- 第 5 步：校验和验证 ---- */
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        return -1;  /* 校验失败 */
    }

    /* ---- 第 6 步：解析到结构体 ---- */
    out->humi_int  = data[0];
    out->humi_dec  = data[1];
    out->temp_int  = data[2];
    out->temp_dec  = data[3];
    out->check_sum = data[4];

    return 0;  /* 成功 */
}

/* ==================== TaskDHT11 ==================== */
void TaskDHT11(void *argument)
{
    (void)argument;
    DHT11_Data_t local;

    /* 初始化 DHT11（含 1 秒等待上电稳定） */
    DHT11_Init();
    printf("[DHT11] Init done\r\n");

    for (;;)
    {
        /* 读温湿度 */
        if (DHT11_Read(&local) == 0)
        {
            /* 拿互斥锁 → 写全局变量 → 放锁（保护 g_dht11_data 被 TaskESP8266 读时不会被踩） */
            if (g_dht11_mutex_handle != NULL) {
                osMutexAcquire(g_dht11_mutex_handle, 100);
            }
            g_dht11_data = local;
            if (g_dht11_mutex_handle != NULL) {
                osMutexRelease(g_dht11_mutex_handle);
            }

            printf("[DHT11] temp=%dC humi=%d%%\r\n",
                   local.temp_int, local.humi_int);
        }
        else
        {
            printf("[DHT11] read failed (checksum error or no response)\r\n");
        }

        /* DHT11 采样频率不低于 1 秒一次（官方规定），2 秒最稳 */
        osDelay(2000);
    }
}
