D13：OTA 容错测试与安全加固（CRC32 失败不写 flag + 擦除后读回验证 + 升级中断电自动进串口 IAP + last\_ota\_result 中断标志）（2026-08-28）
阶段归属：阶段5 WiFi OTA 远程升级 · Buffer 日 / 容错测试

***

## 坑1：IsAppValid() 只检查栈指针 → 升级中途断电后误跳半块 APP

* **现象**：T4 测试在 S6 收了 seq=0 和 seq=1 之后拔电源，重新上电后 Bootloader 打印 `APP valid, jumping to 0x08008000`，直接跳 APP 而不是进串口 IAP。跳进去后 APP 只写了前 2KB，后面 31KB 是 0xFF，APP 跑飞或 HardFault。

* **根因**：`IsAppValid()` 实现只检查 0x08008000 处的栈指针高 12 位是否等于 0x200（合法 RAM 地址）：

  ```c
  uint8_t IsAppValid(void) {
      uint32_t app_sp = *(volatile uint32_t *)APP_FLASH_START;
      if ((app_sp & 0xFFF00000) == 0x20000000) {  // 只看高12位
          return 1;
      }
      return 0;
  }
  ```

  S6 写了 seq=0（1024 字节）到 Flash，0x08008000 处已经是新固件的栈指针（合法值如 0x20001000）→ 高 12 位 = 0x200 → `IsAppValid()` 返回 true → Bootloader 认为 APP"有效"→ 跳 APP → 但 APP 只写了 2KB，后面 31KB 是 0xFF → 半块 APP 跑飞。

  * 旧 APP 完好时栈指针合法 → IsAppValid 正确返回 true

  * Flash 全擦除后是 0xFFFFFFFF → 高 12 位 = 0xFFF ≠ 0x200 → IsAppValid 正确返回 false

  * **但 S6 写了一小部分后，新固件的栈指针已经写入 0x08008000 → IsAppValid 错误返回 true**

* **排查过程**：

  1. 断电后重启看到 `APP valid` → 怀疑 IsAppValid 逻辑
  2. 读 IsAppValid 实现 → 只检查栈指针高 12 位
  3. 确认：S6 写 seq=0 后 0x08008000 处是合法栈指针 → IsAppValid 返回 true
  4. 旧 APP 完好时跳 APP 没问题，但半块 APP 也被认为"有效"→ 误跳

* **解决**：引入 `last_ota_result` 字段作为"升级中断标志"，在 S5 擦除 Flash **之前**写 `last_ota_result = 4`（4=升级进行中），升级成功后写 `last_ota_result = 0`。启动时检查：

  ```c
  /* boot_ota.c S5 擦除前 */
  {
      static flash_param_t p_mark;
      if (FlashParam_Load(&p_mark) == 0) {
          p_mark.last_ota_result = 4U;  /* 4=升级进行中 */
          FlashParam_Save(&p_mark);
      }
  }
  /* boot_ota.c S8 成功后 */
  s_param_work.last_ota_result = 0U;   /* 升级成功，清标志 */
  ```

  ```c
  /* main.c 启动检查 */
  if (FlashParam_Load(&g_param_buf) == 0 && g_param_buf.last_ota_result == 4U) {
      printf("[BOOT] OTA interrupted (last_ota_result=4). APP may be incomplete.\r\n");
      printf("[BOOT] Auto-entering Serial IAP rescue (Ymodem 115200 8N1)...\r\n");
      (void)IAP_ProcessSerial();
      while (1) { HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin); HAL_Delay(500); }
  }
  else if (IsAppValid()) {   /* 原来的 if 改 else if */
      /* 跳 APP */
  }
  ```

* **教训**：`IsAppValid()` 只能判断"栈指针是否合法"，不能判断"固件是否完整"。升级中断电后即使栈指针合法，APP 也可能是半块。必须引入**独立的升级状态标志**（`last_ota_result`），让 Bootloader 能区分"正常 APP"和"升级中断的半块 APP"。判别特征：APP 跳进去立刻 HardFault 或跑飞，但 IsAppValid 检查通过 → 优先怀疑 IsAppValid 的检查粒度不够，需要配合升级状态标志。

***

## 坑2：load\_ok 作用域问题 - 局部变量在块外不可访问

* **现象**：在 main.c 的 APP 无效分支前添加升级中断检查，编译报错 `error: #20: identifier "load_ok" is undefined`：

  ```c
  if (load_ok == 0 && g_param_buf.last_ota_result == 4U) {   /* 编译错误 */
  ```

