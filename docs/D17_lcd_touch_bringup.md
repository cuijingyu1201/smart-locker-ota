# D17（D1）：LCD 显示 + 电阻触摸屏移植（2026-09-04）

阶段归属：智能储物柜（1 柜极简触屏版）· 阶段1 人机交互 · LCD/Touch Bringup

* 硬件：正点原子精英 STM32F103ZET6 + 2.8 寸 TFTLCD（ILI9341/ST7789，电阻触摸 XPT2046）

* 软件：FreeRTOS CMSIS\_V2 + HAL 库 + FSMC 并口 16bit + 软件 SPI 触摸

* 目标：把正点原子 TFTLCD / 触摸屏例程移植进 OTA APP 工程，并纳入 CubeMX 管理

***

## 坑1：Bootloader `__disable_irq()` 后 APP 未重新开中断，HAL\_Delay() 死循环（致命，卡了最久）

* **现象**：LCD 白屏、LED 不亮、所有 APP 功能全卡住。串口停在：

  ```
  [BOOT] Jumping to 0x08008000...
  [LCD] BL(PB0)=HIGH set OK.
  ```

  之后无任何输出。加了 `lcd_init()` 关键节点诊断打印后，定位到打印到 `[LCDDBG] HAL_SRAM_Init done` 就没了，下一步 `HAL_Delay(50)` 卡死。

* **根因**：

  1. Bootloader 跳转 APP 前在 `jump_to_app.c` 调用了 `__disable_irq()`，设置 PRIMASK=1，**关闭所有可屏蔽中断**（跳转过程不被打断的标准做法）。
  2. APP 的启动链 `SystemInit() → HAL_Init() → main()` **从头到尾没有调用** **`__enable_irq()`**，PRIMASK 一直保持 1。
  3. 本工程 HAL 时基源是 **TIM4**（D2 已把 SysTick 让给 FreeRTOS）。`HAL_Init() → HAL_InitTick()` 虽然配置并启动了 TIM4，但 TIM4 更新中断被 PRIMASK 屏蔽，**永远进不了中断**，`HAL_TIM_PeriodElapsedCallback → HAL_IncTick()` 不执行，`uwTick` 恒为 0。
  4. `lcd_init()` 里 `HAL_Delay(50)` 的判断是 `(uwTick - tickstart) < wait`，uwTick 和 tickstart 都是 0 → `(0-0) < 50` 永远为真 → **死循环**。

* **为什么之前没暴露**：D2\~D16 的 APP 代码在调度器启动前从不调用 `HAL_Delay()`（只用 FreeRTOS 的 `osDelay`）。而 `osKernelStart() → vTaskStartScheduler()` 内部的 SVC 中断处理会自动执行 `CPSIE I` 开中断，所以调度器一起跑中断就恢复了。D17 首次在 `osKernelStart()` **之前**（`App_Lcd_Init → lcd_init`）调用 `HAL_Delay()`，这个潜伏了 16 个阶段的雷才被引爆。

* **解决**：在 `main.c` 的 `USER CODE BEGIN 1`（HAL\_Init 之前）加一行：

  ```c
  /* USER CODE BEGIN 1 */
  __enable_irq();   /* Bootloader 跳转前 __disable_irq()，APP 必须重新开启全局中断 */
  /* USER CODE END 1 */
  ```

  必须放在 USER CODE 标记内，CubeMX 重新生成才不会丢。

* **教训**：

  1. Bootloader 跳转 APP 前 `__disable_irq()` 是标准安全做法，但 **APP 必须在复位后最早阶段（HAL\_Init 之前）重新** **`__enable_irq()`**，否则所有依赖中断的 HAL 机制（时基、UART DMA、EXTI）全部瘫痪。
  2. 「程序卡死在某行之后」优先怀疑**中断被关**（PRIMASK/BASEPRI）和**时基不跑**（uwTick 不涨），用「在关键行之间插打印」的二分法能快速定位卡死点。
  3. FreeRTOS 调度器启动会自动开中断，这会掩盖「启动前中断关闭」的问题——凡是在 `osKernelStart()` 之前用 HAL 阻塞函数（HAL\_Delay/HAL\_UART\_Receive 超时等）都要警惕。

