# APP 栈溢出问题定位与修复说明

## 1. 问题概述

本次故障发生在 STM32F103 APP 的 FreeRTOS 运行阶段。串口日志中最关键的片段是：

```text
---------- RTOS STATS ----------
TaskPrint_stack_free = 108 words
...
[PARAM] ota_req_magic   = 0x
[ERROR] STACK OVERFLOW in task 'TaskLED' !
```

日志显示，系统已经启动了多次，`TaskLED` 在打印 Flash 参数时发生栈溢出。故障并不是 LED 翻转逻辑本身导致的，而是 `TaskLED` 在第一次运行时执行了大量启动维护工作。

这次修复没有增加任何任务栈大小，而是拆掉了导致栈峰值过高的调用链，并补充了持续监测和更可靠的异常处理。

## 2. 如何从串口日志定位根因

### 2.1 故障出现的位置

栈溢出前最后输出的是：

```text
==== Flash Param Dump @0x0807F000 ====
  magic_header    = ...
  struct_crc32    = ...
  ota_req_magic   = 0x
[ERROR] STACK OVERFLOW in task 'TaskLED' !
```

这说明故障发生在 `FlashParam_Print()` 的连续格式化打印过程中，而不是发生在后续 500ms LED 延时循环中。

### 2.2 对照代码后的调用链

原来的 `TaskLED()` 首次运行会依次执行：

```text
TaskLED
  -> CRC32_InitTable
  -> FlashParam_Load
  -> CRC32_CalcAppFlash       // 扫描整个 APP Flash
  -> FlashParam_Save
       -> FLASH_ErasePage
       -> FLASH_WriteBuf
  -> FlashParam_Print          // 多次 printf
```

每次 `printf` 还会进入：

```text
printf
  -> fputc
  -> osMutexAcquire
  -> FreeRTOS 队列/调度器内部函数
```

所以 `TaskLED` 的栈峰值由“参数初始化 + Flash 操作 + 格式化输出 + RTOS 互斥锁”共同叠加。串口日志正好在参数打印中断，和这个调用链完全对应。

### 2.3 为什么原来的监测没有提前阻止问题

工程中原本使用的是：

```c
#define configCHECK_FOR_STACK_OVERFLOW 1
```

这种模式只能在部分栈指针越界场景下发现问题，不能充分检查栈尾的破坏情况。此外，任务属性中的 `stack_size` 单位是字节，但原代码注释一直按 word 描述，容易错误估算实际容量。

## 3. 处理逻辑

整个修复按下面的顺序进行：

```text
读取串口日志
    |
    v
确定故障任务和最后一条输出
    |
    v
检查任务创建参数、局部数组、printf 调用链和静态调用图
    |
    v
把启动期重操作移出业务任务
    |
    v
把日志格式化缓冲区移出任务栈，并避免逐字符加锁
    |
    v
把 ESP8266 大缓冲区移到静态存储区
    |
    v
启用栈尾哨兵检查、异常安全输出和栈水位监测
    |
    v
使用 Keil 静态调用图重新编译验证
```

核心原则是：

1. 不让周期性业务任务承担一次性的启动维护工作。
2. 不把大数组和日志缓冲区放在任务栈上。
3. 不在栈已经损坏的异常钩子里继续调用 `printf()` 或 `osDelay()`。
4. 不只依赖一次性的栈溢出钩子，而是持续观察任务历史最低栈水位。

## 4. 具体修改内容

### 4.1 把 Flash 参数初始化移到调度器启动前

新增 [app/Src/flash_param.c](app/Src/flash_param.c) 中的 `FlashParam_InitOnBoot()`，并在 [app/Src/main.c](app/Src/main.c) 初始化 USART 后、启动 FreeRTOS 前调用：

```c
(void)FlashParam_InitOnBoot();
osKernelInitialize();
MX_FREERTOS_Init();
osKernelStart();
```

这个函数负责：

- 初始化 CRC32 表；
- 加载并检查参数区；
- 参数无效时装载默认值；
- 更新 boot count 和固件 CRC；
- 擦除并保存参数；
- 重新从 Flash 读回并验证；
- 最后打印参数摘要。

这样这些工作使用的是启动阶段的主栈，不再占用 `TaskLED` 的任务栈。`TaskLED()` 现在只负责 LED、队列、按键事件和延时。

### 4.2 修复日志输出的栈和并发问题

在 [app/Src/app_uart.c](app/Src/app_uart.c) 中，`uart_printf_mutex()` 现在使用文件级静态缓冲区：

```c
static char g_uart_log_buffer[256];
```

输出流程变成：

```text
获取 UART 互斥锁
    -> vsnprintf 写入静态缓冲区
    -> 一次性发送完整消息
    -> 释放互斥锁
```

原来的 `char buf[256]` 位于每个调用者的任务栈上，而且 `fputc()` 是每发送一个字符就申请和释放一次 RTOS 互斥锁。现在改为整条消息加锁，降低了调用深度，也避免了如下串口交叉输出：

```text
---------- RTOS STA[TASK_LED ] ...TS ----------
```

APP 源码中的业务 `printf()` 已统一改用 `uart_printf_mutex()`；`snprintf()` 仍然保留用于生成 AT 指令和 JSON，不能把这两者混淆。

