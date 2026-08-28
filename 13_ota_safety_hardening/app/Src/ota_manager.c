/**
 * @file    ota_manager.c
 * @brief   D11: APP 端 OTA 请求触发实现
 *          接收 MQTT 下行 OTA JSON 命令，写参数区请求标志后软复位
 */
#include "ota_manager.h"
#include "flash_param.h"      /* FlashParam_SetOtaRequest */
#include "app_uart.h"         /* uart_printf_mutex 线程安全打印 */
#include "cmsis_os2.h"        /* osDelay */
#include "stm32f1xx_hal.h"    /* NVIC_SystemReset */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 *   OTA_GetCurrentBuildNum：__DATE__+__TIME__ 做 djb2 哈希
 *   __DATE__ 形如 "Aug 23 2026"，__TIME__ 形如 "12:34:56"
 *   编译时间拼接后每次编译都不同 → 哈希不同 → BUILD 号自动递增
 *   取低 16 位作为 flash_param_t.fw_build_num（uint16_t）
 * ================================================================ */
uint16_t OTA_GetCurrentBuildNum(void)
{
    return (uint16_t)FW_BUILD_NUM;
}

static int version_compare(uint16_t a_major, uint16_t a_minor,
                           uint16_t a_patch, uint16_t a_build,
                           uint16_t b_major, uint16_t b_minor,
                           uint16_t b_patch, uint16_t b_build)
{
    if (a_major != b_major) return (a_major > b_major) ? 1 : -1;
    if (a_minor != b_minor) return (a_minor > b_minor) ? 1 : -1;
    if (a_patch != b_patch) return (a_patch > b_patch) ? 1 : -1;
    if (a_build != b_build) return (a_build > b_build) ? 1 : -1;
    return 0;
}

/* ================================================================
 *   parse_long：从 JSON 字符串里提取 "key":数字 的值
 *   例 json="{\"cmd\":\"ota\",\"size\":30268}", key="size"
 *       → 找到 "\"size\":" 之后用 strtol 解析出 30268
 *   返回：解析出的整数值，找不到/非法 → -1
 *   说明：用 strstr+strtol 够用，app_esp8266.c 里 led_on/led_off 也是同一套
 *        轻量依赖，不引 cJSON
 * ================================================================ */
static long parse_long(const char *json, const char *key)
{
    char pattern[32];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p;

    if (n <= 0 || n >= (int)sizeof(pattern)) return -1;   /* 拼 pattern 失败 */

    p = strstr(json, pattern);
    if (p == NULL) return -1;                              /* 没找到 key */
    p += n;                                                /* 跳过 "key": */

    while (*p == ' ' || *p == '\t') p++;                  /* 跳空格 */

    return strtol(p, NULL, 10);                            /* 十进制解析 */
}

/* ================================================================
 *   OTA_ParseCommand：解析 OTA JSON 命令
 *   命令格式：
 *     {"cmd":"ota","major":1,"minor":0,"patch":0,"build":2,
 *      "size":30268,"crc":1503837739}
 *   返回 1 = 是 ota 命令且关键字段 size/crc 都对应
 *        0 = 非 ota 命令 / size 或 crc 缺省或无效
 *   说明：版本字段缺省默认为 0（不强求），size 和 crc 必须同时存在才有效
 * ================================================================ */
int OTA_ParseCommand(const char *json, int json_len,
                     uint16_t *out_major, uint16_t *out_minor,
                     uint16_t *out_patch, uint16_t *out_build,
                     uint32_t *out_size, uint32_t *out_crc,
                     uint8_t *out_force)
{
    long major, minor, patch, build, force;
    unsigned long size, crc;        /* size/crc 是无符号，>=2^31 时不溢出 */
    int size_found = 0, crc_found = 0;

    if (json == NULL || json_len <= 0 || out_major == NULL ||
        out_minor == NULL || out_patch == NULL || out_build == NULL ||
        out_size == NULL || out_crc == NULL || out_force == NULL) return 0;

    if (strstr(json, "\"cmd\":\"ota\"") == NULL) return 0;

    major = parse_long(json, "major");
    minor = parse_long(json, "minor");
    patch = parse_long(json, "patch");
    build = parse_long(json, "build");
    force = parse_long(json, "force");
    /* size 和 crc 是无符号 32 位，用 strtoul 防止 >=2^31 时 strtol 截断 */
    {
        const char *ps, *pc;
        char  pats[32], patc[32];
        int   ns = snprintf(pats, sizeof(pats), "\"size\":");
        int   nc = snprintf(patc, sizeof(patc), "\"crc\":");
        ps = (ns > 0) ? strstr(json, pats) : NULL;
        pc = (nc > 0) ? strstr(json, patc) : NULL;
        if (ps) { ps += ns; while (*ps==' '||*ps=='\t') ps++; size = strtoul(ps, NULL, 10); size_found = 1; }
        if (pc) { pc += nc; while (*pc==' '||*pc=='\t') pc++; crc  = strtoul(pc, NULL, 10); crc_found  = 1; }
    }

    /* size 和 crc 是关键字段，找不到字段就拒绝（不再用 <0 判断，因为 unsigned 永不 <0）*/
    if (!size_found || !crc_found || major < 0 || major > 65535L ||
        minor < 0 || minor > 65535L || patch < 0 || patch > 65535L ||
        build < 0 || build > 65535L || size == 0UL ||
        size > APP_FLASH_SIZE) return 0;

    *out_major = (major < 0) ? 0 : (uint16_t)major;
    *out_minor = (minor < 0) ? 0 : (uint16_t)minor;
    *out_patch = (patch < 0) ? 0 : (uint16_t)patch;
    *out_build = (build < 0) ? 0 : (uint16_t)build;
    *out_size  = (uint32_t)size;
    *out_crc   = (uint32_t)crc;
    *out_force = (force == 1L) ? 1U : 0U;
    return 1;
}

