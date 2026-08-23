# Ymodem 串口 IAP 踩坑记录与原理说明

> 适用工程：D:\iot_ota_project\10_ymodem_serial_iap<br>
> 芯片：STM32F103ZE<br>
> 串口：USART1，115200 8N1，无硬件流控<br>
> 目标：通过 Tera Term 的 Ymodem 把 app.bin 写入 Bootloader 之后的 APP Flash 区

本文只记录这次 Ymodem 串口传输问题的定位和修复过程，面向刚接触 UART、Ymodem 和 Bootloader 的读者。全文按“现象 -> 原因 -> 修复 -> 如何判断日志”的顺序说明。

---

## 1. 最终结论

这次问题不是单独的 CRC 算法错误，而是几个问题叠加：

1. UART 接收使用阻塞轮询时，Bootloader 发出 ACK/C 后，发送端可能立即开始下一帧。接收端在发送调试文本或切换接收阶段时来不及取走字节，导致串口接收错位或溢出。
2. Ymodem 接收器对首帧、重传、EOT 结束帧和尾部控制字节的兼容性不足，导致已经收到有效数据时仍然误报失败。
3. EOT 分支在旧代码中提前返回，没有把已接收大小写回 received_size，所以日志中的 size=0 可能是统计错误，并不一定代表真的没有接收数据。
4. Flash 写入、RAM 流式 CRC32 和 Flash 回读 CRC32 必须配套验证，否则只能知道“串口收到了数据”，不能知道“Flash 中的 APP 完整”。

最终版本加入 USART1 RX 中断环形缓冲区、完整帧校验、重试与重复包处理、兼容单/双 EOT、流式写 Flash 和 CRC 双重校验后，传输成功。

成功日志中的核心证据：

~~~text
[IAP] Verifying: CRC calc from flash (0x08008000, 30268 bytes)...
[IAP]   CRC_ram   (stream accum) = 0x59B4EA2B
[IAP]   CRC_flash (read-back)   = 0x59B4EA2B
[IAP] CRC MATCH - Firmware integrity confirmed.
[IAP] Param Save OK. build=2, fw_crc=0x59B4EA2B, fw_size=30268B
[IAP] ===== ALL DONE. Soft-reset in 300ms... =====
[BOOT] APP valid, jumping to 0x08008000...
~~~

这说明：接收数据大小正确、RAM 中收到的数据和 Flash 回读数据一致、参数保存成功、复位后 APP 能运行。

---

## 2. 先理解整个数据链路

一次串口 IAP 不是“串口收到字节后直接跳转 APP”，而是下面几层连续工作：

~~~text
Tera Term
    |
    | UART 字节流：C、SOH/STX、序号、数据、CRC16、ACK/NAK/EOT
    v
USART1 RX 中断
    |
    | 把每个字节放入 4 KB 环形缓冲区
    v
Ymodem 帧解析器
    |
    | 检查帧头、序号、序号反码、CRC16、重传、EOT
    v
IAP 回调 iap_on_packet()
    |
    | 每收到一包：更新 CRC32 + 写入 APP Flash
    v
Flash 回读校验
    |
    | CRC32(RAM 接收流) == CRC32(Flash 实际内容)
    v
保存固件大小/CRC参数 -> 软件复位 -> Bootloader 检查 APP -> 跳转 APP
~~~

排查时要分层：

- UART 层：字节有没有收到、有没有溢出、TX/RX/GND 是否正确。
- Ymodem 层：一帧是否完整、CRC16 是否匹配、序号是否正确。
- Flash 层：收到的有效数据是否真的写入、最后不足 4 字节的数据是否丢失。
- 固件完整性层：Flash 回读 CRC32 是否与接收流 CRC32 相同。
- 启动层：向量表、栈指针和 Reset_Handler 是否允许 Bootloader 跳转。

前几次失败主要发生在 UART/Ymodem 层；最后成功日志证明 Flash 和启动层也通过了验证。

---

## 3. Ymodem 协议基础