* **根因**：`load_ok` 是 `BootMainProcess` 函数内一个 `{...}` 块的局部变量，作用域仅在该块内：

  ```c
  int BootMainProcess(void) {
      /* ... */
      {
          int load_ok = FlashParam_Load(&g_param_buf);   /* L113: load_ok 定义在这个块里 */
          if (load_ok == 0 && g_param_buf.ota_request_magic == MAGIC_OTA_REQUEST) {
              /* ... OTA 流程 ... */
          }
      }   /* L158: load_ok 作用域结束 */
      /* ... */
      if (load_ok == 0 && g_param_buf.last_ota_result == 4U) {   /* L191: load_ok 已失效 */
  ```

  块内的 `load_ok` 在 `}` 之后就被销毁了，L191 在块外面，访问不到。

* **排查过程**：

  1. 编译器报 `identifier "load_ok" is undefined`，定位到 L191
  2. 往上找 `load_ok` 定义 → 在 L113 的 `{...}` 块内
  3. 确认：块的作用域到 L158 的 `}` 结束，L191 在块外

* **解决**：在检查处重新加载参数区，不依赖 `load_ok` 变量：

  ```c
  /* 错误写法：引用块内已失效的 load_ok */
  if (load_ok == 0 && g_param_buf.last_ota_result == 4U)

  /* 正确写法：重新加载参数区，用全局变量 g_param_buf */
  if (FlashParam_Load(&g_param_buf) == 0 && g_param_buf.last_ota_result == 4U)
  ```

  `g_param_buf` 是全局变量，整个函数都能访问，重新 Load 一次即可。

* **教训**：C 语言中 `{...}` 块内定义的变量作用域仅限于该块，块外不可访问。**不要依赖块内的局部变量做块外的条件判断**。如果需要跨块使用，要么把变量提到块外（改为函数级局部变量），要么在使用处重新获取值。判别特征：编译报 `identifier is undefined` 但变量看起来"就在上面" → 查是不是在 `{...}` 块内定义的。

***

## 坑3：APP 端不清 last\_ota\_result → IAP 刷完后下次 Reset 又进 IAP 死循环

* **现象**：用 Ymodem 刷回 APP 成功后，APP 正常启动，但按 Reset 键重启后，Bootloader 又打印 `OTA interrupted (last_ota_result=4)` → 又进 IAP → 无限循环。

* **根因**：Bootloader 在 S5 擦除前把 `last_ota_result` 写成 4，S8 成功时写回 0。但如果通过 IAP 刷回（不是 WiFi OTA 成功），参数区的 `last_ota_result` 仍然是 4。APP 端 `FlashParam_InitOnBoot()` 只改了 `boot_count++` 和 `last_reset_reason`，**没清** **`last_ota_result`**：

  ```c
  /* APP 端 flash_param.c FlashParam_InitOnBoot() */
  g_param_buf_internal.boot_count++;
  g_param_buf_internal.last_reset_reason = RCC->CSR;
  /* last_ota_result 还是 4，没清！ */
  FlashParam_Save(&g_param_buf_internal);
  ```

  IAP 刷完后跳 APP → APP 启动 Save（`last_ota_result` 还是 4）→ Reset 重启 → Bootloader 看到 `last_ota_result==4` → 又进 IAP → 刷完跳 APP → Save（还是 4）→ Reset → 无限循环。

* **排查过程**：

  1. IAP 成功跳 APP 后 Reset → Bootloader 又进 IAP
  2. 读 APP 端 `FlashParam_InitOnBoot` → 发现只改了 boot\_count，没清 last\_ota\_result
  3. 确认：APP 正常启动 = 固件没问题，应该把 last\_ota\_result 清成 0

* **解决**：在 APP 端 `FlashParam_InitOnBoot()` 的 `boot_count++` 下面加一行：

  ```c
  g_param_buf_internal.boot_count++;
  g_param_buf_internal.last_ota_result = 0U;   /* APP正常启动=固件没问题，清升级中断标志 */
  g_param_buf_internal.last_reset_reason = RCC->CSR;
  ```

  APP 每次正常启动都会把 `last_ota_result` 清成 0，下次 Reset 重启 Bootloader 不会再误进 IAP。

* **教训**：参数区的状态标志**必须由产生它的一方负责清除**。Bootloader 写了 `last_ota_result=4`（升级进行中），APP 启动后应该清成 0（固件没问题）。如果 APP 不清，Bootloader 永远看到 4。这条规则通用——Flash 参数区的任何状态标志（`ota_request_magic`、`last_ota_result`、`ota_rollback_count`）都必须有明确的"设置方"和"清除方"，不能指望对方替你清。

***

## 坑4：断电时机不对 - 擦除前断电 APP 完好，擦除后断电才测得到中断

* **现象**：第一次做 T4 测试，在 S6 刚开始收 DATA 时拔电源，重启后 Bootloader 打印 `APP valid, jumping to 0x08008000`，直接跳 APP，没进 IAP。

