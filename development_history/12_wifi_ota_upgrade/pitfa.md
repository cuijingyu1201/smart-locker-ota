# WiFi OTA 升级踩坑记录与原理说明

> 适用工程：D:\iot_ota_project\12_wifi_ota_upgrade<br>
> 芯片：STM32F103ZE<br>
> 调试串口：USART1，115200 8N1，无硬件流控<br>
> ESP8266 串口：USART2，115200 8N1，无硬件流控<br>
> 目标：APP 收到 MQTT 命令后复位，Bootloader 通过 ESP8266 下载 app.bin，写入 APP Flash，校验后启动新 APP

本文只记录这次 WiFi OTA 升级问题的定位和修复过程，面向刚接触 UART、ESP8266 AT、TCP、MQTT 和 Bootloader 的读者。每个问题都按照“现象 -> 原因 -> 底层逻辑 -> 修复 -> 如何判断日志”的顺序说明。

---

## 1. 最终结论

这次 OTA 不是只出现了一个问题，而是不同层次的问题先后暴露：

1. Bootloader 函数中的 4 KB 局部变量耗尽主栈，状态机可能刚进入就异常。
2. HELLO JSON 和 `AT+CIPSEND` 共用一个缓冲区，JSON 被 AT 命令覆盖。
3. USART2 使用阻塞轮询接收，ESP8266 连续输出 `+IPD` 时产生 ORE，S5 的 INFO 丢失。
4. Server 如果连续发 DATA，Bootloader 擦除或写 Flash 时来不及接收，所以加入 READY/ACK 停等流控。
5. S6 在 ESP8266 主动接收模式下只收到 DATA0；ACK0 成功后 DATA1 没有再到 STM32。最终改为 ESP8266 被动接收，由 Bootloader 主动查询和拉取 TCP 数据。
6. 全链路跑通后，APP 又把正确的 BIN size/CRC 覆盖成整个 APP 分区的 size/CRC，导致安装元数据失真。
7. OTA 版本原来没有完整传递到 Bootloader。现在增加四段版本、普通升级保护和 `force=1` 恢复通道。
8. 成功日志还暴露 TaskLED 和 TaskSemHandle 栈余量偏低，因此增加任务栈，并补充自动验收和故障注入工具。

已经由硬件实机证明跑通的接收基线：

~~~text
[WIFI-OTA] [S5] ESP passive receive mode enabled
[WIFI-OTA] [S6] OTA   3% (1024/32680 bytes, seq=0)
...
[WIFI-OTA] [S6] OTA 100% (32680/32680 bytes, seq=31)
[WIFI-OTA] [S8]   recv_bytes        = 32680 (INFO.size=32680)
[WIFI-OTA] [S8]   CRC_stream(RAM)   = 0x0CA0B45A
[WIFI-OTA] [S8]   CRC_readback(APP) = 0x0CA0B45A (INFO.crc=0x0CA0B45A)
[WIFI-OTA] [S8] All 3 verify OK. Updating param area and JumpToApp...
~~~

跳转后 APP 重新连接 WiFi 和 MQTT，并继续发布传感器数据，证明 OTA 已形成完整闭环。

需要区分两代证据：

~~~text
实机成功基线：size=32680，CRC=0x0CA0B45A
当前加固版本：size=33336，CRC=0xC6FFC87C，version=1.0.0.4
~~~

当前加固版本已通过 Keil `0 Error(s), 0 Warning(s)` 构建，但还需要重新上板回归。不能把“编译通过”写成“已经实机通过”。

---

## 2. 先理解整个 OTA 链路

### 2.1 三个主要角色

可以先把系统看成三个角色：

~~~text
ota_server.py：保管并发送 app.bin
APP：接收 MQTT 命令，决定是否允许升级
Bootloader：真正下载、擦除、写 Flash、校验和启动新 APP
~~~

APP 不能一边运行完整业务，一边直接覆盖自己所在的 Flash。因此 APP 只负责“登记升级请求并复位”，真正写 APP 分区的是 Bootloader。

### 2.2 完整流程

~~~text
MQTT 发布 OTA JSON
        |
        v
APP 解析版本、size、CRC、force
        |
        | 写入 Flash 参数区
        v
STM32 软件复位
        |
        v
Bootloader 检测 OTA_REQUEST
        |
        | 控制 ESP8266 加 WiFi、连 PC:9000
        v
HELLO -> INFO -> 擦除 Flash -> READY
        |
        v
DATA0 -> 写入 -> ACK0
DATA1 -> 写入 -> ACK1
...
        |
        v
END -> 长度校验 -> RAM CRC -> Flash 回读 CRC
        |
        v
保存真实安装版本、size、CRC
        |
        v
JumpToApp -> 新 APP 恢复 WiFi、MQTT 和业务
~~~

### 2.3 为什么要分层排查

一次 OTA 经过很多层：

| 层 | 主要问题 |
|---|---|
| MQTT | 命令有没有到 APP，JSON 是否完整 |
| 参数区 | APP 写的目标信息能否被 Bootloader 正确读取 |
| ESP AT | AT、入网、TCP、CIPSEND 是否成功 |
| UART | STM32 是否真的收到 ESP 输出，有没有 ORE |
| TCP 协议 | HELLO/INFO/READY/DATA/ACK/END 是否按顺序 |
| Flash | 数据是否写到正确地址，最后一包是否正确 |
| 完整性 | RAM 数据、Flash 数据、Server BIN 是否相同 |
| 启动 | 新 APP 是否真正开始运行 |

