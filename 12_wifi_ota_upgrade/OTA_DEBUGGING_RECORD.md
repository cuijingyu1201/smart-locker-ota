# D12 WiFi OTA 项目对话与调试修改完整记录

> 整理日期：2026-08-26  
> 工作目录：`D:\iot_ota_project\12_wifi_ota_upgrade`  
> 当前重点：ESP 被动接收 v4 已完成全链路实机验证；后续元数据、版本保护、栈余量和回归工具加固已完成构建，待下一轮实机回归  
> 修改边界：本轮栈、S1、S5 调试仅修改 Bootloader；未修改 APP 端业务代码

## 1. 文档目的与资料范围

本文档用于完整交接本项目从 D12 OTA 功能建设到当前 S5 故障定位的上下文，包括：

1. 项目具体要实现什么。
2. 用户在对话中报告过的所有关键现象和要求。
3. 已经修改过哪些文件、哪些函数、为什么修改。
4. 哪些问题已经被证据确认并解决。
5. 哪些修改属于防御性增强或尚未验证的假设。
6. 当前 S5 故障已经排除了什么、还缺什么证据。
7. 当前可烧录产物、构建结果、哈希和下一步验证方法。

资料来源：

- 当前会话中用户提供的全部串口日志和 Python Server 日志。
- 项目中的 `D12.png` 需求图。
- 历史对话原始记录：`D:\iot_ota_project\traework_dialog_full.txt`，约 625 KB。
- `bootloader/Core/Src/boot_ota.c.bak_utf8` 与当前 `boot_ota.c` 的代码差异。
- `bootloader/Core/Src/main.c.bak_utf8` 与当前 `main.c` 的代码差异。
- `bootloader/MDK-ARM/00_bootloader/codex_*build*.log` 历次构建日志。
- 当前 AXF/MAP/HEX 构建产物。

历史原始对话很长，本文按时间、故障和修改进行结构化整理；原始逐字内容仍保留在上述 `traework_dialog_full.txt`，没有删除或覆盖。

## 2. 项目目标

该项目要实现 STM32F103 的远程 WiFi OTA 升级闭环。

整体链路如下：

```text
MQTTX / MQTT Server
        |
        | OTA JSON 命令
        v
STM32 APP
  1. 接收 OTA 命令
  2. 将期望固件 size / CRC / OTA_REQUEST magic 写入参数区
  3. 软件复位
        |
        v
STM32 Bootloader
  1. 检测 OTA_REQUEST
  2. 清除请求标志，避免失败后反复重启
  3. 通过 USART2 控制 ESP8266
  4. 加入 WiFi
  5. TCP 连接 PC:9000
  6. 与 ota_server.py 执行 HELLO/INFO/READY/DATA/ACK/END
  7. 擦除 APP Flash，接收并写入 app.bin
  8. 校验长度和 CRC
  9. 更新参数区
 10. JumpToApp
```

同时保留救砖路径：上电或复位时按住 KEY0，进入 USART1 Ymodem 串口 IAP。

## 3. Flash、串口和网络配置

### 3.1 Flash 分区

根据当前 Bootloader 日志和代码：

| 区域 | 地址/大小 |
|---|---|
| Bootloader | `0x08000000`，32 KB |
| APP | `0x08008000`，464 KB |
| OTA 参数/标志页 | `0x0807F000` |

### 3.2 串口职责

| 串口 | 用途 |
|---|---|
| USART1 | Bootloader 日志、串口 Ymodem IAP |
| USART2 | STM32 与 ESP8266 AT 固件通信 |

### 3.3 当前网络参数

| 参数 | 当前值 |
|---|---|
| WiFi SSID | `cai` |
| OTA Server IP | `10.127.21.17` |
| OTA Server Port | `9000` |
| Device ID | `dev001` |
| 固件块大小 | 1024 字节 |
| JSON 缓冲 | 2048 字节 |

WiFi 密码保存在 `bootloader/Core/Inc/boot_ota.h`，本文不重复展示明文。

## 4. OTA 协议

当前协议是 JSON over TCP，JSON 帧以 `\r\n` 结束。ESP8266 在非透传模式下将服务器数据包装为 `+IPD,<length>:<payload>`。

### 4.1 HELLO

Bootloader 发送：

```json
{"type":"HELLO","dev":"dev001","expect_crc":211858522,"expect_size":32680}
```

其中：

- `expect_crc=211858522` 等于十六进制 `0x0CA0B45A`。
- `expect_size=32680`。
- 这两个值来自 APP 在参数区写入的 OTA 请求，不是 Bootloader 固定写死的固件值。

### 4.2 INFO

Python Server 返回：

```json
{"type":"INFO","size":32680,"crc":211858522,"packets":32,"pktsize":1024}
```

Bootloader 需要验证：

- `size > 0` 且不超过 APP Flash 区。
- `packets > 0`。
- `pktsize > 0`。
- `packets == ceil(size / pktsize)`。
- INFO 的 `size/crc` 与 HELLO 中的期望值一致。

### 4.3 READY

Bootloader 成功解析 INFO 并擦除 APP Flash 后发送：

```json
{"type":"READY","dev":"dev001"}
```

Server 收到 READY 后才开始发送 DATA，避免 STM32 擦除 Flash 时 ESP8266 已经把大量 TCP 数据推到 USART2，造成硬件接收溢出。

### 4.4 DATA 和 ACK

Server 每次发送一个 DATA：

```json
{"type":"DATA","seq":0,"data":"<base64>"}
```

Bootloader 完成 base64 解码、长度检查、Flash 写入和流式 CRC 更新后返回：

```json
{"type":"ACK","seq":0}
```

Server 收到当前序号 ACK 后才发送下一包。这是停等流控。

### 4.5 END 和最终校验

Server 发送：

```json
{"type":"END","total_bytes":32680}
```

Bootloader 最终验证：

1. 实际接收字节数等于 INFO.size。
2. RAM 流式 CRC 等于 INFO.crc。
3. APP Flash 回读 CRC 等于 INFO.crc。

三项均通过后更新参数区并跳转 APP。

## 5. 对话与故障时间线

### 5.1 D12 早期实现阶段

历史对话记录显示，项目按以下步骤建设：

1. APP 端完成 `ota_manager.c/.h`，解析 MQTT OTA 指令并写 OTA 参数。
2. Bootloader 建立 ESP8266 AT 驱动和状态机骨架。
3. 补齐 HELLO、INFO、DATA、END、base64、Flash 写入、CRC 和跳转。
4. 在 `main.c` 中把原先的 OTA 等待占位逻辑替换为 `WiFi_IAP_Init()` 和 `WiFi_IAP_Process()`。
5. 创建 `ota_server.py`，自动读取 BIN、计算 size/CRC 并提供固件。
6. 保留 KEY0 + Reset 的串口 IAP 救砖路径。

