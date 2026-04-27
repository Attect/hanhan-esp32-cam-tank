# AGENTS.md — ESP32 遥控坦克项目

> 本文档供 AI 编程助手阅读。项目语言：中文注释 / 中文 UI。

---

## 项目概述

本项目是一个基于 **ESP32-COM / ESP32-CAM** 的遥控坦克固件，单文件 Arduino 草图（`tank.ino`）。

主要功能：
- 通过 **WiFi + WebSocket** 低延迟遥控双电机驱动坦克
- **OV3660 摄像头** 实时 MJPEG 视频流
- **舵机** 控制铲子/照明模块（GPIO 2 磁吸更换）
- **LED 车灯** 亮度可调（0~255），使用软件 PWM 避免 LEDC 通道冲突
- 内嵌响应式 Web 控制页面（HTML/CSS/JS），手机浏览器直接访问
- 支持**虚拟摇杆、键盘、Xbox 手柄**三种操控方式
- 自动画质切换、无客户端自动休眠省电、摄像头独立开关

---

## 技术栈

| 层级 | 技术 |
|------|------|
| 硬件平台 | ESP32-COM / ESP32-CAM（OV3660） |
| 开发框架 | Arduino Core for ESP32 **3.x** |
| 语言 | C++（Arduino 方言） |
| 通信协议 | WiFi (STA 模式)、HTTP、WebSocket、MJPEG 视频流 |
| 前端 | 纯手写 Canvas/Touch，无外部库 |

依赖库（需在 Arduino IDE / PlatformIO 中安装）：
1. **WebSockets** by Markus Sattler
2. **esp32-camera**（随 ESP32 板包自带）

---

## 项目结构

项目为**单文件**结构：

```
tank/
├── tank.ino          # 唯一的源文件（~780 行）
├── README.md
├── AGENTS.md
└── LICENSE           # Apache 2.0
```

代码按功能区域划分：
1. 头文件、引脚定义与用户配置区
2. 全局变量与软 PWM 定时器回调
3. 摄像头初始化 (`initCamera`) 与动态画质
4. 电机 / 灯光 / 舵机初始化与控制
5. 休眠与唤醒 (`goToSleep` / `wakeUp`)
6. WebSocket 事件处理 (`webSocketEvent`)
7. HTTP 兼容端点 (`handleMotor`, `handleLight`, `handleShovel`)
8. 内嵌 Web 页面（HTML/CSS/JS Raw String）
9. 视频流 FreeRTOS 任务 (`streamTask`)
10. `setup()` / `loop()`

---

## 硬件连接与引脚定义

### 电机驱动（L298N，单 PWM 调速）
| 功能 | GPIO | LEDC 通道 | 说明 |
|------|------|-----------|------|
| 左电机 IN1（方向） | 12 | — | digitalWrite |
| 左电机 IN2（PWM） | 13 | **3** | ledcWrite(pin, duty)，频率 1kHz，8bit |
| 右电机 IN3（方向） | 15 | — | digitalWrite |
| 右电机 IN4（PWM） | 14 | **2** | ledcWrite(pin, duty)，频率 1kHz，8bit |

> **单 PWM 接线注意**：ENA/ENB 跳线帽短接（固定 HIGH）。IN1/IN3 控制方向，IN2/IN4 控制速度。由于 L298N 逻辑，**反转时 PWM 需要反相**（PWM=0 对应全速，PWM=255 对应刹车）。代码中已通过 `255 + left` 处理。

### 灯光
| 功能 | GPIO | 说明 |
|------|------|------|
| LED 车灯 | 4 | 软件 PWM（esp_timer），周期 500μs（2kHz），不占用 LEDC 通道 |

### 舵机 / 照明模块（共用 GPIO 2，磁吸更换）
| 功能 | GPIO | LEDC 通道 | 说明 |
|------|------|-----------|------|
| 舵机信号 | 2 | **6** | 50Hz，10bit，角度 0~100 映射到 135°~45° |

### 摄像头（OV3660，ESP32-CAM 标准引脚）
| 信号 | GPIO | 说明 |
|------|------|------|
| XCLK | 0 | LEDC 通道 **0**，20MHz |
| SIOD | 26 | SCCB 数据 |
| SIOC | 27 | SCCB 时钟 |
| D0~D7 | 5, 18, 19, 21, 36, 39, 34, 35 | 并行数据 |
| VSYNC | 25 | 帧同步 |
| HREF | 23 | 行同步 |
| PCLK | 22 | 像素时钟 |
| PWDN | 32 | 电源控制 |
| RESET | -1 | 未使用 |

---

## LEDC 通道分配（不可冲突）

| 通道 | 用途 | 频率 | 分辨率 | 定时器 |
|------|------|------|--------|--------|
| 0 | 摄像头 XCLK | 20 MHz | — | LEDC_TIMER_0 |
| 2 | 右电机 IN4 | 1 kHz | 8 bit | 自动分配 |
| 3 | 左电机 IN2 | 1 kHz | 8 bit | 自动分配 |
| 6 | 舵机 | 50 Hz | 10 bit | 独立（避免与电机共用定时器） |

> 灯使用 **软件 PWM（esp_timer）**，不占用 LEDC 硬件通道。

---

## 运行时架构

ESP32 启动后连接 WiFi，开启三个服务：

