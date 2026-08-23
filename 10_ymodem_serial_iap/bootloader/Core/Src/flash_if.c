#include "flash_if.h"
#include <string.h>
#include <stdio.h>

static uint32_t flash_end_address(void)
{
    return PARAM_FLASH_START + PARAM_FLASH_SIZE;
}

HAL_StatusTypeDef FLASH_ErasePage(uint32_t addr)
{
    return FLASH_ErasePages(addr, 1U);
}

HAL_StatusTypeDef FLASH_ErasePages(uint32_t addr, uint32_t page_count)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error;
    HAL_StatusTypeDef status;

    if (page_count == 0U || (addr % BL_FLASH_PAGE_SIZE) != 0U ||
        addr < BL_FLASH_START || addr >= flash_end_address() ||
        page_count > ((flash_end_address() - addr) / BL_FLASH_PAGE_SIZE)) {
        return HAL_ERROR;
    }
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return HAL_ERROR;
    }

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks = FLASH_BANK_1;
    erase_init.PageAddress = addr;
    erase_init.NbPages = page_count;
    status = HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();
    return status;
}

HAL_StatusTypeDef FLASH_WriteWord(uint32_t addr, uint32_t data)
{
    HAL_StatusTypeDef status;

    if ((addr & 3U) != 0U || addr < BL_FLASH_START ||
        addr > (flash_end_address() - 4U)) {
        return HAL_ERROR;
    }
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return HAL_ERROR;
    }
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);
    HAL_FLASH_Lock();
    return status;
}

HAL_StatusTypeDef FLASH_WriteBuf(uint32_t addr, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint32_t offset = 0U;
    uint32_t end_addr;
    uint32_t word;
    uint8_t word_bytes[4];

    if ((len != 0U && buf == NULL) || (addr & 3U) != 0U ||
        addr < BL_FLASH_START) {
        return HAL_ERROR;
    }
    end_addr = addr + (uint32_t)len;
    if (end_addr < addr || end_addr > flash_end_address()) {
        return HAL_ERROR;
    }
    end_addr = (end_addr + 3U) & ~3U;
    if (end_addr > flash_end_address() || len == 0U) {
        return (len == 0U) ? HAL_OK : HAL_ERROR;
    }
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return HAL_ERROR;
    }

    while (offset < (uint32_t)len) {
        uint32_t remaining = (uint32_t)len - offset;
        uint32_t copy_len = (remaining < 4U) ? remaining : 4U;
        memset(word_bytes, 0xFF, sizeof(word_bytes));
        memcpy(word_bytes, buf + offset, copy_len);
        memcpy(&word, word_bytes, sizeof(word));
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                   addr + offset, word);
        if (status != HAL_OK) {
            break;
        }
        offset += 4U;
    }

    HAL_FLASH_Lock();
    return status;
}

uint32_t FLASH_ReadWord(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

HAL_StatusTypeDef FLASH_EraseAppArea(void)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error;
    HAL_StatusTypeDef status;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return HAL_ERROR;
    }
    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks = FLASH_BANK_1;
    erase_init.PageAddress = APP_FLASH_START;
    erase_init.NbPages = APP_FLASH_SIZE / BL_FLASH_PAGE_SIZE;
    status = HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();
    if (status != HAL_OK) {
        printf("[FLASH] EraseAppArea FAIL! PageError=0x%08X Status=%u\r\n",
               (unsigned)page_error, (unsigned)status);
    }
    return status;
}
