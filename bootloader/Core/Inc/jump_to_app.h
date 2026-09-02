#ifndef __JUMP_TO_APP_H
#define __JUMP_TO_APP_H

#include "main.h"
#include "flash_partition.h"


/* 检查 APP 是否有效（看 APP 起始地址有没有有效的栈指针）*/
uint8_t IsAppValid(void);

/* 核心：跳转到 APP 的 6 步操作 */
void JumpToApp(void);

#endif /* __JUMP_TO_APP_H */