***

## 坑2：MicroLib 下带格式参数的 printf 走 `_write` 而非 `fputc`

* **现象**：排查坑1 时最初怀疑 `lcd.c` 里 `printf("LCD ID:%x\r\n", lcddev.id)` 卡死（因为它带 `%x` 格式参数）。

* **根因**：MicroLib 的 printf 分流机制：

  * **纯字符串/字面量** printf（如 `printf("hello\r\n")`）可能被优化成 `puts/fputc`，走 `fputc` 重定向。

  * **带格式参数**（`%d/%x/%s` 等）的 printf 走 **`_write`** **系统调用**（批量输出），**不走 fputc**。

  * 工程只在 main.c 实现了 `fputc`，没实现 `_write`，MicroLib 的默认 `_write` stub 会导致输出异常/卡死。

* **解决**：在 main.c 的 fputc 之后实现 `_write`，重定向到 huart1：

  ```c
  int _write(int fd, const char *ptr, int len)
  {
      (void)fd;
      if ((ptr != NULL) && (len > 0) && (huart1.Instance != NULL)) {
          (void)HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100U);
      }
      return len;
  }
  ```

  加空指针/长度/句柄防御检查，超时用 100ms（不用 0xFFFF）防止 UART 故障死锁。

* **教训**：

  1. MicroLib 工程 **fputc 和 \_write 都要实现**：fputc 管单字符，\_write 管 fputs/printf 批量输出。Bootloader 阶段只实现 fputc 能跑是因为它的 printf 多为字面量；APP 引入正点原子代码（带格式 printf）后必须补 \_write。
  2. 多任务环境下 printf 不带互斥锁，任务里应优先用 `uart_printf_mutex`；`_write` 只作为「调度器启动前 / 第三方代码」的安全网。

***

## 坑3：启动文件主栈只有 1KB，深调用链 + 大局部变量有溢出风险

* **现象**：排查阶段为排除栈因素，核对启动文件栈配置。

* **根因**：`startup_stm32f103xe.s` 的 `Stack_Size EQU 0x400`（1KB）。这是**主栈（MSP）**，用于中断、调度器启动前代码、HAL 调用链。`lcd_init()` 内有 `GPIO_InitTypeDef` + 2 个 `FSMC_NORSRAM_TimingTypeDef` 局部结构体，且调用链 `lcd_init → HAL_SRAM_Init → HAL_SRAM_MspInit` 较深，1KB 偏紧（Bootloader 早已按教训设为 4KB）。

* **解决**：`Stack_Size` 改为 `0x1000`（4KB），与 Bootloader 一致。

* **教训**：

  1. 主栈（startup 文件 Stack\_Size）和 FreeRTOS 任务栈（osThreadAttr.stack\_size）是**两个独立的栈**，都要够大。主栈负责中断嵌套和调度器启动前代码，不能按任务栈的思路省。
  2. 大结构体（SRAM/FSMC InitTypeDef、flash\_param\_t 等）在 Bootloader/启动阶段尽量声明 `static` 放 BSS，避免压栈。

***

## 坑4：HardFault\_Handler 静默死循环，崩溃无任何线索

* **现象**：怀疑程序可能 HardFault，但默认 `HardFault_Handler` 是 `while(1){}` 空循环，崩了串口毫无输出，无法区分「卡死」还是「崩溃」。

* **解决**：在 `stm32f1xx_it.c` 的 HardFault\_Handler（USER CODE 标记内）加崩溃输出：

  ```c
  void HardFault_Handler(void)
  {
    /* USER CODE BEGIN HardFault_IRQn 0 */
    extern void uart_panic_write(const char *reason, const char *detail);
    uart_panic_write("HardFault", "crash");
    /* USER CODE END HardFault_IRQn 0 */
    while (1) { }
  }
  ```

* **教训**：Fault 处理函数（HardFault/MemManage/BusFault/UsageFault）默认都是静默死循环，移植阶段务必接上串口/LED 告警，否则「死机」和「死循环」无法区分，调试效率极低。


***

