D12：WiFi OTA 远程升级（Bootloader 端 ESP8266 TCP 拉固件 + 被动接收模式 + ACK 停等流控 + 三重 CRC 校验）（2026-08-24 ~ 2026-08-27）
阶段归属：阶段5 WiFi OTA 远程升级 · Bootloader 端拉固件层

---

## 坑1：Bootloader 主栈被 4KB flash_param_t 局部变量吃光 → 进 OTA 流程立刻卡死

- **现象**：APP 写 `ota_request_magic=0x4F544131` 软复位后，Bootloader 打印 `[BOOT] OTA_REQUEST FLAG detected!` 就死掉，串口再无输出，LED0 不闪。把 `WiFi_IAP_Process()` 整段注释掉只留 `for(;;) HAL_Delay(200)` 又能正常双闪，说明问题出在 OTA 流程入口本身，不是外设初始化。
- **根因**：D12 在 `boot_ota.c` 顶部声明了一堆大缓冲区给状态机用：
  ```c
  /* 错误写法：全部是局部变量，进 WiFi_IAP_Process 立刻爆栈 */
  flash_param_t s_param_work;        /* 4096 字节 */
  uint8_t s_at_buf[2048];            /* 2KB，AT 命令 + payload 复用 */
  uint8_t s_tx_payload[2048];        /* 2KB，HELLO/DATA 副本 */
  uint8_t s_rx_line[512];           /* 512B，单行解析 */
  uint8_t s_json_buf[1024];         /* 1KB，JSON 拼装 */
  uint8_t s_uart_rx_ring[4096];     /* 4KB，USART2 环形 */
  ```
  加起来约 13KB，而 Bootloader 的主栈在 `startup_stm32f103xx.s` 里默认 `STACK EQU 0x00000400`（1KB），即使改到 0x1000（4KB）也装不下 13KB → 进函数的瞬间踩栈底 canary → HardFault 或调度器链表损坏 → 卡死。
- **排查过程**：
  1. 看串口只到 `OTA_REQUEST FLAG detected!` 就没下文 → 锁定死在 `WiFi_IAP_Process` 入口
  2. 读代码发现一堆大数组都是局部变量 → 算总栈占用 13KB
  3. 打开 .s 文件看 `STACK EQU 0x400` → 1KB 主栈，差 12 倍
  4. 把栈改到 0x1000（4KB）还是不够（13KB > 4KB）
- **解决**：**所有大缓冲区一律改 `static` 放 BSS 段**，不占栈：
  ```c
  /* 正确写法：static 放 BSS，主栈只压函数调用开销几十字节 */
  static flash_param_t s_param_work;
  static uint8_t s_at_buf[2048];
  static uint8_t s_tx_payload[2048];
  static uint8_t s_rx_line[512];
  static uint8_t s_json_buf[1024];
  static uint8_t s_uart_rx_ring[4096];
  static volatile uint16_t s_uart_rx_head, s_uart_rx_tail;
  ```
  同时把 `startup_stm32f103xx.s` 的 `STACK EQU 0x1000`（4KB）作为硬约束，永远不低于这个值。
- **教训**：Bootloader 没有 RTOS 任务概念，所有变量都压在唯一的主栈上。**任何 > 64 字节的缓冲区都必须 `static`**。判别标准：函数内声明的数组只要超过 256 字节就要警惕，超过 512 字节基本必须改 `static`。`flash_param_t` 这种 4KB 结构体放栈上是 100% 爆栈。这条规则和 D9 坑3（`FlashParam_Save` 栈溢出）同源——只是 D12 的缓冲区更多更大，必须批量改 `static`。

---

## 坑2：S1 AT 探测失败 - ESP8266 启动时序 + RX 残留 + 回显污染

- **现象**：进 S1 阶段发 `AT\r\n`，ESP8266 完全无响应，3 次重试全失败，直接跳 FAIL。串口看不到 `OK`，偶尔看到 `busy p...`（busy processing）或乱码。
- **根因**（三重原因叠加）：
  1. **启动时序**：APP 软复位 STM32 时虽然会先发 `AT+RST` 复位 ESP8266，但 Bootloader 启动后 ESP8266 可能还在初始化（射频校准、加载 AT 固件），收到的 AT 被直接丢弃。
  2. **RX 残留**：ESP8266 复位期间会往 USART2 吐一堆启动 banner（`wdt reset`、`csum 0x...`、`ready`、`WIFI DISCONNECT` 等），这些数据堆在 RX 缓冲区，Bootloader 发 AT 后读到的"回复"其实是旧 banner，匹配不到 `OK`。
  3. **回显污染**：ESP8266 默认 `ATE1` 开回显，`AT+CIPSEND=46\r\n` 的 46 字节 payload 会被 ESP8266 原样回显给 STM32 → 解析 +IPD 时 payload 字节混进 AT 回显，JSON 解析错乱。
