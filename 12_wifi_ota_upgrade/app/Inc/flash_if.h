#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#include "main.h"
#include "flash_partition.h"

/* Flash 页大小（F103ZE 每页 2KB）*/
#define BL_FLASH_PAGE_SIZE   2048

/* Flash 操作函数声明 */
HAL_StatusTypeDef FLASH_ErasePage(uint32_t addr);
HAL_StatusTypeDef FLASH_WriteWord(uint32_t addr, uint32_t data);
HAL_StatusTypeDef FLASH_WriteBuf(uint32_t addr, uint8_t *buf, uint16_t len);
uint32_t          FLASH_ReadWord(uint32_t addr);

#endif /* __FLASH_IF_H */