### 3.1 控制字节

本工程使用 Ymodem-CRC，也就是接收端发送字符 C，要求发送端使用 CRC16/XMODEM 校验。

| 字节 | 十六进制 | 含义 |
|---|---:|---|
| SOH | 0x01 | 后面跟 128 字节数据 |
| STX | 0x02 | 后面跟 1024 字节数据 |
| EOT | 0x04 | 发送端表示文件数据发送完毕 |
| ACK | 0x06 | 接收正确，发送端可以继续 |
| NAK | 0x15 | 接收失败，请发送端重传 |
| CAN | 0x18 | 取消传输，通常连续两个 CAN 才算取消 |
| C | 0x43 | 请求使用 CRC16 模式 |
| Ctrl+Z | 0x1A | 传统文件传输填充字节，本工程不把它计入真实文件大小 |

C 不是普通的文本提示，而是协议控制字节。串口终端中看到一个或多个裸 C 是正常现象。

### 3.2 普通数据帧格式

~~~text
+--------+------+-------+-------------------+---------+
| SOH/STX| seq  | ~seq  | payload           | CRC16   |
+--------+------+-------+-------------------+---------+
| 1 byte | 1 B  | 1 B   | 128 or 1024 bytes | 2 bytes |
+--------+------+-------+-------------------+---------+
~~~

- seq 是帧序号，从 1 开始递增，超过 255 后按 8 位回绕。
- ~seq 是序号按位取反。例如 seq=0x15 时，反码必须是 0xEA。
- seq 和 ~seq 用来发现帧头错位或单字节错误。
- CRC16 计算范围只包括 payload，不包括 SOH/STX、序号和 CRC 自身。
- 本工程使用 CRC16/XMODEM：多项式 0x1021，初始值 0，CRC 高字节在前。

### 3.3 第 0 帧：文件头

Ymodem 的第 0 帧不是 APP 数据，而是文件信息，payload 通常是：

~~~text
app.bin\0
30268\0
~~~

也就是“文件名 + 0 字节 + 十进制文件大小 + 0 字节 + 填充”。

接收器必须先解析这个大小，后面才知道：

- APP 应该接收多少字节；
- 最后一帧的多余填充不能写入 Flash；
- EOT 到来时，接收总量是否真的等于声明值。

### 3.4 EOT 为什么可能出现一次或两次

常见流程是：

~~~text
发送端 -> EOT
接收端 -> NAK
发送端 -> EOT
接收端 -> ACK
~~~

但不同终端软件实现不完全一致。有些发送端只发一次 EOT，有些发送端在 EOT 后还带一个额外控制字节。接收器如果只接受一种形式，就会出现“数据已经完整，但最后一步失败”。

当前代码：

- 第一次 EOT 回 NAK；
- 再收到第二个 EOT 时回 ACK；
- 等待第二个 EOT 超时，也接受单 EOT；
- 只有明确收到 CAN 才取消；
- 对单 EOT 后的非 CAN 额外字节进行丢弃，然后继续完成大小和 CRC 校验。

### 3.5 结束时的空 block-0

标准 Ymodem 批量传输结束时，接收端可能再发送一次 C，发送端用一个空的 block-0 表示“批量传输结束”。本工程在 ymodem.c 的 phase 3 会尝试接收这个空块。

实际测试中，Tera Term 可能不发送这个空块，或者发送端口上残留一个 0x00。只要前面的真实文件大小和 CRC32 已经验证通过，就不能把这个可选结束块当作 APP 数据错误。

---

## 4. 原始失败日志说明

### 4.1 r3：首帧根本没有稳定进入解析器

典型日志：

~~~text
[YMODEM] Frame-start timeout
[YMODEM] Header wait/reject retry 1/5
[IAP] Ymodem FAIL! code=-2 (size=0 bytes)
...
hdr=0xFB seq=0 neg=0x00 len=0
header_ok=0
~~~

