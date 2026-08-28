# D15 Pitfalls（Qt6上位机 MQTT 控制台 Tab）

**日期**：2026-08-28
**任务**：补 MQTT 控制台 Tab：JSON 输入框 + 发送按钮 + 彩色日志；手动实现 MQTT 3.1.1 协议（Qt 6.11.2 在线安装器无 MQTT 模块）
**Checkpoint**：上位机 MQTT 和设备同连 broker.emqx.io，互相能看到对方发的消息

***

## 坑 1：hex 流操作符 Qt6 用法变化 → 'hex' was not declared

* **现象**：mqttclient.cpp parseIncoming 里 `qDebug() << "MQTT: unhandled packet type" << hex << pktType` 报 `'hex' was not declared in this scope`

* **根因**：Qt5 的 `QDebug` 会把 `std::hex` 之类的 iomanip 转进流里；Qt6 做了更严格的类型限制，必须显式用 Qt 自己的接口或 `QString::number`

* **排查过程**：

  1. 错误指向 `qdebug.h` 里面 → 说明不是 mqttclient 自己的问题
  2. 回忆 sensortab.cpp 之前的 qrand 错误 → 又是 Qt5→Qt6 迁移问题
  3. 其实 `hex` 本身在 `<iomanip>` 里有，但 qDebug 的 operator<< 不再接 std 流操作符

* **解决**：两种办法：

  1. 简单的：直接去掉 `<< hex`，只打印 pktType 数值
  2. 需要十六进制显示的话用 `QString`：

     ```cpp
     qDebug() << "MQTT: unhandled packet type"
              << QString("0x%1").arg(pktType, 0, 16);
     ```

* **教训**：Qt6 对 std 流操作符不再隐式兼容，调试输出里的 `hex`/`dec`/`endl` 全部要换掉或用 Qt 自己的格式函数

***


## 坑 2：QTcpSocket readyRead 时一次性读不全，误以为协议解析有问题

* **现象**：收 PUBLISH 帧时，`readAll()` 只拿到了 4 字节（固定头+remlen+一半topic len），后面一半过几十毫秒才收到，parseIncoming 报"remlen=256 但 buffer 只有 50 字节" → 解析失败

* **根因**：TCP 是"字节流"，不是"消息流"。一个 MQTT 帧可能被分 2\~3 个 TCP 包发过来，`readyRead` 可能触发多次，每次只带来一部分。如果你假设 readAll() 能拿到一个完整帧，就会把半条帧当完整的解析

* **排查过程**：

  1. 给 `readyRead` 打日志，发现同一个 MQTT 帧触发了 2 次 readyRead
  2. 第一次 readAll() 了 40 字节，第二次 readAll() 了 120 字节
  3. 两次加起来刚好是完整帧的 160 字节

* **解决**：维护一个**持久化的接收缓冲区** **`m_rxBuffer`**，每次 readyRead 把 readAll() append 进去；parseIncoming() 从 m\_rxBuffer 开头尝试解析一个完整包；够就解，解完把消费掉的字节 remove 掉；不够就等下次 readyRead 再进来

* **教训**：TCP 永远按"字节流"想，不要按"消息"想。D12 WiFi OTA 处理 AT+CIPRECVDATA 时也是同样的道理——必须有跨调用累积缓冲。同样的知识点从 STM32 ESP8266 移植到了 Qt QTcpSocket

***


## 坑 3：PUBLISH 帧 QoS0 多写了 Packet ID → Broker 认为帧格式错，连接被强制断开

* **（易错）**

* **现象**：PUBLISH 发出去后 Broker 立刻断连接（TCP FIN），状态变 Disconnected，没有任何错误日志

* **根因**：MQTT PUBLISH 帧结构：

  * QoS0：Topic + Payload，没有 Packet ID 字段

  * QoS1：Topic + Packet ID（2 字节）+ Payload

  * QoS2：Topic + Packet ID（2 字节）+ Payload + 额外流程

  如果 QoS0 的帧里多塞了 2 字节 Packet ID，Broker 解析时会把那 2 字节当成 Payload 的一部分，然后 Remaining Length 就不匹配，认为是畸形包 → 按 MQTT 规范必须断连接且不回任何报文

* **排查过程**：

  1. QoS0 发布失败，QoS1 却成功 → 这是关键信号
  2. 对比 buildPublishPacket 里 `if (qos > 0) add pktId` 的逻辑
  3. 发现条件写成了 `if (qos >= 0)`，导致 QoS0 也加了 2 字节

* **解决**：严格按标准，Packet ID 只在 QoS ≥ 1 时存在

* **教训**：手写协议帧时，每个条件分支用表格对照。PUBLISH 是 MQTT 里最复杂的可变帧，QoS 位和 DUP/RETAIN 位的组合有 8 种，每种帧结构都可能不同

***

## 坑 4：MQTT 变长 Remaining Length 编解码漏了 continuation bit → 大 payload 解析错位

* **（易错，实现 MqttClient 时必须搞对）**

* **现象**：小消息（<127 字节）收发正常，大 JSON（>128 字节）发出去 Broker 不认，或者收到的消息被截断，payload 前面少了 1 字节，后面多了 1 字节的乱码

* **根因**：MQTT 第 2 字节开始是 "Remaining Length"，采用**变长编码**：

  * 0\~127：1 字节，值直接写

  * 128\~16383：2 字节，**第一个字节最高位必须置 1**（continuation bit），表示"后面还有字节"

  * 16384 以上：3\~4 字节，同样前 N-1 个字节最高位都要 |=0x80

  忘了置 continuation bit（比如 128 直接写成 0x80 不带后续字节，或者写 0x01 0x00 不带 continuation bit），Broker 会把后续字节当成 payload 的一部分，导致整个帧解析错位

* **排查过程**：

  1. 短消息正常，长消息异常 → 长度相关问题
  2. 抓包看第二字节：payload 150 字节 → remlen 应该是 `0x96 0x01`（150 = 0x16，最高位+1=0x96，下一字节是 1）
  3. 如果发出去是 `0x16 0x01`，那就是没加 continuation bit

* **解决**：encodeRemLen 里一定要有 `if (len > 0) digit |= 0x80;`，decodeRemLen 里一定要有 `while ((byte & 0x80) != 0)` 直到 continuation bit = 0

* **教训**：MQTT 协议最容易写错的就是 Remaining Length 的编解码。测试时一定要覆盖三种长度：<127（1字节）、128\~16383（2字节）、>16384（3字节）。D15 发的 JSON 都 <64 字节，这个坑在后面做大 payload（OTA 信息包）时一定会冒出来

***

## D15 成果总结

* MqttClient 类：纯 QTcpSocket 手写 MQTT 3.1.1 协议（CONNECT/SUBSCRIBE/PUBLISH/PINGREQ/PUBACK 5 种帧 + 变长 Remaining Length 编解码 + 流式接收入口环形缓存）

* MQTT Console Tab：Broker 连接区 + 订阅区 + JSON 发布区 + 分颜色彩色日志（sensor 蓝 / ota 紫 / 发绿 / 出错红）

* Checkpoint 达成：Qt 上位机 <-> MQTTX 模拟器 <-> broker.emqx.io 三方互发互收，主题/时间戳完全对齐