历史代码审查还发现过：

- DATA 解码大数组若放在局部栈中会造成严重栈风险，当前版本已是 `static`。
- `ALREADY CONNECTED` 的字符串判断存在逻辑矛盾，当前连接判断先处理 ERROR/CLOSED/FAIL，再把包含 `CONNECT` 的成功响应作为连接成功。
- base64 查表必须将未使用表项初始化为 `0xFF`，否则非法字符可能被当作有效值。

### 5.2 用户首次报告：进入 WiFi OTA 后卡住

用户最初提供的日志停在：

```text
[BOOT] OTA_REQUEST FLAG (magic=0x4F544131) detected!
[BOOT]   Expected new FW: size=32680 bytes, CRC=0xD5E2513B
[BOOT] OTA_request flag pre-cleared (safe for fail-retries)
[BOOT] Enter WiFi-OTA pull (D12).
[WIFI-OTA] Init: wait ESP8266 boot 3s (Hard Constraints)
[WIFI-OTA] Init done, ESP8266 should be ready for AT.
```

用户要求读取代码并解决由于栈溢出导致的卡死。

### 5.3 栈溢出定位和修复

本轮通过 AXF 反汇编确认 `WiFi_IAP_Process()` 中存在：

```c
flash_param_t pm;
```

`flash_param_t` 对应一个约 4 KB 的参数页，而启动文件当前主栈也是 4 KB：

```text
Stack_Size EQU 0x1000
```

修复前函数入口栈帧证据：

```asm
SUB sp, sp, #0x1000
```

这意味着函数刚进入就一次性占满整个主栈，后续函数调用、printf、HAL 调用都会越界。

修改为文件级静态 BSS：

```c
static flash_param_t s_param_work;
```

并将 S8 参数区读写全部改用 `s_param_work`。

修复后函数栈帧：

```asm
SUB sp, sp, #0x30
```

即 `WiFi_IAP_Process()` 自身栈帧从 4096 字节下降为 48 字节。该问题有反汇编和链接结果支持，属于已确认并解决的问题。

### 5.4 用户第二次报告：S1 AT 探测失败

第一类日志：

```text
[WIFI-OTA] [S1] ESP init: AT test
[ESP] WAT
[ESP]
[WIFI-OTA] [S1] !!! FAIL: ESP no AT response.
```

随后出现三次发送 AT 但均无 OK：

```text
[WIFI-OTA] Init: discarded 1 stale UART byte(s).
[WIFI-OTA] [S1] ESP init: AT test (1/3)
[ESP] AT
[WIFI-OTA] [S1] ESP init: AT test (2/3)
[ESP] AT
[WIFI-OTA] [S1] ESP init: AT test (3/3)
[ESP] AT
[WIFI-OTA] [S1] !!! FAIL: ESP no AT response after 3 attempts.
```

用户明确指出：

- 之前每次都可以进入 S1。
- 不是 APP 端问题。
- 问题必须在 Bootloader 内定位。
- 不允许继续修改 APP 端代码。

针对 S1 做过以下 Bootloader 修改：

1. 增加 `uart_flush_rx()`，读取 STM32F1 的 `SR` 后读 `DR`，清 RXNE、ORE、FE、NE、PE。
2. ESP 上电等待后清理残留字节。
3. S1 AT 探测最多重试 3 次。
4. 每次探测前等待 USART2 一段安静窗口。
5. 接收 ESP 响应过程中去掉同步 `[ESP]` printf，避免 USART1 阻塞打印期间 USART2 单字节寄存器发生 ORE。
6. AT 成功后发送 `ATE0`，关闭 ESP 命令回显。

后续用户确认 S1、S2、S3、S4 均可正常通过：

```text
[WIFI-OTA] [S1] AT OK, command echo disabled.
[WIFI-OTA] [S2] Set CWMODE=1 (STA mode)
[WIFI-OTA] [S3] WiFi got IP OK.
[WIFI-OTA] [S4] TCP connect OK.
```

因此当前 S1 问题已经不再复现，但“同步日志必然导致了原始 S1 故障”没有完整原始字节证据。准确表述应为：Bootloader 的接收清理和时序经过增强，硬件现已稳定进入 S1；当时具体丢失哪个字节未被原始捕获证明。

### 5.5 用户首次报告 S5：Server 收到的是 AT 命令而不是 HELLO

Bootloader 日志：

```text
[WIFI-OTA] [S4] TCP connect OK.
[WIFI-OTA] [S5] HELLO dev=dev001 crc=0x0CA0B45A size=32680 bytes
[WIFI-OTA] [S5] !!! INFO wait timeout 10s (server not OTA server?)
```

Python Server 日志：

```text
[OTA] RX HELLO: AT+CIPSEND=74
[OTA] !!! HELLO parse fail: Expecting value: line 1 column 1 (char 0)
```

这组日志给出了确定证据：TCP Server 收到的 payload 是 `AT+CIPSEND=74`，不是 HELLO JSON。

根因在 `cip_send()` 的共享缓冲覆盖：

```c
snprintf(s_at_buf, ..., "{HELLO JSON}");
cip_send(s_at_buf, hello_len);
```

而 `cip_send()` 内部又执行：

```c
snprintf(s_at_buf, ..., "AT+CIPSEND=%d\r\n", len);
```

传入的 `payload` 指针本身指向 `s_at_buf`，因此 HELLO 在真正发送前已经被 AT 命令覆盖。

修复方式：

```c
static char s_tx_payload[OTA_AT_BUF_SIZE];

memcpy(s_tx_payload, payload, len);
snprintf(s_at_buf, ..., "AT+CIPSEND=%d\r\n", len);
...
uart_send(s_tx_payload, len);
```

同时 HELLO 增加 `\r\n` 结束符。

后续 Server 已能正确收到并解析：

```text
[OTA] RX HELLO: {"type":"HELLO","dev":"dev001","expect_crc":211858522,"expect_size":32680}
[OTA]   dev=dev001 expect_crc=0x0CA0B45A expect_size=32680
```

因此共享缓冲覆盖问题属于已确认并解决的问题。

### 5.6 关于 app.bin CRC 的对话

用户发送的 MQTT JSON：

```json
{"cmd":"ota","major":1,"minor":0,"patch":0,"build":3,"size":32680,"crc":211858522}
```

换算结果：

```text
211858522 decimal = 0x0CA0B45A
```

当前 `app/MDK-ARM/app.bin`：

```text
size = 32680 bytes
CRC  = 0x0CA0B45A
```

“启动 ota_server.py 时必须指定 CRC 为某个固定值”这一说法容易误导。实际机制是：

```powershell
python ota_server.py app\MDK-ARM\app.bin
```

Server 会读取本次传入的 BIN，并自动计算：

```python
fw_size = len(fw_data)
fw_crc = zlib.crc32(fw_data) & 0xFFFFFFFF
```