重点是 header_ok=0：文件头第 0 帧没有被确认接收。0xFB 不是合法 Ymodem 帧头，合法帧头只有 0x01（SOH）或 0x02（STX），所以此时不能说明 APP 数据已经开始写入。

code=-2 在当前工程中表示 Ymodem CRC/帧接收错误，不是 APP 固件 CRC32 错误。失败发生在“协议帧还没成功收完整”的阶段。

可能原因：

- Bootloader 还没发出 C，发送端就开始发送；
- 发送端的首字节被接收端错过；
- UART 阻塞接收和调试打印同时使用 USART1，接收窗口被打断；
- 串口工具选择了 XMODEM 或 YMODEM-G，而不是普通 YMODEM；
- RX/TX 交叉、GND 或串口参数不正确。

### 4.2 r6：文件头通过了，但数据帧同步丢失

典型信息：

~~~text
header_ok=1
hhdr=0x01 hseq=0 hlen=128
header_size=30268 parse=0
phase=2
hdr=0x81 seq=0 neg=0x00 len=0
~~~

这说明文件头已经成功解析，文件大小也已经得到；失败点从 header phase 进入了 data phase。

0x81 不是 SOH、STX 或 EOT，通常说明接收器从错误的字节边界开始解释数据。典型过程：

~~~text
发送端收到 ACK/C
    -> 立即发送下一帧
接收端仍在阻塞发送调试文本或切换 HAL_UART_Receive
    -> RXNE/ORE 处理不及时
    -> 丢掉帧头或序号中的某个字节
    -> 后续 payload 中的某个字节被误认为“帧头”
~~~

这类问题看起来像 CRC 算法错误，其实 CRC 甚至还没有机会正确计算，因为解析器已经不知道一帧从哪里开始。

### 4.3 r7：真正收到 EOT，但旧日志把大小显示成 0

典型信息：

~~~text
frame_ret=-1
hdr=0x04
header_ok=1
header_size=30268
size=0 bytes
~~~

0x04 就是 EOT，frame_ret=-1 是 y_recv_frame() 用来表示 EOT 的返回值。它不是 CRC 失败。

旧代码在 EOT 分支直接 break 或返回，没有先执行：

~~~c
*received_size = total_received;
~~~

于是 IAP 层拿到的 g_total_size 仍然是初始化值 0，打印出了误导性的 size=0。实际上前面的数据包可能已经写入了大部分甚至全部 APP。

排查 Ymodem 时不能只看 code 和 size，还要看：

- hdr 是否是 0x04（EOT）；
- header_ok 是否为 1；
- header_size 是否正确；
- flash_written 是否已经增长；
- 最终是否有 Flash 回读 CRC32。

---

## 5. 关键修复一：用 RX 中断环形缓冲区解决 UART 竞争

### 5.1 原来的问题：阻塞接收不是“持续监听”

如果接收代码大致是：

~~~c
HAL_UART_Receive(&huart1, &header, 1, timeout);
HAL_UART_Receive(&huart1, &seq, 1, timeout);
HAL_UART_Receive(&huart1, data, 1024, timeout);
~~~

看起来像逐字节接收，但代码只有执行到某个 HAL_UART_Receive() 时才主动等待。此时如果 CPU 正在：

- 发送 ACK 或 C；
- 用 printf 输出调试信息；
- 写 Flash；
- 从一个接收阶段切换到下一个阶段；

发送端可能已经把下一个字节送到 USART1。若 RXNE 没有及时清除，就可能出现 ORE（Overrun，接收溢出），字节丢失后整个帧就错位了。

### 5.2 修复后的结构

当前 ymodem.c 使用 4 KB 环形缓冲区：

~~~c
#define Y_RX_RING_SIZE 4096U
static volatile uint8_t  y_rx_ring[Y_RX_RING_SIZE];
static volatile uint16_t y_rx_head;
static volatile uint16_t y_rx_tail;
~~~

USART1 RX 中断只做很少的事情：

