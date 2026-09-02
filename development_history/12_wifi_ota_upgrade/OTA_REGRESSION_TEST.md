# WiFi OTA 回归测试

## 1. 发布版本规则

APP 版本在 `app/Inc/ota_manager.h` 中维护：

```c
#define FW_VER_MAJOR 1U
#define FW_VER_MINOR 0U
#define FW_VER_PATCH 0U
#define FW_BUILD_NUM 4U
```

正式发布时必须按字典序递增 `MAJOR.MINOR.PATCH.BUILD`。普通 OTA 会拒绝低于或等于当前版本的命令。仅现场恢复或重复压力测试可以在 MQTT 命令中加入 `"force":1`。

旧参数区曾使用不同的版本语义，第一次迁移到新规则时应使用一次 `force=1`。成功后后续版本恢复正常递增。

## 2. 构建与 MQTT 命令

Keil 构建 APP 后生成 BIN：

```powershell
& 'D:\keil5-32\ARM\ARMCC\bin\fromelf.exe' --bin --output app\MDK-ARM\app.bin app\MDK-ARM\app\app.axf
```

启动 Server 并打印与 BIN 精确匹配的 MQTT 命令：

```powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4
```

首次旧参数迁移或重复安装同版本：

```powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force
```

将 Server 打印的单行 JSON 发布到 `iot/dev001/ota`，不要手工填写 size 或 CRC。

## 3. 正常链路验收

必须同时满足：

1. APP 收到 OTA 命令并打印目标版本、size、CRC、force。
2. Bootloader 打印相同的 `Target version`、size 和 CRC。
3. S6 序号从 0 连续到 `packets - 1`。
4. `recv_bytes == INFO.size`。
5. RAM CRC、Flash 回读 CRC、INFO CRC 三者相等。
6. JumpToApp 后参数区保留真实 BIN size/CRC 和目标版本。
7. APP 重新进入 `MQTT_WORKING` 并继续发布传感器数据。

保存串口日志后自动检查：

```powershell
python tools\verify_ota_log.py 串口数据.txt --firmware app\MDK-ARM\app.bin --version 1.0.0.4
```

## 4. 连续升级与边界大小

- 连续执行至少 10 次。正式升级逐次增加 BUILD；同版本压力测试使用 `force=1`。
- 覆盖 BIN 长度小于 1024、正好为 1024 的整数倍、最后一包仅 1 字节，以及接近 APP 分区上限的有效镜像。
- 每次都运行 `verify_ota_log.py`，禁止只看进度到 100%。

## 5. Server 故障注入

收到 seq=5 的 ACK 后主动断链：

```powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force --disconnect-after-seq 5
```

预期：Bootloader 不进入 S8 成功路径，最终进入超时/FAIL，不能跳入半写入 APP。

收到 seq=5 的 ACK 后暂停 35 秒：

```powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force --delay-after-seq 5 --delay-seconds 35
```

预期：触发 30 秒接收超时，诊断显示被动接收统计，不进入 JumpToApp。

破坏 seq=5 的一个固件位：

```powershell
python ota_server.py app\MDK-ARM\app.bin --version 1.0.0.4 --force --corrupt-seq 5
```

预期：32 包可以接收完成，但 S8 的 CRC 校验失败，不能更新已安装版本和固件元数据。

## 6. 断电与救援

分别在以下阶段人工断电：

- APP 写 OTA 请求参数时；
- Bootloader 擦除 APP 后；
- S6 接收约 50% 时；
- S8 写回参数区时。

每种场景重新上电后检查参数区 CRC、APP 合法性和启动分支。APP 已擦除或半写入时，按住 KEY0 后复位，使用 USART1 Ymodem IAP 恢复有效 APP。

## 7. 栈与长稳

升级成功后至少运行 30 分钟，确认：

- TaskLED 和 TaskSemHandle 不再出现低于 128 字节的栈告警；
- MQTT PUBLISH、PING 和 OTA 订阅持续正常；
- 无 malloc fail、stack overflow、USART 错误和意外复位。
