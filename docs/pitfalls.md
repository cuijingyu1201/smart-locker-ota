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
