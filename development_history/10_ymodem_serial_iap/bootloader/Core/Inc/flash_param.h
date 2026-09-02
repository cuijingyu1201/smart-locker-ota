#ifndef __FLASH_PARAM_H
#define __FLASH_PARAM_H

#include "flash_partition.h"

/* ================================================================
 *   CRC32 计算（查表法，IEEE 802.3 多项式 0xEDB88320）
 *   查表法比逐位算快 8~16 倍，464KB 固件 CRC 不到 100ms 算完
 * ================================================================ */
void     CRC32_InitTable(void);                         /* 初始化 CRC 表，上电调用一次 */
uint32_t CRC32_Calc(const uint8_t *data, uint32_t len); /* 计算一段数据的 CRC32 */
uint32_t CRC32_CalcAppFlash(void);                      /* 直接计算整个 APP 区的 CRC32 */

/* ================================================================
 *   参数区读写 API
 * ================================================================ */

/* 读参数区到 RAM 缓存 p_out，返回 0=成功 -1=magic或crc不合法（参数区未初始化/损坏）*/
int FlashParam_Load(flash_param_t *p_out);

/* 把 RAM 里的 p_in 计算 CRC 后整个写回参数区，返回 0=成功 -1=失败
 * 注意：写参数区会自动先擦除整页，因为 Flash 写之前必须擦 */
int FlashParam_Save(const flash_param_t *p_in);

/* 只写 OTA 请求字段（擦一页代价高，升级时只需要写 ota_request_magic 等3个字段，用改字方法）
 * 返回 0=成功 -1=失败 */
int FlashParam_SetOtaRequest(uint32_t new_fw_crc32, uint32_t new_fw_size);

/* 清除 OTA 请求标志（升级成功/失败后调用，防止下次上电又走升级流程）*/
int FlashParam_ClearOtaRequest(void);

/* 调试用：用 printf 把参数区所有字段打印出来 */
void FlashParam_Print(const flash_param_t *p);

/* ================================================================
 *   CRC32 流式接口（给 D10 IAP 层用，避免 iap.c 再复制一份 CRC32 表）
 *   用法：
 *     uint32_t ctx;
 *     CRC32_StreamReset(&ctx);
 *     CRC32_StreamUpdate(&ctx, data1, len1);    // 第1包
 *     CRC32_StreamUpdate(&ctx, data2, len2);    // 第2包
 *     ...
 *     uint32_t result = CRC32_StreamFinalize(&ctx);  // 最终标准CRC32值
 * ================================================================ */
void     CRC32_StreamReset(uint32_t *ctx);
void     CRC32_StreamUpdate(uint32_t *ctx, const uint8_t *data, uint32_t len);
uint32_t CRC32_StreamFinalize(uint32_t *ctx);

#endif /* __FLASH_PARAM_H */