例如 Server 打印“INFO 已发送”，只能证明 PC 调用了发送函数，不能证明 INFO 已经经过 WiFi、ESP8266 和 USART2 到达 STM32。

---

## 3. 新手需要先知道的底层概念

### 3.1 UART 为什么会丢字节

USART2 是 STM32 和 ESP8266 之间的串口。ESP8266 一个字节接一个字节地发送，STM32 必须及时读取硬件数据寄存器。

STM32F1 的 UART 硬件不是一个很大的仓库。前一个字节还没取走，下一个字节又到了，就可能发生：

~~~text
ORE = Overrun Error = 接收溢出
~~~

发生 ORE 后，某些字节会丢失。JSON 少一个字符就可能无法解析，Base64 少一个字符则整包固件都不能正确解码。

### 3.2 中断环形缓冲区是什么

可以把 UART 硬件看成一个很小的窗口，把环形缓冲区看成 SRAM 中的等候区：

~~~text
ESP8266 发来字节
        |
        v
USART2 RX 中断立即取走
        |
        v
4096 字节环形缓冲区
        |
        v
主循环稍后解析 AT 响应和 JSON
~~~

中断只负责快速搬运字节，不打印日志、不解析 JSON、不写 Flash。这样主循环短时间忙碌时，字节不会立即丢失。

### 3.3 TCP 为什么需要 CRLF 和长度

TCP 是连续字节流，不保证“一次 send 就对应对方一次 recv”。例如 Server 发送一条 JSON，对方可能分两次收到；两条 JSON 也可能粘在一次接收中。

因此协议需要边界。本工程使用：

~~~text
JSON 内容 + \r\n
~~~

ESP8266 的 `+IPD` 或 `+CIPRECVDATA` 还会提供长度，上层应按声明长度取完整数据，再按 `\r\n` 判断一条协议帧结束。

### 3.4 Flash 分区大小不等于 BIN 大小

当前 APP 分区最大容量是：

~~~text
APP_FLASH_SIZE = 0x74000 = 475136 字节
~~~

当前实际 app.bin 是：

~~~text
33336 字节
~~~

可以理解为一个容量 475136 字节的柜子，目前只放入了 33336 字节的文件。柜子容量用于检查文件不能越界；文件大小用于确定实际写入和 CRC 范围，两者不能混用。

### 3.5 CRC 是什么

CRC 可以理解为一段数据的数字指纹。只要数据中有一个位发生改变，CRC 通常就会变化。

本工程最终比较三方：

~~~text
Server app.bin 的 CRC
        ==
Bootloader 接收到的数据流 CRC
        ==
APP Flash 回读 CRC
~~~

三者相等，才能证明 PC 上的 BIN、STM32 收到的内容和 Flash 中保存的内容一致。

### 3.6 版本和 force 是什么

当前版本由四个数字组成：

~~~text
major.minor.patch.build
例如：1.0.0.4
~~~

普通升级只允许目标版本更高。`force=1` 表示维护人员明确允许同版本重装或降级，但仍然必须通过全部 size/CRC 校验。

---

## 4. 问题一：Bootloader 进入 OTA 后卡住

### 4.1 现象

最初日志停在 OTA 初始化附近：

~~~text
[BOOT] OTA_REQUEST FLAG detected!
[BOOT] Enter WiFi-OTA pull (D12).
[WIFI-OTA] Init done, ESP8266 should be ready for AT.
~~~

没有明确进入后续状态，容易误以为 ESP8266 或 WiFi 没响应。

### 4.2 原因

`WiFi_IAP_Process()` 中曾有局部变量：

~~~c
flash_param_t pm;
~~~

这个结构体约 4096 字节，而 Bootloader 主栈也是 4096 字节。函数一进入就把主栈用完，后续调用 printf、HAL 或其他函数时发生栈越界。

### 4.3 底层逻辑

局部变量通常放在栈中。函数进入时，CPU 会移动 SP，为局部变量留空间。

反汇编看到：

~~~asm
SUB sp, sp, #0x1000
~~~

`0x1000` 就是 4096 字节。主栈总共也只有 4096 字节，所以已经没有空间保存返回地址、寄存器和子函数局部变量。

### 4.4 怎么修复

把大结构从函数局部栈移到静态 BSS：

~~~c
static flash_param_t s_param_work;
~~~

静态变量由链接器放在固定 SRAM 地址，不会随着函数调用压入主栈。

修复后函数栈帧降为：

~~~asm
SUB sp, sp, #0x30
~~~

即 48 字节。

### 4.5 如何判断是否修好

不能只看 C 代码，应检查：

1. AXF 反汇编中函数栈帧是否从 `0x1000` 降下来。
2. MAP 中 `s_param_work` 是否位于 BSS。
3. BSS、主栈和 SRAM 顶部是否重叠。
4. 串口是否能继续进入 S1。

看到 S1 日志只能说明卡死消失；反汇编和 MAP 才能证明栈布局确实改变。

---

## 5. 问题二：S1 连续发送 AT，但没有 OK

### 5.1 现象

~~~text
[WIFI-OTA] [S1] ESP init: AT test (1/3)
[ESP] AT
[WIFI-OTA] [S1] ESP init: AT test (2/3)
[ESP] AT
[WIFI-OTA] [S1] ESP init: AT test (3/3)
[ESP] AT
[WIFI-OTA] [S1] !!! FAIL: ESP no AT response after 3 attempts.
~~~

