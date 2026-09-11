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

/* 调度器启动前完成参数加载、校验、更新与写回，避免占用业务任务栈。 */
int FlashParam_InitOnBoot(void);

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
int FlashParam_SetOtaRequest(uint32_t new_fw_crc32, uint32_t new_fw_size,
                            uint16_t ver_major, uint16_t ver_minor,
                            uint16_t ver_patch, uint16_t build_num);

int FlashParam_GetInstalledVersion(uint16_t *ver_major, uint16_t *ver_minor,
                                   uint16_t *ver_patch, uint16_t *build_num);

/* 清除 OTA 请求标志（升级成功/失败后调用，防止下次上电又走升级流程）*/
int FlashParam_ClearOtaRequest(void);

/* 调试用：用 printf 把参数区所有字段打印出来 */
void FlashParam_Print(const flash_param_t *p);

/* ================================================================
 *   柜态持久化 API（状态机进稳态时调用）
 *   注意：每次写都会整页擦+写，不能在循环里高频调用
 * ================================================================ */

/* 写柜门状态（状态机进 CLOSED/FAULT 时调）
 *   state: 0=CLOSED 1=OPEN 2=FAULT
 *   返回 0=成功 -1=失败 */
int FlashParam_SetDoorState(uint8_t state);

/* 读柜门状态，返回 0=CLOSED 1=OPEN 2=FAULT（参数区无效时返回 0） */
uint8_t FlashParam_GetDoorState(void);

/* 记录一次开柜：写 door_state=1 + last_open_ts=当前秒数
 *   在 Cabinet_FSM_OpenRequest 被接受时调用
 *   返回 0=成功 -1=失败 */
int FlashParam_RecordOpen(void);

/* Flash 擦写计数 +1，返回当前计数值
 *   每次 SetDoorState/RecordOpen 内部会自动 +1
 *   满第一千次时调用方应上报告警 */
uint32_t FlashParam_IncEraseCnt(void);


/* 读当前取件码到 out（至少 7 字节空间，含末尾 '\0'）
 * 返回 0=成功 -1=失败（参数区无效时 out 填默认 "123456"）*/
int FlashParam_GetCurrentCode(char *out, uint8_t len);

#endif /* __FLASH_PARAM_H */