- **排查过程**：
  1. 单独用 USB-TTL 接 ESP8266 发 AT 能回 `OK` → 接线/波特率没问题
  2. STM32 上发 AT 无响应 → 怀疑启动时序，加 `HAL_Delay(3000)` 等 ESP8266 启动，偶尔能通但不稳定
  3. 在发 AT 前打印 `s_uart_rx_ring` 内容 → 发现一堆旧 banner 残留 → RX 没清干净
  4. 发 `AT+CIPSEND=46` 后看 RX → 发现 payload 被回显，JSON 解析失败
- **解决**（三件套同时上）：
  ```c
  /* 1. S1 入口先清 RX 300ms，把 ESP8266 启动 banner 全冲掉 */
  uart_flush_rx(300);

  /* 2. AT 探测加 3 次重试，每次 500ms 间隔 */
  for (int retry = 0; retry < 3; retry++) {
      ESP8266_SendRaw("AT\r\n", 4);
      if (at_cmd_expect("OK", 2000)) break;
      HAL_Delay(500);
  }

  /* 3. 关回显，避免 CIPSEND 的 payload 被原样吐回来污染 JSON 解析 */
  at_cmd_expect("ATE0\r\n", "OK", 1000);
  ```
- **教训**：AT 指令交互有三条铁律：**① 发指令前先清 RX，避免旧数据干扰 ② 外设模块有启动稳定期，必须延时或重试 ③ 回显要关**。ESP8266 的 `ATE0` 是 OTA 流程必不可少的一步——D4 阶段主动接收模式下回显影响不大，但 D12 用 `AT+CIPSEND` 发 JSON payload 时，回显的 payload 字节会混进 +IPD 解析，直接导致 JSON 解析失败。

---

## 坑3：S5 HELLO 帧 payload 被 AT+CIPSEND 命令覆盖 - cip_send 用局部缓冲

- **现象**：S5 阶段 STM32 发 `AT+CIPSEND=46\r\n` 后 ESP8266 回 `>`，STM32 紧接着发 HELLO JSON（`{"type":"HELLO","dev":"dev001",...}`），ESP8266 回 `Recv 46 bytes` `SEND OK`，但 Python 服务器 `recv_line` 死等不到任何数据，30 秒超时。
- **根因**：调用者把 HELLO JSON 拼到全局 `s_at_buf`，然后调 `cip_send(s_at_buf, 46)`。`cip_send` 内部又用同一个 `s_at_buf` 拼装 `AT+CIPSEND=46\r\n` 命令：
  ```c
  /* 错误写法：AT 命令拼到 s_at_buf，覆盖了调用者放好的 payload */
  int cip_send(char *buf, int len) {
      snprintf(s_at_buf, sizeof(s_at_buf), "AT+CIPSEND=%d\r\n", len);
      ESP8266_SendRaw(s_at_buf, strlen(s_at_buf));   /* 发 AT 命令 */
      /* 等 '>' */
      strcpy(s_at_buf, buf);                         /* ← 这里 buf 就是 s_at_buf */
      ESP8266_SendRaw(s_at_buf, len);                /* 发 payload */
  }
  ```
  调用 `cip_send(s_at_buf, 46)` 时 `buf == s_at_buf`，`strcpy(s_at_buf, buf)` 是自拷贝，但此时 `s_at_buf` 里已经是 `AT+CIPSEND=46\r\n` 字符串 → 发出去的 payload 是 AT 命令字符串而不是 HELLO JSON。服务器收到的不是 JSON 是 `AT+CIPSEND=46` → 无法解析。
- **排查过程**：
  1. STM32 侧日志 `Recv 46 bytes SEND OK` → 以为发送成功
  2. Python 侧 `recv_line` 30 秒超时 → 怀疑网络问题
  3. 在 Python 端 `conn.recv` 打印原始字节 → 发现收到的是 `AT+CIPSEND=46\r\n` 字符串，不是 JSON
  4. 读 `cip_send` 代码 → 发现 AT 命令和 payload 共用 `s_at_buf` → 锁定自覆盖
- **解决**：`cip_send` 用**局部小缓冲**拼 AT 命令，payload 直接用调用者传入的指针：
  ```c
  int cip_send(const char *payload, int len) {
      char cmd[24];    /* 局部缓冲，只装 AT 命令 */
      int cmd_len = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", len);
      ESP8266_SendRaw(cmd, cmd_len);
      if (!at_cmd_expect(">", 2000)) return -1;
      ESP8266_SendRaw(payload, len);    /* payload 用原指针，不拷贝 */
      return at_cmd_expect("SEND OK", 3000) ? 0 : -1;
  }
  ```