下次生成新 BIN 后，size 和 CRC 当然会变化。需要保证的是同一次 OTA 中：

- MQTT JSON 中写入参数区的 `size/crc`；
- 启动 `ota_server.py` 时传入 BIN 的实际 `size/crc`；
- Bootloader 收到的 INFO `size/crc`；

三者对应同一个 BIN，而不是永远固定为 `0x0CA0B45A`。

### 5.7 当前 S5 故障：Server 发出 INFO，但 Bootloader 没有处理

修复 HELLO 后，用户提供的最新 Bootloader 日志：

```text
[WIFI-OTA] [S4] TCP connect OK.
[WIFI-OTA] [S5] HELLO dev=dev001 crc=0x0CA0B45A size=32680 bytes
[WIFI-OTA] [S5] !!! INFO wait timeout 10s (server not OTA server?)
[WIFI-OTA] !!! FAIL state. Press KEY0+Reset for Serial IAP rescue.
```

Python Server 日志：

```text
[OTA] === Device connected from 10.127.21.115:64619 ===
[OTA] RX HELLO: {"type":"HELLO","dev":"dev001","expect_crc":211858522,"expect_size":32680}
[OTA]   dev=dev001 expect_crc=0x0CA0B45A expect_size=32680
[OTA] TX INFO: size=32680 crc=0x0CA0B45A packets=32 pktsize=1024
[OTA] Waiting for READY (device erased flash)...
[OTA] !!! No READY, device timeout
```

这组日志能确定：

1. S1 到 S4 正常。
2. Bootloader 的 HELLO 已通过 ESP 和 TCP 完整到达 Server。
3. Server 已调用 `sendall()` 发送 INFO。
4. Bootloader 没有打印 `INFO OK`。
5. Bootloader 没有进入 Flash 擦除。
6. Bootloader 没有发送 READY。
7. 当前问题与 APP MQTT 解析无关。
8. 当前 CRC 和 size 匹配，不是 INFO 校验拒绝导致；如果进入了校验拒绝分支，会打印 MISMATCH，而现在只有等待超时。

但仅凭上述日志还不能确定：

- ESP8266 是否从 TCP 收到 INFO。
- ESP8266 是否向 USART2 输出 `+IPD`。
- STM32 USART2 是否发生 ORE/FE/NE/PE。
- `+IPD` 是否被拆成多个 `uart_read_line()` 返回片段。
- INFO 是否在等待 `SEND OK` 时到达并被错误消费。
- INFO 是否完整到达，但 `strip_ipd()` 或 JSON 搜索没有识别。

### 5.8 S5-DIAG v1 实机结果：确认 USART2 ORE

用户烧录诊断版后得到：

```text
[S5-DIAG v1] cip_stage=0x0F pending_len=0
[S5-DIAG] rx_bytes=40 stored=40 timeout_calls=894 busy=0 error=0
[S5-DIAG] uart_flags ORE=1 FE=0 NE=0 PE=0
[S5-DIAG] raw_hex:
0D 0A 4F 4B 0D 0A 0D 0A 3E 0D 0A 52 65 63 76 20
37 36 20 62 79 74 65 73 0D 0A 0D 0A 53 45 4E 44
20 4F 4B 0D 0A 0D 0A 2B
```

十六进制解码为：

```text
\r\nOK\r\n\r\n>\r\nRecv 76 bytes\r\n\r\nSEND OK\r\n\r\n+
```

由此可以确定：

1. `cip_stage=0x0F`，HELLO 的 CIPSEND 四步全部完成。
2. ESP 已开始输出 `+IPD`，最后捕获的 `0x2B` 就是字符 `+`。
3. 同一窗口记录到 `ORE=1`。
4. `+IPD` 后续字节没有进入上层，所以 INFO 不可能被 JSON 解析器识别。
5. 根因位于 USART2 轮询接收发生硬件 Overrun，不是 Server、CRC、APP 或 INFO JSON 内容。

HAL 源码同时证明：轮询 `HAL_UART_Receive()` 等待 RXNE 时检测到 ORE，会读取 SR/DR 清错、结束本次接收并返回 `HAL_ERROR`。被清掉的 DR 数据不会交给 `uart_read_line()`，当前上层也没有可靠恢复完整帧的能力。

### 5.9 S5 修复通过，进入 S6 ACK 定位

中断环形缓冲版实机日志确认 S5 已通过：

```text
[WIFI-OTA] [S5] INFO OK: size=32680 crc=0x0CA0B45A packets=32 pktsize=1024
[WIFI-OTA] [S5] Erasing APP flash (464KB)...
[WIFI-OTA] [S5] Erase OK
[WIFI-OTA] [S6] OTA   3% (1024/32680 bytes, seq=0)
[WIFI-OTA] [S6] !!! DATA recv timeout 30000 ms. got=1024/32680
```

双端诊断版再次运行后确认：ACK0 完全正常，Server 随后发送 wire_seq=1，但 Bootloader 未收到第二包。

```text
[S6-ACK-DIAG v1] attempted=1 cipsend_ok=1 seq=0 len=24
[S6-ACK-DIAG] timing data->flash=30 ms flash->ack_start=6 ms ack_cipsend=225 ms
[S6-ACK-DIAG] payload={"type":"ACK","seq":0}
[S6-ACK-DIAG] cip_stage=0x0F ring_overflow=0 ORE=0
```

Server 同时确认已经完整收到并解析 ACK0，随后进入 `Waiting ACK wire_seq=1`。这排除了 ACK 内容、序号、USART2 ORE、环形缓冲满和 Server ACK 解析问题。

实测 ACK 的 ESP CIPSEND 完成耗时为 225 ms，但 Server 在 ESP 向 STM32 报告 `SEND OK` 之前已经可以收到 TCP ACK，并立即回推下一包。DATA1 因此落入 ESP 仍处于 CIPSEND 完成窗口的竞态。

已加入不改变线协议的验证：

- Bootloader RAM 记录 DATA 到 Flash、Flash 到 ACK、ACK CIPSEND 的时间。
- 记录 ACK 原文、长度、序号、CIPSEND 阶段和 ESP 原始响应。
- S6 失败后输出 `S6-ACK-DIAG v2`，并单独捕获 ACK 后 DATA 下行窗口。
- Server 输出 `ACK-DIAG-v2`、脚本绝对路径、`packet` 和 `wire_seq`。
- Server socket 超时打印实际已收到的字节数和部分十六进制数据。
- Server 每次确认 ACK 后等待 300 ms，再发送下一包，覆盖实测 225 ms CIPSEND 完成窗口。
- 本机 socket 两包模拟测得实际保护间隔 301 ms，wire_seq 严格为 0、1，随后正常 END。

## 6. S5 曾做过的修改及证据等级

### 6.1 必要且已验证：CIPSEND payload 副本

修改：增加 `s_tx_payload`，发送前复制 payload。

