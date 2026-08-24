/**
 * @file    ota_manager.c
 * @brief   D11: APP 侧 OTA 请求触发层实现
 *          解析 MQTT 收到的 OTA JSON 命令 → 写参数区标志 → 软复位
 */
#include "ota_manager.h"
#include "flash_param.h"      /* FlashParam_SetOtaRequest */
#include "app_uart.h"        /* uart_printf_mutex（线程安全打印）*/
#include "cmsis_os2.h"       /* osDelay */
#include "stm32f1xx_hal.h"   /* NVIC_SystemReset */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 *   OTA_GetCurrentBuildNum：用 __DATE__+__TIME__ 做 djb2 哈希
 *   __DATE__ 例 "Aug 23 2026"，__TIME__ 例 "12:34:56"
 *   两者拼接每次编译都不同 → 哈希不同 → BUILD 号自动变
 *   取低 16 位是因为 flash_param_t.fw_build_num 是 uint16_t
 * ================================================================ */
uint16_t OTA_GetCurrentBuildNum(void)
{
    static const char *build_str = __DATE__ " " __TIME__;
    uint32_t hash = 5381U;
    const char *p;

    for (p = build_str; *p != '\0'; p++) {
        hash = ((hash << 5) + hash) + (uint32_t)(*p);   /* hash*33 + c (djb2) */
    }
    return (uint16_t)(hash & 0xFFFFU);
}

/* ================================================================
 *   parse_long：从 JSON 字符串里提取 "key":数字 的值
 *   例 json={"\"cmd\":\"ota\",\"size\":30268}，key="size"
 *       → 找到 "\"size\":" → 跳过 → strtol 解析 30268
 *   返回 解析到的数值；找不到/格式错返回 -1
 *   说明：用 strstr+strtol，和 app_esp8266.c 现有
 *        led_on/led_off 解析风格一致，不引入 cJSON
 * ================================================================ */
static long parse_long(const char *json, const char *key)
{
    char pattern[32];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p;

    if (n <= 0 || n >= (int)sizeof(pattern)) return -1;   /* 拼 pattern 失败 */

    p = strstr(json, pattern);
    if (p == NULL) return -1;                              /* 没找到 key */
    p += n;                                               /* 跳过 "key": */

    while (*p == ' ' || *p == '\t') p++;                  /* 跳过空格 */

    return strtol(p, NULL, 10);                           /* 十进制解析 */
}

/* ================================================================
 *   OTA_ParseCommand：解析 OTA JSON 命令
 *   期望格式：
 *     {"cmd":"ota","major":1,"minor":0,"patch":0,"build":2,
 *      "size":30268,"crc":1503837739}
 *   返回 1 = 是 ota 命令且关键字段齐全，应升级
 *        0 = 不是 ota 命令 / size 或 crc 缺失，不升级
 *   说明：版本号字段缺失默认 0（不强制），
 *        size 和 crc 是升级必需，缺失就不升
 * ================================================================ */
int OTA_ParseCommand(const char *json, int json_len,
                     uint16_t *out_major, uint16_t *out_minor,
                     uint16_t *out_patch, uint16_t *out_build,
                     uint32_t *out_size, uint32_t *out_crc)
{
    long major, minor, patch, build, size, crc;

    /* 防御：空指针或长度非法 */
    if (json == NULL || json_len <= 0) return 0;

    /* 先确认是 ota 命令（区别于 led_on/led_off）*/
    if (strstr(json, "\"cmd\":\"ota\"") == NULL) return 0;

    major = parse_long(json, "major");
    minor = parse_long(json, "minor");
    patch = parse_long(json, "patch");
    build = parse_long(json, "build");
    size  = parse_long(json, "size");
    crc   = parse_long(json, "crc");

    /* size 和 crc 是关键字段，缺失不升级；版本号缺失默认 0 */
    if (size < 0 || crc < 0) return 0;

    *out_major = (major < 0) ? 0 : (uint16_t)major;
    *out_minor = (minor < 0) ? 0 : (uint16_t)minor;
    *out_patch = (patch < 0) ? 0 : (uint16_t)patch;
    *out_build = (build < 0) ? 0 : (uint16_t)build;
    *out_size  = (uint32_t)size;
    *out_crc   = (uint32_t)crc;
    return 1;
}

/* ================================================================
 *   OTA_TriggerUpgrade：写参数区 OTA 标志 + 软复位
 *   1. 打印升级信息（调试用）
 *   2. FlashParam_SetOtaRequest 写参数区：
 *        ota_request_magic = "OTA1" (MAGIC_OTA_REQUEST)
 *        ota_new_fw_crc32  = new_fw_crc
 *        ota_new_fw_size   = new_fw_size
 *   3. osDelay(300) 给 UART 把上面几行打印完
 *   4. NVIC_SystemReset() 软复位，进 Bootloader，不返回
 *   失败路径：写参数区失败 → 打印错误 → return（不复位，继续跑当前 APP）
 * ================================================================ */
void OTA_TriggerUpgrade(uint32_t new_fw_crc, uint32_t new_fw_size)
{
    int ret;

    uart_printf_mutex("[OTA] Trigger upgrade: size=%lu crc=0x%08lX\r\n",
           (unsigned long)new_fw_size, (unsigned long)new_fw_crc);
    uart_printf_mutex("[OTA] Writing OTA request flag to param area...\r\n");

    ret = FlashParam_SetOtaRequest(new_fw_crc, new_fw_size);
    if (ret != 0) {
        uart_printf_mutex("[OTA] !!! SetOtaRequest FAIL (ret=%d), abort, no reset.\r\n", ret);
        return;   /* 写失败不复位，继续跑当前 APP */
    }

    uart_printf_mutex("[OTA] Flag written OK. Reset in 300ms to enter Bootloader...\r\n");
    osDelay(300);                /* 给 UART 打印完的时间 */

    NVIC_SystemReset();          /* 软复位，进 Bootloader，本函数不返回 */
}
