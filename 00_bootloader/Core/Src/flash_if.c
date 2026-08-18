#include "flash_if.h"

/* ================================================================
 *   FLASH_ErasePage
 *   擦除指定地址所在的 2KB 页
 *   Flash 写之前必须先擦除（物理特性），擦除后全变成 0xFF
 * ================================================================ */
HAL_StatusTypeDef FLASH_ErasePage(uint32_t addr)
{
    HAL_StatusTypeDef status;
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error;

    HAL_FLASH_Unlock();                     /* 1. 解锁 Flash */

    erase_init.TypeErase    = FLASH_TYPEERASE_PAGES;
    erase_init.Banks        = FLASH_BANK_1;
    erase_init.PageAddress  = addr;          /* 擦除这个地址所在的页 */
    erase_init.NbPages      = 1;             /* 只擦 1 页 */

    status = HAL_FLASHEx_Erase(&erase_init, &page_error);

    HAL_FLASH_Lock();                       /* 2. 重新上锁 */
    return status;
}

/* ================================================================
 *   FLASH_WriteWord
 *   在指定地址写 4 字节（32位）
 *   STM32 Flash 只能按 16位/32位写，不能按 8位写
 * ================================================================ */
HAL_StatusTypeDef FLASH_WriteWord(uint32_t addr, uint32_t data)
{
    HAL_StatusTypeDef status;

    HAL_FLASH_Unlock();
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);
    HAL_FLASH_Lock();

    return status;
}

/* ================================================================
 *   FLASH_WriteBuf
 *   批量写入缓冲区（按 4 字节对齐循环写）
 *   注意：len 必须是 4 的倍数，否则最后几个字节会被忽略
 * ================================================================ */
HAL_StatusTypeDef FLASH_WriteBuf(uint32_t addr, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint16_t i;
    uint32_t *p = (uint32_t *)buf;    /* 把 buf 当成 32位数组来访问 */

    HAL_FLASH_Unlock();

    for (i = 0; i < len / 4; i++) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                    addr + i * 4, p[i]);
        if (status != HAL_OK) break;
    }

    HAL_FLASH_Lock();
    return status;
}

/* ================================================================
 *   FLASH_ReadWord
 *   读 4 字节
 *   Flash 读取不需要解锁，直接用指针解引用
 * ================================================================ */
uint32_t FLASH_ReadWord(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}
