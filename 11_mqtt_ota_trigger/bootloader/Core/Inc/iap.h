#ifndef __IAP_H
#define __IAP_H

#include "main.h"

/* ================================================================
 *   IAP_ProcessSerial：串口 Ymodem IAP 完整业务流程
 *
 *   做什么（5 步流水线）：
 *     1. Ymodem 流式收固件（每包回调写 Flash + 累计 CRC32）
 *     2. 收完后从 Flash 读回算 CRC32，和内存累计值双层比对
 *     3. 更新参数区（fw_size_bytes / fw_crc32 / build_num / last_ota_result）
 *     4. 清 ota_request_magic（避免砖了反复跳 IAP）
 *     5. NVIC_SystemReset() 软复位 → Bootloader 分支3自动 JumpToApp
 *
 *   注意：
 *     - 成功路径不返回（直接 NVIC 复位）
 *     - 失败才返回负值（让上层继续跑分支3"APP有效则跳"，救砖用）
 *     - 返回值：0=理论不会走到这里（成功时复位了），负值=失败
 * ================================================================ */
int IAP_ProcessSerial(void);

#endif /* __IAP_H */
