#include "flash_if.h"
#include "app_uart.h"
#include <stdio.h> 

/* 擦除指定地址所在的 2KB 页 */
HAL_StatusTypeDef FLASH_ErasePage(uint32_t addr)
{
    HAL_StatusTypeDef status;
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error;

    HAL_FLASH_Unlock();

    erase_init.TypeErase    = FLASH_TYPEERASE_PAGES;
    erase_init.Banks        = FLASH_BANK_1;
    erase_init.PageAddress  = addr;
    erase_init.NbPages      = 2;

    status = HAL_FLASHEx_Erase(&erase_init, &page_error);

    HAL_FLASH_Lock();
    return status;
}

/* 写 4 字节（32位）*/
HAL_StatusTypeDef FLASH_WriteWord(uint32_t addr, uint32_t data)
{
    HAL_StatusTypeDef status;

    HAL_FLASH_Unlock();
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);
    HAL_FLASH_Lock();

    return status;
}

/* 按字节写缓冲区，内部按 4 字节对齐循环写 */
HAL_StatusTypeDef FLASH_WriteBuf(uint32_t addr, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef status = HAL_OK;
    uint16_t i;
    uint32_t *p = (uint32_t *)buf;

    HAL_FLASH_Unlock();

		for (i = 0; i < len / 4; i++) {
				/* 写前读目标地址，确认是不是真擦干净了 */
				uint32_t cur = *(volatile uint32_t *)(addr + i * 4);
				if (cur != 0xFFFFFFFFUL) {
						uart_printf_mutex("[FLASH] @0x%08X NOT erased! cur=0x%08X\r\n",
									 (unsigned)(addr + i * 4), (unsigned)cur);
				}

				status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
																		addr + i * 4, p[i]);
				if (status != HAL_OK) {
						uart_printf_mutex("[FLASH] Write FAIL @0x%08X i=%u status=%u SR=0x%08X\r\n",
									 (unsigned)(addr + i * 4), (unsigned)i,
									 (unsigned)status, (unsigned)FLASH->SR);
						HAL_FLASH_Lock();
						return status;
				}
		}
		
    HAL_FLASH_Lock();
    return status;
}

/* 读 4 字节 */
uint32_t FLASH_ReadWord(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}
