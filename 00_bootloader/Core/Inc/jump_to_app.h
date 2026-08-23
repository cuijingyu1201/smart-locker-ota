#ifndef __JUMP_TO_APP_H
#define __JUMP_TO_APP_H

#include "main.h"

/* APP 起始地址（和 flash_if.h 里一样，这里重复定义方便单独引用）*/
#define APP_FLASH_START     0x08008000

/* 检查 APP 是否有效（看 APP 起始地址有没有有效的栈指针）*/
uint8_t IsAppValid(void);

/* 核心：跳转到 APP 的 6 步操作 */
void JumpToApp(void);

#endif /* __JUMP_TO_APP_H */