以前能进入 S1，修改后却连续收不到 OK。

### 5.2 原因

这个阶段没有捕获到“具体丢失了哪一个字节”的完整原始证据，因此不能武断地说只有一个根因。

当时可以确认的风险包括：

- STM32 复位时 ESP8266 仍保留上一次连接状态和残留输出；
- USART2 可能残留 RXNE、ORE、FE、NE、PE；
- ESP 启动时间有波动；
- 接收关键窗口同步打印大量日志会增加时序压力；
- ESP 命令回显增加了不需要解析的文本。

### 5.3 底层逻辑

STM32 软件复位不一定让 ESP8266 同时复位。ESP 是独立芯片，可能仍处于 MQTT/TCP 状态。Bootloader 一启动就发送 AT 时，收到的可能是旧连接尾部、启动信息或命令回显，而不是干净的 `OK`。

### 5.4 怎么修复

Bootloader 做了以下增强：

1. 等待 ESP8266 启动完成。
2. 清理 USART2 残留字节和错误标志。
3. AT 最多重试 3 次。
4. 每次探测前等待短安静窗口。
5. 接收关键窗口不逐字节同步 printf。
6. AT 成功后发送 `ATE0`，关闭命令回显。
7. APP 触发 OTA 前发送 `AT+RST`，再复位 STM32，让 Bootloader 接手一个更干净的 ESP 状态。

### 5.5 如何判断是否修好

正常应看到：

~~~text
[WIFI-OTA] [S1] AT OK, command echo disabled.
[WIFI-OTA] [S2] Set CWMODE=1
[WIFI-OTA] [S3] WiFi got IP OK.
[WIFI-OTA] [S4] TCP connect OK.
~~~

如果仍失败，应查看 UART 错误计数和原始字节，不要只因为没有 `OK` 就判断 ESP 模块损坏。

准确结论是：增强接收清理和复位时序后，S1-S4 已恢复稳定；最初的单一丢字节位置没有被原始捕获证明。

---

## 6. 问题三：Server 收到 AT+CIPSEND，不是 HELLO JSON

### 6.1 现象

Bootloader 打印：

~~~text
[WIFI-OTA] [S5] HELLO dev=dev001 crc=0x0CA0B45A size=32680 bytes
~~~

但 Server 收到：

~~~text
[OTA] RX HELLO: AT+CIPSEND=74
[OTA] !!! HELLO parse fail
~~~

### 6.2 原因

HELLO JSON 和 AT 命令使用了同一个 `s_at_buf`。

外层先生成 HELLO：

~~~c
snprintf(s_at_buf, ..., "{HELLO JSON}");
cip_send(s_at_buf, hello_len);
~~~

`cip_send()` 内部又执行：

~~~c
snprintf(s_at_buf, ..., "AT+CIPSEND=%d\r\n", len);
~~~

传入的 payload 指针也指向 `s_at_buf`，所以 HELLO 在真正发送前已经被覆盖。

### 6.3 底层逻辑

C 语言函数参数传递的是地址，不是自动复制整段字符串。

~~~text
payload 指针 ----+
                 +----> 同一个 s_at_buf
s_at_buf --------+
~~~

修改 `s_at_buf` 后，payload 指向的内容也一起变化。这不是 TCP 把 JSON 变成 AT 命令，而是 STM32 本身发送了被覆盖后的内容。

### 6.4 怎么修复

增加独立 payload 缓冲：

~~~c
static char s_tx_payload[OTA_AT_BUF_SIZE];

memcpy(s_tx_payload, payload, len);
snprintf(s_at_buf, sizeof(s_at_buf), "AT+CIPSEND=%d\r\n", len);
...
uart_send(s_tx_payload, len);
~~~

现在：

~~~text
s_at_buf：AT 控制命令
s_tx_payload：要发到 TCP 的 HELLO/READY/ACK JSON
~~~

同时所有 JSON 控制帧增加 `\r\n`，让 TCP 对端能按行识别完整帧。

### 6.5 如何判断是否修好

修改前：

~~~text
Server RX HELLO: AT+CIPSEND=74
~~~

修改后：

~~~text
Server RX HELLO: {"type":"HELLO","dev":"dev001",...}
~~~

Server 能成功解析 `type=HELLO`、expect_size 和 expect_crc，才说明 HELLO 链路真正修好。

---

## 7. 问题四：Server 已发送 INFO，Bootloader S5 仍超时

### 7.1 现象

Server：

~~~text
[OTA] RX HELLO: {"type":"HELLO",...}
[OTA] TX INFO: size=32680 crc=0x0CA0B45A packets=32 pktsize=1024
[OTA] Waiting for READY
[OTA] !!! No READY, device timeout
~~~

Bootloader：

~~~text
[WIFI-OTA] [S5] !!! INFO wait timeout 10s
~~~

说明 HELLO 已经到 Server，但 Bootloader 没有处理 INFO。

### 7.2 原因

一开始只能知道 INFO 在返回链路某处丢失：

~~~text
Server -> WiFi -> ESP8266 TCP -> ESP UART -> STM32 USART2 -> JSON 解析
~~~

为避免猜测，加入低干扰 RAM 原始字节捕获。实机结果：