1. 读取 USART 数据寄存器；
2. 把一个字节放到 ring[head]；
3. 推进 head；
4. 如果 head 追上 tail，记录 overrun 和 UART 错误。

主循环中的 y_recv_byte() 再从 ring[tail] 取字节，并负责超时判断。

这样，CPU 即使短时间在发送 ACK、打印文本或执行其他逻辑，RX 中断仍然会持续搬运字节，发送端的下一帧不会因为主循环暂时没调用 HAL_UART_Receive() 而直接丢失。

### 5.3 为什么环形缓冲区合适

环形缓冲区把“接收字节”和“解析帧”解耦：

~~~text
UART RX 中断：快，不能阻塞，只负责存字节
        |
        v
环形缓冲区：临时吸收发送端和主循环之间的速度差
        |
        v
Ymodem 解析：可以按协议字段逐个读取
~~~

中断不能调用 printf、不能写 Flash、不能等待超时，否则会把“接收中断必须快”的优势再次破坏。

### 5.4 ACK/C 前先打印诊断文本

文件头通过后，发送端通常会在收到 ACK 和第二个 C 后立即发送第一帧数据。因此当前代码特意先打印：

~~~c
printf("[YMODEM] Header OK: ...\r\n");
y_send_byte(Y_ACK);
y_send_byte(Y_C_CHAR);
~~~

如果把 printf 放在 y_send_byte(Y_C_CHAR) 后面，发送端可能已经开始发数据，而 Bootloader 此时忙于逐字符发送调试文本。RX 中断可以降低风险，但仍应把协议控制字节之后的调试输出减到最低。

---

## 6. 关键修复二：把 Ymodem 帧解析做完整

当前 y_recv_frame() 的处理顺序：

~~~text
1. 等待一个帧起始字节
2. 判断 SOH/STX/EOT/CAN
3. 读取 seq 和 ~seq
4. 检查 seq == (uint8_t)~seq
5. 按 SOH/STX 决定读取 128/1024 字节
6. 读取两个 CRC 字节
7. 计算 CRC16/XMODEM 并比较
8. 只有全部通过才把这一帧交给上层
~~~

这些步骤不能省略：

- 只检查 CRC，不检查序号，可能把旧的重传包当成新包写两次；
- 只检查序号，不检查 CRC，可能把损坏数据写入 Flash；
- 只看到 SOH/STX 就写 Flash，还没收完 payload 和 CRC，无法判断完整性。

### 6.1 SOH 和 STX 必须同时支持

Tera Term 或其他 Ymodem 发送器可能选择：

- SOH：128 字节小包；
- STX：1024 字节大包。

文件头通常是 SOH，但数据帧通常是 STX。只支持其中一种，会出现“文件头能过，数据帧永远失败”或相反的情况。

### 6.2 序号和重复包

接收器维护 expected_seq：

~~~text
文件头：seq = 0
第一个数据包：seq = 1
第二个数据包：seq = 2
...
~~~

发送端没有及时收到 ACK 时，可能重传上一帧。此时接收器看到的序号等于 last_acked_seq，应该再次 ACK，但不能再次写 Flash、不能再次累加 CRC32。

当前代码的设计意图：

~~~c
if (seq == last_acked_seq && expected_seq != 1U) {
    y_send_byte(Y_ACK);
    continue;
}
~~~

如果收到既不是期望序号、也不是合法重复包的序号，则回 NAK，让发送端重传。

### 6.3 超时、CRC 错误和取消要区分

当前日志信息对应：

~~~text
Frame-start timeout       没等到下一帧的第一个字节
Header sequence timeout   收到帧头，但 seq/~seq 没收完整
Data timeout              payload 没收满
CRC bytes timeout         两个 CRC 字节没收满
CRC mismatch              收到了完整帧，但 CRC 不匹配
Bad sequence complement   seq 和 ~seq 不互补
Single CAN received       收到一个 CAN，但未形成明确取消
~~~