证据：修改前 Server 收到 `AT+CIPSEND=74`；修改后 Server 收到正确 HELLO JSON。

结论：已验证有效，必须保留。

### 6.2 必要且已验证：HELLO 增加 CRLF

修改：

```c
"{...HELLO...}\r\n"
```

原因：Server 的 `recv_line()` 以 `\r\n` 分帧。虽然 Server 对超时部分数据有兼容处理，但明确的帧结束符是正确协议行为。

证据：修改后 Server 可立即按完整行解析 HELLO。

### 6.3 防御性修改：S5 前清理 CIPSTART 尾部响应

修改：S5 第一次发送 HELLO 前调用短安静窗口的 `uart_flush_rx()`，用于清理 `CONNECT` 后可能残留的 `OK`。

注意：该清理发生在 HELLO 发送之前，此时 Server 还不可能响应 INFO，因此正常时不应清掉 INFO。但这项修改并未单独通过原始字节捕获验证必要性。

### 6.4 尚未验证：pending INFO 暂存

为处理“Server INFO 可能早于 ESP 的 SEND OK 到达”这一时序，曾增加：

```c
static char s_pending_line[OTA_JSON_BUF_SIZE];
static uint16_t s_pending_len;
```

以及：

```c
ota_defer_line()
ota_read_line()
```

`cip_send()` 等待 SEND OK 时，如果读到包含 `+IPD,` 或 `"type":` 的行，会先暂存，之后 S5 优先消费。

用户烧录该版本后仍然 S5 超时。因此：

- 不能宣称问题就是 INFO 提前到达。
- 不能宣称 pending 机制已经解决问题。
- 当前只保存一行，对半帧、粘包和多帧并不健壮。
- 必须先看原始 USART2 字节再决定保留、重写或删除。

### 6.5 INFO 后 READY、DATA 后 ACK

修改了 Bootloader 和 Python Server 的协议流控：

```text
INFO -> Bootloader 擦除 Flash -> READY -> Server DATA
DATA(seq=N) -> Bootloader 写入 -> ACK(seq=N) -> 下一包
```

原因：原始 Server 连续推送 DATA 时，Bootloader 擦除/写 Flash 会暂停及时读取 USART2，ESP8266 输出可能导致 UART 溢出和丢包。

历史硬件日志曾显示在没有严格序号控制时出现：

```text
seq=0
seq=1
seq=26
```

这说明中间包已经丢失或分帧错乱。READY/ACK 是为解决数据阶段流控而加入的。不过当前流程尚未通过 S5，因此完整 READY/ACK 链路仍待实机验证。

## 7. 当前加入的 S5 验证代码

用户明确要求：有猜测或拿不准的地方必须写验证代码，不要继续盲改。

第一阶段在 `boot_ota.c` 加入了 `S5-DIAG v1`，它已经完成定位任务并确认 ORE。第二阶段根据该证据改为 USART2 RXNE/ERR 中断环形缓冲，诊断版本升级为 `S5-DIAG v2`。

### 7.1 v1 设计原则

- 只修改 Bootloader。
- 接收期间不调用 USART1 printf。
- 原始 USART2 字节先写入静态 RAM。
- 只有 S5 失败后才打印诊断结果。
- 记录硬件 UART 错误标志和 HAL 返回状态。
- 打印固定版本号，确认烧录的确实是新 HEX。

### 7.2 v1 采集内容

```text
s_s5_diag_raw[384]    USART2 原始字节
s_s5_diag_rx_bytes    实际收到的总字节数
s_s5_diag_timeouts    HAL_UART_Receive 超时次数
s_s5_diag_busy        HAL_BUSY 次数
s_s5_diag_errors      其他 HAL 错误次数
s_s5_diag_ore         Overrun Error 观测次数
s_s5_diag_fe          Framing Error 观测次数
s_s5_diag_ne          Noise Error 观测次数
s_s5_diag_pe          Parity Error 观测次数
s_s5_diag_cip_stage   CIPSEND 阶段位图
```

### 7.3 `cip_stage` 位定义

| 位 | 数值 | 含义 |
|---|---:|---|
| bit 0 | `0x01` | 已发送 `AT+CIPSEND=<len>` |
| bit 1 | `0x02` | 已收到 `>` prompt |
| bit 2 | `0x04` | 已发送 HELLO payload |
| bit 3 | `0x08` | 已收到 `SEND OK` |
| bit 4 | `0x10` | 等待 SEND OK 时识别到 `+IPD` 或 JSON |

正常完成 HELLO CIPSEND 至少应看到：

```text
cip_stage=0x0F
```

若 INFO 在 SEND OK 前被暂存，则可能是：

```text
cip_stage=0x1F
```

### 7.4 v2 修复和预期诊断输出

v2 的接收运输层变化：

- USART2 开启 RXNE 和 ERR 中断。
- ISR 立即读取 SR/DR，将字节写入 4096 字节静态环形缓冲。
- `uart_read_line()` 从环形缓冲消费，不再逐字节调用阻塞式 `HAL_UART_Receive()`。
- ISR 记录 ORE/FE/NE/PE 和环形缓冲满计数。
- READY/ACK 停等流控保持不变。

烧录最新 HEX 后，S5 开始时必须出现：

```text
[WIFI-OTA] [S5-DIAG v2] RAM capture enabled
```

失败后出现：

```text
[S5-DIAG v2] cip_stage=0x.. pending_len=...
[S5-DIAG] rx_bytes=... stored=... empty_polls=... ring_overflow=...
[S5-DIAG] uart_flags ORE=... FE=... NE=... PE=...
[S5-DIAG] raw_hex:
[S5-DIAG] 000: .. .. ..
...
[S5-DIAG] raw_end
```

### 7.5 如何根据输出下结论

#### 情况 A：只有 SEND OK，没有 `+IPD`

原始字节能解出 `SEND OK`，但 10 秒内完全没有 `+IPD`：

- STM32 的行解析不是首要问题。
- 应继续检查 ESP 的 TCP 接收模式、连接状态或 ESP 是否输出了其他提示。
- Server 的 `sendall()` 已执行，但还需要确认网络数据是否到达 ESP。

#### 情况 B：ORE 大于 0

- USART2 在 STM32 侧发生硬件溢出。
- 轮询接收架构无法保证实时读取。
- 下一步应改为 USART2 RXNE 中断或 DMA 环形缓冲，而不是继续改字符串匹配。

#### 情况 C：raw_hex 有完整 `+IPD,...INFO...\r\n`

- 物理串口和 ESP 均正常。
- 问题集中在 `uart_read_line()` 的分片、pending 或 `strip_ipd()`。
- 可用这组真实字节写确定性的离线解析测试，再替换解析器。

#### 情况 D：raw_hex 中 INFO 被拆成多段