## 坑5：FreeRTOS 堆（configTOTAL\_HEAP\_SIZE）太小，加任务后 heap exhausted

* **现象**：加入触摸任务 TaskTouch 后烧录，串口打印 `[FATAL] FreeRTOS heap exhausted` + `System halted.`，调度器没启动，LCD 白屏。

* **根因**：`FreeRTOSConfig.h` 的 `configTOTAL_HEAP_SIZE = 10240`（10KB）。新增 TaskTouch（栈 2048B）后，堆需求超过 10KB：

  * 各任务栈合计约 11.6KB（TaskLED 512 + Print 768 + KeyPoll 256 + SemHandle 384 + DHT11 1536 + ESP8266 2048 + LcdTest 2048 + Touch 2048 + defaultTask 512 + Idle/Timer \~1.5KB）

  * 加上 ~~11 个 TCB + 队列/互斥锁/信号量/事件组等内核对象约 2~~3KB

  * 总需求约 14KB > 10KB → `pvPortMalloc` 失败 → vApplicationMallocFailedHook 触发 panic。

* **解决**：CubeMX → Middleware → FREERTOS → Config parameters → `TOTAL_HEAP_SIZE` 从 10240 改为 **24576**（24KB），重新生成。STM32F103ZET6 有 64KB SRAM：主栈 4KB + 堆 24KB，剩余给全局变量仍很充裕。

* **教训**：

  1. FreeRTOS 堆（heap\_4 的 ucHeap）是**静态分配的全局数组**，占用 SRAM；所有任务栈、TCB、队列、信号量、互斥锁都从这个堆里 malloc。**每加一个任务都要评估堆余量**。
  2. 用 `xPortGetFreeHeapSize()` / 栈高水位监控（本工程的 App\_StackMonitorRegister + RTOS STATS）持续观察，不要等 malloc 失败才发现堆不够。
  3. 改堆大小务必在 CubeMX 里改并重新生成，直接改 FreeRTOSConfig.h 会被下次生成覆盖。

***

## D17 成果

* **LCD 显示移植完成**：正点原子 TFTLCD 驱动（lcd.c / lcd\_ex.c / lcdfont.h）成功移植进 OTA APP 工程，FSMC 16bit 并口（Bank4/NE4 + A10/RS）驱动 2.8 寸屏，蓝屏 + 文字 + 闪烁方块正常。

* **FSMC 纳入 CubeMX 管理**：在 .ioc 中正确配置 Bank4 SRAM、11bit 地址线、16bit 数据、读 5/7/0 写 2/5/0 时序，CubeMX 自动生成 fsmc.c 并启用 HAL\_SRAM\_MODULE，重新生成代码不再丢失 FSMC 配置。

* **电阻触摸移植完成**：XPT2046 软件 SPI 驱动（touch.c/app\_touch.c），5 引脚（PF10/PF11/PB2/PF9/PB1）全部空闲无冲突；DWT 微秒延时、5 次去极值 + 双次一致性滤波、LCD 总线互斥锁；TaskTouch 实现触摸画线 + 左上角清屏，串口打印原始 AD 值与像素坐标。

* **解决 3 个致命启动问题**：

  * Bootloader `__disable_irq()` 后 APP 未开中断 → HAL\_Delay 死循环（`__enable_irq()` 修复）。

  * MicroLib `_write` 缺失 → 带格式 printf 异常（补 `_write` 实现）。

  * FreeRTOS 堆 10KB 不足 → heap exhausted（扩到 24KB）。

* **工程健壮性提升**：主栈 1KB→4KB；HardFault\_Handler 接 uart\_panic\_write 崩溃告警；明确了「USER CODE 标记内才安全」「MspInit 归 CubeMX」「每次生成后 diff 检查」等 CubeMX 协作规范。

* 系统总任务数 `n_tasks=11`（新增 TaskLcdTest、TaskTouch），RTOS STATS 中各任务栈余量正常，编译 0 Error。

* **待办（后续 UI 阶段）**：触摸校准（四角 AD 采样反推 xfac/xc/yfac/yc，提升触摸精度与坐标对齐）；LCD ID 读时序余量持续观察。