不要把所有负返回值都叫“CRC 错误”。例如 Frame-start timeout 更可能是握手、发送时序或接收溢出；它还没有读到完整帧，不能证明 CRC 算法有问题。

### 6.4 文件头大小必须安全解析

文件头中的大小是 ASCII 十进制字符串，例如 30268。解析时必须：

- 只接受数字；
- 检查乘 10 和加下一位时是否 32 位溢出；
- 大小不能为 0；
- 大小不能超过 APP Flash 区域；
- 收完数据后 total_received 必须等于文件头声明值。

否则发送错误头部或超大文件，可能导致写越界或把填充数据错误地当成固件。

---

## 7. 关键修复三：正确处理 EOT

当前 EOT 逻辑核心：

~~~c
if (ret == -1) {                 /* y_recv_frame() 收到 EOT */
    y_send_byte(errors == 0 ? Y_NAK : Y_ACK);
    /* 兼容第二个 EOT、单 EOT、额外控制字节和 CAN */
    ...
    break;
}
~~~

跳出数据循环后必须更新：

~~~c
if (received_size != NULL) {
    *received_size = total_received;
}
~~~

这样 IAP 层的 g_total_size 才能拿到真实值。

还要注意：EOT 只表示发送端不再发送更多帧，不自动表示文件完整。因此 EOT 后仍必须执行：

~~~c
if (total_received != file_total_size) {
    return Y_ERR_SIZE;
}
~~~

如果文件头说有 30268 字节，但 EOT 到来时只收到 28000 字节，必须失败，不能因为“收到了 EOT”就成功。

### 7.1 为什么 r7 的 frame_ret=-1 是好线索

在失败日志中：

~~~text
frame_ret=-1 hdr=0x04
~~~

这两个字段说明：

- frame_ret=-1：内部识别为 EOT；
- hdr=0x04：确认字节确实是 EOT；
- header_ok=1：文件头已经通过；
- header_size=30268：发送端文件大小已经正确读取。

因此当时重点不是继续追查 CRC16 算法，而是检查 EOT 分支是否正确保存接收大小、是否把单 EOT 当非法、是否在 EOT 后错误返回。

---

## 8. 关键修复四：Ymodem 接收和 Flash 写入配合

Ymodem 只负责把“通过 CRC16 的 payload”交给上层。真正写 Flash 的工作在 iap_on_packet()：

~~~c
CRC32_StreamUpdate(&g_running_crc, data, len);
FLASH_WriteBuf(APP_FLASH_START + offset, data, len);
g_written += len;
~~~

### 8.1 只写真实文件长度，不写填充

1024 字节 STX 帧的最后一帧通常包含填充，但文件头声明的真实大小可能不是 1024 的整数倍。当前代码：

~~~c
write_len = data_len;
if ((uint32_t)write_len > (file_total_size - total_received)) {
    write_len = (uint16_t)(file_total_size - total_received);
}
~~~

例如文件真实大小是 30268 字节，最后一帧即使收到 1024 字节，也只能把剩余真实字节写入 Flash，不能把后面的 0x1A 或其他填充写进固件。

### 8.2 Flash 写入不足 4 字节时不能丢尾巴

STM32F1 Flash 通常按 32 位字写入。如果 FLASH_WriteBuf() 只循环 len / 4 次，最后 1~3 字节就会被丢掉。正确做法是把最后不足 4 字节的临时字节补成 0xFF 后再按一个 32 位字写入，同时 CRC32 仍然只统计真实长度。

必须区分：

~~~text
写入物理单位：4 字节（必要时用 FF 补齐）
固件逻辑长度：文件头声明的真实字节数
CRC32 统计范围：真实字节数，不包含物理补齐字节
~~~

### 8.3 必须同时做两个 CRC32

接收时累加的 CRC32_Stream 证明“上层收到的数据是什么”；Flash 回读的 CRC32_Calc 证明“Flash 实际保存的内容是什么”。只有两者相等，才能排除 Flash 写入丢字节、地址错位或读回异常：

~~~text
CRC32(串口有效 payload 流)
        ==