* **根因**：断电时机太早——在 S5 `Erase OK` 之前断电，Flash 还没被擦，旧 APP 完好。`IsAppValid()` 返回 true，Bootloader 跳旧 APP，测不到"升级中断→APP 无效→自动 IAP"的路径。

  * S3/S4 断电：APP 完好，跳 APP ✅（正常，测不到中断）

  * S5 擦除中/擦除前断电：APP 完好，跳 APP ✅（正常，测不到中断）

  * **S5 擦除完成 + S6 收了至少 1 包之后断电**：APP 被擦且部分写入，IsAppValid 可能返回 true（新固件栈指针已写入），需要 last\_ota\_result=4 辅助判断 → 才能测到中断路径

* **排查过程**：

  1. 断电后重启看到 `APP valid` → 怀疑没擦到 Flash
  2. 回忆断电时串口最后打印 → 确认在 `Erase OK` 之前就拔了
  3. 确认：擦除前断电 = APP 完好 = 跳 APP = 测不到中断

* **解决**：断电时机必须在 `Erase OK` + `Erase verify OK` 之后，S6 收了 seq=0 之后再拔：

  ```
  [WIFI-OTA] [S5] Erase OK                        ← 擦除完成
  [WIFI-OTA] [S5] Erase verify OK                 ← D13 验证通过
  [WIFI-OTA] [S6] OTA   3% (1024/33336 bytes, seq=0)  ← 收了 1 包
  [WIFI-OTA] [S6] OTA   6% (2048/33336 bytes, seq=1)  ← 收了 2 包
  ← 看到这行之后再拔电源！
  ```

  此时 Flash 已被擦 + 写了 2 包 → APP 是半块 → last\_ota\_result=4 → 重启进 IAP。

* **教训**：**断电时机是 T4 测试的关键**。必须在"擦除完成 + 至少 1 包 DATA 写入"之后断电，才能同时满足两个条件：① last\_ota\_result=4（S5 擦除前写的）② APP Flash 不是全 0xFF（否则 IsAppValid 返回 false 也能进 IAP，但那是"APP 全空"不是"升级中断"的场景）。判别方法：串口看到 `Erase verify OK` 之后 `DATA seq=0` 之后再拔，此时 Flash 状态最典型——擦空了 + 写了一点，最能模拟真实的升级中途断电场景。

***

## 坑5：S5 CRC 预检查 MISMATCH - INFO 帧 CRC 与参数区期望不一致

* **现象**：做 T2 测试时，发 OTA 命令后串口打印：

  ```
  [WIFI-OTA] [S5] !!! MISMATCH: param crc=0x344BDD29 size=33336, server crc=0x34518D29 size=33336
  ```

  直接进 FAIL，没擦 Flash，没写参数区。

* **根因**：MQTT.fx 发的 OTA JSON 命令里 `crc` 字段（`0x344BDD29`）和 Python 服务器加载的 bin 文件实际 CRC（`0x34518D29`）不一致。APP 从 MQTT 命令解析出 crc 写进参数区，Bootloader 在 S5 阶段用参数区的 crc 发 HELLO 给服务器，服务器回 INFO 时带的是 bin 的真实 crc，Bootloader 比对两个 crc → 不一致 → MISMATCH → FAIL。

  * 这不是 bug，是 D13 的安全机制在正常工作——CRC 不匹配时连 Flash 都不擦，旧 APP 完好。

* **排查过程**：

  1. 看 MISMATCH 日志 → param crc ≠ server crc
  2. 查 Python 服务器启动时打印的 `fw_crc=0x34518D29` → 服务器 bin 的真实 CRC
  3. 查 MQTT.fx 发的 OTA JSON → crc 字段填的是 `0x344BDD29`（十进制 877427369）
  4. 确认：手填 crc 时输错了

* **解决**：让 MQTT 命令里的 crc 和服务器 bin 的 CRC 一致。Python 服务器启动时会打印 `fw_crc=0x...`，记下来填到 MQTT 命令里。

* **教训**：S5 阶段的 CRC 预检查是 D13 的第一道安全防线——在擦除 Flash 之前就校验了"参数区期望的 CRC"和"服务器声明的 CRC"是否一致。这条检查比 S8 的三重 CRC 校验更早拦住了不匹配的请求，避免了擦除 Flash 的开销和风险。**串口看到 MISMATCH 不是 bug，是安全机制在工作**。判别特征：T2 测试故意用错 crc 触发 MISMATCH → 验证安全机制生效 → 用正确 crc 发命令就不会触发。

***

## 坑6：编译错误 - 中文注释乱码导致 ARMCC 编译器报错

