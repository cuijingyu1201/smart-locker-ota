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

---

# D3：FreeRTOS 4 大 IPC（2026-08-14）

## 坑1：编译时间戳不变（串口 Build 行日期时间永远是旧的）

- **现象**：改了业务代码 → 按 F7 编译 → 下载烧录 → 打开串口看 Build: 行，日期时间和上一次一模一样，以为新代码没烧进去。
- **根因**：`__DATE__` 和 `__TIME__` 是 **C 编译器内置的编译期宏**，只有**包含它们的那个源文件（这里是 main.c）被重新编译时**才会被替换成新的值。Keil 默认按 F7 是「增量编译 Build Target」—— 增量编译只会重编「修改时间比 .o 文件新」的源文件。如果 main.c 没改动，Keil 就跳过它不重编，所以 `__DATE__` / `__TIME__` 永远是旧的。
- **重点区分两个「时间戳」问题（不要混为一谈）**：
  - **Build 时间戳不变** = 编译期问题，和程序逻辑完全没关系；解决方法是「强制 main.c 重编」。
  - **rtos_tick 不再增长** = 运行期卡死 / HardFault，和编译没关系；解决方法是「排查任务栈溢出 / 状态机崩溃」。
- **解决**（任选一种）：
  1. 需要新 Build 时间戳时，Keil 菜单 **Project → Rebuild all target files**（全量重编所有源文件）。
  2. 或先 **Project → Clean Targets** 删除所有 .o，再按 F7（效果同上）。
  3. 最简单：main.c 里随便打个空行保存 → 再 F7，Keil 检测到 main.c 修改时间变了就会重编。
- **教训**：Keil F7 = 增量编译（只会重编改过的文件）。`__DATE__`、`__TIME__`、`__FILE__` 这类编译期宏，一定要对应源文件被重编才会更新。

## 坑2：rtos_tick 运行期卡死（系统 HardFault / 调度器状态机崩溃）

- **现象**：TaskLED 打印到一半（比如 `[TASK_LED ] rtos_tick=14895` 之后不再继续），LED 停止闪烁，按任何按键没反应，串口再也没有新输出。
- **根因排查过程（二分法定位）**：
  1. 先把 TaskLED 砍到只剩「2 行翻转 + 1 行 osDelay」，能稳定 500ms 闪，说明调度器本身没问题。
  2. 第一次加 `uart_printf_mutex`（带 buf[256] + vsnprintf 版本）→ 立刻卡死。根因：TaskLED 的栈(256 words / 1024B) 被 vsnprintf + buf[256] 吃了 600B 以上，栈底的 canary 被踩 → `STACK OVERFLOW in task 'TaskLED'` 或 huart1 句柄被损坏后 `HAL_UART_Transmit` 死等 TXE。
  3. 去掉 vsnprintf，换回裸 `printf` + 互斥锁，又出现卡死。根因：TaskSemHandle 优先级 = 40（High，最高），ISR give 信号量后立刻抢占正在打印的 TaskLED(prio=24)，两个任务在 fputc 里争抢同一个互斥锁 → 每个字符(87μs@115200) 都触发一次 PendSV 上下文切换 → 调度器内部链表耗到最终状态错乱，系统挂死。
- **解决**：
  1. 凡是要调 `printf` 的任务，栈大小至少 384 words（vsnprintf 版本至少 512 words）。永远用 `uxTaskGetStackHighWaterMark(NULL)` 的结果来判断要不要加栈：< 20 words 就必须加。
  2. **ISR 触发的高优先级任务（信号量延迟处理任务）绝对不能调 printf / HAL_UART_Transmit / osMutexAcquire(长 timeout)**—— 这些操作在高优先级里会把调度器耗死。只做「写全局变量」的操作，几微秒就结束，立刻重新阻塞等下一次信号量。
- **教训**：运行期卡死 = 任务栈 / 调度器 / 外设状态机 三类问题之一。用二分法往回删代码（先砍新写的 IPC → 再砍 printf → 最后砍到只剩 3 行翻转+osDelay），先让最小功能稳定跑，再一行行加回去，100% 能定位到根因。

## 坑3：互斥锁实现的两次迭代（uart_printf_mutex → 放到 fputc 里）

### 第 1 版（有问题，被淘汰）：上层加锁 + vsnprintf + buf[256]

```c
void uart_printf_mutex(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[256];                                 // ← 256B 局部变量
    int len = vsnprintf(buf, sizeof(buf), fmt, args); // ← vsnprintf 格式化
    va_end(args);

    if (osMutexAcquire(g_uart_mutex_handle, 100) == osOK) {
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, 0xFFFF);
        osMutexRelease(g_uart_mutex_handle);
    }
}
```

- **致命缺陷 1：吃栈太深**。单次调用栈深度 ≈ 256B(buf) + 200B(vsnprintf 内部) + 50B(va_list/参数) ≈ 500B。TaskLED 256 words(1024B) 勉强够但踩 canary，TaskSemHandle 128 words(512B) **调一次直接爆栈卡死**。
- **致命缺陷 2：调用方全局替换容易漏**。必须所有任务把 printf 改成 uart_printf_mutex，漏一个任务没改（比如 IdleHook 或钩子函数里的 printf）→ 两个任务一个走互斥锁、一个直接走裸 HAL_UART_Transmit → 打印乱码+冲突卡死概率指数上升。

### 第 2 版（最终采用）：互斥锁下沉到 fputc（重定向函数最底层）

main.c 的 fputc 重定向代码改成：

```c
#ifdef __GNUC__
  #define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
  #define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif
PUTCHAR_PROTOTYPE {
    /* 互斥锁没创建好（启动阶段）就跳过加锁，避免死锁 */
    if (g_uart_mutex_handle != NULL) {
        osMutexAcquire(g_uart_mutex_handle, 100);
    }
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    if (g_uart_mutex_handle != NULL) {
        osMutexRelease(g_uart_mutex_handle);
    }
    return ch;
}
```

### 第 2 版的 4 个优点
1. **上层零改动**：所有任务、钩子函数、初始化代码都继续写裸 `printf(...)`，不用全局替换 → 永远不会漏改某个 printf 导致冲突。
2. **栈占用砍掉 600B**：没有 buf[256] 局部变量，也不调 vsnprintf，每个字符的栈深度只有函数调用开销（几十字节），任务爆栈概率大幅降低。
3. **兼容启动阶段**：`g_uart_mutex_handle != NULL` 判断——App_IPC_Init 之前（启动信息打印时互斥锁还没建）不加锁直接发；创建好之后自动加锁。这就避免了「锁是 NULL 还去 acquire」死锁。
4. **粒度虽然按字符但足够用**：115200 波特率下 1 个字符只要 87μs，即使 87μs 被切换一次也不会乱日志（反正一次只会被切走一个字符，下次切回来继续发剩下的，串口字节流顺序天然正确）。

- **教训**：共享资源（USART1 外设、I2C 总线、SPI Flash 等）的互斥锁永远是**越靠近底层驱动越可靠**。放在 fputc 这种最底层比上层每个调用者自己写一套加锁逻辑简单、安全、省栈。

## 坑4：WK_UP 按键随机卡死 / EXTI 中断触发系统崩溃

- **现象**：不按按键跑 10 分钟都不崩，连续快速按 WK_UP 5~10 次后系统 100% 卡死（rtos_tick 停、LED 停、按键没反应）。
- **根因 1（直接死因）**：TaskSemHandle 优先级 = osPriorityHigh(40，**所有用户任务里最高**)，ISR 里 `xSemaphoreGiveFromISR + portYIELD_FROM_ISR` 会让 TaskSemHandle 在 ISR 返回瞬间立刻抢占正在执行的任务。如果当时 TaskLED(prio=24) 正在 `printf → fputc → osMutexAcquire` 的中间，刚拿到锁还没放 → TaskSemHandle 又调 `printf → fputc → osMutexAcquire` → 同一个锁在两个任务间来回 PendSV 切几十次 → 调度器状态机被高频切换拖崩。
- **根因 2（放大器）**：机械按键按下瞬间金属触点弹跳 5~20ms，期间会产生 ~20 次下降沿 → EXTI0 中断连发 20 次 → 每次都 Give 信号量 + 请求调度 → 相当于 1 次按键把系统强行 PendSV 切 20 次，本来能正常跑的系统在这个峰值下被冲垮。
- **解决**（3 条同时上，1 条都不能漏）：
  1. **TaskSemHandle 绝对不调 printf / HAL_UART_Transmit**。只做两件事：拿到信号量 → 防抖判断通过后写全局变量 `g_irq_cnt++` → 重新 `osSemaphoreAcquire(osWaitForever)` 阻塞。几微秒跑完立刻挂起，绝不抢 CPU。
  2. **防抖逻辑放到任务里，不放 ISR 里**。TaskSemHandle 里用 `now - last_tick < 50 → continue` 过滤抖动。ISR 里只调 Give，不要做任何 tick 读取 / 计数 / 判断——xTaskGetTickCountFromISR 虽然说是 FromISR 安全，但改 BASEPRI 的方式和 FreeRTOS 内核临界区叠加时偶尔会出 Bug，能不用就不用。
  3. **irq_cnt 的打印统一交给 TaskPrint 每 1 秒统计一次**。这种「按了几次按键」的信息完全没有实时性要求，不需要按键瞬间就打出来，一秒打一次统计值绰绰有余。
- **教训**：**ISR 和它唤醒的高优先级任务，代码永远遵循「最少操作原则」**——能写全局变量就不调函数，能不做判断就不做判断，能不打印就绝对不打印。任何耗时/阻塞/拿锁操作统统甩回最低优先级的任务慢慢做。

## D3 成果
- 4 大 IPC 全部接入并实测通过：
  - **Queue（消息队列）**：TaskLED 生产者 → TaskPrint 消费者，传递 LED 翻转 tick 和状态；队列深度 10 条；timeout=0 策略（队列满直接丢旧样本，LED 这种状态型信号的正确做法）。
  - **Mutex（互斥锁）**：下沉到 fputc 重定向函数里；全局裸 printf 线程安全；启动信息阶段自动跳过未初始化锁；串口日志完整无交错。
  - **EventGroup（事件组）**：BIT_KEY_DOWN=1 表示 WK_UP 按住；TaskLED osEventFlagsWait(timeout=0) 查位后选择 100ms 快闪 / 500ms 慢闪；松开立刻恢复。
  - **BinarySemaphore（二值信号量）**：EXTI0 下降沿中断（WK_UP 按下）→ Give 信号量 → TaskSemHandle 唤醒 + 50ms 防抖 → g_irq_cnt++，TaskPrint 每秒统计打印。
- 系统总任务数 `n_tasks=6`：TaskLED(24)、TaskPrint(16)、TaskKeyPoll(32)、TaskSemHandle(40)、Idle Task(0)、Timer Service Task(31)。
- 所有任务栈剩余（uxTaskGetStackHighWaterMark）≥ 48 words，无栈溢出风险。
- 压力测试：连续按 WK_UP 20+ 次、按住 3 秒不松手、系统跑 5 分钟 → 均无 HardFault / 卡死，调度器稳定运行。
- 按键消抖：`irq_cnt` 按一次只递增 1（按 10 次 irq_cnt 增长 8~10 次，偶尔漏 1 次是消抖窗口 50ms 刚好卡边界，正常现象）。




D4：WiFi + DHT11 + TCP 数据上报（2026-08-15 ~ 2026-08-16）
##坑1：Native FreeRTOS 与 CMSIS-RTOS V2 API 混用导致链接错误（L6218E）
现象：编译链接阶段报错 Error: L6218E: Undefined symbol xSemaphoreGiveFromISR (referred from main.o)，1 Error 0 Warning，无法生成 .axf。
根因：CubeMX 配置的 FreeRTOS 接口是 CMSIS_V2，工程只编译了 cmsis_os2.c，没有包含 Native FreeRTOS 的 semphr.c。但 main.c 的 HAL_GPIO_EXTI_Callback 中断回调里直接调了 Native API xSemaphoreGiveFromISR()，链接器在全工程找不到该符号的实现 → 报 undefined。
排查过程：
看错误信息 referred from main.o → 锁定问题在 main.c。
搜索 xSemaphoreGiveFromISR → 只在 EXTI 回调里出现一次。
对比 freertos.c 里其他任务用的都是 osSemaphoreRelease()（CMSIS_V2）→ 确认是 API 混用。
检查 Middlewares 目录 → 只有 CMSIS_RTOS_V2/cmsis_os2.c，没有 semphr.c → 确认 Native API 没被编译进去。
解决：把 main.c 中断回调里的 xSemaphoreGiveFromISR(g_irq_sem_handle, NULL) 替换为 CMSIS_V2 的 osSemaphoreRelease(g_irq_sem_handle)。CMSIS_V2 的 osSemaphoreRelease 内部会自动判断是否在 ISR 上下文，ISR 和任务里都能安全调用。
教训：一个工程中要么全用 Native FreeRTOS API（xTaskCreate、xSemaphoreGive、xQueueSend），要么全用 CMSIS_V2（osThreadNew、osSemaphoreRelease、osMessageQueuePut），绝对不能混用。判断标准：CubeMX → FREERTOS → Interface 选的是 CMSIS_V2 就全用 os* 前缀函数。抄网上代码时第一件事就是把 Native API 翻译成 CMSIS_V2 等价接口。
##坑2：AT 指令状态机重试逻辑只发一次，超时后不重发
现象：ESP8266 状态机进入 AT_TEST 后，串口只打印一次 [ESP8266] 进入 AT_TEST (retry=0)，之后即使超时 5 次，retry_cnt 永远停在 0，AT 指令不会重发，最终直接跳 RECONNECT。
根因：所有状态（AT_TEST / SET_STA / JOIN_WIFI / CONN_TCP）的重试逻辑都写成：