CRC32(Flash[APP_FLASH_START : APP_FLASH_START + file_size])
~~~

本次成功值为 0x59B4EA2B，两边完全一致。

---

## 9. 最终成功日志逐行解读

成功日志文件是 串口数据.txt。

### 9.1 CC 是握手，不是错误

~~~text
[YMODEM] RX build 20260823-r8-debug (CRC16/XMODEM, SOH/STX)
CC[YMODEM] Invalid frame start: 0x00
~~~

裸 C 是接收端发出的 CRC16 握手字符。根据发送端软件时序，初始握手、文件头确认后的继续请求、结束阶段请求空 block-0 都可能产生 C。

### 9.2 Invalid frame start: 0x00 为什么没有让传输失败

本次日志没有出现 Ymodem FAIL，反而直接进入：

~~~text
[IAP] Verifying: CRC calc from flash ...
~~~

从当前代码流程看，这个 0x00 很可能发生在最后可选空 block-0 阶段。phase 3 会尝试接收结束空块，但不会用这个可选帧的失败结果否定已经完成的真实文件传输。

判断是否真的有问题，要看：

1. 文件大小是否等于文件头声明值；
2. RAM 流式 CRC32 和 Flash 回读 CRC32 是否相等；
3. 复位后是否能跳转并运行 APP。

本次三项全部满足，因此这个 0x00 是结束握手兼容性/日志噪声，不是 APP 内容错误。若希望日志更干净，可以在 phase 3 单独打印 optional final block missing，不要调用通用帧错误打印。

### 9.3 为什么没有 Data timeout 或 CRC mismatch

因为这次没有发生对应错误：

- Data timeout 只有在某一帧已经开始、但在超时时间内没有收够 payload 时才打印；
- CRC mismatch 只有在收到完整 payload 和两个 CRC 字节，但计算结果不一致时才打印；
- 成功路径不会把每一帧的 CRC MATCH 逐帧打印，而是在整个文件接收完成后做 CRC32 双重校验。

所以没有这两行是好现象，不是日志缺失导致的失败。

### 9.4 APP 已经真正运行

后续日志出现：

~~~text
[BOOT] APP valid, jumping to 0x08008000...
[DHT11] Init done
[DHT11] temp=23C humi=76%
---------- RTOS STATS ----------
~~~

这说明 Bootloader 不只是“认为 Flash 有数据”，而是已经跳转到 APP，APP 的时钟、FreeRTOS、传感器和任务都开始运行。

---

## 10. 这次代码修改的作用总表

| 修改 | 解决什么问题 | 为什么有效 |
|---|---|---|
| y_data_buf 改为静态全局缓冲区 | 1024 字节包放在 Bootloader 栈上，可能栈溢出 | 大缓冲区不再占用局部调用栈 |
| USART1 RXNE/ERR 中断 + 4 KB ring | ACK/C 后下一帧来得太快导致丢字节、错位、ORE | 接收和解析解耦，UART 持续收字节 |
| 检查 SOH/STX、序号反码、CRC16 | 错帧、错位、损坏帧不能被识别 | 只有完整且可信的帧才进入写 Flash 回调 |
| 头部解析文件名和十进制大小 | 不知道真实文件长度，无法正确截断最后一包 | 防止填充数据被写入并检查总量 |
| 首帧异常发送 NAK 并允许重传 | 首个 header 被丢失时直接失败 | 发送端收到 NAK 后可重新发送 block-0 |
| 限制单包重试次数 | 线路持续异常时不会无限 NAK | 超过上限发送 CAN 并退出 |
| 识别重复包但只 ACK 不重复写 | ACK 丢失时发送端会重传上一帧 | 防止 Flash 和 CRC 被重复处理 |
| 兼容单/双 EOT | 不同发送端结束流程略有差异 | 数据完整时不会因结束握手差异误失败 |
| EOT 前后更新 received_size | 修复 size=0 的误导日志 | IAP 层拿到真实接收长度 |
| 接收流 CRC32 + Flash 回读 CRC32 | 仅检查串口帧不足以证明 Flash 正确 | 同时验证接收内容和落盘内容 |
| IAP 失败输出 phase/frame/retry/CRC/UART 状态 | 以前只能看到一个模糊错误码 | 能定位在握手、数据帧、EOT 还是 Flash |