- **教训**：AT 指令 + payload 的发送函数里，**AT 命令拼装缓冲和 payload 缓冲必须物理隔离**。最稳妥的写法是 AT 命令用局部小缓冲（24 字节够装 `AT+CIPSEND=65535\r\n`），payload 用调用者传入的 `const char *` 指针直传，不做任何拷贝。判别特征：日志显示 `SEND OK` 但服务器收不到数据，优先查 `cip_send` 内部是不是用同一个缓冲拼 AT 命令和 payload。

---

## 坑4：S5 INFO 接收超时 - HELLO 末尾缺 \r\n + Python recv_line 超时不协调

- **现象**：修完坑3后 Python 服务器能收到 HELLO 了，但 STM32 发完 HELLO 后等 INFO 10 秒超时，串口打印 `[WIFI-OTA] [S5] !!! INFO timeout`。Python 端日志显示收到 HELLO 并解析成功，正在发 INFO，但 STM32 侧就是收不到。
- **根因**（双重原因）：
  1. **HELLO 末尾缺 `\r\n`**：STM32 的 HELLO JSON 是 `{"type":"HELLO","dev":"dev001"}` 结尾只有 `}` 没有 `\r\n`。Python 的 `recv_line` 用 `socket.recv` 累积字节直到遇到 `\n` 才返回，永远等不到行尾 → 15 秒后超时返回部分数据，但此时 STM32 早就先超时了。
  2. **超时不协调**：Python `recv_line` 超时 15 秒，STM32 等 INFO 10 秒 → STM32 先超时跳 FAIL，Python 还在等行尾。
- **排查过程**：
  1. STM32 日志 `INFO timeout 10000 ms` → STM32 等 INFO 超时
  2. Python 日志 `RX HELLO: {"type":"HELLO","dev":"dev001"}` → Python 收到了 HELLO 并解析成功
  3. 但 Python 发 INFO 后 STM32 没响应 → 怀疑 STM32 没收到 INFO
  4. 在 STM32 加打印 `s_uart_rx_ring` 内容 → 发现 ring 里根本没有 INFO 字节
  5. 怀疑 Python 没发 INFO，加 `print("TX INFO")` → Python 打印了 TX INFO 但 STM32 还是没收到
  6. 重新看 Python 的 `recv_line` 实现 → 发现它等 `\n`，但 HELLO 没有 `\n` → `recv_line` 阻塞 15 秒才返回，此时 STM32 已超时
- **解决**：
  ```c
  /* STM32 侧：HELLO JSON 末尾加 \r\n */
  int len = snprintf(s_tx_payload, sizeof(s_tx_payload),
      "{\"type\":\"HELLO\",\"dev\":\"%s\",\"fw_size\":%lu,\"fw_crc\":0x%08lX}\r\n",
      OTA_DEVICE_ID, expect_size, expect_crc);
  cip_send(s_tx_payload, len);
  ```
  ```python
  # Python 侧：recv_line 超时改 3 秒（短于 STM32 的 10 秒），超时返回部分数据
  def recv_line(conn, timeout=3):
      data = b''
      conn.settimeout(timeout)
      while b'\n' not in data:
          try:
              chunk = conn.recv(256)
          except socket.timeout:
              return data.decode('utf-8', errors='replace')  # 超时返回部分
          if not chunk:
              return data.decode('utf-8', errors='replace')
          data += chunk
      return data.decode('utf-8', errors='replace')
  ```
- **教训**：跨语言通信协议的**行尾分隔符必须双方一致**。STM32 发 JSON 必须带 `\r\n`，Python 才能用 `recv_line` 正确分割。同时**Python 端超时要短于 STM32 端超时**——STM32 等 INFO 10 秒，Python `recv_line` 必须 < 10 秒（用 3 秒），否则 STM32 先超时跳走，Python 还在阻塞等行尾，两边时序错位。超时返回部分数据是关键容错——即使没收到 `\n`，至少把已收到的字节返回，不浪费已传输的数据。

---

## 坑5：S5 阶段 ORE 丢 INFO - 主动接收模式 + printf 阻塞导致 USART2 Overrun