if (retry_cnt == 0) {        // 只在 retry_cnt==0 时发送
    ESP8266_SendRaw("AT\r\n", 4);
    retry_cnt++;              // 变成 1
}
// ... 等待信号量超时 ...
第一次进入时 retry_cnt=0 → 发送 AT → retry_cnt 变 1。超时后重新进入 case，retry_cnt 是 1 不是 0 → if 条件不成立 → AT 不重发，但 retry_cnt 也不自增 → 死锁在"不发指令也不计数"的状态。
排查过程：
串口日志只有一次 进入 AT_TEST (retry=0)，没有 retry=1/2/3 → 说明循环在跑但 retry_cnt 没增长。
读代码发现 retry_cnt++ 写在 if (retry_cnt == 0) 里面 → 只有第一次进 if 时才自增。
超时分支只改 state 不重发指令 → 确认重试和重发脱节。
解决：去掉 if (retry_cnt == 0) 判断，让每次循环都重发指令 + 计数自增：

case ESP_STATE_AT_TEST:
    ESP8266_ClearRxBuf();              // 每次清空
    ESP8266_SendRaw("AT\r\n", 4);      // 每次都发
    retry_cnt++;                        // 每次都计数
    // ... 轮询等待回复 ...
    if (retry_cnt >= 5) { state = ESP_STATE_RECONNECT; }
4 个状态（AT_TEST / SET_STA / JOIN_WIFI / CONN_TCP）全部做同样修改。
教训：重试逻辑的核心原则是**"重试计数"和"重发指令"必须绑定在同一个代码路径里**。推荐用"每次循环都发 + 计数判超时"的写法，比"进入时发一次 + 超时判重发"更不容易出错——后者要求超时分支里也写一遍发送逻辑，很容易漏。
##坑3：ESP8266 冷启动期间发 AT 指令石沉大海
现象：TaskESP8266 一启动就调 ESP8266_StartReceiveIT() + 发 AT\r\n，ESP8266 完全无反应，串口收不到任何回显和回复。偶尔能收到 busy p...（busy processing）。
根因：ESP8266-12F 模块冷启动（上电）需要 2-3 秒完成内部初始化流程：LDO 稳定 → CH340 初始化 → 射频校准 → 加载 AT 固件 → 串口就绪。在这期间收到的串口数据会被直接丢弃。代码在 osKernelStart() 后立刻运行 TaskESP8266，此时 ESP8266 可能才刚上电几百毫秒，AT 指令全丢了。
排查过程：
先怀疑接线问题 → 用 USB-TTL 单独接 ESP8266 手动发 AT → 回复 OK → 接线没问题。
怀疑波特率 → 单独测试 115200 能通 → 波特率没问题。
对比"单独测试 OK"和"STM32 上跑不通"的唯一区别 → STM32 上电后立刻发 AT，没有等待。
查 ESP8266 数据手册启动时序 → 冷启动需要 2-3 秒 → 确认是启动时序问题。
解决：TaskESP8266 开头加启动延时：

printf("[ESP8266] Waiting for module boot (3s)...\r\n");
osDelay(3000);   // 等 ESP8266 冷启动完成
ESP8266_StartReceiveIT();
教训：任何带独立 MCU 的外设模块（ESP8266、蓝牙模块、OLED、DHT11）上电后都有启动稳定期。上电后先延时再发指令是嵌入式系统的通用规则。常见启动时间：ESP8266=2-3s、DHT11=1-2s、OLED=100-200ms。不确定就查数据手册的 Power-on Timing 图。
##坑4：二值信号量遇回显换行提前唤醒，永远读不到 OK
现象：串口只打印 [ESP8266] RX: AT（回显），永远看不到 OK。每次都是等信号量超时后进入 RECONNECT，AT 测试永远过不了。
根因：USART2 中断回调里遇到 \n 就释放信号量。但 ESP8266 对 AT 指令的完整回复是两段：

回显：AT\r\n       ← 第 1 个 \n，信号量立刻释放！Task 被唤醒
回复：OK\r\n       ← 第 2 个 \n
TaskESP8266 被回显的 \n 唤醒后，调 ESP8266_GetLine() 读出 AT\n，判断没有 OK → 调 ESP8266_ClearRxBuf() 清空缓冲 → 后面到达的 OK\r\n 直接被清掉，永远读不到。
排查过程：
用 USB-TTL 单独抓 ESP8266 的 TX 脚 → 确认 ESP8266 确实回复了 AT\r\nOK\r\n 两段。
对比 STM32 串口日志 → 只有 RX: AT 没有 RX: OK → 怀疑 OK 被清掉了。
读代码发现 ESP8266_GetLine() 读完后立刻 esp_rx_wr_idx = 0（清空）→ 确认 OK 到达时缓冲已经被清。
读中断回调 → 发现 \n 就 Give 信号量 → 回显的 \n 也会触发 → 确认是"回显换行提前唤醒"问题。
解决：废弃"信号量唤醒 + 单次 GetLine"机制，改成主动轮询缓冲区：

case ESP_STATE_AT_TEST:
    ESP8266_ClearRxBuf();
    ESP8266_SendRaw("AT\r\n", 4);
    retry_cnt++;
    {
        uint32_t wait_start = osKernelGetTickCount();
        int found_ok = 0;
        while ((osKernelGetTickCount() - wait_start) < 1000)  // 1 秒总超时
        {
            osDelay(50);                                        // 每 50ms 轮询一次
            if (ESP8266_GetLine(line, sizeof(line)) > 0) {
                printf("[ESP8266] RX: %s", line);
                if (strstr(line, "OK") != NULL) {
                    found_ok = 1;
                    break;                                      // 真正找到 OK 才退出
                }
            }
        }
        if (found_ok) { /* 进入下一状态 */ }
    }
中断回调只负责往环形缓冲写字节，不再给信号量。任务用 50ms 间隔轮询 GetLine()，直到读到 OK 或超时。
教训：用信号量做串口协议解析时，必须明确**"一次完整交互 = 一次信号量"**。如果一次 AT 回复包含多个 \n（回显 + 状态 + OK），用二值信号量会在第一个 \n 就唤醒任务，后续数据被清掉。更稳妥的方案是：中断只写缓冲，任务用轮询 + 超时解析协议，代码更简单且不会漏事件。
##坑5：ESP8266 回复 busy processing，指令被拒绝
现象：AT 指令能收到回显，但 ESP8266 回复 busy p...（busy processing...），意思是"我正忙着处理上一条指令，别发了"。每隔几秒出现一次，AT 测试反复失败。
根因（双重原因）：
启动延时不够：3 秒有时不够 ESP8266-12F 完全启动，模块还在初始化就收到了 AT。
指令发送太密：重试逻辑改成"每次循环都重发"后，如果上一条 AT 还没处理完，下一条又来了 → ESP8266 回 busy。
排查过程：
串口看到 busy p... → 查 ESP8266 AT 指令手册 → busy processing 表示"模块忙，拒绝处理新指令"。
分析时间线：发 AT → 50ms 后轮询 → 没收到 OK → 下一轮循环立刻又发 AT → ESP8266 还在处理第一条 → busy。
对比启动阶段：模块刚上电时内部初始化也需要 CPU 时间 → 3 秒延时期间如果发 AT 也会 busy。
解决：
启动延时从 3 秒增加到 5 秒。
每次发完 AT 指令后先 osDelay(300) 等 ESP8266 回复完整，再开始轮询。
轮询中遇到 busy 就 osDelay(500) 多等一会儿，不急着重试：

if (strstr(line, "busy") != NULL) {
    osDelay(500);   // busy 了就多等 500ms
}
教训：AT 指令交互有节奏要求，不能"发完立刻循环重发"。正确节奏是：发指令 → 延时 200-500ms → 轮询读回复 → 判断结果。常见 AT 指令典型响应时间：AT/OK=100ms、AT+CWMODE=200ms、AT+CWJAP（WiFi连接）=3-10s、AT+CIPSTART（TCP连接）=1-5s。遇到 busy 就加等待，不要硬冲。
##坑6：误判 WIFI DISCONNECT 为连接失败，WiFi 连上就断
现象：ESP8266 发 AT+CWJAP 连接 WiFi，串口收到 WIFI DISCONNECT 后代码立刻判失败进入 RECONNECT。但 WiFi 路由器端能看到 ESP 确实连上了，只是马上又断开，反复循环。
根因：ESP8266 连接 WiFi 的完整且正常的流程是三步：

WIFI DISCONNECT    ← ① 先断开旧连接（正常中间状态！不是失败！）
WIFI CONNECTED      ← ② 正在连接 AP
WIFI GOT IP         ← ③ DHCP 获取 IP，连接成功
代码在第 ① 步就把 WIFI DISCONNECT 当失败信号判死了，直接进 RECONNECT 打断正在进行的连接。3 秒后重新发 CWJAP → 又断开旧连接 → 又判失败 → 无限循环。WiFi 路由器侧看到的就是"连上就断"。
排查过程：
串口日志显示收到 WIFI DISCONNECT 后立刻 WiFi join FAIL! → 怀疑误判。
用 USB-TTL 单独抓 ESP8266 TX → 手动发 CWJAP → 完整回复是 WIFI DISCONNECT → WIFI CONNECTED → WIFI GOT IP → OK 四段。
对比代码 → 发现 WIFI DISCONNECT 被放在失败判断条件里 → 确认是误杀正常中间状态。
WiFi 路由器端确认 ESP 能连上 → 排除密码/网络问题 → 纯代码逻辑 Bug。
解决：只把真正的失败信号判失败，去掉 WIFI DISCONNECT：

// 修改前（误杀中间状态）
if (strstr(line, "WIFI DISCONNECT") != NULL ||
    strstr(line, "ERROR") != NULL)

// 修改后（只判真正的失败）
if (strstr(line, "FAIL") != NULL ||
    strstr(line, "+CWJAP:1") != NULL ||   // 密码错误
    strstr(line, "+CWJAP:2") != NULL ||   // 找不到 AP
    strstr(line, "+CWJAP:3") != NULL ||   // 连接超时
    strstr(line, "ERROR") != NULL)
// WIFI DISCONNECT / WIFI CONNECTED 是中间状态，继续等 WIFI GOT IP
教训：写协议状态机前，一定先用串口手动发一遍指令，把完整的回复序列打印出来研究清楚，不要脑补"我觉得什么是失败"。ESP8266 AT 指令手册明确列出了每个指令的 URC（主动上报）消息，哪些是中间状态、哪些是失败信号要逐一确认。最稳妥的原则：只把"明确标注 FAIL/ERROR 的响应"判失败，其余都继续等超时。
##D4 成果
DHT11 温湿度传感器驱动完成：DWT 微秒延时 + 单总线时序 + 校验和验证，2 秒采样周期，mutex 保护数据读写。
ESP8266 AT 指令驱动完成：USART2 中断接收 + 环形缓冲 + 状态机（INIT→AT_TEST→SET_STA→JOIN_WIFI→CONN_TCP→WORKING→RECONNECT）。
WiFi 连接 + TCP 客户端建立 + 每 5 秒 JSON 上报全部验证通过。
系统总任务数 n_tasks=8：TaskLED、TaskPrint、TaskKeyPoll、TaskSemHandle、TaskDHT11、TaskESP8266、Idle Task、Timer Service Task。
NetAssist TCP Server 端稳定接收 {"temp":26,"humi":74,"tick":12150,"uptime":12} 格式 JSON。
编译 0 Error 0 Warning，连续运行稳定无 HardFault。




D5：ESP8266 建立 TCP 连接，验证网络通路（2026-08-16）
阶段归属：阶段2 网络通信 ·

---

## D5 成果

- D4→D5 工程复制 + CubeMX GENERATE CODE 流程跑通，工程名和内部引用全部正确更新为 `05_tcp_connect_verify`。
- ESP8266 能主动建立 TCP 连接到电脑 NetAssist（TCP Server，端口 1883）：
  - 串口日志能看到 `AT+CIPSTART=TCP,IP,1883 → CONNECT → OK` 完整链路。
  - NetAssist 侧看到 `Client xxx.xxx.xxx.xxx:xxxxx connected` 接入提示。
