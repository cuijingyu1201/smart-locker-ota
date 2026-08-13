\# IoT OTA 项目 · 踩坑记录



\## D1 - 环境验证 \& LED 闪烁工程



\### 坑1：CubeMX 里 PA10 (USART1\_RX) 的 GPIO Mode 只能选 Input mode，没有 Alternate Function

\- \*\*现象\*\*：RX 引脚配置时想按 TX 的思路选复用推挽输出，但下拉框里没有这个选项，怀疑自己操作错了。

\- \*\*原因\*\*：STM32F1 系列的「复用功能输入」不需要专门配置 AF 模式，引脚信号是广播的，USART1 只要开了时钟就能直接拿到引脚电平。GPIO 层面只要配成普通 Input mode 就行。

\- \*\*官方对齐\*\*：CubeMX 生成的 usart.c 第 82 行就是 `GPIO\_MODE\_INPUT`，和正点原子官方工程完全一致。

\- \*\*耗时\*\*：20 分钟



\### 坑2：串口助手看不到启动打印（只有 while 循环的周期打印）

\- \*\*现象\*\*：上电后串口助手只能看到 \[tick:xxx] 循环打印，看不到 System Clock 那几行启动信息，以为代码写漏了。

\- \*\*原因\*\*：板子上电后 20ms 就把启动信息发完了，但我 2 秒后才打开串口助手，之前的数据已经丢失了。

\- \*\*解决方法\*\*：保持串口助手打开状态，按一下开发板的 RST 复位键，就能完整看到启动信息了。或者在 USER CODE BEGIN 2 开头加 HAL\_Delay(3000) 给串口助手留出打开时间。

\- \*\*耗时\*\*：15 分钟


---

# D2: FreeRTOS 任务创建与调度（2026-08-13）

## 坑1：HAL 时基源与 FreeRTOS 心跳冲突（致命）
- **现象**：CubeMX 启用 FreeRTOS 后，如果 HAL 时基源仍为 SysTick，运行时必 HardFault。
- **根因**：FreeRTOS 调度器强制独占 SysTick 定时器（直接写 SysTick->LOAD 寄存器），而 HAL_Delay/HAL_GetTick 也默认用 SysTick → 两个抢同一个定时器 → 冲突崩溃。
- **解决**：CubeMX → SYS → Timebase Source 从 SysTick 改为 TIM4（TIM2~TIM7 任选）。HAL 库自动生成 `stm32f1xx_hal_timebase_tim.c` 用 TIM4 当时基，FreeRTOS 独占 SysTick。
- **验证**：串口同时打印 rtos_tick（SysTick）和 hal_tick（TIM4），两者独立增长，差值约 25ms（HAL_Init 比 osKernelStart 早执行的时间差），证明双时基分离成功。

## 坑2：CubeMX 重生成代码后 MicroLIB 被清
- **现象**：每次点 GENERATE CODE 后，Keil Options → Target → Use MicroLIB 复选框被清掉。
- **根因**：CubeMX 生成的 .uvprojx 不保留 MicroLIB 勾选状态。
- **后果**：printf 重定向编译 0 Error 但运行时完全不打印（因为没用 MicroLIB，fputc 重定向不生效）。
- **解决**：每次 CubeMX 重生成代码后，必须重新打开 Keil → Alt+F7 → Target → 勾选 Use MicroLIB。

## 坑3：UTF-8 / GBK 编码冲突导致中文乱码
- **现象**：CubeMX 生成的 .c/.h 文件中文注释全是乱码。
- **根因**：CubeMX 默认输出 UTF-8 编码，Keil 在中文 Windows 下默认用 GBK(ANSI) 打开 → 两套编码不兼容。
- **解决**：Keil → Edit → Configuration → Editor → Encoding 选 UTF-8 → 关闭 Keil 删除 .uvoptx 缓存 → 重新打开工程。
- **教训**：嵌入式 Windows 开发环境，编译器(ARMCC 5.06)和 IDE 默认用系统 ANSI(GBK)，而 CubeMX 默认输出 UTF-8，必须统一编码源。