| 服务 | 端口 | 说明 |
|------|------|------|
| HTTP 服务器 | 80 | 提供根页面 `/` 及 REST 端点 `/motor`、`/light`、`/servo` |
| MJPEG 视频流 | 81 | `/stream`（前端硬编码为 `http://<ip>:81/stream`） |
| WebSocket 服务器 | 82 | 低延迟双向控制 |

**FreeRTOS 任务**：
- `loop()` 运行在 Core 0（默认），处理 HTTP 与 WebSocket。
- `streamTask` 显式绑定到 **Core 1**，避免与 WiFi 协议栈争抢资源。

---

## WebSocket 控制协议

客户端 → 服务器：

| 消息格式 | 示例 | 说明 |
|----------|------|------|
| `M,<left>,<right>` | `M,255,-255` | 电机速度，范围 `-255~255` |
| `S,<angle>` | `S,50` | 铲子角度，`0=放下 ~ 100=抬起` |
| `L` | `L` | 切换灯光开关 |
| `L,<brightness>` | `L,128` | 设置灯亮度 `0~255`（0=关） |
| `Q,<mode>` | `Q,AUTO` | 画质：`AUTO`/`QVGA`/`VGA`/`SVGA` |
| `C,<1/0>` | `C,0` | 摄像头开关 |

服务器 → 客户端（广播）：

| 消息 | 说明 |
|------|------|
| `LIGHT,<brightness>` | 同步当前灯光亮度 |
| `QUALITY,<mode>` | 同步当前画质模式 |
| `CAM,<1/0>` | 同步摄像头开关状态 |

---

## HTTP 兼容端点

保留传统 HTTP GET 端点供外部调用：

- `GET /motor?left=<int>&right=<int>`
- `GET /light`
- `GET /servo?angle=<int>`

---

## 构建与烧录

### Arduino IDE
1. 在 **文件 → 首选项** 中添加 ESP32 板包地址。
2. 通过 **工具 → 开发板** 选择 `ESP32 Dev Module` 或对应的 ESP32-CAM 板型。
3. 安装依赖库：**WebSockets** by Markus Sattler。
4. 打开 `tank.ino`，修改顶部 WiFi SSID 和密码。
5. 根据实际硬件修改引脚定义。
6. 选择正确端口，点击上传。

### PlatformIO
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
    links2004/WebSockets @ ^2.4.1
```

---

## WiFi 配置

在 `tank.ino` 顶部修改：

```cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
```

首次启动后，通过串口监视器（波特率 `115200`）查看分配的 IP 地址，然后在同一局域网内的浏览器访问该 IP。

---

## 代码风格与约定

- 注释语言：**中文**
- 区域划分：使用 `// =================== 标题 ===================` 形式的大分隔线
- 硬件初始化函数以 `init` 为前缀（`initCamera`, `initMotors`, `initLightPWM`, `initServo`）
- 控制函数以 `set` 为前缀（`setMotor`, `setShovel`, `setLightBrightness`）
- 内嵌 HTML 使用 C++11 Raw String Literal：`R"rawliteral(... )rawliteral"`
- 前端 UI 标签均为中文

---

## 关键实现细节

### 1. ESP32 Arduino Core 3.x API
- `ledcWrite(pin, duty)` 第一参数是 **GPIO 引脚号**，不是通道号
- `ledcAttachChannel(pin, freq, resolution, channel)` 明确指定通道
- `ledcSetup`/`ledcAttachPin` **已移除**

### 2. 灯软件 PWM
灯使用 `esp_timer` 中断模拟 PWM（周期 500μs = 2kHz），不占用任何 LEDC 通道。最小脉冲宽度 50μs，避免低亮度时闪烁。

### 3. 自动画质
- `autoQuality = true` 时，检测到履带运动自动降为 `FRAMESIZE_QVGA`（240p）
- 停止后 1.5s 恢复手动设定分辨率
- `setFrameSize()` 通过 `esp_camera_sensor_get()->set_framesize()` 动态切换

### 4. 休眠省电
- 无客户端（WebSocket + 视频流）持续 5s 后进入休眠
- 上电后 60s 冷却期内不进入休眠
- 休眠时：关闭摄像头、关灯、降低 WiFi 发射功率至 7dBm
- 有新客户端时自动唤醒

### 5. 摄像头重初始化
`esp_camera_deinit()` 后重新 `initCamera()` 必须先调用 `gpio_uninstall_isr_service()`，否则报 `GPIO isr service already installed`。

### 6. 前端左右交换
硬件接线左右交叉，前端发送时交换 `motorL`/`motorR` 位置：`wsSend(\`M,${motorR},${motorL}\`)`

---

## 测试建议

- 烧录后先打开串口监视器（115200），确认 WiFi 连接成功并获取 IP。
- 在手机或电脑浏览器访问 `http://<ESP32_IP>/`，确认视频流、摇杆、舵机滑块、灯光按钮均正常。
- 断开 WebSocket 后观察前端是否自动重连（3 秒间隔）。
- 检查电机转向：若实际方向与预期相反，交换 L298N 对应电机的两根线。
- 测试无客户端 5s 后是否自动休眠，重新打开页面是否自动唤醒。

---

## 已知限制

- 单文件 monolithic 结构，所有功能耦合在一起，扩展新传感器或端点需要直接修改 `tank.ino`。
- 视频流未做帧率限制，完全依赖摄像头取帧和 WiFi 带宽。
- 前端 HTML 直接硬编码在 C++ 字符串中，修改 UI 需要重新编译烧录。
- 无 OTA（Over-The-Air）升级功能。
- 无身份验证，同一局域网内任何设备均可访问。