- TCP 双向通路验证通过：
  - **上行（ESP→电脑）**：NetAssist 每 5 秒收到一行 `{"temp":25,"humi":74,"tick":xxxxx,"uptime":xx}` 格式 JSON。
  - **下行（电脑→ESP）**：NetAssist 发送框输入字符串发送后，串口立刻打印 `[ESP8266] RX: xxxx`，证明 USART2 RX 中断→环形缓冲→轮询解析链路完整无丢。
- 3 项压力测试通过：
  - 连续跑 5 分钟：JSON 上报不间断，连接不丢。
  - 路由器断电 10 秒再插回：30 秒内自动 RESET→重连 WiFi→重连 TCP→恢复上报。
  - 关闭 NetAssist 再重新打开：ESP8266 检测到 CLOSED→RECONNECT→重新接入→恢复上报。
- 系统总任务数 n_tasks=8，所有任务栈剩余 ≥ 20 words，无 HardFault / 栈溢出风险。
- Checkpoint 全部通过，具备进入 D6（MQTT 协议封装 + 公网 Broker 联调）的条件。



D6：MQTT 协议层实现 + 公网 Broker 联调（2026-08-17）
阶段归属：阶段2 网络通信 · 纯协议层

---

## 坑1：MQTT 二进制报文用 printf %s 打印导致截断（调试陷阱）

- **现象**：MQTT CONNECT 发出后，串口打印 `[MQTT] RX (52 bytes):` 后面内容为空或乱码，看不到 CONNACK 的 `20 02 00 00`，以为 Broker 没回复。
- **根因**：MQTT 是二进制协议，报文里包含大量 `0x00` 字节（如 CONNACK 的 Byte2/Byte3 都是 `0x00`，PUBLISH 报文的主题长度字段 MSB 也是 `0x00`）。C 语言 `printf("%s", str)` 的语义是「从 str 地址开始一直打印，直到遇到第一个 `\0`（即 0x00 字节）停止」。所以 CONNACK 实际收到了 4 字节 `20 02 00 00`，但 `%s` 打印到 Byte2 的 `0x00` 就停止了，看起来像空内容。
- **排查过程**：
  1. 串口日志显示 `[MQTT] CONNACK success!` → 说明 `MQTT_IsConnackSuccess()` 函数已经匹配到了 CONNACK 标志，物理链路没问题。
  2. 但前面的调试行 `printf("[MQTT] RX: %s", line)` 只打印了字节数和空内容 → 把怀疑点缩小到「printf 格式」本身。
  3. 查 CONNACK 帧结构：`20 02 00 00` → Byte2 就是 `0x00` → 100% 确定是 `%s` 遇 0x00 截断。
  4. 同样的问题还会出现在 PUBLISH 下行报文：ESP8266 收到 Broker 转发的 PUBLISH 后，日志里显示 `+IPD,41:0'` 就结束了，后面的主题和 payload 全部被 `%s` 吃掉。
- **解决**：协议层调试日志统一用**十六进制打印**，彻底放弃 `%s` 打印二进制内容。最多打印前 32 字节（MQTT 关键信息全在前 32B）：
  ```c
  printf("[MQTT] RX (%d bytes):", rlen);
  for (int j = 0; j < rlen && j < 32; j++) {
      printf(" %02X", (uint8_t)line[j]);
  }
  printf("\r\n");
  ```
- **效果对比**：
  - 修改前（用 %s）：`[MQTT] RX (52 bytes):  `（空的）
  - 修改后（十六进制）：`[MQTT] RX (52 bytes): 41 54 2B 43 49 50 53 45 4E 44 3D 32 36 0D 0A 0D 0A 4F 4B 0D 0A 0D 0A 3E 0D 0A 52 65 63 76 20 32`
- **教训**：调试协议类代码时必须区分「ASCII 文本协议」和「二进制协议」：
  - ASCII 协议（如 AT 指令、HTTP 头）→ 可以用 `%s` 打印。
  - 二进制协议（MQTT、Modbus RTU、I2C/SPI 寄存器读值）→ **一律用 `%02X` 十六进制逐字节打印**。
  另外，判断数据是否到达永远看**字节长度**（rlen 参数），不要依赖「字符串能不能打印出来」。这次就是典型的「数据其实到了，只是你看不见」的调试陷阱，差点浪费时间去查 TCP 链路问题。

## 坑2：MQTT PUBLISH 的 payload 长度必须 100% 精确（AT+CIPSEND 血泪教训）

- **现象**：ESP8266 回复 `SEND OK`（TCP 层确认发送成功），但 MQTTX 订阅端就是看不到板子的 JSON。检查 Broker 连接状态一切正常（绿灯），MQTTX 自己 Publish 的测试消息能收到。
- **根因**：TCP 发送链路有三层长度字段，三者必须**逐字节完全相等**，任何一个不匹配都会导致 Broker 解析失败后静默丢弃报文。三层长度分别是：
  1. **AT+CIPSEND=<len>**：告诉 ESP8266 接下来要发多少字节的 TCP payload。
  2. **ESP8266_SendRaw(buf, len)**：实际通过串口发给 ESP8266 的字节数。
  3. **MQTT 报文固定头里的「剩余长度」**：由 `MQTT_BuildPublish` 根据主题长度和 payload 长度计算出来。
  这次踩坑的原因是：从 D5 拷贝代码时，`snprintf` 拼 JSON 那行末尾多了 `\r\n`（D5 里 NetAssist 文本显示用），导致 JSON 多了 2 字节。但 MQTT_BuildPublish 的 `payload_len` 和 AT+CIPSEND 的长度一致，所以 ESP8266 成功发出了——然而 Broker 收到的 MQTT payload 尾部多了 `\r\n`，和 `iot/dev001/sensor` 主题匹配的订阅者过滤器虽然通过，但实际 JSON 解析端可能被换行符影响，或者 Broker 认为 payload 格式异常就丢弃了。更根本的问题是**长度来源写死成数字**，没有统一用 `snprintf` 的返回值当唯一来源。
- **排查过程**：
  1. 先确认 ESP8266 侧没问题：串口有 `SEND OK` → 到 ESP8266 的 UART→WiFi→TCP 链路全通。
  2. 再确认 Broker 侧没问题：MQTTX 手动 Publish `iot/dev001/sensor` → 自己能收到 → Broker 和订阅过滤器全通。
  3. 剩下唯一可能是板子发出的**报文内容有问题**。
  4. 打印 `MQTT_BuildPublish` 的返回值（`mqtt_len`）和 AT+CIPSEND 的长度：两者都是 69，一致。
  5. 再打印 JSON 的实际字节数（`json_len`）和 JSON 字符串内容：发现末尾有 `\r\n` → 确认是多了换行符。
  6. 查 MQTT 3.1.1 规范：PUBLISH payload 是「不透明字节序列」，理论上 `\r\n` 不会导致丢弃，但和文档里约定的 JSON 格式不一致。
  7. 去掉 `\r\n` 后重新烧录 → MQTTX 立刻收到消息 → 根因确认。
- **解决（3 条规则，以后所有带 CIPSEND 的代码都严格遵守）**：
  - **规则 1：JSON 尾部不加 `\r\n`**。MQTT 协议靠帧头的「剩余长度」字段做边界，不需要额外分隔符。`\r\n` 是文本协议（TCP 调试、串口日志）的习惯，二进制协议里要彻底改掉。
  - **规则 2：payload_len 的唯一来源是 `snprintf` 返回值**，绝对不能写死 `48`、`67` 这种数字：
    ```c
    int json_len = snprintf(json_buf, sizeof(json_buf),
                            "{\"temp\":%u,\"humi\":%u,\"tick\":%lu,\"uptime\":%lu}",
                            ...);
    ```
    `json_len` = 实际写入 json_buf 的字节数（不含 `\0` 终止符），是 100% 精确的值。
  - **规则 3：mqtt_len 的唯一来源是 `MQTT_BuildPublish` 返回值**，然后用这个 mqtt_len 同时填 AT+CIPSEND 和 ESP8266_SendRaw：
    ```c
    int mqtt_len = MQTT_BuildPublish(mqtt_buf, sizeof(mqtt_buf),
                                     MQTT_TOPIC_SENSOR,
                                     (uint8_t *)json_buf, json_len);
    /* 两个地方都用 mqtt_len */
    snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", mqtt_len);
    ESP8266_SendRaw(at_cmd, strlen(at_cmd));
    osDelay(200);
    ESP8266_SendRaw((char *)mqtt_buf, (uint16_t)mqtt_len);
    ```
- **教训**：这个坑是 D4「AT+CIPSEND 长度不匹配导致 ESP8266 断连」在 D6 的重演，本质上是**换了协议、没换原则**。以后只要涉及 `AT+CIPSEND`，不管 TCP payload 里装的是裸字符串、JSON 还是 MQTT 二进制报文，都必须遵守「一个长度来源 + 三处完全相等」的铁律。这个原则值得记在本上，后面 D9 OTA 用 TCP 传固件时还要用 100 次。

## 坑3：ESP8266 +IPD 前缀与 MQTT 二进制数据混合解析

- **现象**：Broker 回复的 CONNACK 明明是 4 字节（`20 02 00 00`），但 ESP8266 转发给 STM32 的环形缓冲里，前面混入了大量 AT 回显：`AT+CIPSEND=26\r\nOK\r\n>\r\nRecv 26 bytes\r\nSEND OK\r\n+IPD,4: `。调试日志打印出来一大串 ASCII 字符，根本不知道哪几个字节才是真正的 MQTT 报文。第一次写的 `MQTT_IsConnackSuccess` 直接判断 `data[0]==0x20 && data[1]==0x02`，完全匹配不上，一直以为 CONNACK 没收到。
- **根因**：ESP8266 非透传模式（`AT+CIPMODE=0`，默认模式）下，所有数据收发都要包一层 AT 指令外壳：
  - STM32 → ESP8266 → Broker：**不能直接发 MQTT 字节**，必须先包一层 `AT+CIPSEND=<len>\r\n` + `>` 提示符 + 实际 MQTT 字节。这层 AT 指令和提示符，ESP8266 会**原样回显**给 STM32 的 TX 脚。
  - Broker → ESP8266 → STM32：ESP8266 收到 TCP 数据后，不直接转发裸数据，而是包一层 `+IPD,<len>:` 前缀再转发给 STM32。意思是「Internet Protocol Data Received，收到 <len> 字节了，后面跟着实际内容」。
  所以环形缓冲里的内容是「发送回显 + +IPD 前缀 + 实际 MQTT 字节」**混合在一起的字节流**。如果用「固定位置读 MQTT 标志（看 data[0]）」，100% 匹配不到。
- **排查过程**：
  1. 先看串口日志的 `RX (52 bytes)` 的十六进制内容：前 48 字节是 ASCII（AT+CIPSEND、OK、>、Recv、SEND OK、+IPD,4:），最后 4 字节是 `20 02 00 00`。
  2. 原来写的 `MQTT_IsConnackSuccess` 只检查 `data[0]` 和 `data[1]` → 因为前面有 48 字节前缀，所以 `data[0]` 是 `'A'`（0x41），不是 `0x20` → 永远匹配失败。
  3. 改成「搜索模式」，用 for 循环遍历整个缓冲区查找 `0x20 0x02` → 立刻匹配成功。
- **解决**：`MQTT_IsConnackSuccess` 以及后续所有解析函数，统一用「搜索模式」而非「固定位置模式」：
  ```c
  int MQTT_IsConnackSuccess(const uint8_t *data, int len)
  {
      int i;
      if (data == NULL || len < 4) return 0;

      /* 遍历整个缓冲区，搜索 CONNACK 的固定头 0x20 0x02 */
      for (i = 0; i <= (len - 4); i++) {
          if (data[i] == 0x20 && data[i + 1] == 0x02) {
              /* 找到后再检查返回码（i+3 位置）是不是 0x00 */
              if (data[i + 3] == 0x00) {
                  return 1;   /* CONNACK 成功 */
              } else {
                  return 0;   /* CONNACK 失败，返回码非 0 */
              }
          }
      }
      return 0;   /* 没找到 CONNACK 标志 */
  }
  ```
  搜索模式能正确跳过 AT 回显、`SEND OK`、`+IPD,len:` 等所有前缀，找到真正的 MQTT 帧头。