---

## 11. 当前使用时的正确操作

### 11.1 Bootloader 烧录

必须把最新 Bootloader HEX 烧到 0x08000000，当前工程文件：

~~~text
bootloader\MDK-ARM\00_bootloader\00_bootloader.hex
~~~

不要只重新编译 APP。若板子上仍是旧版 Bootloader，串口日志不会出现对应版本：

~~~text
[YMODEM] RX build 20260823-r8-debug (CRC16/XMODEM, SOH/STX)
~~~

### 11.2 Tera Term 参数

- Baud rate：115200
- Data：8 bit
- Parity：None
- Stop：1 bit
- Flow control：None
- 操作：File -> Transfer -> YMODEM -> Send
- 文件：app\MDK-ARM\app\app.bin
- 不要选择 XMODEM；不要选择 YMODEM-G

YMODEM-G 没有普通 Ymodem 的每帧 ACK/NAK，接收器实现不同，不能混用。

### 11.3 板上操作顺序

~~~text
1. 打开串口终端
2. 按住 KEY0
3. 按下并释放 Reset
4. 看到 Serial Ymodem Mode 和 C
5. 在 Tera Term 选择 YMODEM Send
6. 选择 app.bin
7. 等待 CRC MATCH、Param Save OK、ALL DONE
8. 等待软复位后看到 APP valid 和 APP 启动信息
~~~

如果在串口终端打开前就复位，最开始的 Bootloader 文本可能已经发完；这不影响协议，但会影响完整日志收集。

---

## 12. 以后看到日志如何快速判断

### 12.1 成功模板

至少应看到：

~~~text
[IAP] Pre-erase OK
[IAP] CRC MATCH - Firmware integrity confirmed.
[IAP] Param Save OK
[IAP] ===== ALL DONE
[BOOT] APP valid, jumping to 0x08008000...
~~~

最好再确认：

~~~text
CRC_ram == CRC_flash
fw_size == app.bin 文件实际大小
~~~

### 12.2 失败模板对照

| 日志 | 说明 | 优先检查 |
|---|---|---|
| Frame-start timeout | 等不到下一帧第一个字节 | 是否真的开始发送、握手、波特率、RX 线、发送端时序 |
| Invalid frame start | 当前字节不是 SOH/STX/EOT/CAN | 丢字节、接收错位、残留控制字节、协议模式 |
| Header rejected | block-0 格式或文件大小非法 | 文件名/大小字段、YMODEM 模式、文件是否为空/超区 |
| Bad sequence complement | 序号反码不正确 | UART 错位、字节丢失、线路噪声 |
| Data timeout | 帧已经开始但 payload 没收完 | 发送端暂停、波特率、RX 溢出、流控 |
| CRC mismatch | 完整帧内容和 CRC16 不一致 | 线路、串口参数、流控、发送模式 |
| frame_ret=-1 hdr=0x04 | 收到 EOT | 先检查是否已收足文件，不要直接当 CRC 错误 |
| size=0 且 header_ok=0 | 还没有成功接收文件头/数据 | 握手和首帧 |
| size=0 但 hdr=0x04 | 旧代码可能漏写 received_size | 检查 EOT 统计逻辑，不能仅凭 size=0 判断 |
| CRC MISMATCH（IAP 大写） | Flash 回读和接收流不同 | Flash 写入、最后不足 4 字节、地址范围、供电 |
| APP NOT valid | 向量表栈指针不在 RAM 范围 | APP 地址、烧录地址、HEX/APP 偏移 |

### 12.3 错误码和帧头要一起看

当前 ymodem.h 定义：