- **现象**：修完坑4后 Python 能发 INFO 了，但 STM32 解析 INFO 失败，串口打印 `INFO parse fail` 或 `INFO size=0 crc=0`。加调试打印后现象更严重——打印越多越收不到 INFO。
- **根因**：ESP8266 主动接收模式（`AT+CIPRECVMODE=0`，默认）下，收到服务器数据会主动推 `+IPD,<len>:<data>` 到 USART2。STM32 此时在 `printf` 打印调试信息（`printf("[WIFI-OTA] [S5] RX: %s", line)`），`printf` 内部调 `HAL_UART_Transmit` 阻塞等 USART1 TXE（每字节 87μs @115200）。期间 USART2 RXNE 触发但 CPU 在 busy-wait → 没及时读 DR → ORE 置位 → 后续字节全部丢失 → INFO JSON 不完整。
  - STM32F1 的 USART2 **没有硬件 FIFO**，RXNE 置位后必须在下一个字节到达前读 DR，否则 ORE。
  - `printf` 一个 80 字节的调试行要阻塞 7ms（80 × 87μs），ESP8266 @115200 连发 INFO 期间 7ms 足够丢几十个字节。
- **排查过程**：
  1. INFO 解析失败，加 `printf` 打印 raw bytes → 现象更严重
  2. 怀疑 printf 阻塞，注释掉所有 printf → INFO 解析成功
  3. 读 USART2 的 `huart2.ErrorCode` → ORE 位置 1
  4. 确认根因：printf 阻塞期间 USART2 ORE
- **解决**：引入 **USART2 中断 + 4KB 环形缓冲区**，把"接收字节"和"解析协议"解耦：
  ```c
  #define OTA_UART_RX_RING_SIZE 4096U
  #define OTA_UART_RX_RING_MASK (OTA_UART_RX_RING_SIZE - 1U)
  static volatile uint8_t  s_uart_rx_ring[OTA_UART_RX_RING_SIZE];
  static volatile uint16_t s_uart_rx_head;
  static volatile uint16_t s_uart_rx_tail;
  static volatile uint32_t s_uart_rx_overflow;   /* ORE 计数 */

  /* USART2 RX 中断：只做 3 件事，绝对不 printf */
  void BootOta_UART_IRQHandler(void) {
      uint32_t sr = huart2.Instance->SR;
      if ((sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_NE |
                 USART_SR_FE | USART_SR_PE)) != 0U) {
          uint8_t b = (uint8_t)(huart2.Instance->DR);   /* 读 DR 清 RXNE 和 ORE */
          uint16_t next = (uint16_t)((s_uart_rx_head + 1U) & OTA_UART_RX_RING_MASK);
          if (next != s_uart_rx_tail) {
              s_uart_rx_ring[s_uart_rx_head] = b;
              s_uart_rx_head = next;
          } else {
              s_uart_rx_overflow++;    /* 环满，丢弃 */
          }
      }
  }
  ```
  主循环用 `uart_rx_pop()` 从 ring 取字节，`printf` 阻塞期间中断仍持续收字节进 ring，INFO 不丢。
- **教训**：STM32F1 USART2 **无硬件 FIFO**，任何 > 1 字节时间的阻塞操作都会导致 ORE。`printf` 在 115200 下每字节 87μs，打印 80 字节要 7ms，必丢字节。**USART 接收期间禁止 printf 阻塞**，必须用中断 + 环形缓冲区。中断里绝对不能 `printf`、不能写 Flash、不能等超时——只搬字节。这条规则和 D10 坑2（Ymodem 阻塞接收 ORE）同源，只是 D12 是 USART2 不是 USART1。

---

## 坑6：S6 DATA 大面积丢包 - 主动推送 + Flash_WriteBuf 阻塞 → 帧间隔方案与被动模式

- **现象**：进 S6 阶段后串口打印 `[WIFI-OTA] [S6] !!! DATA recv timeout 30000 ms. got=0/32680` 或 `got=3268/32680`（只收到一小部分）。Python 端日志显示 INFO 已发、DATA 已发完 32 包，但 STM32 只收到 0~3 包就卡住。
- **根因**：ESP8266 主动接收模式下，Python 服务器连发 32 包 DATA，ESP8266 持续推 `+IPD,<len>:<base64data>` 到 USART2。STM32 收到一包后调 `FLASH_WriteBuf` 写 1KB 到 Flash，写 Flash 期间 CPU stall ~12ms（STM32F1 同一 Bank 取指/写 Flash 互斥）。12ms 内 ESP8266 @115200 连发约 138 字节（12ms × 115200/8/1000），USART2 没有硬件 FIFO → RXNE 累积 → ORE → 字节丢失 → 帧不完整 → 解析失败 → 后续所有包全部错位丢弃。
  - `FLASH_WriteBuf` 调 `HAL_FLASH_Program`，内部 `FLASH_WaitForLastOperation` busy-wait 等 Flash 完成，CPU 完全阻塞。
  - STM32F1 USART2 的 RXNE 中断虽然能进，但 ISR 搬字节到 ring 后主循环还在 stall 等 Flash → ring 溢出 → 仍然丢。