- **教训**：ESP8266 非透传模式下，RX 环形缓冲的数据结构永远是「AT 回显垃圾 + +IPD 包裹头 + 实际 TCP 数据」。解析时必须牢牢记住三个原则：
  1. **永远不要用 `data[0]` 直接判断协议类型**。真正的数据可能在缓冲区的任何偏移位置。
  2. **用「搜索帧头标志」的方式定位协议报文的起始位置**。MQTT 的帧头标志就是「报文类型字节（0x10/0x20/0x30/0x82/0xC0/0xD0）+ 后面的剩余长度字节」。
  3. **+IPD 是可靠边界**。虽然这次用搜索模式绕过了 +IPD 解析，但更严谨的做法是先解析 +IPD 里的 `<len>`，再从 `:` 后面开始读 `<len>` 字节，那才是真正的 TCP payload。D7 做下行数据解析时要补这层逻辑，不能一直靠「搜索帧头」（万一 payload 里恰好也出现了 `0x20 0x02` 就会误匹配）。D6 阶段用搜索模式是符合最小可行原则的简化方案。

## D6 成果

- MQTT 3.1.1 协议层完全手写实现，**不依赖任何第三方 MQTT 库**，5 个核心函数：
  - `MQTT_BuildConnect()`：拼装 CONNECT 报文（固定头 2 字节 + 可变头 10 字节 + Client ID 载荷）。
  - `MQTT_BuildPublish()`：拼装 PUBLISH QoS0 报文（固定头 + 主题长度/主题 + JSON payload）。
  - `MQTT_BuildSubscribe()`：拼装 SUBSCRIBE 报文（固定头 0x82 + 报文 ID + 主题/QoS）。
  - `MQTT_BuildPingreq()`：拼装 PINGREQ 心跳（固定 2 字节 0xC0 0x00）。
  - `MQTT_IsConnackSuccess()`：搜索模式解析 CONNACK，跳过 AT 回显和 +IPD 前缀。
- ESP8266 状态机扩展为 10 态：INIT → AT_TEST → SET_STA → JOIN_WIFI → CONN_TCP → MQTT_CONNECT → MQTT_SUBSCRIBE → MQTT_WORKING → RECONNECT（+ default 安全网）。
- 30 秒**独立心跳定时器**：PINGREQ 定时器和 PUBLISH 5 秒定时器完全分离，即使业务数据因为互斥锁/AT 指令延迟卡住，心跳也能准时发出，不会被 Broker 踢下线。
- 公网 Broker（broker.emqx.io:1883）联调全部通过：
  - **上行（板子→云端）**：MQTTX 订阅端每 5 秒收到一条 `iot/dev001/sensor` 的 JSON，字段齐全（temp/humi/tick/uptime），数值和 DHT11 实测一致，节奏稳定。
  - **下行（云端→板子）**：MQTTX 发送到 `iot/dev001/ota` 的消息，板子串口日志显示 `+IPD,41:` + MQTT PUBLISH 二进制帧，证明下行链路物理通路 100% 打通。
- **2 分钟稳定性测试通过**：连续运行 247 秒（> 120 秒），PUBLISH 不间断，PINGREQ 准时发出 8 次，期间无 `CLOSED` / `WIFI DISCONNECT` / HardFault，调度器正常、任务栈剩余 ≥ 20 words。
- D6 文档核心 Checkpoint 全部满足：**MQTTX 订阅到设备消息、JSON 正确、持续 2 分钟不断线**。
- 系统总任务数 `n_tasks=9`：TaskLED、TaskPrint、TaskKeyPoll、TaskSemHandle、TaskDHT11、TaskESP8266、Idle Task、Timer Service Task，以及 CubeMX 重生成后新增的 1 个系统任务。
- 具备进入 D7（掉线自动重连 + MQTT 互斥锁加固 + 下行 PUBLISH 解析）的条件。





D7：掉线自动重连 + MQTT 互斥锁加固 + 下行 PUBLISH 解析（2026-08-18）
阶段归属：阶段2 网络通信 · 生产级加固

---

## 坑1：用 retry_cnt 判断「首次连接 vs 重连」导致 CleanSession 逻辑全错

- **现象**：D7 要求首次连接用 CleanSession=1（清空旧会话），重连用 CleanSession=0（恢复会话补发离线消息）。最初用 `retry_cnt == 0` 判断是否首次连接，结果两种场景都出错：
  - 首次连接 CONNACK 超时重试时，retry_cnt 变成 1 → 误判成"重连" → 用 CleanSession=0，但此时根本还没连上过，没有会话可恢复。
  - 真正掉线重连时，状态机走 RECONNECT → INIT，INIT 里 `retry_cnt = 0` → 又变回首次 → 用 CleanSession=1，Broker 清空了掉线期间缓存的消息，恢复会话完全失效。
- **根因**：`retry_cnt` 是 MQTT_CONNECT 状态内的重试计数器，它在两个地方被重置为 0：
  1. 从 CONN_TCP 成功进来时（`retry_cnt = 0`）
  2. INIT 状态里（`retry_cnt = 0`）
  所以 retry_cnt 既无法正确识别"首次连接的重试"（场景A误判），也无法在真正重连时保持 CleanSession=0（场景B被重置）。retry_cnt 的语义是"当前状态重试了几次"，和"板子有没有成功连过 Broker"是两个完全不同的概念。
- **排查过程**：
  1. 串口日志显示首次连接 CONNACK 超时后，第二次重试打印了 `Reconnect (CleanSession=0)` → 明显错误，还没连上过不该走 Resume。
  2. 拔 WiFi 触发掉线重连，串口日志打印 `First connect (CleanSession=1)` → 真正的重连反而用了首次连接的逻辑。
  3. 读代码发现 INIT 状态里 `retry_cnt = 0` → 真正重连经过 INIT 后 retry_cnt 被重置 → 误判成首次。
  4. 再读代码发现 CONNACK 超时重试时 retry_cnt++ → 变成 ≥1 → 误判成重连。
  5. 确认 retry_cnt 的生命周期和"是否首次连接"完全不对应。
- **解决**：新增独立的 `mqtt_first_connect` static 变量，只在 CONNACK 成功那一刻翻成 0，RECONNECT 和 INIT 都不碰它：
  ```c
  static uint8_t mqtt_first_connect = 1;   /* 1=首次连接 0=重连 */

  /* MQTT_CONNECT 状态里 */
  if (mqtt_first_connect) {
      pkt_len = MQTT_BuildConnect(...);         /* 首次：CleanSession=1 */
  } else {
      pkt_len = MQTT_BuildConnect_Resume(...);  /* 重连：CleanSession=0 */
  }

  /* CONNACK 成功后 */
  if (found_ack) {
      ...
      mqtt_first_connect = 0;   /* 标记已连过，下次重连走 Resume */
  }
  ```
- **教训**：状态机里的标志变量必须和它代表的语义严格对应。`retry_cnt` 代表"当前状态重试次数"，会在多个地方被重置；`mqtt_first_connect` 代表"板子生命周期内是否成功连过 Broker"，只应该在一处被翻转。用错变量表达语义是状态机最常见的逻辑 bug，排查时一定要画一遍状态流转图，确认变量在每个状态的取值是否符合预期。

## 坑2：TaskLED 和 MQTT 命令冲突控制 LED0（多控制源写同一 GPIO）

- **现象**：MQTTX 发 `{"cmd":"led_on"}` 后，串口打印了 `[MQTT] CMD: LED ON`，但 LED0 肉眼看不出任何变化——一直在 500ms 频率闪烁，无法观察到"常亮"效果。发 `led_off` 也一样看不出效果。
- **根因**：TaskLED 任务每 500ms 执行 `HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin)` 翻转 LED0，而 MQTT 命令用 `HAL_GPIO_WritePin` 设置 LED0。两个控制源同时写同一个 GPIO，互相打架：
  ```
  MQTT 命令 WritePin(RESET) → 灯亮了
  → 最多 500ms 后 TaskLED 循环 Toggle → 灯又灭了
  → 再过 500ms TaskLED 又 Toggle → 又亮了
  ```
  MQTT 命令的 WritePin 效果被 TaskLED 的 Toggle 立刻覆盖，肉眼看到的就是一直在 500ms 闪烁。
- **排查过程**：
  1. 串口日志确认 `[MQTT] CMD: LED ON` 已打印 → 命令解析和执行逻辑没问题。
  2. 观察 RTOS STATS 日志：`led0=1` 一直不变 → 但实际 LED 在闪 → 说明有另一个控制源在不断翻转 LED0。
  3. 全局搜索 `LED0` → 发现 TaskLED 里 `HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin)` 每 500ms 执行一次。
  4. 确认是两个控制源冲突：TaskLED Toggle + MQTT WritePin 打架。
- **解决**：TaskLED 不再控制 LED0，只保留 LED1 做心跳指示。LED0 完全交给 MQTT 命令控制：
  ```c
  /* app_task.c TaskLED 里 */
  // D7：LED0 给 MQTT 远程控制专用，TaskLED 只 Toggle LED1 做心跳
  // HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
  HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
  ```
- **教训**：遇到"状态回不来/灯不灭/偶发常亮"这类问题，优先做"写 GPIO 源头清单"——全局搜索所有写同一 PIN 的模块，列出来再决定收敛到单一控制源。多个模块并发写同一个 GPIO 是嵌入式最常见的逻辑冲突，解决原则是"控制权收敛"：要么删除/禁用其他模块对同一 PIN 的写操作，要么抽象一个统一的 led_manager 由各模块提交意图，最终只有一个地方落 GPIO。

## 坑3：MQTT_FindPublishFrame 被 +IPD 前缀里的 ASCII 数字误判为 PUBLISH 帧头

- **现象**：MQTTX 往 `iot/dev001/ota` 发 `{"cmd":"led_on"}`，板子串口打印 `[DEBUG] FindPublishFrame: offset=5` 和 `[DEBUG] ParsePublish: ret=-4, topic_len=0`，解析失败，命令不执行。
- **根因**：ESP8266 非透传模式下，下行数据格式为 `+IPD,34:0x30 0x20 0x00 0x0E ...`。`MQTT_FindPublishFrame` 的实现是遍历整个缓冲区，找高 4 位 = 0011（即 `0x30~0x3F`）的字节。但 `+IPD,34:` 里的 `'3'`（0x33）恰好满足 `0x33 & 0xF0 == 0x30`，被误判成 PUBLISH 帧头！
  - 真正的 PUBLISH 帧头 `0x30` 在 offset=8（`+IPD,34:` 之后）
  - 但函数在 offset=5 先匹配到了 `'3'`（0x33）
  - 从 offset=5 开始解析：data[0]=0x33('3')，data[1]=0x34('4') → remaining_len=52，topic_len=(0x3A<<8)|0x30=14896 → 远超 remaining_len → 返回 -4
  - MQTT 数据本身是完美的，只是帧头搜索被 AT 前缀里的 ASCII 数字骗了。
- **排查过程**：
  1. 看串口十六进制日志：`2B 49 50 44 2C 33 34 3A 30 20 00 0E ...` → 翻译成 ASCII 是 `+IPD,34:` + MQTT 数据。
  2. `[DEBUG] FindPublishFrame: offset=5` → offset=5 对应的字节是 `0x33`（'3'）→ 确认被 ASCII 数字误判。
  3. `[DEBUG] ParsePublish: ret=-4` → topic_len 计算出 14896，远超 remaining_len → 解析函数正确地拒绝了错误数据。
  4. 读 `MQTT_FindPublishFrame` 代码：`(data[i] & 0xF0) == 0x30` → `0x33 & 0xF0 = 0x30` → 确认匹配逻辑无法区分 ASCII 数字 '3' 和真正的 PUBLISH 帧头 0x30。
- **解决**：废弃帧头搜索方案，改成"找 `+IPD,` 前缀直接跳到纯 MQTT 数据"：
  ```c
  /* 找 +IPD, 前缀，跳过它定位到纯 MQTT 数据 */
  char *ipd_tag = strstr(line, "+IPD,");
  if (ipd_tag != NULL) {
      char *colon = strchr(ipd_tag + 5, ':');
      if (colon != NULL) {
          int mqtt_start = (int)((colon + 1) - line);
          int mqtt_len   = rlen - mqtt_start;
          /* 直接从 MQTT 数据开始解析，不用搜索帧头 */
          int ret = MQTT_ParsePublish(
              (uint8_t *)&line[mqtt_start],
              mqtt_len,
              &topic, &topic_len,
              &payload, &payload_len);
      }
  }
  ```
  `+IPD,` 是 ESP8266 的可靠边界标记，`:` 后面就是 100% 纯净的 TCP payload（即 MQTT 报文），不需要再搜索帧头。
- **教训**：D6 坑3 里用"搜索帧头"方式解析 CONNACK 时就埋了这个隐患——当时 CONNACK 只有 4 字节（`20 02 00 00`），AT 前缀里不太可能出现 `0x20 0x02` 的组合，所以搜索模式能工作。但 D7 解析下行 PUBLISH 时，AT 前缀 `+IPD,34:` 里的 ASCII 数字 `0x33`('3') 恰好满足 PUBLISH 帧头条件 `0x3?`，搜索模式就失效了。**D6 坑3 的教训里已经预言了这个问题**——"万一 payload 里恰好也出现了 0x20 0x02 就会误匹配"，实际比预言更糟，AT 前缀里就有误匹配。正确做法是利用 `+IPD` 这个可靠边界，从 `:` 后面直接取纯 MQTT 数据，彻底避免和 AT 前缀里的字节冲突。

