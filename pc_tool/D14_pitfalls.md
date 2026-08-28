D14：Qt6 上位机 4 Tab 骨架 + Sensor Monitor QPainter 实时曲线（2026-08-28）
阶段归属：阶段4 上位机+PID · Qt6 上位机开发

***

## 坑1：qrand() / qsrand() 在 Qt6 中已被删除 - Qt5→Qt6 API 迁移坑

* **现象**：Sensortab.cpp 编译报错：

  ```
  'qrand' was not declared in this scope; did you mean 'srand'?
  ```

* **根因**：Qt 5 提供 `qrand()` 和 `qsrand()` 作为随机数接口，但 Qt 6 已经彻底删除了这两个函数，因为 C++ 标准库 `<cstdlib>` 的 `rand()` / `srand()` 已经足够。项目文档（链接）里写的是"Qt 6.5 MinGW"，实际安装了 Qt 6.11.2，版本更老的代码示例还在用 qrand()。

  | 对比  | Qt 5           | Qt 6                                      |
  | --- | -------------- | ----------------------------------------- |
  | 随机数 | `qrand()`      | 已删除 → 用 `rand()`                          |
  | 种子  | `qsrand(seed)` | 已删除 → 用 `srand(seed)`                     |
  | 头文件 | 不需要额外 include  | `#include <cstdlib>` + `#include <ctime>` |

* **排查过程**：

  1. 编译报 `qrand was not declared`，定位到 sensortab.cpp:36
  2. 确认 Qt 版本 → 6.11.2
  3. 查 Qt 6 文档 → qrand() 已删除，改用 C++ 标准库
  4. 同时发现种子初始化也需要从 `qsrand(time(0))` 改成 `srand(time(nullptr))`

* **解决**：三处修改：

  ```cpp
  // 1. 文件顶部加 include
  #include <cstdlib>      // rand() / srand()
  #include <ctime>        // time()

  // 2. 构造函数里加种子初始化（只调用一次，不要在 onTimer 里调）
  SensorTab::SensorTab(QWidget *parent)
      : QWidget(parent)
  {
      srand(time(nullptr));   // 随机种子，放在构造函数里只跑一次
      // ...
  }

  // 3. onTimer() 里 qrand() 全改成 rand()
  m_curTemp = 25.0 + 3.0 * qSin(m_simPhase) + (rand() % 100) / 50.0;
  m_curHumi = 60.0 + 8.0 * qSin(m_simPhase + 0.5) + (rand() % 100) / 30.0;
  ```

* **教训**：Qt 5→Qt6 迁移有一批 API 被删除/重命名，**写 Qt 代码前先确认当前版本**。文档里写 Qt 6.5 但实际装了 6.11.2（版本号越高越新），API 删除可能更多。如果从网上或旧文档抄代码，遇到 "was not declared" 错误，**先查 Qt 版本**，大概率是 API 变更。常见 Qt5→Qt6 变更清单：

  * `qrand()` → 删除，用 `rand()`

  * `qSort()` → 改为 `std::sort()`

  * `QTextCodec` → 删除，用 `QString::fromUtf8()` 等

  * `QRegExp` → 建议改用 `QRegularExpression`

***


## D14 成果

* **Qt6 环境搭建完成**：Qt 6.11.2 MinGW + Qt Creator，安装包配置正确，核心模块（core/gui/widgets/network）全部就绪。SerialPort 模块暂未安装，D16 做串口 IAP Tab 时补装。

* **pc\_tool 工程骨架（4 Tab 切换）**：

  * `IotOtaTool.pro`：qmake 工程配置，4 个源文件 + 4 个头文件，编译输出到 `bin/`

  * `main.cpp`：程序入口，创建 QApplication 和 MainWindow

  * `mainwindow.h/cpp`：主窗口类，QTabWidget 容器 + 4 个 Tab

* **Sensor Monitor Tab（QPainter 纯手绘实时曲线）**：

  * `sensortab.h/cpp`：独立 SensorTab 类

  * 顶部大数字区：温度（红）+ 湿度（蓝）+ 固件版本 + Online 状态

  * 中间双图表：温度曲线（15~~35°C，红色线+浅红填充）+ 湿度曲线（30~~90%RH，蓝色线+浅蓝填充）

  * 环形缓冲 100 点：`m_temp[100]` / `m_humi[100]` + `m_writeIdx` / `m_count` 实现固定窗口滑动

  * QTimer 500ms 定时触发 `onTimer()` → 模拟数据生成 → `update()` 请求重绘 → `paintEvent` 执行

  * `valueToY()` 辅助函数处理 Y 轴反转 + 越界保护

  * `QPainterPath` + `Antialiasing` 实现平滑曲线 + 面积填充

  * `resizeEvent` 窗口缩放自动重绘

* **Qt 新手 7 个坑全部踩过**：从 vtable 链接错误到 qrand() 被删、从 Configure Project 页面看不懂到坐标反转，每个坑都有明确的根因和解决方案，D15-D18 开发可以直接参考避坑。

* **编译 0 Error 0 Warning**，Ctrl+R 运行正常，4 Tab 能切换，Sensor Monitor 曲线持续滚动。

* **代码量**：新增约 400 行（sensortab.h 45 行 + sensortab.cpp 260 行 + mainwindow 改动约 10 行 + .pro 改动 2 行），全部 D14 独立代码，与 STM32 端无耦合（后续接真实 MQTT 数据只需改 onTimer 里的数据源）。

* **工程文件清单**：

  ```
  d:\iot_ota_project\pc_tool\
  ├── IotOtaTool.pro      ← 工程配置
  ├── main.cpp            ← 入口
  ├── mainwindow.h/cpp    ← 4 Tab 容器
  ├── sensortab.h/cpp     ← 实时曲线（环形缓冲 + QPainter）
  └── bin\                ← 编译输出
  ```

* **D15 预告**：补 MQTT 控制台 Tab（JSON 输入框 + 发送按钮 + 彩色日志）+ 把 SensorTab 的模拟数据换成真实 MQTT 订阅数据。需要安装 Qt MQTT 模块或用第三方库，当前 .pro 的 QT += network 已具备 TCP 基础。