- **排查过程**：
  1. 加 S6-ACK-DIAG v2 打印每包 ORE 计数 → 发现每包写 Flash 后 ORE +几十
  2. 加 v3 打印 ring_head/tail → 发现 Flash 写入期间 ring 持续增长但主循环不消费 → ring 满溢
  3. 确认根因：Flash 写入阻塞 + ESP8266 主动推送 → USART2 ORE/ring 溢出
- **解决方案**（两条路，最终选 B）：
  - **方案 A（简单）**：Python 服务器加大帧间隔 `FRAME_GAP_MS = 200`，给 STM32 12ms 写 Flash + 余量。缺点：464KB 固件 32 包要 6.4 秒，传输慢；且仍依赖 ESP8266 主动推送，边界不稳定。
  - **方案 B（最终采用）**：启用 **ESP8266 被动接收模式**（`AT+CIPRECVMODE=1`），STM32 主动拉取数据：
    ```c
    /* S5 擦完 Flash 后切被动模式 */
    at_cmd_expect("AT+CIPRECVMODE=1\r\n", "OK", 2000);

    /* S6 循环：主动查 + 主动拉 */
    while (state == BOOT_OTA_RECV_DATA) {
        at_cmd_expect("AT+CIPRECVLEN?\r\n", "+CIPRECVLEN:", 500);
        /* 解析 len，>0 才拉 */
        if (len > 0) {
            snprintf(cmd, sizeof(cmd), "AT+CIPRECVDATA=%d\r\n", len);
            at_cmd_expect(cmd, "+CIPRECVDATA:", 1000);
            /* 解析拉到的 TCP 数据，base64 解码，写 Flash */
        }
    }
    ```
    被动模式下 ESP8266 **不主动推 +IPD**，数据缓存在 ESP8266 内部 TCP buffer，STM32 写 Flash 期间 USART2 完全安静 → 不会 ORE → 不会丢包。
- **教训**：STM32F1 USART2 无硬件 FIFO + Flash 写入 CPU stall 是**结构性矛盾**，主动推送模式下无解。唯一出路是让 ESP8266 不主动推——即 `AT+CIPRECVMODE=1`。被动模式下 STM32 完全掌控拉取时机，写 Flash 时 ESP8266 安静，写完再拉下一包。这是整个 D12 最关键的架构决策，没这一步 S6 永远过不去。

---

## 坑7：S6 只收 DATA0 - 主动模式 ACK0 后 DATA1 消失 → 被动模式 + ACK 停等流控

- **现象**：用方案 A（加大帧间隔）后能收到前几包，但收到 DATA0 写 Flash 发 ACK0 后，DATA1 永远等不到。串口打印 `seq=0 ack=0` 后就卡住，Python 端显示 ACK0 已收，DATA1 已发，但 STM32 收不到。
- **根因**：主动模式下，STM32 发 `AT+CIPSEND=10` 回 ACK0 给服务器，ESP8266 处理 CIPSEND 期间（~50ms），服务器紧跟着发的 DATA1 被 ESP8266 缓存，等 CIPSEND 一完成 ESP8266 立刻把 DATA1 的 `+IPD` 推给 USART2。但此时 STM32 刚从 CIPSEND 流程出来，状态机还在切换，ring 可能已满或解析器未就绪 → DATA1 的 `+IPD` 帧丢失。
- **排查过程**：
  1. S6-ACK-DIAG v4 显示 ACK0 发送后 ring_head 短暂增长然后停止 → DATA1 没进 ring
  2. Python 端确认 DATA1 已发 → 怀疑 ESP8266 主动推送时机和 STM32 状态机不同步
  3. 切到被动模式（坑6 方案 B）后 DATA1 能收到 → 确认是主动推送时序问题
- **解决**：被动模式 + **ACK 停等流控**（Server 收 ACK 才发下一包）：
  ```c
  /* STM32 侧：每包写完 Flash 发 ACK */
  snprintf(ack, sizeof(ack), "{\"type\":\"ACK\",\"seq\":%d}\r\n", seq);
  cip_send(ack, strlen(ack));
  /* 然后主动拉下一包 AT+CIPRECVDATA */
  ```
  ```python
  # Python 侧：收到 ACK 才发下一包
  for seq in range(packets):
      send_json(conn, {"type":"DATA","seq":seq,"data":b64})
      ack = recv_line(conn, 30)   # 等 ACK
      if ack is None or '"seq":%d' % seq not in ack:
          print(f"[OTA] !!! ACK{seq} timeout/missing")
          return
      time.sleep(FRAME_GAP_MS / 1000.0)   # 帧间隔兜底
  ```
  停等流控保证：STM32 发完 ACK 后主动拉下一包，ESP8266 不会在 ACK 和下一包之间抢推数据。