## 坑4：多任务并发调用 AT+CIPSEND 导致 ESP8266 数据错乱（互斥锁加固）

- **现象**：D6 只有一个任务发 AT 指令，不存在并发问题。D7 新增"收到 report 命令立即上报"功能后，MQTT_WORKING 状态里同时存在 3 个会触发 AT+CIPSEND 的路径：5 秒定时 PUBLISH、30 秒定时 PINGREQ、report 命令触发的立即 PUBLISH。如果两个路径同时到点，两个 AT+CIPSEND 交叉执行，ESP8266 会收到乱序数据直接死掉。
- **根因**：`AT+CIPSEND=<len>` + `osDelay(200)` + `ESP8266_SendRaw(mqtt_buf)` 是一个不可分割的原子操作——ESP8266 要求 CIPSEND 后必须紧接着发数据，中间不能插别的指令。如果没有互斥锁保护，两个发送路径交叉执行时，ESP8266 可能收到：
  ```
  AT+CIPSEND=67    ← PUBLISH 的 CIPSEND
  AT+CIPSEND=2     ← PINGREQ 的 CIPSEND 插进来了！
  <PUBLISH 数据>   ← ESP8266 以为是 PINGREQ 的数据，只收 2 字节
  <PINGREQ 数据>
  <剩余 PUBLISH 数据>  ← ESP8266 当成新指令处理 → 报错/死机
  ```
- **排查过程**：
  1. D6 代码直接拷贝到 D7，没有加锁 → 快速连发 report 命令时偶发死机。
  2. 分析 MQTT_WORKING 状态的 3 个发送路径：5 秒 PUBLISH、30 秒 PINGREQ、report 立即 PUBLISH。
  3. 确认 3 个路径都调 `AT+CIPSEND + SendRaw`，且中间有 `osDelay(200)` 让出 CPU → 其他路径可以插入。
  4. 确认需要互斥锁保护"AT+CIPSEND + osDelay + SendRaw"三件套不被打断。
- **解决**：4 处 AT+CIPSEND 全部加 `g_mqtt_mutex_handle` 互斥锁：
  1. MQTT_CONNECT 状态的 CONNECT 发送
  2. MQTT_SUBSCRIBE 状态的 SUBSCRIBE 发送
  3. MQTT_PublishSensor 函数内部的 PUBLISH 发送（锁放函数内，调用者不用管锁）
  4. MQTT_WORKING 状态的 PINGREQ 发送
  ```c
  if (g_mqtt_mutex_handle != NULL) {
      osMutexAcquire(g_mqtt_mutex_handle, osWaitForever);
  }
  snprintf(at_cmd, sizeof(at_cmd), "AT+CIPSEND=%d\r\n", len);
  ESP8266_SendRaw(at_cmd, strlen(at_cmd));
  osDelay(200);
  ESP8266_SendRaw((char *)buf, (uint16_t)len);
  if (g_mqtt_mutex_handle != NULL) {
      osMutexRelease(g_mqtt_mutex_handle);
  }
  ```
- **教训**：嵌入式系统里"AT 指令 + 数据"的组合操作天然是临界区，必须用互斥锁保护。判断标准很简单：如果两段代码会操作同一个硬件外设（这里是 USART2 → ESP8266），且中间有 `osDelay` 让出 CPU，就必须加锁。锁的粒度要包住整个"指令 + 数据"序列，不能只锁指令不锁数据。PUBLISH 的锁放在 `MQTT_PublishSensor` 函数内部是更好的设计——调用者不用关心锁，封装性更强。

## D7 成果

- **掉线自动重连 + CleanSession 会话恢复**：首次连接用 CleanSession=1 清空旧会话，掉线重连用 CleanSession=0 恢复会话，Broker 自动补发掉线期间的离线消息。用独立的 `mqtt_first_connect` 变量区分首次/重连，避免 retry_cnt 误判。
- **MQTT 互斥锁加固**：新增 `g_mqtt_mutex_handle` 互斥锁，4 处 AT+CIPSEND 发送全部加锁保护，防止 5 秒 PUBLISH / 30 秒 PINGREQ / report 立即上报 三个并发路径交叉执行导致 ESP8266 数据错乱。快速连发 5 条 report 命令不死机。
- **下行 PUBLISH 解析 + 远程控制**：实现 `MQTT_FindPublishFrame` + `MQTT_ParsePublish` 两个解析函数，能从 `+IPD,len:` 前缀中提取纯 MQTT 数据，解析出主题和 payload。支持 3 个远程命令：
  - `{"cmd":"led_on"}` → LED0 常亮
  - `{"cmd":"led_off"}` → LED0 常灭
  - `{"cmd":"report"}` → 立即上报一次温湿度 JSON，不用等 5 秒
- **LED 控制权收敛**：TaskLED 不再 Toggle LED0，只保留 LED1 做心跳指示。LED0 完全交给 MQTT 命令控制，消除多控制源冲突。
- **+IPD 前缀解析方案**：废弃 D6 的"搜索帧头"方案（被 AT 前缀里 ASCII 数字 0x33 误判），改成"找 `+IPD,` 前缀直接跳到纯 MQTT 数据"，彻底避免帧头误匹配问题。
- **协议层新增 3 个函数**：`MQTT_BuildConnect_Resume`（CleanSession=0）、`MQTT_ParsePublish`（解析 PUBLISH 报文）、`MQTT_FindPublishFrame`（搜索 PUBLISH 帧头，保留但不再作为主解析方案）。
- **所有 D7 验证项通过**：首次连接 CleanSession=1、掉线重连 CleanSession=0、LED 远程开关、report 立即上报、互斥锁并发不死机。
- 具备进入 D8（JSON 解析库接入 + 多命令扩展）的条件。





D8：Bootloader 基础框架开发 + Flash 三分支状态机（2026-08-19）
阶段归属：阶段3 OTA系统架构 · Bootloader层


## 坑1：验证跳转APP时死机，D7 工程 IROM1 起始地址没手动改成 0x08008000

- **现象**：Bootloader 烧录进去，D7（APP）也烧录进去，按 Reset 后串口打印了 `[BOOT] APP valid, jumping to 0x08008000...`，然后板子立刻死机（LED0不亮，串口无任何输出，看门狗不复位）。Bootloader 本身工作正常（KEY0进IAP、APP无效慢闪都正常），问题只出在"跳转APP"这一步。
- **根因**：CubeMX 新建 STM32F103ZET6 工程时，IROM1 的默认配置是 `Start=0x08000000，Size=0x00080000`（整个512KB Flash 都给 APP）。现在 Bootloader 占了前 32KB（0x08000000~0x08007FFF），APP 必须从 0x08008000 开始，**但 Keil/CubeMX 不会知道你有一个 Bootloader，它不会自动帮你把 APP 的链接地址往后挪 32KB**。如果 D7 的 IROM1 还是默认值 0x08000000，会发生两个致命问题：
  1. **向量表放错位置**：APP 的向量表（MSP、Reset_Handler、中断服务函数地址表）被链接器放在了 0x08000000，而不是 0x08008000。Bootloader 的 JumpToApp 函数读 0x08008000 处的栈指针时，读的其实是 APP 向量表中间的某个字段（不是初始栈顶），值是非法地址，`__set_MSP` 设置了一个非法栈后，紧接着调用 APP 的 Reset_Handler 会立刻触发 HardFault。
  2. **代码地址和跳转地址不匹配**：即使向量表侥幸对齐，APP里所有函数调用、全局变量引用都是基于 0x08000000 基址算出来的，而实际 APP 被烧录在 0x08008000，函数地址全部错位，CPU 跳到非法地址必死。
- **排查过程**：
  1. Bootloader 串口能正常打印 Banner，说明 Bootloader 的 USART1、GPIO、printf、时钟配置都没问题，故障点就在 JumpToApp 之后。
  2. 串口成功打印了 `APP valid`，说明 `IsAppValid()` 函数通过了——`*(volatile uint32_t*)0x08008000` 读出来的高12位是 0x200。这里其实**已经埋下了误导**：如果 D7 的 IROM1 还是 0x08000000，那 APP 的向量表在 0x08000000，0x08008000 处是 APP 代码段中间某个字（恰好高12位也是 0x200，侥幸通过检查）。
  3. 打开 D7 工程魔术棒 → Target → 看 IROM1：果然还是默认的 `Start=0x08000000，Size=0x00080000`，完全没改。
  4. 确认根因：APP 的链接地址和实际烧录位置不匹配，向量表和函数调用地址全部错位。
- **解决**：打开 D7 工程（07_mqtt_reconnect）手动改 IROM1，**改完必须 Rebuild（不是 Build）**：
  ```
  Options for Target → Target 选项卡 → Read/Only Memory Areas (ROM)
      IROM1: Start = 0x08008000   （APP 从第 32KB 偏移开始，跳过 Bootloader 区）
             Size  = 0x00074000   （464KB，0x80000 - 0x8000 = 0x74000，剩余全给 APP）
  ```
  Rebuild 完后，再烧录 D7 的 hex（方法1：Keil 直接 Load；方法2：ST-Link Utility 打开 hex，确认起始地址是 0x08008000 再 Program）。按 Reset 后，Bootloader 打印完 `jumping to 0x08008000...` 后，隔 2~3 秒会继续打印 D7 的 APP 启动日志 `========== FreeRTOS APP v1.0 ==========`，跳转成功。
- **教训**：这是 Bootloader + APP 双工程结构里最经典、最容易踩的坑，90% 的初学者第一次做都会在这里死机几小时。核心意识是：**Keil 的 IROM1 配置决定了"链接器认为代码应该放在哪个地址"，它和"你实际把 hex 烧录到 Flash 的哪个地址"必须 100% 一致，差一个字节都不行。** CubeMX 默认的 IROM1 是按"整片 Flash 只有一个程序"设计的，只要你引入了 Bootloader 双工程架构，所有 APP 工程（D6、D7、后续所有阶段）的 IROM1 都要手动改成 0x08008000/0x00074000，**Keil 永远不会自动帮你改**。每次从 Dx 复制到 Dx+1 新建工程时，第一件事就是改 IROM1，不要等死机了再回头找。

## D8 成果

- **独立 Bootloader 工程搭建完成**：`00_bootloader` 工程基于 HAL 库、无 FreeRTOS（纯裸机状态机），Keil IROM1 配置为 `0x08000000 / 0x00008000`（仅占前 32KB），与 APP 区完全分离，互不覆盖。
- **Flash 分区方案落地**：三块区域划分清晰，Bootloader（32KB:0x08000000-0x08007FFF）→ APP（464KB:0x08008000-0x0807EFFF）→ OTA 参数区（4KB:0x0807F000-0x08080000），为后续 D10（串口IAP）和 D12（WiFi OTA）预留了正确的地址边界。
- **flash_if.c 四个 Flash 操作函数实现**：`FLASH_ErasePage`（按地址擦2KB页，擦前解锁擦后上锁）、`FLASH_WriteWord`（按4字节写）、`FLASH_WriteBuf`（按缓冲循环写，4字节对齐）、`FLASH_ReadWord`（直接指针读，无需解锁）。封装统一，D10/D12 写新固件时直接调用，不用重复写 Flash 解锁/上锁模板代码。
- **jump_to_app.c 跳转框架实现**：`IsAppValid()` 校验 0x08008000 处的初始栈指针（高12位必须等于 0x200，即指向 RAM），避免跳到空白 Flash 区；`JumpToApp()` 严格按 6 步跳转（读MSP→读ResetHandler→关中断→清NVIC挂起标志→设VTOR→设MSP→跳转），保证 APP 向量表、中断状态、栈指针三者与 Bootloader 完全解耦，跳转后无残留中断触发。
- **三分支状态机验证通过**：
  - **分支1（OTA_FLAG）**：读 0x0807F000 匹配 0xA5A5A5A5 → 清除 FLAG，等 KEY0 进 Serial IAP。OTA_FLAG 写入逻辑由 D12 APP 端完成，D8 预留处理分支正确。
  - **分支2（KEY0）**：上电后 100ms 内检测 KEY0 为低 → 打印 `KEY0 pressed! Serial IAP mode`，双 LED 每 200ms 快闪，标识进入 IAP 模式（D10 在此基础上加 Ymodem 接收）。
  - **分支3（APP有效）**：APP 校验通过 → 打印 `APP valid, jumping to 0x08008000...`，200ms 后执行 JumpToApp，D7 FreeRTOS APP 正常启动，温湿度上报、MQTT 通信、LED 远程控制功能全部不受影响。
  - **分支4（APP无效）**：APP 区全 0xFF（未烧录或写崩）→ 打印 `APP NOT valid!`，LED0 每 500ms 慢闪，提示用户按 KEY0 + Reset 进入 IAP 模式烧录固件。