### 4.3 移除异常路径中的 RTOS 和 libc 调用

原来的栈溢出钩子会执行：

```c
printf(...);
osDelay(1000);
```

此时任务栈可能已经损坏，而且钩子可能是在调度器切换上下文中触发，继续调用 RTOS API 会让故障更加不稳定。

现在新增 `uart_panic_write()`，直接轮询 USART1 寄存器发送有限长度的固定文本，不依赖：

- `printf/vsnprintf`；
- UART 互斥锁；
- FreeRTOS 调度；
- 延时函数。

`vApplicationStackOverflowHook()` 和 `vApplicationMallocFailedHook()` 在关闭中断后调用这个安全输出函数，然后停机等待调试器或外部复位。

### 4.4 把 ESP8266 大数组移出任务栈

在 [app/Src/app_esp8266.c](app/Src/app_esp8266.c) 中，接收行缓冲、MQTT 报文、JSON 和 AT 命令缓冲区改为 `static` 存储：

```c
static char line[256];
static uint8_t mqtt_buf[256];
static char json_buf[128];
static char at_cmd[32];
```

这些缓冲区由单个 ESP8266 任务独占，不存在多个任务同时使用的问题。它们现在位于 BSS 区，不再随着状态机函数调用压入任务栈。

### 4.5 启用更严格的栈检查

在 [app/Inc/FreeRTOSConfig.h](app/Inc/FreeRTOSConfig.h) 和 `app/app.ioc` 中统一改为：

```c
#define configCHECK_FOR_STACK_OVERFLOW 2
```

模式 2 会检查任务栈尾的填充值是否被破坏，比原来的模式 1 更可靠。任务栈宏也改成了明确的字节命名：

```c
#define TASK_LED_STACK_SIZE_BYTES   (384U)
#define TASK_PRINT_STACK_SIZE_BYTES (768U)
```

数值没有增加，只是明确说明 CMSIS-RTOS `osThreadAttr_t.stack_size` 的单位是字节。

### 4.6 增加任务栈水位监控

在 [app/Src/app_task.c](app/Src/app_task.c) 中增加任务登记和检查：

- 创建任务后记录任务句柄、名称和配置的字节数；
- 使用 `osThreadGetStackSpace()` 读取历史最低剩余栈；
- 剩余栈低于总栈的 25% 或 128 字节时立即输出 `WARN`；
- 每 30 秒输出一次所有已登记应用任务的栈水位；
- 监控周期使用 `pdMS_TO_TICKS()`，不会依赖固定的 tick 频率。

这能在未来有人给任务增加局部变量或新的深调用链时，先看到告警，而不是等到真正越界后才发现。

### 4.7 清理 Idle Hook

Idle Hook 原来会定期 `printf()`。Idle Hook 不能阻塞，也不应该执行复杂日志格式化。现在只保留：

```c
__WFI();
```

这样 Idle 任务不会因为日志或互斥锁消耗额外栈空间，也遵守 FreeRTOS Idle Hook 的调用约束。

## 5. 修改前后的效果

Keil ARMCC5 全量构建结果：

```text
0 Error(s)
```

静态调用图对比：

| 项目 | 修改前 | 修改后 |
|---|---:|---:|
| 整个固件最大可追踪栈深 | 1048B | 408B |
| `TaskESP8266` 最大调用深度 | 1048B | 248B |
| `TaskLED` 最大调用深度 | 旧调用链包含 Flash/CRC/打印 | 192B |
| `TaskLED` 配置栈大小 | 384B | 384B |
| `TaskESP8266` 配置栈大小 | 2048B | 2048B |

最重要的一点是：任务栈配置值没有增加，变化来自调用链和内存归属的调整。

## 6. 以后如何判断是否又出现同类风险

### 6.1 查看串口日志

正常运行时应能看到类似：

```text
[STACK]        TaskLED       free=...B total=384B
[STACK]        TaskPrint     free=...B total=768B
```

如果出现：

```text
[STACK] WARN TaskLED       free=...B total=384B
```

说明已经接近风险线，应检查最近是否新增了局部数组、格式化输出或深层函数调用。

### 6.2 新增代码时遵守的规则

1. 不要在任务函数中声明 100 字节以上的局部数组；需要时放到静态工作区，并确认只有一个任务使用。
2. 不要在任务中直接调用标准 `printf()`；使用 `uart_printf_mutex()`。
3. 不要在 Idle Hook、Malloc Hook、Stack Overflow Hook 中调用阻塞式 RTOS API。
4. 创建任务时把栈宏命名为 `*_STACK_SIZE_BYTES`，不要按 word 估算字节值。
5. 修改任务逻辑后重新查看 Keil `.htm` 静态调用图，并观察运行期最低水位。
6. 不要只看“任务启动成功”，要确认栈水位、堆余量和异常钩子都正常。

## 7. 本次验证边界

本次已使用工程现有 Keil ARMCC5 配置完成全量编译和静态栈调用图检查。串口日志用于定位故障，代码修复后还需要在实际硬件上运行一轮，确认新的 `[STACK]` 水位输出和参数初始化流程符合预期。

构建时仍有少量原工程警告，例如 `flash_partition.h` 文件末尾缺少换行和 ESP8266 中未使用的状态时间变量，但没有编译错误，也不影响本次栈溢出修复。

