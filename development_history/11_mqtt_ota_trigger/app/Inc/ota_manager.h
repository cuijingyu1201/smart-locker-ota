#ifndef __OTA_MANAGER_H
#define __OTA_MANAGER_H

#include <stdint.h>

/* ================================================================
 *   OTA Manager —— APP 侧 OTA 请求触发层 (D11)
 *
 *   作用：
 *     APP 通过 MQTT 收到云端 OTA 命令后，解析 JSON、写参数区
 *     OTA 请求标志 (ota_request_magic="OTA1")，然后软复位，
 *     把控制权交给 Bootloader 执行升级。
 *
 *   定位：
 *     - D10：Bootloader 能通过"串口 Ymodem"拉固件（本地救砖）
 *     - D11：APP 能响应"云端 MQTT 命令"并触发升级（远程触发）
 *     - D12：Bootloader 能通过"WiFi"自动拉固件（远程拉固件）
 *     本文件只声明 API，实现在 ota_manager.c
 *   依赖：flash_param.h (FlashParam_SetOtaRequest)
 * ================================================================ */

/* ---------- 当前固件版本号（手动维护）----------
 *   每次发版改这里，配合 BUILD 号一起做版本比较
 *   例：1.0.0 → major=1, minor=0, patch=0
 *   注意：和 flash_param_t 里的 fw_ver_* 字段是同一套语义
 */
#define FW_VER_MAJOR    1
#define FW_VER_MINOR    0
#define FW_VER_PATCH    0

/* ---------- BUILD 号（每编译一次自动变）----------
 *   用 __DATE__ + __TIME__ 字符串做 djb2 哈希，取低 16 位
 *   __DATE__ 例 "Aug 23 2026"，__TIME__ 例 "12:34:56"
 *   两者拼起来每次编译都不同 → 哈希不同 → BUILD 号自动 +变化
 *   好处：不用手动改 build 号，每次 Rebuild 都不同
 *   代价：哈希极小概率冲突，但 BUILD 只做"要不要升"辅助判断，
 *        不参与 CRC32，冲突无害
 */
uint16_t OTA_GetCurrentBuildNum(void);

/* ---------- 解析 MQTT 收到的 OTA JSON 命令 ----------
 *   json     : MQTT PUBLISH 的 payload（JSON 字符串）
 *   json_len : payload 字节数
 *   out_major/minor/patch/build : 输出，新固件的版本号
 *   out_size : 输出，新固件字节数
 *   out_crc  : 输出，新固件 CRC32（十进制）
 *   返回 1 = 是 ota 命令且字段齐全，应升级
 *        0 = 不是 ota 命令 / 字段缺失，不升级
 *   说明：用 strstr+strtol 解析，和 app_esp8266.c 现有
 *        led_on/led_off 风格一致，不引入 cJSON
 */
int OTA_ParseCommand(const char *json, int json_len,
                     uint16_t *out_major, uint16_t *out_minor,
                     uint16_t *out_patch, uint16_t *out_build,
                     uint32_t *out_size, uint32_t *out_crc);

/* ---------- 执行 OTA 请求：写参数区标志 + 软复位 ----------
 *   new_fw_crc : 新固件 CRC32
 *   new_fw_size: 新固件字节数
 *   注意：本函数正常情况不返回（内部 NVIC_SystemReset 软复位）
 *         写参数区失败会打印错误后返回（不复位）
 */
void OTA_TriggerUpgrade(uint32_t new_fw_crc, uint32_t new_fw_size);

#endif /* __OTA_MANAGER_H */