- **LED 状态编码体系建立**：通过 LED0/LED1 的组合快速判断当前模式，不用接串口也能排障——LED0亮+LED1灭=Bootloader运行中；LED0慢闪(500ms)=APP无效等固件；LED0+LED1快闪(200ms)=IAP模式；LED0灭+LED1慢闪(500ms)=已跳APP，TaskLED心跳。
- **Bootloader printf 基础设施搭建**：`fputc` 重定向到 USART1 + Keil MicroLIB 勾选，启动时打印 Banner 展示 Flash 分区布局、三分支决策日志，后续调试不用示波器直接看串口即可。
- **所有 D8 验证项通过**：编译 0 Error 0 Warning、APP无效慢闪、KEY0进IAP双闪、APP有效跳转D7正常运行、Bootloader烧APP/D7烧Bootloader互不覆盖。具备进入 D9（Ymodem协议理论 + 文件传输帧结构）的条件。



D9：参数区(flash_param_t) + CRC32校验 + Bootloader/APP共享读写（2026-08-20）
阶段归属：阶段3 OTA系统架构 · 参数区层

---

## 坑1：_Static_assert 在 ARMCC V5 不支持（编译错误 #79/#260-D）

- **现象**：`flash_partition.h(88): error: #79: expected a type specifier` + `#260-D: explicit type is missing ("int" assumed)`，报错指向 `_Static_assert(sizeof(flash_param_t) == 4096, ...)` 这行，Bootloader 和 APP 两个工程都编译失败。
- **根因**：`_Static_assert` 是 **C11** 标准引入的关键字。Keil ARMCC V5.06（V5 编译器）默认用 C99 标准，不认识 `_Static_assert`，把它当成普通标识符解析 → 语法错误。ARMCC V6（Clang based）才默认支持 C11。
- **排查过程**：
  1. 报错行就是 `_Static_assert(...)` → 直接锁定。
  2. 查 Keil 编译器版本：`V5.06 update 7 (build 960)` → 确认是 ARMCC V5。
  3. 查 _Static_assert 标准归属：C11 → V5 默认不支持。
- **解决**：用 C99 兼容的静态断言宏（typedef 一个数组，条件不满足时数组大小为负，编译器报错）：
  ```c
  #define STATIC_ASSERT_CONCAT_(a, b) a##b
  #define STATIC_ASSERT_CONCAT(a, b) STATIC_ASSERT_CONCAT_(a, b)
  #define STATIC_ASSERT(cond, msg) \
      typedef char STATIC_ASSERT_CONCAT(static_assert_, __LINE__)[(cond) ? 1 : -1]

  STATIC_ASSERT(sizeof(flash_param_t) == 4096, param_size_must_be_4096);
  ```
  条件成立时 typedef `char arr[1]`（合法），条件不成立时 typedef `char arr[-1]`（负数组大小，编译报错）。
- **教训**：跨编译器写代码时，C11/C99 特性要先确认编译器支持。ARMCC V5 默认 C99，V6 默认 C11/C17。用 `_Static_assert`、`_Alignas`、匿名结构体成员等 C11 特性前，要么切 V6，要么写 C99 兼容版本。静态断言用 typedef 数组负大小的技巧是嵌入式最通用的写法，V5/V6/gcc 都认。

## 坑2：CRC32 查表法静态表手动输入列数错误（#146 too many initializer values）

- **现象**：`flash_param.c(43): error: #146: too many initializer values`，指向 CRC32 静态查表的初始化。
- **根因**：最初用"静态写死 256 项 CRC32 表"的方式（`static uint32_t crc32_table[256] = {0x00000000, 0x77073096, ...}`）。256 项太多，手动输入时某一行多打了一个逗号或少打了一个值，编译器认为初始化值个数 > 数组大小 → #146。
- **排查过程**：
  1. 报错行就是表初始化 → 锁定。
  2. 数 256 项的个数 → 数到眼花也数不准到底是 255 还是 256 项。
  3. 意识到"手动输入 256 项永远会错"→ 改方案。
- **解决**：废弃静态表，改成**运行时生成**（多项式 0xEDB88320，上电调一次 `CRC32_InitTable()` 填表，占 1KB RAM，生成耗时约 50 微秒）：
  ```c
  static uint32_t crc32_table[256];
  void CRC32_InitTable(void) {
      for (uint32_t i = 0; i < 256; i++) {
          uint32_t crc = i;
          for (uint32_t bit = 0; bit < 8; bit++) {
              crc = (crc & 1u) ? ((crc >> 1) ^ CRC32_POLYNOMIAL) : (crc >> 1);
          }
          crc32_table[i] = crc;
      }
  }
  ```
- **教训**：查表法查表可以静态写死，但**超过 32 项的表就别手输了**，列数/项数迟早数错。运行时生成更可靠：代码只有两层 for 循环，复制即正确，代价是 1KB RAM + 50μs 启动时间，对 STM32F103 完全可接受。

## 坑3：flash_param_t (4096字节) 放栈上导致栈溢出卡死

- **现象**：`FlashParam_Save` 函数里声明 `flash_param_t buf;`（4096 字节局部变量），调用后系统直接卡死，串口无任何输出，LED 停闪。
- **根因**：`flash_param_t` 强制大小 4096 字节（`_Static_assert` 校验）。`FlashParam_Save` 是被 `TaskAppParam`（栈 512 words = 2048 字节）调用的，光这一个局部变量就吃掉 4096 字节，是栈的 2 倍 → 栈底 canary 被踩 → HardFault 或调度器状态机崩溃。
- **排查过程**：
  1. 调用 `FlashParam_Save` 后立刻卡死，不调用就正常 → 锁定该函数。
  2. 读代码发现 `flash_param_t buf;` 在栈上 → 4096 字节 > 任务栈 2048 字节 → 必爆。
  3. 用 `uxTaskGetStackHighWaterMark` 确认剩余栈为 0。
- **解决**：把局部变量改成**全局静态变量** `g_param_buf_internal`，放 BSS 段（不占栈）：
  ```c
  static flash_param_t g_param_buf_internal;  /* 放 BSS 段，避免栈溢出 */
  int FlashParam_Save(const flash_param_t *p_in) {
      memcpy(&g_param_buf_internal, p_in, sizeof(flash_param_t));
      /* 后续操作都用 g_param_buf_internal */
  }
  ```
- **教训**：嵌入式的铁律——**单任务栈不能放超过栈大小 1/4 的局部变量**。512 words(2048B) 的任务，单个局部变量上限 512 字节。`flash_param_t` 这种 4KB 结构体必须放全局静态或堆。判别标准：局部变量 > 256 字节就要警惕，> 512 字节基本必须改全局/静态。

## 坑4：__disable_irq() 关中断导致 HAL 时间基失效 → HAL_FLASH_Program 超时判断崩

- **现象**：`[PARAM] Erase OK` 但 `[PARAM] Write FAIL!`，写入一直失败。参考链接的 AI 建议加 `__disable_irq()` 防止"中断打断 Flash 写入"，加了之后反而更糟。
- **根因**：这是参考链接 AI 建议**搞反因果**的典型。STM32F103 写 Flash 时 CPU 硬件会自动 stall（同一 Bank 取指/取数等写完），根本不需要软件关中断保护。而 `__disable_irq()` 的真正危害是：
  1. 项目 HAL 时间基是 **TIM4 中断**（不是 SysTick，被 FreeRTOS 占了）。
  2. `__disable_irq()` 关全局中断 → TIM4 中断进不来 → `uwTick` 冻结。
  3. `HAL_FLASH_Program` 内部调 `FLASH_WaitForLastOperation`，用 `HAL_GetTick()` 做超时判断（`while(... && (HAL_GetTick() - tickstart) < Timeout)`）。
  4. `uwTick` 冻结 → `HAL_GetTick()` 返回值不变 → 超时判断失效 → 返回 HAL_TIMEOUT → 你看到 Write FAIL。
  也就是说，参考链接让你加的 `__disable_irq()` **本身就是 Write FAIL 的制造者**。
- **排查过程**：
  1. 参考链接（AI 生成，顶部标注"may not be fully accurate"）建议加关中断 → 加了还是 FAIL。
  2. 加诊断打印 `HAL_FLASH_Program` 返回的 `status` 值 → 是 `0x01`（HAL_ERROR），不是 timeout（HAL_TIMEOUT=3）。
  3. 但关中断的危害是让 `HAL_GetTick` 失效，这会间接导致 `FLASH_WaitForLastOperation` 误判。
  4. 读 `flash_if.c` 确认 `__disable_irq()` / `__enable_irq()` 还在 → 移除。
- **解决**：移除 `FLASH_WriteBuf` 里的 `__disable_irq()` 和 `__enable_irq()`。STM32F103 写 Flash 硬件自动 stall CPU，不需要软件关中断；时间基依赖 TIM4 中断，关中断必然冻 `uwTick`。
- **教训**：AI 生成的修改建议（尤其带"可能不准确的"标注的）要带着怀疑看。判断"关中断"是否必要的关键是：① 写 Flash 这种操作硬件本身是否需要原子性保护（STM32F1 不需要，硬件 stall）；② 关中断会不会影响 HAL 时间基（项目用 TIM4 当时间基，关中断必冻 `uwTick`）。两个条件叠加，`__disable_irq()` 在这个项目里永远是错的。

## 坑5：FLASH_ErasePage 的 NbPages=1 只擦 2KB，参数区 4KB 跨 2 页 → 第二次启动卡死（最致命）

- **现象**：
  - 第一次上电（全新芯片）：`Write OK`，参数区能写，系统正常跑。
  - 按一次 Reset（第二次启动）：Bootloader 读到 `boot_count=1`，APP 调 `FlashParam_Save` 重新写参数区，写到一半串口卡在 `[FLA`（打印 `[FLASH]` 开头就死），再怎么 Reset 也没用，板子彻底卡死。
- **根因**：STM32F103**ZE**T6 每页 = **2KB**（HAL 库 `FLASH_PAGE_SIZE = 2048`），不是 4KB。参数区定义是 4KB（`PARAM_FLASH_SIZE = 0x1000`），**跨了 2 页**：
  - 第 1 页：0x0807F000–0x0807F7FF（结构体前 2KB，i=0~511）
  - 第 2 页：0x0807F800–0x0807FFFF（结构体后 2KB 含 tail_marker，i=512~1023）
  而 `FLASH_ErasePage` 里 `erase_init.NbPages = 1`，**只擦第 1 页，第 2 页从来没被擦过**。这导致两次启动行为不同：
  | 启动 | 第 2 页状态 | 写入结果 |
  |---|---|---|
  | 第 1 次 | 出厂全新 0xFF | 1024 word 全写成功 → `Write OK`（假象） |
  | 第 2 次 | 有第 1 次的旧数据 ≠0xFF | i=512 @0x0807F800 触发 PGERR → 打印 `[FLA` 到一半 HardFault → 卡死 |
  `flash_partition.h` 里注释 `/* 4KB = 1 page @ F103ZE */` 是**错的**，F103ZE 每页 2KB，4KB 是 2 页。
- **排查过程**：
  1. 第一次 `Write OK` 但第二次卡死 → 怀疑擦除不充分（第 2 页没擦干净）。
  2. 加诊断打印 `status` 值 → 第一次 `status=0x01`（HAL_ERROR，PGERR/WRPRTERR），失败位置 `i=1020 @0x0807FFF0` 正是 `tail_marker`，在第 2 页。
  3. 查 STM32F103ZET6 页大小 → HAL 库 `FLASH_PAGE_SIZE=2048`，每页 2KB。
  4. 算参数区页数：4KB / 2KB = 2 页，`NbPages=1` 只擦一半。
  5. 确认第 2 页（0x0807F800–0x0807FFFF，含 tail_marker）从来没被擦。
- **解决**：`FLASH_ErasePage` 里 `NbPages` 从 `1` 改成 `2`：
  ```c
  erase_init.NbPages = 2;   /* 参数区 4KB = 2 页 @ F103ZE(每页2KB) */
  ```
  并把 `flash_partition.h` 的误导注释改成 `/* 4KB = 2 pages @ F103ZE (每页 2KB) */`。改完第一次 `Write OK`，**按 Reset 第二次不再卡**，`boot_count` 能从 3 累加到 4，跨复位持久化全通。
- **教训**：STM32F1 系列页大小按容量分档——中小容量（≤128KB Flash）每页 1KB，大容量（≥256KB，如 ZET6）每页 2KB。写 Flash 擦除函数时，`NbPages` 必须按"参数区大小 / 实际页大小"算，不能想当然写 1。`flash_partition.h` 里这种"4KB=1 page"的注释是埋雷，必须和实际页大小对齐。这类 bug 的特征是"第一次能过、第二次必死"，遇到就优先查擦除范围。