* **现象**：在 boot\_ota.c 的擦除验证代码块里加了中文注释，Keil 编译报错 `error: #20: identifier undefined` 或 `warning: #1-D: last line of file ends with a newline` 等莫名其妙的错误，指向中文注释所在行。

* **根因**：ARMCC 编译器不支持 UTF-8 with BOM，对非 ASCII 字符的处理依赖源文件编码。如果源文件是 UTF-8 with BOM 或 GBK 编码，中文注释可能被编译器误解析为指令或产生编码错误。之前 D9 阶段踩过同一个坑（硬约束：ARMCC 不支持 UTF-8 with BOM）。

* **排查过程**：

  1. 编译报错指向中文注释行
  2. 检查源文件编码 → UTF-8 with BOM
  3. 确认：ARMCC 对 BOM 敏感

* **解决**：

  1. 把源文件转成 UTF-8 without BOM（用 VS Code 的 `Save with Encoding` → `UTF-8`）
  2. 或者把中文注释改成英文（更稳妥）
  3. 或者在 Keil 的 `Options for Target` → `C/C++` → `Misc Controls` 加 `--locale=utf8`

* **教训**：ARMCC 编译器的编码兼容性是硬约束，D9 阶段的坑在 D13 又遇到了。**任何新增代码的源文件必须是 UTF-8 without BOM**，中文注释尽量少用或用英文。这条规则在整个项目中持续有效——每次改完代码保存前检查编码。

***

## D13 成果

* **CRC32 校验失败不写 flag（已有机制，D13 验证通过）**：S8 校验失败时 `break` 跳过参数区写入 + main.c L130 升级前预清 magic → 双重保证失败时不留 OTA 请求标志，重启 Bootloader 看不到 magic → 直接跳旧 APP（完好），不进 WiFi OTA 流程。S5 CRC 预检查（MISMATCH）更早拦住不匹配请求，连 Flash 都不擦。

* **擦除后读回验证（新增）**：S5 `FLASH_EraseAppArea()` 返回 OK 后，遍历 APP 区每个页的起始地址读 word，检查是否为 `0xFFFFFFFF`。如果有任何一页没擦干净，打印失败地址和值，进入 FAIL，不继续写 Flash。这道检查在 HAL 返回 OK 之上增加了物理验证，防止 Flash 物理损坏或擦除中断导致的擦除不完整。

* **升级中断电自动进串口 IAP（新增）**：引入 `last_ota_result` 字段作为升级状态标志。S5 擦除前写 `last_ota_result=4`（升级进行中），S8 成功后写 `last_ota_result=0`。Bootloader 启动时检查 `last_ota_result==4` → 自动调 `IAP_ProcessSerial()` 进串口 IAP，不需要用户按 KEY0。APP 端 `FlashParam_InitOnBoot()` 启动时清 `last_ota_result=0`，防止 IAP 刷完后下次 Reset 又进 IAP 死循环。

* **IsAppValid 检查粒度补全**：原 `IsAppValid()` 只检查栈指针高 12 位，升级中断电后写了 seq=0 新固件栈指针合法 → 误判为"有效"。通过 `last_ota_result` 独立标志补全了检查粒度——即使栈指针合法，只要 `last_ota_result==4` 就认为 APP 是半块，强制进 IAP。

* **T1\~T4 全链路验证通过**：

  * T1 正常 OTA：`Erase verify OK` 打印 + 升级成功 + 版本号更新

  * T2 故意用错 bin：S5 MISMATCH → FAIL → 无 Flash 擦除/写入 → Reset 跳旧 APP → MQTT 正常重连

  * T3 故意断网（S3 阶段）：WiFi join timeout → FAIL → 无 Flash 擦除/写入 → Reset 跳旧 APP → MQTT 正常重连

  * T4 升级中断电：S6 seq=1 后拔电源 → Reset 打印 `OTA interrupted` + `Auto-entering Serial IAP rescue` → Ymodem 刷回 → 跳 APP → APP 清 `last_ota_result=0` → 下次 Reset 正常跳 APP

* **代码改动量**：Bootloader 新增约 25 行（擦除验证 + last\_ota\_result 读写 + 启动检查），APP 新增 1 行（`last_ota_result=0`），共约 26 行防御性代码。

* **编译 0 Error 0 Warning**，Bootloader + APP 双工程干净通过。**硬件实测全链路通过**：T1\~T4 四个场景全部验证通过，任何异常情况下板子都不变砖，KEY0+Reset 能救回。

* **项目安全闭环**：D12 实现了 WiFi OTA 远程升级的全链路功能，D13 在此基础上增加了三道安全锁——① CRC 不匹配连 Flash 都不擦 ② 擦除后读回验证物理完整性 ③ 升级中断电自动进 IAP 救回。配合 D10 的 KEY0+Reset 强制 IAP，板子在任何异常场景下都有救砖通道。