~~~text
[S5-DIAG v1] cip_stage=0x0F
[S5-DIAG] uart_flags ORE=1 FE=0 NE=0 PE=0
[S5-DIAG] raw_hex:
... 53 45 4E 44 20 4F 4B 0D 0A 0D 0A 2B
~~~

末尾 `0x2B` 是字符 `+`，说明 ESP 已开始输出 `+IPD`；同时 ORE=1，后面的 INFO 字节丢失。

### 7.3 底层逻辑

旧代码使用阻塞轮询：

~~~c
HAL_UART_Receive(&huart2, &byte, 1, timeout);
~~~

CPU 不在该函数里读取 UART 时，ESP仍可能连续输出。STM32F1 来不及取走前一个字节就会 ORE。HAL 清除 ORE 时，被清掉的 DR 字节不会自动交回给上层，因此 JSON 已经不完整。

### 7.4 怎么修复

USART2 改为 RXNE/ERR 中断加 4096 字节环形缓冲：

~~~text
中断立即读取 SR/DR
-> 记录 ORE/FE/NE/PE
-> 字节写入 ring[head]
-> 主循环从 ring[tail] 读取和解析
~~~

中断中禁止 printf、Flash 写入和 JSON 解析。

曾增加过 pending 单行暂存，试图处理 INFO 早于 SEND OK 到达的情况，但实机仍超时，因此不能宣称 pending 是根因或最终修复。真正有证据的根因是 ORE，真正有效的修复是中断环形缓冲。

### 7.5 如何判断是否修好

修复后实机进入：

~~~text
[WIFI-OTA] [S5] INFO OK: size=32680 crc=0x0CA0B45A packets=32 pktsize=1024
[WIFI-OTA] [S5] Erasing APP flash (464KB)...
[WIFI-OTA] [S5] Erase OK
~~~

同时应确认：

~~~text
ORE=0
ring_overflow=0
~~~

如果 raw 中已有完整 INFO，但仍没有 `INFO OK`，问题才应转向分片和 JSON 解析，而不是继续修改 UART。

---

## 8. 问题五：连续发送 DATA 容易丢包或跳序号

### 8.1 现象

早期 Server 连续发送 DATA，历史日志曾出现：

~~~text
seq=0
seq=1
seq=26
~~~

中间大量包没有被正确处理。

### 8.2 原因

Bootloader 收到 INFO 后还要擦除整个 APP 分区；每收到一包后还要 Base64 解码、计算 CRC、写 Flash 和打印进度。

如果 Server 不等待设备，连续发送几十包，ESP8266 会连续向 USART2 输出，4096 字节 ring 很快也会装满。

### 8.3 底层逻辑

环形缓冲只用于吸收短时间的速度差，不是存放整个固件的仓库。

一个 1024 字节固件块经过 Base64 和 JSON 包装后约 1.4 KB。连续几包就可能超过 4 KB ring。只增加 ring 大小不能从根本上解决发送端无限推送。

### 8.4 怎么修复

协议增加两层流控。

第一层是 READY：

~~~text
Server 发 INFO
-> Bootloader 擦除 APP
-> 擦除完成后发 READY
-> Server 才开始 DATA
~~~

第二层是 ACK：

~~~text
Server 发 DATA(seq=N)
-> Bootloader 解码、校验、写 Flash
-> Bootloader 发 ACK(seq=N)
-> Server 才发下一包
~~~

Bootloader 同时维护 `expected_seq`，序号跳跃、重复或越界都不能继续写入。

### 8.5 如何判断是否修好

正常日志必须满足：

~~~text
先 INFO OK 和 Erase OK
再 READY
然后 seq=0,1,2...packets-1 连续
每个 DATA 在 Server 侧都有对应 ACK
~~~

不能只看最终进度。序号中间缺失，即使累计进度看起来增加，也不能认为固件完整。

---

## 9. 问题六：S6 只收到 DATA0，ACK0 后 DATA1 消失

### 9.1 现象

中断环形缓冲修好 S5 后，S6 变成：

~~~text
[WIFI-OTA] [S6] OTA 3% (1024/32680 bytes, seq=0)
[WIFI-OTA] [S6] !!! DATA recv timeout 30000 ms. got=1024/32680
~~~

诊断显示 ACK0 完全成功：

~~~text
[S6-ACK-DIAG] attempted=1 cipsend_ok=1 seq=0 len=24
[S6-ACK-DIAG] payload={"type":"ACK","seq":0}
[S6-ACK-DIAG] ack cip_stage=0x0F ring_overflow=0 ORE=0
~~~

Server 也收到了 ACK0，并开始等待 ACK1。

### 9.2 原因

可以确认的故障边界是：ACK0 后的 DATA1 没有作为 UART 字节到达 STM32。

给 Server 增加 300 ms 延时后仍失败，因此“仅仅因为 ACK CIPSEND 需要约 225 ms”不是完整根因。

当时不能进一步证明 DATA1 是否已经进入 ESP8266 内部 TCP 缓冲。v3 日志中的 `probe_stage=0` 表示探针没有真正执行，不能据此说 TCP 已断开或缓冲长度为 0。

### 9.3 底层逻辑

ESP8266 主动模式下，TCP 数据一到就可能异步输出：

~~~text
+IPD,<len>:<DATA JSON>
~~~

但 Bootloader 此时可能正在：

