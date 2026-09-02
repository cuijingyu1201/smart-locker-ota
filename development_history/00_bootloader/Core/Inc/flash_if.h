#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#include "main.h"

/* ==================== Flash 地址定义 ==================== */
#define BL_FLASH_PAGE_SIZE      2048         /* F103ZE 每页 2KB */
#define APP_FLASH_START      0x08008000   /* APP 起始地址 */
#define APP_FLASH_SIZE       0x00074000   /* APP 大小 464KB */
#define OTA_FLAG_ADDR        0x0807F000   /* OTA 标志区（最后一个扇区起始）*/
#define OTA_FLAG_MAGIC       0xA5A5A5A5   /* OTA 升级标志值 */

/* ==================== Flash 操作函数 ==================== */
HAL_StatusTypeDef FLASH_ErasePage(uint32_t addr);
HAL_StatusTypeDef FLASH_WriteWord(uint32_t addr, uint32_t data);
HAL_StatusTypeDef FLASH_WriteBuf(uint32_t addr, uint8_t *buf, uint16_t len);
uint32_t          FLASH_ReadWord(uint32_t addr);

#endif /* __FLASH_IF_H */