- 当前 `uart_read_line()` 在 200 ms 总超时时返回部分数据，调用方却把每次返回当作完整行。
- 需要持久化字节流缓存，按 `+IPD` 声明长度组帧。
- 不应继续使用单次局部行缓冲直接做 `strstr(INFO)`。

#### 情况 E：`cip_stage` 未达到 `0x0F`

- `0x01`：没收到 `>`。
- `0x03`：收到 `>`，但还未记录 payload 发送。
- `0x07`：payload 已发送，但没识别到 SEND OK。
- 此时应先修 CIPSEND AT 交互，不进入 INFO 分析。

## 8. 代码修改清单

### 8.1 `bootloader/Core/Src/boot_ota.c`

相对 `boot_ota.c.bak_utf8`，当前文件累计约增加 302 行、删除 50 行。主要变化：

1. `flash_param_t` 工作区从函数局部变量移到静态 BSS。
2. base64 反查表默认填充 `0xFF`。
3. base64 输入长度必须是 4 的倍数。
4. 增加 `uart_flush_rx()` 清串口残留和错误。
5. 去掉 ESP 接收关键窗口中的同步 `[ESP]` 打印。
6. S1 增加最多 3 次 AT 重试。
7. S1 成功后发送 `ATE0`。
8. S3/S4 加强 ERROR、FAIL、CLOSED 判断。
9. 增加 `s_tx_payload`，修复 CIPSEND 覆盖 payload。
10. HELLO 增加 `\r\n`。
11. 增加 INFO packet geometry 检查。
12. 增加 READY。
13. DATA 增加严格连续序号检查。
14. DATA 增加每包解码长度检查。
15. DATA 成功写入后发送 ACK。
16. S6 的 `expected_seq` 和进度状态在 OTA 开始时重置。
17. 增加 pending 单行暂存机制，但尚未证明有效。
18. S5/S6/S7 改为优先读取 pending 行。
19. 增加 `S5-DIAG v1` RAM 原始字节和 UART 错误诊断，实机确认 ORE。
20. 增加 4096 字节 USART2 RX 中断环形缓冲，替换阻塞轮询接收。
21. 诊断升级为 `S5-DIAG v2`，增加环形缓冲溢出计数。

### 8.2 `bootloader/Core/Src/main.c`

当前 `main.c` 已包含 D12 Bootloader 入口逻辑：

1. 加载参数区。
2. 检测 `MAGIC_OTA_REQUEST`。
3. 打印期望 size/CRC。
4. 预清 OTA 请求标志。
5. 调用 `WiFi_IAP_Init()`。
6. 调用 `WiFi_IAP_Process(expect_crc, expect_size)`。
7. KEY0 + Reset 进入串口 Ymodem IAP。
8. 无 OTA 请求时验证 APP 并跳转。

相对 `.bak_utf8` 的大部分差异是注释删减/乱码变化，OTA 调用主逻辑已经存在。当前 S5 调试没有再次修改此文件。

### 8.3 `ota_server.py`

当前脚本功能：

1. 从命令行读取 BIN。
2. 自动计算 size、CRC32、包数。
3. 监听 `0.0.0.0:9000`。
4. 接收并解析 HELLO。
5. 发送 INFO。
6. 等待 READY，超时 30 秒。
7. 每次发送一个 DATA 并等待对应 ACK。
8. 发送 END。

当前 S5 诊断版本没有修改该脚本。READY/ACK 是此前为解决数据推送过快而加入的协议变化。

### 8.4 APP 目录

用户多次明确要求不要把 Bootloader 问题归因到 APP，也不要继续改坏 APP。

本轮处理栈溢出、S1 和 S5 时没有修改 `app/`。APP 在当前问题中的作用仅是提前把以下数据写入参数区并复位：

```json
{"cmd":"ota","major":1,"minor":0,"patch":0,"build":3,"size":32680,"crc":211858522}
```

Bootloader 已正确打印这两个值，并正确放入 HELLO，所以当前 S5 INFO 接收故障不在 APP。

## 9. 已解决、部分解决和未解决问题

| 问题 | 状态 | 证据 |
|---|---|---|
| `WiFi_IAP_Process()` 入口栈溢出 | 已解决 | 栈帧从 `0x1000` 降为 `0x30` |
| HELLO 被 `AT+CIPSEND` 覆盖 | 已解决 | Server 从收到 AT 命令变为收到正确 JSON |
| HELLO 没有明确行结束符 | 已解决 | 已增加 `\r\n`，Server 正常按行解析 |
| S1 无法进入 | 当前已恢复 | 用户日志确认 S1-S4 均通过；原始丢字节原因未完整捕获 |
| DATA 连发造成丢包/跳序号 | 协议已增强，待全链路验证 | 已加入 READY/ACK 和连续序号检查 |
| Server INFO 已发送但 Bootloader S5 超时 | 根因已确认，修复待实机验证 | raw 在 `+IPD` 首字节后停止且 `ORE=1`；已改中断环形缓冲 |
| INFO 是否提前到达 SEND OK 窗口 | 未确认 | pending 修改后仍超时，没有原始字节证据 |
| USART2 是否发生 ORE | 已确认 | `S5-DIAG v1` 实机输出 `ORE=1` |
| `+IPD` 是否到达 STM32 | 已确认 | raw 最后一个字节为 `0x2B ('+')` |

## 10. 构建历史

本轮及相关历史构建：

| 构建日志 | 时间 | 结果 | Program Size 摘要 |
|---|---|---|---|
| `codex_stackfix_build.log` | 19:13:04 | 0E 0W | Code 18560, ZI 43284 |
| `codex_uartfix_build.log` | 21:24:31 | 0E 0W | Code 18844, ZI 43284 |
| `codex_rxfix_build.log` | 21:41:11 | 0E 0W | Code 18932, ZI 43284 |
| `codex_s5fix_build.log` | 21:49:38 | 0E 0W | Code 18892, ZI 43796 |
| `codex_pending_ipd_build.log` | 22:00:49 | 0E 0W | Code 19052, ZI 45844 |
| `codex_s5_diag_build.log` | 22:16:14 | 0E 0W | Code 19668, ZI 46232 |
| `codex_uart_ring_build.log` | 22:32:08 | 0E 0W | Code 19708, ZI 50324 |
| `codex_s6_ack_diag_build.log` | 22:45:30 | 0E 0W | Code 20264, ZI 50388 |
| `codex_s6_guard_build.log` | 2026-08-27 11:53:01 | 0E 0W | Code 20268, ZI 50388 |

最新构建完整结果：

```text
Program Size: Code=20268 RO-data=2368 RW-data=172 ZI-data=50388
FromELF: creating hex file...
"00_bootloader\00_bootloader.axf" - 0 Error(s), 0 Warning(s).
```

MAP 结果：

```text
s_uart_rx_ring  0x20008310  size 4096
s_param_work    0x200094D0  size 4096
STACK           0x2000B580  size 4096
__initial_sp     0x2000C580
```