- 等 ACK 的 `>` 提示符；
- 发送 ACK payload；
- 等 `SEND OK`；
- 写 Flash；
- 打印进度。

主动输出的时刻由网络和 ESP 决定，Bootloader 很难控制。

### 9.4 怎么修复

不再继续猜测主动模式内部时序，而是改为 ESP8266 被动接收。

READY 前执行：

~~~text
AT+CIPRECVMODE=1
~~~

S6 循环执行：

~~~text
AT+CIPRECVLEN?
-> 如果可用长度大于 0
-> AT+CIPRECVDATA=<len>
-> 累积完整 TCP/JSON 帧
-> 解析 DATA
-> 写 Flash
-> 回复 ACK
~~~

TCP 数据在 Bootloader 忙时留在 ESP 内部，Bootloader 空闲后主动取出。

同时修复：

- `strip_ipd()` 兼容 `+IPD` 和 `+CIPRECVDATA`；
- 按声明长度累积分片，完整 `\r\n` 后才解析；
- 拉取长度不能超过静态帧缓冲；
- 前缀和 JSON 分离，避免重叠 memcpy；
- S6 每次操作后重新读取实时 tick；
- S8 成功后恢复 `CIPRECVMODE=0` 并关闭 OTA TCP，避免影响 APP MQTT。

### 9.5 如何判断是否修好

首先必须出现：

~~~text
[WIFI-OTA] [S5] ESP passive receive mode enabled
~~~

然后序号连续：

~~~text
seq=0,1,2...31
~~~

最终看到：

~~~text
recv_bytes=32680
CRC_stream=0x0CA0B45A
CRC_readback=0x0CA0B45A
All 3 verify OK
~~~

当前 `串口数据.txt` 已经证明这条链路完整通过。

如果出现 `ESP passive receive mode not supported`，说明 ESP AT 固件不支持当前命令，不能继续按同一方案分析。

---

## 10. 问题七：进度 100%，仍不能直接认为升级成功

### 10.1 现象

S6 可能已经打印 100%，但 Flash 数据仍可能因为写入长度、地址、最后一包或数据损坏而不正确。

### 10.2 原因

进度只是：

~~~text
累计处理字节数 / INFO.size
~~~

它不能证明这些字节内容正确，也不能证明 Flash 实际保存了相同内容。

### 10.3 底层逻辑

例如当前 BIN 为 33336 字节，块大小 1024：

~~~text
前 32 包：32 * 1024 = 32768
最后一包：33336 - 32768 = 568 字节
~~~

最后一包只能写 568 个真实字节。多写填充或少写尾部都会让 Flash CRC 不匹配。

### 10.4 怎么修复

S8 必须做三项检查：

~~~text
1. recv_bytes == INFO.size
2. CRC_stream(RAM) == INFO.crc
3. CRC_readback(APP Flash, INFO.size) == INFO.crc
~~~

只有三项全通过，才能更新参数区并 JumpToApp。

任何一步失败都进入 FAIL，不再跳转可能已经被擦除或半写入的 APP。KEY0 + Reset 保留 USART1 Ymodem 救砖路径。

### 10.5 如何判断是否修好

成功必须看到：

~~~text
[S8] recv_bytes == INFO.size
[S8] CRC_stream == INFO.crc
[S8] CRC_readback == INFO.crc
[S8] All 3 verify OK
[S8] JumpToApp
~~~

还要继续看新 APP 是否启动、重新进入 MQTT_WORKING 并恢复业务。只看到 `JumpToApp` 文本，不能证明跳转后的程序真正运行。

---

## 11. 问题八：APP 启动后覆盖了正确的 size/CRC

### 11.1 现象

Bootloader OTA 成功时保存：

~~~text
fw_size_bytes = 32680
fw_crc32      = 0x0CA0B45A
~~~

新 APP 启动后，参数却变成整个 APP 分区：

~~~text
fw_size_bytes = 475136
fw_crc32      = 整个 475136 字节分区的 CRC
~~~

APP 仍能运行，所以这个问题容易被忽略。

### 11.2 原因

旧 APP 启动执行：

~~~c
g_param_buf_internal.fw_size_bytes = APP_FLASH_SIZE;
g_param_buf_internal.fw_crc32 = CRC32_CalcAppFlash();
~~~

`APP_FLASH_SIZE` 是分区最大容量，不是当前 app.bin 的真实长度。

### 11.3 底层逻辑

服务器 CRC 的对象是：

~~~text
app.bin 的 32680 或 33336 个真实字节
~~~

旧 APP CRC 的对象却是：

~~~text
真实 BIN + 后面大量未使用的 0xFF 或历史残留数据
~~~

计算对象不同，CRC当然不同。它不会破坏已经写入的代码，但会让参数区无法回答“设备安装的是哪个发布文件”。

### 11.4 怎么修复

删除 APP 启动时这两次覆盖。APP 启动只更新：

~~~text
boot_count
last_reset_reason
~~~

`fw_size_bytes/fw_crc32` 只由 Bootloader 在 S8 校验全部通过后写入：

~~~c
s_param_work.fw_size_bytes = g_info.fw_size;
s_param_work.fw_crc32      = g_info.fw_crc;
~~~

### 11.5 如何判断是否修好

使用当前加固 BIN OTA 成功后，APP 重启并重新加载参数区时仍应看到：

~~~text
fw_size_bytes = 33336
fw_crc32      = 0xC6FFC87C
~~~