/* ================================================================
 *   OTA_TriggerUpgrade：写参数区 OTA 标志 + 软复位
 *   1. 打印调试信息（线程安全）
 *   2. FlashParam_SetOtaRequest 写参数区三字段：
 *        ota_request_magic = "OTA1" (MAGIC_OTA_REQUEST)
 *        ota_new_fw_crc32  = new_fw_crc
 *        ota_new_fw_size   = new_fw_size
 *   3. osDelay(300) 等 UART 缓冲冲刷完所有打印
 *   4. NVIC_SystemReset() 软复位进入 Bootloader 升级分支
 *   失败路径：写参数区失败 → 打印告警 + return（不复位，继续跑当前 APP）
 * ================================================================ */
void OTA_TriggerUpgrade(uint16_t ver_major, uint16_t ver_minor,
                        uint16_t ver_patch, uint16_t build_num,
                        uint32_t new_fw_crc, uint32_t new_fw_size,
                        uint8_t force)
{
    int ret;
    uint16_t cur_major, cur_minor, cur_patch, cur_build;

    if (FlashParam_GetInstalledVersion(&cur_major, &cur_minor,
                                       &cur_patch, &cur_build) == 0) {
        int cmp = version_compare(ver_major, ver_minor, ver_patch, build_num,
                                  cur_major, cur_minor, cur_patch, cur_build);
        if (cmp <= 0 && force == 0U) {
            uart_printf_mutex("[OTA] REJECT version %u.%u.%u.%u <= installed %u.%u.%u.%u; use force=1 only for recovery\r\n",
                              ver_major, ver_minor, ver_patch, build_num,
                              cur_major, cur_minor, cur_patch, cur_build);
            return;
        }
        if (cmp <= 0) {
            uart_printf_mutex("[OTA] WARN forced downgrade/reinstall %u.%u.%u.%u -> %u.%u.%u.%u\r\n",
                              cur_major, cur_minor, cur_patch, cur_build,
                              ver_major, ver_minor, ver_patch, build_num);
        }
    }

    uart_printf_mutex("[OTA] Trigger upgrade: version=%u.%u.%u.%u size=%lu crc=0x%08lX\r\n",
           ver_major, ver_minor, ver_patch, build_num,
           (unsigned long)new_fw_size, (unsigned long)new_fw_crc);
    uart_printf_mutex("[OTA] Writing OTA request flag to param area...\r\n");

    ret = FlashParam_SetOtaRequest(new_fw_crc, new_fw_size,
                                   ver_major, ver_minor, ver_patch, build_num);
    if (ret != 0) {
        uart_printf_mutex("[OTA] !!! SetOtaRequest FAIL (ret=%d), abort, no reset.\r\n", ret);
        return;   /* 写失败不复位，先保住当前 APP */
    }

    uart_printf_mutex("[OTA] Flag written OK. Reset in 300ms to enter Bootloader...\r\n");
    osDelay(300);                /* 等 UART 打印冲刷完成 */

    /* Reset ESP8266 before STM32 reset, so it's clean when Bootloader talks to it.
     * Without this, ESP8266 stays in MQTT-connected state after STM32 soft reset,
     * and Bootloader's AT commands get no OK response (echo only, no OK). */
    {
        extern UART_HandleTypeDef huart2;
        uart_printf_mutex("[OTA] Resetting ESP8266 (AT+RST)...\r\n");
        HAL_UART_Transmit(&huart2, (uint8_t *)"AT+RST\r\n", 8, 100);
        osDelay(500);  /* give ESP8266 time to process and start resetting */
    }

    NVIC_SystemReset();          /* 软复位 → Bootloader 判定 OTA 请求 */
}