这些对象均不与主栈重叠。

## 11. 当前产物

最新 Bootloader HEX：

```text
D:\iot_ota_project\12_wifi_ota_upgrade\bootloader\MDK-ARM\00_bootloader\00_bootloader.hex
```

生成时间：

```text
2026-08-27 11:53:01
```

SHA-256：

```text
D100C6695F5E8E9DA38E12620332E56D811D3744A5CF0DBF44028BBEB74688F5
```

对应构建日志：

```text
bootloader\MDK-ARM\00_bootloader\codex_s6_guard_build.log
```

## 12. 下一次实机验证步骤

1. 只烧录最新 `00_bootloader.hex`。
2. 确认串口打印包含：

```text
[WIFI-OTA] [S5-DIAG v2] RAM capture enabled
```

若没有这行，说明烧录的不是当前诊断 HEX，不应分析后续日志。

3. 使用与 MQTT 参数匹配的 BIN 启动 Server：

```powershell
python ota_server.py app\MDK-ARM\app.bin
```

4. 触发 OTA。
5. 保存从 Bootloader 启动到 `[S5-DIAG] raw_end` 的完整串口输出。
6. 同时保存 Python Server 从连接建立到连接关闭的完整输出。
7. 首先解码 raw_hex，不再先修改代码。
8. 根据第 7.5 节的证据分支决定下一步。

## 13. 当前实现的已知技术风险

### 13.1 `uart_read_line()` 不是严格的 TCP/IPD 帧解析器

当前函数：

- 单字节调用 `HAL_UART_Receive(..., 10)`。
- 单次调用有约 200 ms 总超时。
- 遇到 `\r\n` 返回。
- 超时也会返回已经收到的部分字节。

上层却可能把超时返回的部分数据当成完整行。这对 AT 短响应通常可用，但对 `+IPD,length:JSON` 并不严谨。

### 13.2 pending 只能保存一行

当前 pending 机制不能正确覆盖：

- `+IPD` 头和 JSON 分两次返回。
- 一次返回含 `SEND OK` 和半个 `+IPD`。
- 多个 `+IPD` 连续到达。
- DATA JSON 超过一次行读取的实际时序窗口。

### 13.3 轮询 USART2 的实时性有限

STM32F1 USART 硬件接收缓冲很浅。任何长时间 printf、Flash 擦写或其他阻塞操作期间，ESP 连续输出都可能引发 ORE。READY/ACK 可以降低数据阶段压力，但最终可靠设计仍应考虑：

- USART2 RXNE 中断；
- 静态环形缓冲；
- AT 响应和 `+IPD` 字节流解析；
- 严格按 `+IPD` 声明长度提取 TCP payload。

是否立即进行这项架构修改，必须以最新 S5 raw_hex 和 UART 错误计数为依据。

## 14. 对话中形成的明确约束

后续处理必须遵守：

1. 当前 S5 问题优先在 Bootloader 中定位。
2. 未经用户明确要求，不修改 APP 端。
3. 未经新证据，不把问题归因于 APP 或 USART2 硬件接线。
4. 对不确定原因先加低干扰验证代码。
5. 接收关键窗口禁止逐字节同步打印。
6. 每版都要提供可识别版本标记、构建结果和 HEX 哈希。
7. 不把时序推测、pending 假设或串口溢出假设写成已确认根因。
8. 修改后必须通过 Keil 构建，至少达到 `0 Error(s), 0 Warning(s)`。

## 15. 关键对话摘要

以下按用户表达顺序保留当前调试阶段的核心诉求：

1. Bootloader 在 ESP 初始化日志后卡住，要求读取代码解决栈溢出。
2. S1 连续三次 AT 均无响应，询问为什么此前可以进入、现在不能进入。
3. 明确认为问题位于 Bootloader，不允许改坏 APP。
4. S1 恢复后卡在 S5；Server 最初收到 `AT+CIPSEND=74`，要求参考 D12 图修代码。
5. 询问为什么必须让 Server 固定使用 `0x0CA0B45A`，指出以后会生成新 BIN。
6. 提供 MQTT JSON，并确认当前 size/CRC。
7. HELLO 修复后 Server 能发送 INFO，但 Bootloader 仍然 S5 超时且 Server 等不到 READY。
8. 指出最开始 S5 曾经可以通过，要求明确说明修改过什么。
9. 要求所有不确定之处先写验证代码，不要一版版猜测修改。
10. 最后要求将全部对话、修改方式、已解决问题和当前状态整理为详细 MD 文件，即本文档。

## 16. 结论

截至本文整理时，已经有充分证据确认并解决两个关键 Bootloader Bug：

1. `flash_param_t` 局部变量导致 `WiFi_IAP_Process()` 入口占用完整 4 KB 主栈。
2. `cip_send()` 复用 `s_at_buf` 导致 HELLO 被 `AT+CIPSEND` 命令覆盖。

S1 到 S5 已经稳定通过。v1 诊断确认并修复了 S5 接收 `+IPD` 时的 USART2 ORE。S6 已正确解析和写入第一包 `seq=0`，ACK0 也从 Bootloader、ESP 到 Server 全链路正确。

S6 的 v2 实机结果进一步证明：即使 Server 在 ACK0 后等待 300 ms，第二包仍未在 Bootloader 的 USART2 侧产生任何接收字节。因此“仅由 225 ms CIPSEND 完成窗口导致”已被实机证伪，不能继续作为根因。当前已确认的故障边界是 Server 成功收到 ACK0 后的下行链路：Server/TCP -> ESP8266 TCP 接收 -> ESP8266 `+IPD` 输出 -> Bootloader USART2 中断。下一步必须用 v3 状态探针在这个边界内继续定位。

## 17. S6 ACK-DIAG v3：定位第二包消失位置

### 17.1 v2 最新证据

Bootloader：

```text
[S6-ACK-DIAG v2] attempted=1 cipsend_ok=1 seq=0 len=24
[S6-ACK-DIAG] ack cip_stage=0x0F rx_bytes=39 ring_overflow=0 ORE=0 FE=0 NE=0 PE=0
[S6-ACK-DIAG] post_ack pending_len=0 rx_bytes=0 ring_overflow=0 ORE=0 FE=0 NE=0 PE=0
```

Server：

```text
[OTA-DIAG] RX ACK raw: {"type":"ACK","seq":0}
[OTA-DIAG] ACK wire_seq=0 OK; guard 0.30s before next DATA
[OTA-DIAG] ACK wire_seq=1: timeout after 0 byte(s)
```

可以严格得出：

