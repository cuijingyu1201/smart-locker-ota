#include "jump_to_app.h"

/* 函数指针类型：用于跳转到 APP 的 Reset_Handler */
typedef void (*pFunc)(void);

/* ================================================================
 *   IsAppValid
 *   检查 APP 是否有效
 *
 *   原理：APP 编译后，0x08008000 地址存的是「初始栈指针 MSP」
 *   这个值必须指向 RAM 区域（0x20000000~0x2000FFFF）
 *   如果 APP 没烧，这个地址是 0xFFFFFFFF（Flash 擦除后的默认值）
 * ================================================================ */
uint8_t IsAppValid(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_FLASH_START;

    /* 栈指针高 12 位必须等于 0x200（RAM 起始 0x20000000）*/
    if ((app_sp & 0xFFF00000) == 0x20000000) {
        return 1;   /* APP 有效 */
    }
    return 0;       /* APP 无效（没烧或写坏了）*/
}

/* ================================================================
 *   JumpToApp
 *   从 Bootloader 跳转到 APP 的 6 步操作
 *
 *   APP 在 Flash 里的向量表布局：
 *     0x08008000 + 0  =  初始栈指针 MSP
 *     0x08008000 + 4  =  Reset_Handler 地址
 *     0x08008000 + 8  =  NMI_Handler 地址
 *     ...
 *
 *   跳转 = 把 CPU 的「栈指针」和「PC」都设成 APP 的值
 * ================================================================ */
void JumpToApp(void)
{
    /* 1. 读 APP 的栈指针（向量表第 0 项）*/
    uint32_t app_sp = *(volatile uint32_t *)APP_FLASH_START;

    /* 2. 读 APP 的 Reset_Handler 地址（向量表第 1 项）*/
    uint32_t app_reset = *(volatile uint32_t *)(APP_FLASH_START + 4);
    pFunc jump = (pFunc)app_reset;     /* 转成函数指针 */

    /* 3. 关闭所有中断（跳转过程中不能被打断）*/
    __disable_irq();

    /* 4. 清除所有 NVIC 挂起标志（防止跳转后残留中断触发）*/
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;   /* 关闭所有中断 */
        NVIC->ICPR[i] = 0xFFFFFFFF;   /* 清除所有挂起标志 */
    }

    /* 5. 设置 VTOR 指向 APP 的向量表 */
    SCB->VTOR = APP_FLASH_START;

    /* 6. 设置 MSP 栈指针，然后跳转 */
    __set_MSP(app_sp);     /* 设置主栈指针为 APP 的栈顶 */
    jump();                /* 跳转到 APP 的 Reset_Handler */
    /* 跳转后不会返回 */
}