- **教训**：ACK 停等协议的核心价值不是"流控"本身，而是**让发送端和接收端的时序完全解耦**。主动推送模式下，发送端发完一包就发下一包，接收端写 Flash 期间数据堆积丢失；停等模式下，发送端必须等 ACK 才发下一包，接收端写完 Flash 主动拉下一包，两边节奏完全由接收端掌控。代价是传输慢（每包往返 RTT），但 464KB 固件 32 包也就 30 秒，可接受。

---

## 坑8：Flash_WriteBuf 返回值误判 - HAL_OK vs write_len 导致每包"写失败"

- **现象**：S6 阶段每包写 Flash 后串口都打印 `[WIFI-OTA] [S6] !!! Write Flash FAIL seq=0`，但实际 Flash 里数据是写进去的（最后 CRC 校验能过）。日志和实际结果矛盾，排障方向被严重误导。
- **根因**：`FLASH_WriteBuf` 返回 `HAL_StatusTypeDef`（`HAL_OK=0` / `HAL_ERROR=1` / `HAL_BUSY=2` / `HAL_TIMEOUT=3`），但判断条件写成：
  ```c
  /* 错误写法：HAL_OK(0) != 1024 永远为真 → 误判失败 */
  if (FLASH_WriteBuf(addr, buf, 1024) != 1024) {
      printf("[WIFI-OTA] [S6] !!! Write Flash FAIL seq=%d\r\n", seq);
      state = BOOT_OTA_FAIL;
      break;
  }
  ```
  `HAL_OK = 0`，`0 != 1024` 永远为真 → 每包都误判失败 → 状态机跳 FAIL。但 Flash 实际写成功了，只是逻辑误判。
- **排查过程**：
  1. 日志显示每包 FAIL，但 S8 CRC 校验居然过了 → 矛盾
  2. 读 `FLASH_WriteBuf` 头文件声明 → 返回 `HAL_StatusTypeDef` 不是 `int` 字节数
  3. 读判断条件 → `!= 1024` 是按"返回写入字节数"的旧假设写的
  4. 确认根因：返回值类型理解错误
- **解决**：
  ```c
  /* 正确写法：和 HAL_OK 比较 */
  if (FLASH_WriteBuf(addr, buf, 1024) != HAL_OK) {
      printf("[WIFI-OTA] [S6] !!! Write Flash FAIL seq=%d\r\n", seq);
      state = BOOT_OTA_FAIL;
      break;
  }
  ```
- **教训**：HAL 库函数返回值类型必须查头文件，不能按函数名猜语义。`FLASH_WriteBuf` 听起来像"写缓冲"，容易误以为是"返回写入字节数"，实际返回 `HAL_StatusTypeDef` 状态码。判别特征：日志显示失败但实际结果成功（CRC 过/数据在），优先查返回值判断条件是不是按字节数比较。这条规则通用——所有 `HAL_xxx` 函数先看 `stm32f1xx_hal_def.h` 的 `HAL_StatusTypeDef` 枚举。

---

## 坑9：READY 握手 + APP 复位前 ESP8266 预复位 - 双 MCU 时序协调

- **现象 A**：STM32 发 HELLO 后 Python 立刻发 DATA，但 STM32 此时正在擦 464KB Flash（~7 秒），擦完才开始解析，发现 DATA 早就堆在 ESP8266 buffer 里溢出丢失。
- **现象 B**：APP 写完参数区直接 `NVIC_SystemReset()`，Bootloader 启动后发 AT，ESP8266 回复一堆旧 TCP 残留数据（`+IPD,...` 是 OTA 命令的回包），干扰 S1 探测。
- **根因**：
  - A：Flash 擦除期间 CPU stall，无法接收数据，但服务器不知道 STM32 在擦 Flash，紧跟着发 DATA 导致堆积。
  - B：ESP8266 是独立 MCU，STM32 软复位不会复位 ESP8266，旧 TCP 连接和 buffer 残留干扰新流程。
- **解决**：
  ```c
  /* A：STM32 擦完 Flash 后发 READY 帧通知服务器 */
  FLASH_EraseAppArea();   /* 7 秒 */
  {
      int len = snprintf(s_tx_payload, sizeof(s_tx_payload),
          "{\"type\":\"READY\",\"dev\":\"%s\"}\r\n", OTA_DEVICE_ID);
      cip_send(s_tx_payload, len);
  }
  /* 服务器收到 READY 才发 DATA */
  ```
  ```python
  # Python 端：等 READY 才发 DATA
  ready = recv_line(conn, 30)   # 30 秒超时对齐擦除时间
  if ready is None or 'READY' not in ready:
      print("[OTA] !!! No READY, device timeout")
      return
  # 收到 READY 才开始发 DATA
  ```
  ```c
  /* B：APP 复位 STM32 前先复位 ESP8266 */
  ESP8266_SendRaw("AT+RST\r\n", 8);
  HAL_Delay(500);   /* 等 ESP8266 重启 */
  NVIC_SystemReset();
  ```