再次复位 APP 后也应保持不变，不能又变成 475136。

这一项已经完成代码修改和构建，但当前新加固版仍待硬件实测。

---

## 12. 问题九：版本信息不完整，不能可靠防止降级

### 12.1 现象

旧流程主要保存 size、CRC 和 BUILD。目标 `major.minor.patch.build` 没有完整经过参数区传到 Bootloader，成功后有时只能对 BUILD 自增。

另外旧 BUILD 使用编译时间哈希。哈希会变化，但后一次哈希值不保证一定比前一次大。

### 12.2 原因

参数区没有独立的四段“OTA 目标版本”字段，APP 和 Bootloader 之间缺少完整版本契约。

如果版本不可比较，就无法可靠判断：

~~~text
这是正常升级？
这是同版本重复安装？
这是降级？
~~~

### 12.3 底层逻辑

版本按顺序比较：

~~~text
major -> minor -> patch -> build
~~~

例如：

~~~text
1.0.1.0 > 1.0.0.99
2.0.0.0 > 1.99.99.99
1.0.0.5 > 1.0.0.4
~~~

不能把四个数字相加，也不能使用不可排序的随机哈希。

### 12.4 怎么修复

参数区原保留空间 `0x68~0x6F` 增加：

~~~c
uint16_t ota_new_fw_ver_major;
uint16_t ota_new_fw_ver_minor;
uint16_t ota_new_fw_ver_patch;
uint16_t ota_new_fw_build_num;
~~~

流程变成：

~~~text
MQTT 四段版本
-> APP ota_new_fw_ver_*
-> Bootloader RAM
-> S8 成功后 fw_ver_*/fw_build_num
~~~

整体结构仍为 4096 字节，并增加编译期检查：

~~~text
sizeof(flash_param_t) == 4096
offsetof(ota_new_fw_ver_major) == 0x68
offsetof(tail_marker) == 0xFF0
~~~

APP、Bootloader 和 `shared files` 三份定义必须同步。

BUILD 改为手动递增：

~~~c
#define FW_BUILD_NUM 4U
~~~

普通升级默认拒绝目标版本小于或等于已安装版本。

### 12.5 force 怎么工作

普通升级省略 force：

~~~json
{"cmd":"ota","major":1,"minor":0,"patch":0,"build":5,"size":33336,"crc":3338651772}
~~~

APP 执行正常版本保护。

恢复或同版本重装才发送：

~~~json
{"cmd":"ota","major":1,"minor":0,"patch":0,"build":4,"size":33336,"crc":3338651772,"force":1}
~~~

`force=1` 只允许 APP 写入原本被版本策略拒绝的 OTA 请求，不会跳过 Bootloader 的包序号、size、RAM CRC 和 Flash CRC。

### 12.6 如何判断是否修好

正常升级应看到目标版本高于安装版本并被接受。

同版本或降级且没有 force，应看到：

~~~text
[OTA] REJECT version ... <= installed ...
~~~

带 force 时应看到明确警告，然后进入 OTA：

~~~text
[OTA] WARN forced downgrade/reinstall ...
~~~

升级成功并重启后，参数区安装版本应等于 Server 指定版本，而不是简单 BUILD+1。

---

## 13. 其他加固修改

### 13.1 OTA JSON 缓冲扩大

加入四段版本和 force 后 JSON 变长，原 128 字节可能截断尾部字段。OTA PUBLISH 解析缓冲扩大到 192 字节，并保留结尾 `\0`。

判断方法：APP 应完整打印版本、size、CRC 和 force，不能出现 `OTA cmd parse FAIL` 或尾部字段缺失。

### 13.2 Base64 防御性检查

Base64 反查表未使用项设置为 `0xFF`，输入长度必须是 4 的倍数。非法字符或截断数据不能继续解码和写 Flash。

### 13.3 INFO geometry 检查

Bootloader 检查：

~~~text
packets == ceil(size / pktsize)
size <= APP_FLASH_SIZE
size、packets、pktsize 都大于 0
~~~

错误 INFO 在擦除 APP 前就会被拒绝。

### 13.4 任务栈余量

成功日志暴露：

~~~text
TaskLED       free=96/128B，total=384B
TaskSemHandle free=112B，total=256B
~~~

当前调整：

~~~text
TaskLED       384 -> 512 字节
TaskSemHandle 256 -> 384 字节
~~~

这不是 S6 根因，而是根据实际 high-water mark 做的长期稳定性修复。

### 13.5 Server 和自动验收

`ota_server.py` 新增：

~~~text
--version
--force
--disconnect-after-seq
--delay-after-seq
--delay-seconds
--corrupt-seq
~~~

`tools/verify_ota_log.py` 检查序号、长度、RAM CRC、Flash CRC、INFO CRC、BIN 和安装版本，避免人工只看 100% 就误判成功。

---

## 14. 最终成功日志怎么读

### 14.1 APP 阶段

~~~text
[MQTT] RX PUBLISH topic=iot/dev001/ota payload={...}
[MQTT] CMD: OTA v... size=... crc=...
[OTA] Writing OTA request flag to param area...
[PARAM] Write OK
[OTA] Resetting ESP8266 (AT+RST)...
~~~

说明 APP 收到命令并成功写参数区。

### 14.2 Bootloader 控制阶段