## 坑4：extern 变量声明与 #define 宏冲突（编译错误 #18/#101）
- **现象**：app_task.c 里写了 `extern GPIO_TypeDef *LED0_GPIO_Port;` → 报 `#18 expected a ")"` + `#101 has already been declared`。
- **根因**：main.h 里 LED0_GPIO_Port 是 `#define` 宏（编译期文本替换），不是全局变量。预处理器把 `LED0_GPIO_Port` 替换成 `GPIOB`，再替换成 `((GPIO_TypeDef *) 0x40010C00UL)` → 变量声明的「变量名位置」变成了地址字面量表达式 → C 语法 declarator 结构被破坏 → Parser 崩溃。
- **解决**：删除所有 extern 声明，直接 `#include "main.h"` 使用 CubeMX 的宏定义。
- **教训**：CubeMX 生成的引脚是 #define 宏，不是全局变量。用之前必须读 main.h 确认是 #define 还是真实变量声明。

## 坑5：FreeRTOS 头文件 include 顺序（编译错误 #35）
- **现象**：`#include "task.h"` 报 `#35 #error directive: include FreeRTOS.h must appear before task.h`。
- **根因**：FreeRTOS 的 task.h 第 33 行有编译期断言，要求 FreeRTOS.h 必须在 task.h 之前被包含（因为 FreeRTOSConfig.h 由 FreeRTOS.h 间接包含）。
- **解决**：头文件 include 顺序必须是：`FreeRTOS.h` → `task.h` → `cmsis_os2.h`。
- **教训**：FreeRTOS 内核头文件有强制依赖顺序，CMSIS-RTOS 标准没规定但 FreeRTOS 内核强制。

## 坑6：钩子函数多重定义（链接错误 L6200E）
- **现象**：链接报 `L6200E Symbol vApplicationIdleHook multiply defined (by app_task.o and freertos.o)`。
- **根因**：CubeMX 开启 USE_IDLE_HOOK 等宏后，会在 freertos.c 自动生成空骨架钩子函数。我们在 app_task.c 又写了完整实现 → 链接器看到两份同名强符号定义 → 报错。
- **解决**：把 freertos.c 里 CubeMX 生成的 3 个空钩子函数（vApplicationIdleHook / vApplicationMallocFailedHook / vApplicationStackOverflowHook）用 `#if 0 ... #endif` 注释掉，保留 app_task.c 里的完整实现。
- **教训**：FreeRTOS 钩子函数是弱符号机制的特例，CubeMX 会生成空壳。自己实现时必须注释掉 CubeMX 的空壳，否则 C 语言同一函数名只能有一份强符号定义。

## 坑7：钩子函数声明参数类型冲突（编译错误 #147-D）
- **现象**：`#147-D declaration is incompatible with "void vApplicationStackOverflowHook(TaskHandle_t, char *)"`。
- **根因**：freertos.c 里 CubeMX 自动生成的钩子函数**声明**（带分号 `;` 的 prototype）用了老版 V1 类型 `xTaskHandle` + `signed char *`，而 app_task.h 里我们用 V2 类型 `TaskHandle_t` + `char *` → 两份声明参数类型不一致。
- **解决**：注释掉 freertos.c 里 CubeMX 生成的钩子函数**声明行**（不只是定义，还有前面的 prototype 声明）。
- **教训**：注释函数时要注意「声明」和「定义」都要注释，漏掉声明行仍会报类型冲突。

## D2 成果
- FreeRTOS CMSIS_V2 接入完成
- 3 个任务（TaskLED + TaskPrint + IdleHook）调度正常
- 双时基分离验证通过（SysTick=RTOS tick / TIM4=HAL tick）
- n_tasks=4（TaskLED + TaskPrint + Idle + Timer Service）
- 编译 0 Error 0 Warning，运行稳定无 HardFault