- **教训**：双 MCU 系统（STM32 + ESP8266）的时序协调有两条铁律：**① 长耗时操作后必须用协议握手通知对端**（READY 握手），让对端知道"我准备好了，可以发数据了"；**② 复位 STM32 前必须先复位 ESP8266**，否则 ESP8266 残留的 TCP 连接和 buffer 数据会干扰新流程。READY 帧的设计本质是把"擦 Flash 这种 CPU stall 操作"对协议时序的影响显式化——服务器不用猜 STM32 要擦多久，等 READY 就行。

---

## 坑10：S8 成功后不恢复主动模式 → 跳 APP 后 MQTT 收不到 +IPD

- **现象**：OTA 升级成功，Bootloader 跳 APP，APP 启动后 MQTT 能连上 Broker，但 MQTTX 发的下行命令 APP 收不到。串口日志没有 `+IPD` 前缀，MQTT 解析失败。
- **根因**：Bootloader 在 S5 阶段执行了 `AT+CIPRECVMODE=1`（被动模式），S8 成功后直接跳 APP，**没有恢复 `AT+CIPRECVMODE=0`**（主动模式）。APP 的 MQTT 接收依赖 ESP8266 主动推 `+IPD`，但 ESP8266 还在被动模式，不推数据 → APP 收不到任何下行。
- **排查过程**：
  1. OTA 成功后 APP MQTT 连得上但收不到下行 → 怀疑 ESP8266 状态
  2. 在 APP 启动时发 `AT+CIPRECVMODE?` 查询 → 返回 `1`（被动模式）
  3. 确认根因：Bootloader 没恢复主动模式
- **解决**：S8 成功后、跳 APP 前恢复主动模式 + 关闭 TCP 连接：
  ```c
  /* S8 成功后清理 ESP8266 状态 */
  at_cmd_expect("AT+CIPRECVMODE=0\r\n", "OK", 1000);  /* 恢复主动模式 */
  at_cmd_expect("AT+CIPCLOSE\r\n", "OK", 2000);      /* 关闭 OTA 的 TCP 连接 */
  /* 然后才跳 APP */
  JumpToApp();
  ```
- **教训**：状态切换必须**对称**——进入特殊模式（被动接收）后，退出时必须显式恢复默认模式（主动接收）。OTA 流程把 ESP8266 从"APP 的主动模式"切到"Bootloader 的被动模式"，OTA 结束后必须切回去。判别特征：OTA 成功后 APP 的网络功能异常（MQTT 收不到下行、TCP 不推送数据），优先查 Bootloader 是不是没恢复 ESP8266 状态。这条规则也适用于其他共享外设——OTA 期间改了的外设状态（波特率、引脚复用、中断优先级）都要在跳 APP 前恢复。

---

## D12 成果

- **完整 WiFi OTA 链路落地**：APP 收 MQTT OTA 命令 → 解析四段版本号 + size + crc + force → 写参数区 `ota_request_magic=0x4F544131` → `AT+RST` 复位 ESP8266 → `NVIC_SystemReset` 软复位 → Bootloader 检测 OTA 请求 → 清标志防死循环 → 进 `WiFi_IAP_Process` 拉固件 → S1~S8 八态状态机 → 跳新 APP。
- **Bootloader 端 10 态状态机**（`boot_ota.c` 1403 行）：
  - S1 AT_INIT（3 次重试 + RX 清空 + ATE0 关回显）
  - S2 SET_STA（配 Station 模式）
  - S3 JOIN_WIFI（连路由器，等 `WIFI GOT IP`）
  - S4 CONN_TCP（连 OTA 服务器）
  - S5 ERASE_FLASH（擦 464KB + 发 READY 握手 + 切被动模式）
  - S6 RECV_DATA（被动拉取 + ACK 停等 + 流式 CRC 累加）
  - S7 END_FRAME（收 END 帧，比对 size）
  - S8 VERIFY_JUMP（三重 CRC 校验 + 写参数区 + 恢复主动模式 + 跳 APP）
  - FAIL（打印诊断 + LED0 慢闪 + 死循环等 KEY0 串口 IAP 救砖）