~~~text
[BOOT] OTA_REQUEST FLAG detected
[S1] AT OK
[S3] WiFi got IP OK
[S4] TCP connect OK
[S5] INFO OK
[S5] Erase OK
[S5] ESP passive receive mode enabled
~~~

说明参数区、ESP AT、WiFi、TCP、INFO、Flash 擦除和被动模式正常。

### 14.3 数据阶段

~~~text
seq=0
seq=1
...
seq=31
~~~

必须从 0 连续到 `packets-1`，不能跳号。

### 14.4 校验和启动阶段

~~~text
recv_bytes == INFO.size
CRC_stream == CRC_readback == INFO.crc
All 3 verify OK
JumpToApp
[MQTT] Enter MQTT_WORKING
~~~

最后出现 APP 的 MQTT_WORKING 和传感器发布，才是完整成功。

---

## 15. 这次代码修改作用总表

| 修改 | 解决的问题 | 判断依据 |
|---|---|---|
| 4 KB 参数工作区移到静态 BSS | Bootloader 主栈溢出 | 函数栈帧 `0x1000 -> 0x30` |
| 清 UART 残留、AT 重试、ATE0 | S1 启动状态不稳定 | S1-S4 连续通过 |
| `s_tx_payload` 独立副本 | HELLO 被 CIPSEND 覆盖 | Server 收到正确 HELLO JSON |
| JSON 增加 CRLF | TCP 没有消息边界 | Server 可立即按完整行解析 |
| USART2 中断 + 4 KB ring | S5 INFO 因 ORE 丢失 | ORE/ring 正常且 INFO OK |
| READY | 擦 Flash 时 Server 提前发 DATA | Erase OK 后才开始 DATA |
| DATA/ACK 停等 | 连续推送导致丢包 | seq 严格连续 |
| expected_seq 检查 | 跳包、乱序仍可能写 Flash | 异常序号直接失败 |
| ESP 被动接收 | S6 ACK0 后主动 +IPD 不再输出 | seq=0 到最后一包完整通过 |
| 按长度累积分片 | 一条 JSON 被 UART 拆开 | 完整 CRLF 帧后才解析 |
| 三项 S8 校验 | 100% 不代表 Flash 正确 | size、RAM CRC、Flash CRC 全相等 |
| APP 不覆盖 size/CRC | 安装元数据变成整分区信息 | APP 重启后仍保持真实 BIN 值 |
| 四段目标版本 | Bootloader 不知道准确版本 | 重启后安装版本等于目标版本 |
| 手动递增 BUILD | 时间哈希不可排序 | 版本关系明确 |
| force 恢复通道 | 同版重装/降级没有入口 | 无 force 拒绝，有 force 警告并继续 |
| JSON 128 -> 192 | 新字段导致命令截断 | APP 完整打印全部字段 |
| 两个任务增加栈 | 长期运行栈余量低 | 新硬件日志不再低于告警线 |
| 故障注入和日志脚本 | 只能人工验证正常路径 | 自动得到 PASS/FAIL 和明确原因 |

---

## 16. 当前正确升级操作

### 16.1 烧录新版 Bootloader

通过 Keil/SWD 将：

~~~text
bootloader\MDK-ARM\00_bootloader\00_bootloader.hex
~~~

烧录到 `0x08000000`。

已经实机通过的被动接收基线 SHA-256：

~~~text
1FC4870FA996A6C596BE39EE00594A3AC502644042C24CE49F499CB79413B2B1
~~~

当前加固版 Bootloader SHA-256：

~~~text
B96B39A4BF5C26B881605E424FFF4EED25E1E957A90AD01401B3A20587E8C57F
~~~

当前加固版仍待上板回归。

### 16.2 每次发布先递增版本

例如下一版：

~~~c
#define FW_VER_MAJOR 1U
#define FW_VER_MINOR 0U
#define FW_VER_PATCH 0U
#define FW_BUILD_NUM 5U
~~~

重新编译生成新的 `app\MDK-ARM\app.bin`。任何代码变化都可能改变 BIN 的 size 和 CRC。

### 16.3 启动 Server

普通升级：

~~~powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.5
~~~

首次迁移、同版重装或明确降级：

~~~powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force
~~~

Server 自动计算实际 BIN 的 size/CRC，并打印 MQTT JSON。把它原样发布到：

~~~text
iot/dev001/ota
~~~

不要手工沿用上一次固件的 size 或 CRC。

### 16.4 第一次从旧参数迁移

旧参数可能记录 `1.0.3 build2`，新目标是 `1.0.0.4`，按版本比较属于降级，所以第一次需要 `force=1`。

如果第一次仍由旧 APP 发起，旧 APP 不会写新增的四段目标版本字段。稳妥做法是在新 APP 启动后，对同一个 BIN 再执行一次 `--force` OTA，使安装版本元数据准确写成 `1.0.0.4`。

迁移完成后，后续使用 `1.0.0.5、1.0.0.6...` 正常递增，不再默认使用 force。

### 16.5 验收新日志

~~~powershell
python tools\verify_ota_log.py 串口数据.txt --firmware app\MDK-ARM\app.bin --version 1.0.0.4
~~~

期望：

~~~text
[OTA-VERIFY] PASS packets=... size=33336 crc=0xC6FFC87C
~~~

旧成功日志对应 `32680/0x0CA0B45A/1.0.3.2`。不能用新 BIN 验证旧日志，否则工具正确地报告不匹配。

---