## D9 成果

- **参数区架构落地**：`flash_param_t` 结构体（4096 字节，强制 4 字节对齐 + packed），涵盖固件版本（major/minor/patch/build）、OTA 请求控制（magic/new_crc/new_size/rollback）、运行状态（boot_count/reset_reason/ota_result）、设备信息（device_id/mqtt_prefix）、Tail 冗余校验（tail_marker/tail_crc）。`flash_partition.h` 统一管理三分区地址宏，Bootloader 和 APP 两边 include 同一份，改分区只改一个文件。
- **CRC32 查表法实现**：IEEE 802.3 多项式 0xEDB88320，运行时生成 256 项查表（`CRC32_InitTable` 上电调一次），`CRC32_Calc` 计算任意内存块 CRC，`CRC32_CalcAppFlash` 计算整片 APP 区 CRC（D12 OTA 校验用）。
- **参数区三重校验读写 API**：
  - `FlashParam_Load`：Flash→RAM，校验 magic_header + tail_marker + struct_crc32，任一不过返回 -1（参数区未初始化或损坏）。
  - `FlashParam_Save`：RAM→Flash（自动擦 2 页 + 写 4096 字节），内部填 magic/tail/CRC，写完可被下次 Load 校验通过。
  - `FlashParam_SetOtaRequest` / `FlashParam_ClearOtaRequest`：D12 OTA 请求位写入，Bootloader 读到 `ota_request_magic=0x4F544131` 触发升级流程。
- **Bootloader / APP 共享读写闭环验证通过**：
  - APP 端写参数区（`FlashParam_Save`）→ Flash 持久化。
  - Bootloader 端读参数区（`FlashParam_Load`）→ 打印 `fw=1.0.3 build=1 boot_count=N`。
  - APP 端读参数区 → `boot_count` 跨复位累加（3→4→5...）。
  - magic_header=0x504D5431 / tail_marker=0xDEADBEEF / struct_crc32 三重校验全过。
- **D9 所有踩坑修复**：`_Static_assert`→C99 兼容宏；CRC32 表→运行时生成；栈溢出→全局静态变量；`__disable_irq()`→移除；`NbPages=1`→`NbPages=2`。
- **编译 0 Error 0 Warning**，Bootloader + APP 双工程均干净通过。
- 具备进入 D10（Ymodem 串口 IAP 烧录）的条件：参数区已能保存 OTA 请求标志，Bootloader 三分支状态机已预留 OTA_FLAG 分支，Flash 操作函数已封装好。




D10：Ymodem 串口 IAP 救砖通道（Bootloader 端 Ymodem 接收 + IAP 写 Flash + CRC32 双重校验）（2026-08-23）
阶段归属：阶段4 串口救砖通道 · IAP/Ymodem 层

---

## 坑1：iap.c 重复定义 CRC32 静态查表 → #146 too many initializer values

- **现象**：Rebuild Bootloader 工程报 `iap.c(87): error: #146: too many initializer values`，指向 `static const uint32_t tbl[256] = {0x00000000, 0x77073096, ...}` 这一行。同时 4 个文件报 `#1-D: last line of file ends without a newline` warning。
- **根因**：D9 阶段已经踩过同样的坑（D9 坑2），结论是"超过 32 项的静态表不要手输"。但 D10 写 `iap.c` 时为了让 IAP 层独立计算流式 CRC32，又把 256 项 CRC32 表手输了一遍——256 项的列数/项数只要错一个逗号或漏一项就触发 #146。同一个错误犯两次，是因为没有把 D9 的"运行时生成表"方案对外暴露为公共 API，导致每个用到 CRC32 的新模块都要自带一份表。
- **排查过程**：
  1. 编译日志直接点出 `iap.c(87)` 是 `static const uint32_t tbl[256] = {...}` 这行。
  2. 看到 256 项静态表 + #146 → 立刻想起 D9 坑2 的结论。
  3. 翻 `flash_param.c`，发现它已经有运行时生成的 `crc32_table[256]`，但是是 `static` 的，外部文件 `iap.c` 访问不到 → 只能自己再抄一份。
- **解决**：把 D9 的运行时表通过三个流式接口暴露出来，`iap.c` 删掉自己的静态表，统一调公共 API：
  ```c
  /* flash_param.h 新增 3 个流式接口 */
  void     CRC32_StreamReset(uint32_t *ctx);                              /* ctx = 0xFFFFFFFF */
  void     CRC32_StreamUpdate(uint32_t *ctx, const uint8_t *data, uint32_t len);  /* 流式累加 */
  uint32_t CRC32_StreamFinalize(uint32_t *ctx);                          /* XOR 0xFFFFFFFF 取反 */

  /* flash_param.c 新增 3 个实现，复用本文件已有的 static crc32_table */
  void CRC32_StreamReset(uint32_t *ctx) { if (ctx) *ctx = 0xFFFFFFFFUL; }
  void CRC32_StreamUpdate(uint32_t *ctx, const uint8_t *data, uint32_t len) {
      if (!ctx || !data) return;
      uint32_t crc = *ctx;
      for (uint32_t i = 0; i < len; i++) {
          crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
      }
      *ctx = crc;
  }
  uint32_t CRC32_StreamFinalize(uint32_t *ctx) {
      return (*ctx) ^ 0xFFFFFFFFUL;
  }
  ```
  `iap.c` 里调用方式变成：
  ```c
  uint32_t g_running_crc;
  CRC32_StreamReset(&g_running_crc);
  CRC32_StreamUpdate(&g_running_crc, data, len);   /* 每收到一包累加 */
  uint32_t crc_ram   = CRC32_StreamFinalize(&g_running_crc);
  uint32_t crc_flash = CRC32_Calc((const uint8_t *)APP_FLASH_START, total_size);
  ```
- **教训**：查表法的表本身不应该是"每个模块各抄一份"的资源。一个项目里只要有一份运行时生成的 CRC32 表就够了，但必须通过公共 API 暴露出去，否则后来者要么再抄一份表（必然踩 #146），要么直接访问别人文件里的 `static` 变量（编译报错）。**CRC32 这种公共算法应该一开始就设计成 Reset/Update/Finalize 三段式流式接口**，因为 IAP/OTA 这种场景必然需要"边收边算"，一次性 `CRC32_Calc(buf, len)` 接口在 464KB 固件 + 64KB SRAM 的场景下根本放不下整包缓冲区。

## 坑2：HAL_UART_Receive 阻塞接收导致 ORE 帧错位 → 文件头过了但数据帧永远失败

- **现象**：用 Tera Term 发 `app.bin`，日志显示 `header_ok=1`（文件头解析成功，`header_size=30268`），但进入数据阶段后立刻报 `Invalid frame start: 0x81`，然后 `Frame-start timeout` / `Ymodem FAIL! code=-2 (size=0 bytes)`。反复重试，数据帧阶段永远过不了，但文件头每次都能过。
- **根因**：最初的 `y_recv_frame()` 用 `HAL_UART_Receive(&huart1, &header, 1, timeout)` 逐字节/逐字段阻塞接收。看起来是"持续监听"，实际只有代码执行到 `HAL_UART_Receive()` 这一行时才主动等字节。Ymodem 协议的关键时序是：
  1. 接收端发 `ACK` + `C` 给发送端；
  2. 发送端收到 `C` 后**立即**开始发下一帧（SOH/STX + seq + ~seq + 1024B + CRC16）；
  3. 接收端在发完 `C` 之后，还忙着 `printf` 打调试信息、切换接收阶段状态、写 Flash；
  4. 发送端的下一帧字节已经到 USART1，但 CPU 没在调 `HAL_UART_Receive`，RXNE 没及时清 → ORE（Overrun）→ 帧头字节丢失 → 后续 payload 中某个字节被误认成"帧头"（比如 0x81）→ 整帧错位。
  文件头能过是因为文件头只有 128 字节 + 发送端发完头帧后会等 ACK，时间窗口宽；数据帧是 1024 字节连发，时间窗口窄，必丢。
- **排查过程**：
  1. 看到 `header_ok=1` 但数据帧报 `Invalid frame start: 0x81` → 0x81 不是 SOH(0x01)/STX(0x02)/EOT(0x04)/CAN(0x18) → 说明解析器从错误字节边界开始解释数据。
  2. 0x81 这种值正好是 payload 里的普通字节 → 锁定"帧头错位"而不是 CRC 算法错。
  3. 检查 `huart1.ErrorCode` 发现 ORE 位置过 → 接收溢出。
  4. 对比时序：发完 `C` 后 `printf` 调试文本 + 切阶段，这段时间 CPU 不在 `HAL_UART_Receive` → 发送端的下一帧被错过。
- **解决**：改成 **USART1 RX 中断 + 4KB 环形缓冲区**，把"接收字节"和"解析帧"解耦：
  ```c
  #define Y_RX_RING_SIZE 4096U
  #define Y_RX_RING_MASK (Y_RX_RING_SIZE - 1U)
  static volatile uint8_t  y_rx_ring[Y_RX_RING_SIZE];
  static volatile uint16_t y_rx_head;
  static volatile uint16_t y_rx_tail;

  /* USART1 RX 中断：只做 3 件事，不能阻塞 */
  void Ymodem_UART_IRQHandler(void) {
      uint32_t sr = huart1.Instance->SR;
      if ((sr & USART_SR_RXNE) != 0U) {
          uint8_t byte = (uint8_t)(huart1.Instance->DR & 0xFFU);
          uint16_t next = (y_rx_head + 1U) & Y_RX_RING_MASK;
          if (next == y_rx_tail) {
              y_rx_overrun = 1U;                      /* 环满，标记 overrun */
          } else {
              y_rx_ring[y_rx_head] = byte;
              y_rx_head = next;
          }
      }
  }
  ```
  主循环里的 `y_recv_byte()` 从 `ring[tail]` 取字节 + 超时判断。中断负责把每个字节搬进 ring，CPU 即使在 `printf`/写 Flash/发 ACK，RX 中断仍然持续收字节，发送端的下一帧不会因为主循环暂时没调接收函数而丢失。
- **教训**：Ymodem 这类"发送端不等接收端准备好就连续发包"的协议，**不能用阻塞轮询接收**。阻塞接收的隐藏前提是"CPU 全程只在等字节"，一旦 CPU 要做别的（打印/写 Flash/发控制字节），RXNE 就会漏。中断 + 环形缓冲区是嵌入式串口协议的标准答案：中断快（只搬字节，不调用任何阻塞 API），ring 吸收速度差，解析器按协议字段慢慢取。环形缓冲区大小要 ≥ 2 倍最大单帧（STX 帧 1024B，ring 给 4KB 绰绰有余）。中断里绝对不能 `printf`、不能写 Flash、不能等超时——否则又把"中断必须快"的优势破坏了。

## 坑3：擦 APP Flash 放在 Ymodem 回调里 → 握手期间 PC 发的首包被冲垮

- **现象**：把 `FLASH_EraseAppArea()`（擦 464KB = 232 页，耗时约 2 秒）写在 `iap_on_packet()` 回调里（即"收到第一包数据时再擦 Flash"）。结果 `header_ok=1` 后，Bootloader 边擦 Flash 边等 Ymodem 数据帧，2 秒擦除期间 PC 发来的数据帧全部堆在环形缓冲区里，擦完后解析器从头取字节时已经错位，报 `Frame-start timeout` 或 `Invalid frame start`。即使加大 ring 也救不回来——232 页擦除期间 PC 早就发了好几帧。
- **根因**：把擦 Flash 放回调里的初衷是"懒加载"——以为"等收到第一包再擦，省得空跑"。但 Ymodem 的时序不允许：
  1. Bootloader 发 `C` 握手；
  2. PC 收到 `C` 发文件头（block-0）；
  3. Bootloader 回 `ACK` + `C`；
  4. PC **立刻**开始发第一帧数据帧（不等 Bootloader 干别的）；
  5. 如果此时 Bootloader 在回调里开始擦 2 秒 Flash，PC 的数据帧持续灌进来 → ring 被填满 → 后续帧 overrun 丢字节 → 解析错位。
  擦 Flash 这种"长耗时阻塞操作"必须和"协议时序敏感的接收窗口"完全错开。
- **排查过程**：
  1. 现象是"文件头过了，但数据帧阶段 timeout/错位" → 起初怀疑是坑2的接收问题。
  2. 但坑2已经修了中断+ring，还是失败 → 排查时序。
  3. 在 `iap_on_packet` 里加 `printf` 时间戳，发现"收到第一包"到"擦完 Flash"之间隔了 2 秒，这 2 秒 PC 没停过发包。
  4. 把擦 Flash 移到 `IAP_ProcessSerial()` 入口（握手发 `C` 之前），问题消失。