1. Bootloader 已成功解析、写入 DATA0。
2. ACK0 的内容、长度、序号以及 ESP `AT+CIPSEND` 全部成功。
3. Server 已收到并正确解析 ACK0。
4. ACK0 后 300 ms 保护延时没有解决问题。
5. Bootloader 在 ACK0 后没有读到 DATA1，也没有读到任何 ESP AT/UART 字节。
6. USART2 没有 ORE/FE/NE/PE，环形缓冲也没有溢出。

因此当前不能把问题归因于 ACK JSON、Bootloader DATA JSON 解析、Flash 写入、USART2 ORE 或环形缓冲满。仅凭 v2 日志还不能区分 DATA1 是否已经进入 ESP8266 内部 TCP 缓冲。

### 17.2 v3 低干扰状态探针

`bootloader/Core/Src/boot_ota.c` 的诊断版本升级到：

```text
S6-ACK-DIAG v3
```

ACK0 后若连续无数据，Bootloader 不同步打印，而是在 RAM 捕获窗口内依次发送：

```text
2 s: AT+CIPSTATUS
4 s: AT+CIPRECVMODE?
6 s: AT+CIPRECVLEN?
```

30 秒 S6 超时后统一打印：

- `probe_stage`；
- USART2 环形缓冲 `head/tail`；
- USART2 `SR/CR1/CR3`；
- 所有探针响应的 `post_ack_raw_hex`。

判定规则：

- `STATUS:3`：ESP 的 TCP 连接仍存在。
- `+CIPRECVLEN:<N>` 且 `N > 0`：DATA1 已到 ESP，但没有以 `+IPD` 主动输出；后续应改为被动接收并用 `AT+CIPRECVDATA` 拉取。
- TCP 连接记录不存在或出现 `CLOSED`：应定位 ESP/TCP 连接为何在 ACK0 后关闭。
- 探针有响应且 `CIPRECVLEN=0`：USART2 工作正常，但 Server 数据没有进入 ESP TCP 接收缓冲，应继续检查 Server/TCP/网络侧。
- 完全没有探针响应，同时 `CR1` 中 RXNEIE 仍开启：需要检查 ESP 是否停止 UART 输出或 USART2 IRQ 是否异常。
- 命令返回 `ERROR`：表示当前 ESP AT 固件不支持对应查询，不能把 `ERROR` 当成 TCP 已断开。

### 17.3 Server v3 证据

`ota_server.py` 版本标记为：

```text
[OTA] Server build: ACK-DIAG-v3
```

它会：

- 对已接受连接启用 `TCP_NODELAY`；
- 每次 `sendall()` 后打印 `wire_seq`、JSON 字节数和单调时钟时间；
- 保留 ACK 原始行、序号和超时字节数日志。

实机测试前必须停止旧 Server 进程。启动日志至少应包含：

```text
[OTA] Server build: ACK-DIAG-v3
[OTA] Script path: D:\iot_ota_project\12_wifi_ota_upgrade\ota_server.py
[OTA-DIAG] TCP_NODELAY=1
```

否则得到的仍是旧版日志，不能用于 v3 判定。

### 17.4 v3 构建和内存核验

Keil ARMCC 5 构建结果：

```text
Program Size: Code=20468 RO-data=2460 RW-data=172 ZI-data=50388
0 Error(s), 0 Warning(s)
```

关键 AXF 符号：

```text
USART2_IRQHandler       0x08001E7B  strong
BootOta_UART_IRQHandler 0x08000481  strong
s_uart_rx_ring          0x20008310  size 0x1000
s_param_work            0x200094D0  size 0x1000
STACK                   0x2000B580  size 0x1000
__initial_sp            0x2000C580
```

已使用 SRAM 最高地址为 `0x2000C580`，低于 STM32F103ZE SRAM 顶部 `0x20010000`，剩余 `0x3A80` 字节（14976 字节）。上述静态缓冲与主栈不重叠。

可烧录文件：

```text
bootloader\MDK-ARM\00_bootloader\00_bootloader.hex
SHA-256: 88BC65540E0F33763060BDDB743B7814768A24AB2C379025A4E5D85C3F2EF823
```

构建日志：

```text
bootloader\MDK-ARM\00_bootloader\codex_s6_probe_build.log
```

## 18. S6 ACK-DIAG v3 实机结果与被动接收修复

### 18.1 v3 新日志能确认什么

本次实机输出：

```text
[WIFI-OTA] [S6] OTA   3% (1024/32680 bytes, seq=0)
[WIFI-OTA] [S6] !!! DATA recv timeout 30000 ms. got=1024/32680
[S6-ACK-DIAG v3] attempted=1 cipsend_ok=1 seq=0 len=24
[S6-ACK-DIAG] ack cip_stage=0x0F rx_bytes=39 ring_overflow=0 ORE=0 FE=0 NE=0 PE=0
[S6-ACK-DIAG] post_ack pending_len=0 rx_bytes=0 ring_overflow=0 ORE=0 FE=0 NE=0 PE=0
[S6-ACK-DIAG] probe_stage=0 ring_head=1722 ring_tail=1722 SR=0x000000D0 CR1=0x0000202C CR3=0x00000001
```

严格结论：

1. DATA0 已完整到达、解析、写入 Flash，进度和序号正常。
2. ACK0 的 JSON、CIPSEND 提示符、payload 发送和 `SEND OK` 全部成功。
3. ACK0 后 30 秒内 USART2 没收到任何新字节，环形缓冲为空且无 ORE/FE/NE/PE。
4. `CR1=0x202C` 中 RXNEIE 仍开启，USART2 接收中断没有被软件关闭。
5. 故障边界继续位于 Server/TCP 到 ESP 主动 `+IPD` 输出之间，不在 Flash、ACK JSON、STM32 UART 环形缓冲或 DATA JSON 解析器。

`probe_stage=0` 表示 v3 计划中的三个探针实际没有发出，不能据此判断 TCP 是否仍连接，也不能判断 DATA1 是否已经进入 ESP 内部缓冲。该字段只能说明 v3 探针调度没有提供预期证据，不能把 `CIPRECVLEN=0` 或 TCP 断开当成已确认事实。

### 18.2 v4 修改策略

不再依赖 ESP8266 主动模式异步输出大块 `+IPD`。在发送 READY 前执行：

```text
AT+CIPRECVMODE=1
```

S6 每 50 ms 最多查询一次 `AT+CIPRECVLEN?`。当 ESP 内部有数据时，再按其报告长度执行 `AT+CIPRECVDATA=<len>`。

Server 仍然保持“一包 DATA -> 等 ACK -> 下一包”的协议，但 DATA 在 Bootloader 准备好以前留在 ESP TCP 缓冲中。Flash 写入、进度打印和 ACK CIPSEND 期间不再与 ESP 的大块主动 UART 输出竞争时序。

同时完成以下代码修正：

