#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#include "main.h"
#include "flash_partition.h"

/* ==================== Flash ??????? ==================== */
#define BL_FLASH_PAGE_SIZE      2048         /* F103ZE ?? 2KB */
#define OTA_FLAG_ADDR        0x0807F000   /* OTA ??????????????????????*/
#define OTA_FLAG_MAGIC       0xA5A5A5A5   /* OTA ???????? */

/* ==================== Flash ???????? ==================== */
HAL_StatusTypeDef FLASH_ErasePage(uint32_t addr);
HAL_StatusTypeDef FLASH_ErasePages(uint32_t addr, uint32_t page_count);
HAL_StatusTypeDef FLASH_WriteWord(uint32_t addr, uint32_t data);
HAL_StatusTypeDef FLASH_WriteBuf(uint32_t addr, uint8_t *buf, uint16_t len);
uint32_t          FLASH_ReadWord(uint32_t addr);
HAL_StatusTypeDef FLASH_EraseAppArea(void);   /*  ??????????? APP ????232 ???*/

#endif /* __FLASH_IF_H */