## 17. 看到日志后如何快速判断

| 日志 | 大白话含义 | 优先检查 |
|---|---|---|
| 进入 OTA 后完全卡住 | 状态机可能还没真正运行 | 主栈、大局部变量、反汇编 |
| S1 三次 AT 无 OK | STM32 和 ESP 基本通信没建立 | ESP 复位、启动时间、UART 错误、波特率 |
| Server 收到 AT+CIPSEND | STM32 发错了 payload | 共享缓冲是否覆盖 |
| Server 已发 INFO，Bootloader超时 | 返回链路某处丢失 | raw 字节、ORE、ring、JSON 分片 |
| ORE > 0 | STM32 来不及读 UART | RX 中断和关键窗口阻塞 |
| No READY | INFO 未通过或擦除失败 | S5 INFO 和 Flash 日志 |
| seq 跳号 | 中间包丢了或解析错位 | READY/ACK、expected_seq、被动接收 |
| 只收到 seq=0 | ACK 后的下一包未到 STM32 | 双端 ACK、被动模式、ESP TCP 缓冲 |
| 进度 100% | 只代表累计长度到了 | 继续看 S8 三项校验 |
| RAM CRC 错 | 收到的数据和 Server BIN 不同 | Base64、数据损坏、corrupt 注入 |
| Flash CRC 错 | 收到正确但写入结果不同 | Flash 地址、长度、最后一包、供电 |
| FAIL 后不跳 APP | APP 可能已经被擦除 | 属于保护，KEY0+Reset 救砖 |
| version <= installed 被拒绝 | 版本保护生效 | 提高版本；恢复才使用 force |
| verify 提示 BIN 不匹配 | 日志和 BIN 不是同一次升级 | 找到当次真实 BIN |

不要根据单条日志过早下结论：

- Server 打印“已发送”不等于 STM32 已收到。
- ACK CIPSEND 成功不等于 Server 已收到 ACK。
- ORE=0 不等于 DATA 已经进入 ESP。
- `probe_stage=0` 只代表探针没执行，不能说明 TCP 状态。
- 进度 100% 不等于 CRC 正确。
- 打印 JumpToApp 不等于 APP 已经开始运行。

---

## 18. 当前仍需验证的内容

### 18.1 加固版硬件回归

真实 size/CRC 保留、四段版本、force、任务栈和测试工具已经构建通过，但需要重新烧录当前加固版，在硬件上验证：

~~~text
Target version 正确
seq 连续
三项 CRC 正确
APP 重启后 size=33336、CRC=0xC6FFC87C
安装版本=1.0.0.4
MQTT 业务恢复
~~~

### 18.2 故障注入

断链：

~~~powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force --disconnect-after-seq 5
~~~

延迟 35 秒：

~~~powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force --delay-after-seq 5 --delay-seconds 35
~~~

破坏数据：

~~~powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force --corrupt-seq 5
~~~

预期都不能进入成功 JumpToApp。

### 18.3 断电与救砖

还应分别在写 OTA 参数、擦除后、S6 约 50%、S8 参数提交时断电，验证重新上电行为和 KEY0 + Reset 的 USART1 Ymodem 救砖路径。

### 18.4 参数结构和 SRAM

APP、Bootloader、`shared files` 三份参数结构必须同步。Bootloader 已使用 4 KB UART ring、2 KB被动帧缓冲、4 KB 参数区和 4 KB 主栈，继续加静态数组前必须检查 MAP，不能只看编译成功。

---

## 19. 新手最应该记住的六件事

1. APP 只负责提出 OTA 请求，真正覆盖 APP Flash 的是 Bootloader。
2. UART 会因为 CPU 来不及读取而 ORE；中断 ring 解决短时接收，READY/ACK 和被动接收解决整体速度控制。
3. TCP 是连续字节流，必须用长度和 `\r\n` 还原完整 JSON，不能把一次 recv 当成一条消息。
4. APP 分区容量不是 app.bin 大小；CRC 必须只计算真实 BIN 长度。
5. `force=1` 只绕过版本限制，不绕过 size、包序号和 CRC 校验。
6. 真正成功要看完整闭环：MQTT 命令 -> 参数区 -> S1-S8 -> 三项校验 -> 安装元数据 -> 新 APP -> MQTT 业务恢复。

---

## 20. 参考代码和日志

- OTA 状态机和被动接收：`bootloader/Core/Src/boot_ota.c`
- Bootloader OTA 入口：`bootloader/Core/Src/main.c`
- USART2 中断入口：`bootloader/Core/Src/stm32f1xx_it.c`
- Bootloader 参数读写：`bootloader/Core/Src/flash_param.c`
- APP MQTT OTA 命令入口：`app/Src/app_esp8266.c`
- APP 版本判断和 force：`app/Src/ota_manager.c`
- APP 参数初始化与请求写入：`app/Src/flash_param.c`
- 参数结构：`app/Inc/flash_partition.h`、`bootloader/Core/Inc/flash_partition.h`
- OTA Server：`ota_server.py`
- 日志自动验收：`tools/verify_ota_log.py`
- 成功串口日志：`串口数据.txt`
- 完整调试时间线：`OTA_DEBUGGING_RECORD.md`
- 回归步骤：`OTA_REGRESSION_TEST.md`
- 文档格式参考：`D:\iot_ota_project\10_ymodem_serial_iap\pitfa.md`