- **解决**：把擦 APP Flash 从回调移到 IAP 入口，**先擦完再发 `C` 握手**：
  ```c
  int IAP_ProcessSerial(void) {
      /* 1. 先擦 APP Flash（2 秒），擦完 PC 还没开始发数据 */
      printf("[IAP] Pre-erasing APP flash (0x%08X, %u bytes, 232 pages)...\r\n",
             APP_FLASH_START, APP_FLASH_SIZE);
      if (FLASH_EraseAppArea() != HAL_OK) {
          printf("[IAP] !!! Pre-erase FAIL, abort IAP.\r\n");
          return -1;
      }
      printf("[IAP] Pre-erase OK. Now sending 'C' for Ymodem handshake...\r\n");

      /* 2. 擦完才发 'C'，PC 收到 'C' 才开始发头帧，时序不冲突 */
      ret = Ymodem_Receive(NULL, APP_FLASH_SIZE, &g_total_size, iap_on_packet);
  }
  ```
  回调 `iap_on_packet()` 只负责"写 Flash + 累加 CRC32"，不再做擦除。擦除在握手之前完成，PC 在 Bootloader 擦 Flash 期间根本不会发包（还没收到 `C`），时序完全错开。
- **教训**：Ymodem/IAP 这种"协议时序敏感 + 长耗时 Flash 操作"并存的设计，**长耗时操作必须放在协议握手之前**，不能放在回调里。回调是"每收到一包触发一次"的高频路径，里面放 2 秒擦除等于每包都卡 2 秒——但 Ymodem 发送端不会等，会持续灌包。判断标准：凡是耗时 > 100ms 的操作（擦 Flash、擦参数区、写大块数据），都不能放在 Ymodem 帧回调里，必须前置到握手前或后置到全部接收完后。回调里只做"写当前包到 Flash + 累加 CRC"这种微秒级操作。

## 坑4：EOT 分支不更新 received_size → IAP 层 size=0 误导排错

- **现象**：日志显示 `frame_ret=-1` `hdr=0x04`（0x04 就是 EOT，`frame_ret=-1` 是 `y_recv_frame()` 用来表示收到 EOT 的内部返回值），`header_ok=1` `header_size=30268`，但最终打印 `[IAP] Ymodem FAIL! code=-2 (size=0 bytes)`。看起来"数据一包都没收到"，但 Flash 里其实已经写入了大部分甚至全部 APP。
- **根因**：`Ymodem_Receive()` 在收到 EOT 时，旧代码直接 `break` 跳出数据循环，没有先执行 `*received_size = total_received;`。`received_size` 是出参指针，指向 IAP 层的 `g_total_size`。EOT 分支不更新它，`g_total_size` 就一直是初始化值 0。IAP 层拿到 `size=0` → 以为没收到数据 → 报 `Y_ERR_CRC` → 误导排错方向（去查 CRC 算法，其实 CRC 根本没机会算）。
  这不是"真的没收到数据"，而是"出参没写回去"的统计错误。
- **排查过程**：
  1. 看到 `size=0` 起初以为"一包都没收到" → 但 `hdr=0x04` 说明已经走到 EOT，EOT 是发送端发完所有数据帧后才发的 → 矛盾。
  2. 看 `header_ok=1` `header_size=30268` → 文件头过了，大小也解析对了 → 数据帧阶段肯定收过包。
  3. 检查 `flash_written` 计数器 → 发现已经写了几万字节 → 实际收到了数据。
  4. 锁定问题在"出参没更新"：`y_recv_frame()` 返回 -1（EOT）的分支直接 break，没写 `*received_size`。
- **解决**：在 EOT 分支 `break` 之前，确保 `total_received` 赋值给 `received_size`，在所有可能的退出路径上都正确更新出参：
  ```c
  if (ret == -1) {                 /* y_recv_frame() 收到 EOT */
      y_send_byte(errors == 0 ? Y_NAK : Y_ACK);   /* 兼容单/双 EOT */
      /* ... 兼容第二个 EOT、单 EOT、额外控制字节 ... */
      break;
  }
  /* 跳出数据循环后，统一更新出参（所有退出路径都走这里）*/
  if (received_size != NULL) {
      *received_size = total_received;
  }
  ```
  把"更新出参"从 EOT 分支内部移到循环外统一处理，这样无论是 EOT 退出、CRC 错误退出、超时退出，`received_size` 都能拿到真实值。
  同时补一个硬校验：EOT 不代表文件完整，EOT 后必须比 `total_received != file_total_size` → `return Y_ERR_SIZE`。
- **教训**：Ymodem 这类"多阶段 + 多退出路径"的协议函数，**出参必须在所有退出路径上统一更新**，不能只在某个分支里写。最稳妥的写法是"循环内只 break/continue，循环外统一写出参"。排查这类"size=0 但实际有数据"的日志时，**不能只看 `size` 字段，要交叉看 `hdr`（0x04=EOT）、`header_ok`、`header_size`、`flash_written`**——如果 `hdr=0x04` 且 `flash_written` 已经增长，那"size=0"必然是统计错误而不是真的没收到。

## 坑5：APP 启动后无条件覆盖 IAP 保存的 fw_size_bytes 和 fw_crc32

- **现象**：D10 串口 IAP 成功后，参数区保存了正确的 `fw_size=30268` `fw_crc32=0x59B4EA2B`（IAP 流式 CRC + Flash 回读 CRC 双重一致）。但 APP 跑起来后，下次 Bootloader 读参数区，`fw_size` 变成了 475136（整个 APP 分区大小），`fw_crc32` 变成了全区 CRC——IAP 保存的"真实固件大小/CRC"被覆盖，OTA 校验语义被破坏。
- **根因**：`app/Src/app_task.c` 的参数初始化逻辑是：
  ```c
  int ret = FlashParam_Load(&g_param_buf);
  if (ret == 0) {
      /* 参数区有效 */
  } else {
      /* 参数区无效，建默认值 */
  }
  /* ← 无条件执行这两行，不管参数区有没有效 ← */
  g_param_buf.fw_size_bytes = APP_FLASH_SIZE;       /* 475136 */
  g_param_buf.fw_crc32      = CRC32_CalcAppFlash(); /* 全区 CRC */
  ```
  这两行写在 if/else 外面，导致**无论参数区是否有效**，APP 启动后都把 `fw_size/fw_crc` 覆盖成"整片 APP 分区的值"。IAP 精心保存的"30268 字节真实固件 + 0x59B4EA2B"被冲掉。这不会改变 Flash 里的 APP 内容（APP 还能跑），但破坏了参数区"真实固件大小/CRC"的语义——D12 WiFi OTA 校验时本应比 `fw_size=30268`，现在变成比 475136，必然 mismatch。
- **排查过程**：
  1. D10 成功日志里 `Param Save OK. build=2, fw_crc=0x59B4EA2B, fw_size=30268B` → IAP 写参数正确。
  2. 但 APP 跑起来后，下次 Bootloader 读出来 `fw_size` 变了 → 怀疑 APP 覆盖。
  3. 读 `app_task.c`，发现 `fw_size_bytes = APP_FLASH_SIZE` 这两行在 `FlashParam_Load` 的 if/else 外面 → 无条件执行。
  4. 参数区有效（IAP 刚写过）时，APP 不应该再覆盖，应该直接用 IAP 保存的值。
- **解决**：把 `fw_size_bytes` 和 `fw_crc32` 的赋值移到"参数区无效"的 `else` 分支里，只在需要建默认值时才赋：
  ```c
  int ret = FlashParam_Load(&g_param_buf);
  if (ret == 0) {
      /* 参数区有效，IAP 保存的 fw_size_bytes=30268 / fw_crc32=0x59B4EA2B 是真实值
       * APP 不应该覆盖，直接用 */
  } else {
      /* 参数区无效，建默认值 */
      g_param_buf.fw_ver_major  = 1;
      g_param_buf.fw_ver_minor  = 0;
      g_param_buf.fw_ver_patch  = 0;
      g_param_buf.fw_build_num  = 1;
      g_param_buf.boot_count    = 0;
      g_param_buf.fw_size_bytes = APP_FLASH_SIZE;        /* ← 只在无效时才用整片默认值 */
      g_param_buf.fw_crc32      = CRC32_CalcAppFlash();  /* ← 同上 */
      strncpy(g_param_buf.device_id, "dev001", ...);
      FlashParam_Save(&g_param_buf);
  }
  ```
  参数区有效时 APP 只读不写，IAP 保存的真实 `fw_size/fw_crc` 被保留，D12 OTA 校验语义正确。
- **教训**：Bootloader 和 APP 共享参数区时，**APP 端的"参数初始化"必须区分"首次创建默认值"和"读取已有值"两条路径**，不能无条件覆盖。判别标准：`FlashParam_Load` 返回 0（有效）→ 只读不改；返回 -1（无效/未初始化）→ 才建默认值并 Save。凡是涉及"固件大小/CRC/版本/boot_count"这种被多方写入的字段，APP 端默认初始化时一定要包在 `else` 里，否则会把 Bootloader/IAP 精心保存的状态冲掉。这类 bug 的特征是"IAP/OTA 刚写完是对的，APP 跑一次就变了"——遇到就优先查 APP 的参数初始化是不是无条件覆盖。

## D10 成果

- **Ymodem 串口 IAP 救砖通道落地**：Bootloader 端实现完整 Ymodem-CRC 接收器（`ymodem.c`/`ymodem.h`），支持 SOH(128B)/STX(1024B) 双帧型、序号+反码校验、CRC16/XMODEM（多项式 0x1021）帧级校验、重复包识别（只 ACK 不重写）、单/双 EOT 兼容、CAN 取消。`iap.c`/`iap.h` 实现 IAP 业务层：擦 APP Flash → Ymodem 接收 → 流式写 Flash → CRC32 双重校验 → 更新参数区 → 软复位。
- **USART1 RX 中断 + 4KB 环形缓冲区**：解决阻塞接收丢字节/ORE/帧错位问题。中断只搬字节（不 printf/不写 Flash/不等超时），ring 吸收发送端和主循环速度差，主循环按协议字段逐字节取。`Y_RX_RING_SIZE=4096`（≥ 4 倍 STX 帧）。
- **流式 CRC32 公共 API**：`CRC32_StreamReset/Update/Finalize` 三段式接口（复用 D9 运行时查表），IAP 层边收边算，464KB 固件 + 64KB SRAM 场景下无需整包缓冲。接收流 CRC32 vs Flash 回读 CRC32 双重校验，成功值 `0x59B4EA2B` 两边完全一致。
- **IAP 时序重构**：擦 APP Flash（232 页 ~2 秒）从 Ymodem 回调移到 `IAP_ProcessSerial()` 入口，先擦完再发 `C` 握手，PC 在擦除期间不发包，时序完全错开。回调 `iap_on_packet()` 只做"写当前包 + 累加 CRC32"微秒级操作。
- **EOT 出参统一更新**：`*received_size = total_received` 移到数据循环外统一处理，所有退出路径（EOT/CRC 错/超时）都能拿到真实接收长度。EOT 后补 `total_received != file_total_size` 硬校验，防"EOT≠文件完整"。
- **Bootloader 三分支状态机落地**（`main.c`）：① 参数区有 OTA 请求（`ota_request_magic=MAGIC_OTA_REQUEST`）→ 3 秒倒计时按 KEY0 进串口 IAP（先清 OTA 标志防失败死循环）；② KEY0 + Reset 强制进串口 IAP（硬件救砖兜底）；③ APP 有效（`fw_size/fw_crc` 校验通过 + 栈指针合法）→ 跳转 `0x08008000`。
- **APP 端参数区只读化**：`app_task.c` 把 `fw_size_bytes/fw_crc32` 赋值移到 `FlashParam_Load` 失败的 `else` 分支，参数区有效时 APP 不覆盖 IAP 保存的真实值，D12 OTA 校验语义正确。
- **D10 所有踩坑修复**：CRC32 表重复 → 公共流式 API；阻塞接收 ORE → 中断+ring；擦 Flash 放回调 → 前置到握手前；EOT 不更新出参 → 循环外统一写；APP 覆盖参数 → 移到 else 分支。
- **编译 0 Error 0 Warning**，Bootloader + APP 双工程干净通过。硬件实测：Tera Term Ymodem 发 `app.bin`（30268 字节）→ `Pre-erase OK` → `CRC MATCH` → `Param Save OK` → 软复位 → `APP valid, jumping to 0x08008000` → APP 启动（DHT11/RTOS 任务正常运行），救砖通道全闭环。
- 具备进入 D11（WiFi OTA 远程升级）的条件：Bootloader 已能识别参数区 OTA 请求标志 + 串口 IAP 救砖兜底已就绪，D11 只需在 APP 端通过 MQTT 收到升级命令后写 `ota_request_magic` + 复位即可触发 Bootloader 升级流程。