- **USART2 中断 + 4KB 环形缓冲区**：解决主动模式下 printf 阻塞导致的 ORE 丢包。中断只搬字节（不 printf/不写 Flash/不等超时），ring 吸收速度差。`OTA_UART_RX_RING_SIZE=4096`，被动模式下 USART2 完全安静，ring 几乎不满。
- **ESP8266 被动接收模式（AT+CIPRECVMODE=1）**：彻底解决 Flash 写入阻塞与 USART2 接收的结构性矛盾。STM32 用 `AT+CIPRECVLEN?` + `AT+CIPRECVDATA=<len>` 主动拉取 TCP 数据，ESP8266 不主动推 `+IPD`，写 Flash 期间 USART2 安静，不丢包。
- **READY/ACK 停等流控**：S5 擦完 Flash 发 READY 通知服务器；S6 每包写完 Flash 发 ACK，服务器收 ACK 才发下一包。两边时序由 STM32 完全掌控，不依赖 ESP8266 推送时机。
- **三重 CRC 校验**：① 接收时 RAM 流式 CRC 累加（`CRC32_StreamUpdate`）；② 写完后 Flash 回读整片 CRC（`CRC32_CalcAppFlash`）；③ 服务器 INFO 帧里带的 CRC。三者完全一致才算升级成功，任一不一致跳 FAIL。
- **四段版本号 + force 降级保护**：`major.minor.patch.build` 四段字典序比较，`force=1` 允许降级/平级升级（救砖用）。APP 端 `OTA_ParseCommand` 解析八字段 JSON，`OTA_TriggerUpgrade` 写参数区 + 复位 ESP8266 + 软复位。
- **双 MCU 复位时序协调**：APP 复位 STM32 前先 `AT+RST` 复位 ESP8266，等 500ms 再 `NVIC_SystemReset`，避免 ESP8266 残留 TCP 干扰 Bootloader。
- **S8 状态对称恢复**：OTA 成功后执行 `AT+CIPRECVMODE=0` 恢复主动模式 + `AT+CIPCLOSE` 关闭 OTA TCP 连接，然后才跳 APP，APP 的 MQTT 主动接收不受影响。
- **S5/S6 诊断系统**：S5-DIAG v2 捕获 ORE/FE/NE/PE 计数 + SR/CR1/CR3 寄存器 + raw hex 字节流；S6-ACK-DIAG v4 捕获 cip_stage + ORE 计数 + ring_head/tail + Flash 写入偏移 + 原始帧字节。失败时一次性输出全部诊断信息，定位根因不用反复加打印。
- **Python OTA 服务器**（`ota_server.py`）：HELLO/READY/INFO/DATA/END 五帧协议，`recv_line` 超时返回部分数据（容错缺 `\n` 的 HELLO），`FRAME_GAP_MS` 可调帧间隔，READY 等待 30 秒对齐 Flash 擦除时间。
- **自动日志验收脚本**（`verify_ota_log.py`）：检查 OTA 日志的关键标志（S5 被动模式、S8 三重校验通过、JumpToApp、MQTT_WORKING）+ DATA 序号连续性 + recv_bytes == INFO.size + 三重 CRC 一致性 + BIN size/CRC 匹配 + 安装版本号匹配。
- **D12 所有踩坑修复**：栈溢出 → 全部 static BSS；S1 失败 → RX 清空 + 重试 + ATE0；HELLO 覆盖 → cip_send 局部缓冲；INFO 超时 → HELLO 加 `\r\n` + Python 超时协调；ORE 丢包 → 中断环形缓冲；DATA 丢包 → 被动接收模式；只收 DATA0 → 被动模式 + ACK 停等；Flash 返回值误判 → 比 HAL_OK；擦除丢 DATA → READY 握手；ESP 残留 → AT+RST 预复位；S8 不恢复 → CIPRECVMODE=0 对称退出。
- **编译 0 Error 0 Warning**，Bootloader + APP 双工程干净通过。**硬件实测全链路通过**：MQTTX 发 OTA 命令 → APP 写参数区 + 复位 → Bootloader S1~S8 全态通过 → 三重 CRC 一致（`0x0CA0B45A`）→ 跳新 APP → MQTT 重连 + 下行命令恢复 → LED 远程控制正常。
- **项目整体闭环**：D1（环境）→ D2（RTOS）→ D3（IPC）→ D4（WiFi+DHT11）→ D5（TCP）→ D6（MQTT 协议）→ D7（MQTT 加固 + 下行解析）→ D8（Bootloader 框架）→ D9（参数区 + CRC32）→ D10（Ymodem 串口 IAP 救砖）→ D11（APP 端 OTA 触发）→ D12（WiFi OTA 远程升级）。从环境验证到远程 OTA 全链路闭环，具备生产级远程固件升级能力。