- S6 每次接收操作后重新读取 `HAL_GetTick()`，统一使用实时 tick 做 30 秒超时判断。
- `strip_ipd()` 同时支持 `+IPD,<len>:` 和 `+CIPRECVDATA:<len>,`。
- 禁止超出 JSON 缓冲容量的被动拉取，避免拉取半帧后误解析。
- 按 `+CIPRECVDATA` 声明长度累积 TCP 分片，只有收到完整 `\r\n` 协议帧才交给 JSON 解析。
- 前缀从接收缓冲剥离到独立 JSON 缓冲，避免重叠 `memcpy`。
- v4 失败日志增加 `passive_mode/polls/pulls/last_available`。
- S8 校验通过后恢复 `AT+CIPRECVMODE=0` 并关闭 OTA TCP，避免直接 JumpToApp 后影响 APP 的主动 `+IPD` MQTT 接收。

修改文件为 `bootloader/Core/Src/boot_ota.c` 和本文档；APP 与 Server 协议代码未修改。

### 18.3 v4 构建产物

Keil ARMCC 5：

```text
Program Size: Code=21484 RO-data=2588 RW-data=192 ZI-data=52440
0 Error(s), 0 Warning(s)
```

内存边界：

```text
s_uart_rx_ring       0x20008324  size 4096
s_s6_passive_frame   0x200094E4  size 2048
s_param_work         0x20009CE4  size 4096
STACK                0x2000BD98  size 4096
__initial_sp          0x2000CD98
```

可烧录文件：

```text
bootloader/MDK-ARM/00_bootloader/00_bootloader.hex
SHA-256: 1FC4870FA996A6C596BE39EE00594A3AC502644042C24CE49F499CB79413B2B1
```

构建日志：

```text
bootloader/MDK-ARM/00_bootloader/codex_s6_passive_build.log
```

### 18.4 实机验证判据

烧录后 S5 必须出现：

```text
[WIFI-OTA] [S5] ESP passive receive mode enabled
```

若出现 `ESP passive receive mode not supported`，说明当前 ESP AT 固件不支持被动接收命令，需要保存该次完整串口输出，不能继续按 v4 DATA 结果分析。

正常路径应看到 `seq=0,1,2...31` 连续推进，最后进入 S8 CRC 校验和 JumpToApp。若仍失败，请保留完整 Bootloader 与 Server 日志；v4 的 `last_available` 可区分 Server 数据是否已进入 ESP TCP 缓冲，`pulls` 可确认是否执行过实际拉取。

## 19. v4 全链路通过与成功后加固

### 19.1 2026-08-27 实机成功证据

`串口数据.txt` 已确认完整闭环：

```text
[WIFI-OTA] [S5] ESP passive receive mode enabled
[WIFI-OTA] [S6] OTA   3% (1024/32680 bytes, seq=0)
...
[WIFI-OTA] [S6] OTA 100% (32680/32680 bytes, seq=31)
[WIFI-OTA] [S8]   CRC_stream(RAM)  = 0x0CA0B45A
[WIFI-OTA] [S8]   CRC_readback(APP) = 0x0CA0B45A (INFO.crc=0x0CA0B45A)
[WIFI-OTA] [S8] All 3 verify OK. Updating param area and JumpToApp...
[WIFI-OTA] [S8] JumpToApp 0x08008000 ...
```

JumpToApp 后 APP 再次完成 Wi-Fi、TCP、MQTT CONNECT、SUBSCRIBE，并进入 `MQTT_WORKING` 后继续发布传感器数据。由此确认 MQTT 触发、参数请求、Bootloader Wi-Fi OTA、被动拉取、Flash、CRC、跳转和 APP 恢复全部通过。

实机通过的 Bootloader 基线 SHA-256：

```text
1FC4870FA996A6C596BE39EE00594A3AC502644042C24CE49F499CB79413B2B1
```

自动复核：

```text
python tools\verify_ota_log.py 串口数据.txt
[OTA-VERIFY] PASS packets=32 size=32680 crc=0x0CA0B45A
```

### 19.2 参数元数据修复

成功日志同时暴露 APP 启动时把 Bootloader 写入的真实 `fw_size=32680`、`fw_crc=0x0CA0B45A` 覆盖成整个 APP 分区 475136 字节的 CRC。现已删除 `FlashParam_InitOnBoot()` 中对 `fw_size_bytes/fw_crc32` 的整分区覆盖；APP 启动只更新 boot count 和 reset reason。

升级成功后真实 BIN size/CRC 由 Bootloader 写入，APP 后续启动保持不变。

### 19.3 版本传递与降级保护

参数区预留区 `0x68~0x6F` 增加四个 OTA 目标版本字段，总结构仍为 4096 字节，既有字段地址不变。流程调整为：

```text
MQTT major/minor/patch/build
  -> APP 参数区 ota_new_fw_ver_*
  -> Bootloader RAM 保留目标版本
  -> CRC/Flash 全部通过后提交为 fw_ver_*/fw_build_num
```

APP 使用四段版本字典序比较，默认拒绝低版本和同版本重装。维护恢复可显式加入 `"force":1`。BUILD 改为发布时手动递增的 `FW_BUILD_NUM`，不再使用不可排序的编译时间哈希。

旧参数区曾使用不同版本语义，第一次迁移到新规则需要使用一次 `force=1`。

### 19.4 栈余量与回归工具

- TaskLED 栈从 384 增加到 512 字节。
- TaskSemHandle 栈从 256 增加到 384 字节。
- `ota_server.py` 增加 `--version/--force`，自动打印与 BIN size/CRC 匹配的 MQTT JSON。
- Server 增加 `--disconnect-after-seq`、`--delay-after-seq`、`--corrupt-seq` 故障注入。
- 新增 `tools/verify_ota_log.py` 和 `OTA_REGRESSION_TEST.md`。

### 19.5 加固版本构建产物

Bootloader：

```text
Program Size: Code=21696 RO-data=2588 RW-data=192 ZI-data=52440
0 Error(s), 0 Warning(s)
HEX SHA-256: B96B39A4BF5C26B881605E424FFF4EED25E1E957A90AD01401B3A20587E8C57F
```

APP：

```text
Program Size: Code=31964 RO-data=1144 RW-data=228 ZI-data=22564
0 Error(s), 0 Warning(s)
BIN size=33336
BIN CRC32=0xC6FFC87C (decimal 3338651772)
BIN SHA-256: C3071B441CA3805A18D2311F7497BDEEB817A84B788BC3705A7293C1388E539A
HEX SHA-256: BFB464B8F703268F9E9E0C88649F38E81B7E63CA5CBABE073467CC0F9C3E942E
```

首次迁移测试命令：

```text
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force
```

Server 应打印：

```json
{"cmd":"ota","major":1,"minor":0,"patch":0,"build":4,"size":33336,"crc":3338651772,"force":1}
```

本节加固版本已完成静态检查和 Keil 构建，尚未替代 19.1 的实机成功基线结论；需要按 `OTA_REGRESSION_TEST.md` 再执行一次硬件回归。
