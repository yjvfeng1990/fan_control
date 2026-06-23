# ESP8266 风机智能控制器

基于 ESP8266 ESP-12E 的 25kHz PWM 风机控制系统，支持 Web 管理、MQTT 远程控制、外部旋钮调速、转速实时监测。

## 功能特性

| 功能 | 描述 |
|------|------|
| **PWM 控制** | 25kHz 风机 PWM 输出，0-100% 占空比 |
| **外部输入** | GPIO13 中断方式采集外部 PWM，中值滤波 + EMA 平滑 |
| **转速监测** | GPIO12 TACH 脉冲计数，实时 RPM 显示 |
| **多模式控制** | 外部旋钮 / Web 手动 / MQTT 远程 / 强制控制 四种模式 |
| **Web 管理后台** | 实时仪表盘、滑块调速、转速曲线、WiFi/MQTT 配置 |
| **MQTT 双向通讯** | JSON 状态上报 + 订阅命令调速 |
| **WiFi 双模** | AP (192.168.4.1) 常开 + STA 可连接路由器 |
| **配置持久化** | EEPROM 存储 WiFi/MQTT 配置，断电不丢失 |
| **设备 ID** | MAC 地址派生 6 位唯一 ID，用于多设备区分 |

## 引脚接线

| GPIO | 功能 | 说明 |
|------|------|------|
| GPIO14 (D5) | 风机 PWM 输出 | 25kHz, 0-100% |
| GPIO13 (D7) | 外部 PWM 输入 | **仅支持 3.3V！5V 须串 10kΩ+20kΩ 分压** |
| GPIO12 (D6) | 风机 TACH 输入 | 脉冲计数测速 |

## 控制模式优先级

```
SRC_FORCE > SRC_WEB = SRC_REMOTE > SRC_EXTERNAL
```

- **外部旋钮**：默认模式，跟随 GPIO13 输入占空比
- **Web 手动**：滑块调速，3 秒无操作后外部旋钮可覆盖
- **MQTT 远程**：MQTT 下发 `{"duty":80}` 调速，覆盖逻辑同 Web
- **强制控制**：下发 `{"duty":0,"force":true}` 紧急停机，不会被覆盖

## Web 管理后台

设备启动后连接 AP `Fan_Control`（密码 `12345678`），浏览器访问 `http://192.168.4.1`。

### 页面功能
- 📊 实时 RPM / 占空比仪表盘
- 🎛️ 滑块调速 + 紧急停机 / 全速运行
- 📈 60 点转速历史曲线
- ⚙️ WiFi 配置（连接路由器）
- 📡 MQTT 配置（服务器/端口/主题/订阅/上报间隔）

## MQTT 通讯

### 状态上报 (Publish)

Topic: `fan/status`（可在 Web 后台自定义）

```json
{
  "id": "482931",
  "rpm": 1500,
  "duty": 50,
  "externalDuty": 48,
  "webDuty": 50,
  "remoteDuty": 0,
  "mode": "web",
  "network": "ap+sta",
  "staIP": "192.168.1.100",
  "apIP": "192.168.4.1",
  "wifiSSID": "MyWiFi",
  "rssi": -45,
  "uptime": 3600
}
```

### 远程控制 (Subscribe)

Topic: `fan/command`（可在 Web 后台自定义）

```json
{"duty": 80}                      // 远程调速，3秒后可被外部旋钮覆盖
{"duty": 0, "force": true}        // 强制停机，不会被覆盖
{"duty": 100, "force": true}      // 强制全速
```

## 编译与烧录

### 依赖

- PlatformIO
- 库：`knolleary/PubSubClient@^2.8`

```bash
# 编译
pio run

# 编译并烧录 (COM19)
pio run -t upload

# 串口监视
pio device monitor
```

### 首次启动

1. 烧录后串口输出 `[System] 设备ID: XXXXXX`
2. 连接 WiFi `Fan_Control` (密码 `12345678`)
3. 打开 `http://192.168.4.1`
4. 在 ⚙️ WiFi配置 中设置路由器连接
5. 在 📡 MQTT配置 中设置 broker 信息

## 技术要点

### PWM 采集抗干扰
- 中断方式（CHANGE 触发），非阻塞
- 中值滤波（3 样本）
- EMA 指数平滑（α=0.25）
- 2% 死区防抖

### EEPROM 布局
| 偏移 | 大小 | 内容 |
|------|------|------|
| 0 | 99B | WiFi 配置 (SSID/密码) |
| 200 | 139B | MQTT 配置 (服务器/主题/订阅) |

### JSON 省内存策略
不使用 JSON 库，采用字符串拼接生成 JSON，避免复杂库的内存开销。