~~~c
#define Y_OK          0
#define Y_ERR_TIMEOUT -1
#define Y_ERR_CRC     -2
#define Y_ERR_SEQ     -3
#define Y_ERR_CANCEL  -4
#define Y_ERR_SIZE    -5
~~~

但日志里的 frame_ret 是 y_recv_frame() 的内部状态，不完全等同于 Ymodem_Receive() 的最终错误码：

- 内部 frame_ret=-1 表示收到 EOT；
- 最终 Y_ERR_CRC=-2 可能是帧超时、帧头错误或 CRC16 错误累计超过重试次数；
- 因此要同时看 hdr、phase、header_ok、header_size 和具体的 [YMODEM] 诊断行。

---

## 13. 传输已经解决，但仍要修正的相邻问题

以下问题不是本次 Ymodem 字节传输失败的原因，但成功日志已经暴露出来，后续不要忽略。

### 13.1 APP 覆盖了 IAP 保存的真实固件大小和 CRC

IAP 保存的是：

~~~text
fw_size = 30268
fw_crc  = 0x59B4EA2B
~~~

但 APP 启动任务在 app/Src/app_task.c 中又执行：

~~~c
g_param_buf.fw_size_bytes = APP_FLASH_SIZE;
g_param_buf.fw_crc32 = CRC32_CalcAppFlash();
~~~

结果变成整个 APP 分区的 475136 字节和全区 CRC。这不会改变已经写入的 APP 内容，但会破坏参数区中“实际固件大小/CRC”的含义。应删除这两行，或者只在参数无效时设置默认值。

### 13.2 struct_version 尚未初始化

参数结构注释要求版本为 1，但成功日志中看到：

~~~text
struct_version = 4294967295
~~~

这是 0xFFFFFFFF，表示擦除后的默认值。当前 FlashParam_Load() 没有检查该字段，所以暂时能运行；建议在 Bootloader 和 APP 创建默认参数时设置：

~~~c
param.struct_version = 1;
~~~

### 13.3 参数打印的 CRC 可能比实际保存值旧一代

FlashParam_Save() 把输入结构复制到内部缓冲区，再在内部缓冲区里更新 struct_crc32。如果调用者随后打印原来的输入结构，打印出的 CRC 可能是保存前的旧值，而不是刚写入 Flash 的新值。

这属于日志显示问题，不等于参数区损坏。要验证参数，应该重新从 Flash 调用 FlashParam_Load() 后再打印。

### 13.4 多次 Bootloader 启动要区分手动复位和异常复位

成功日志中出现多次 APP valid 和 boot_count 递增。如果这是测试过程中手动按 Reset，属于正常现象；如果没有手动复位，则要单独排查复位源。当前 RCC->CSR 复位标志没有及时清除，单看 0x1C000000 不能精确判断原因。

---

## 14. 新手最应该记住的四件事

1. 看到 C 不代表开始写 Flash。它只是 CRC16 握手请求；文件头 block-0 通过后，才进入数据阶段。
2. 看到 EOT 不代表文件完整。必须比较 total_received 和文件头大小。
3. Ymodem CRC16 和固件 CRC32 是两层不同的校验。CRC16 检查单个传输帧；CRC32 双重校验检查整个固件和 Flash 落盘结果。
4. 最终是否成功要看闭环：CRC MATCH -> Param Save OK -> 软复位 -> APP valid -> APP 任务运行。只看到“收到了一些包”还不能算升级成功。

---

## 15. 参考代码和日志

- Ymodem 接收器：bootloader/Core/Src/ymodem.c
- Ymodem 接口和常量：bootloader/Core/Inc/ymodem.h
- IAP 写 Flash 与 CRC 校验：bootloader/Core/Src/iap.c
- USART1 中断入口：bootloader/Core/Src/stm32f1xx_it.c
- 成功串口日志：串口数据.txt
- APP 实际文件：app/MDK-ARM/app/app.bin
- 工程整体历史记录：SESSION_HANDOFF_YMODEM_IAP.md
