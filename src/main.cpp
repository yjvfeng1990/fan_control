#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <PubSubClient.h>

// ==================== 引脚定义 ====================
#define PIN_FAN_PWM     14    // GPIO14 (D5) - 风机25kHz PWM输出
#define PIN_EXT_PWM     13    // GPIO13 (D7) - 外部PWM输入 (仅3.3V，5V须加分压电阻: 10k+20k)
#define PIN_TACH        12    // GPIO12 (D6) - 风机TACH脉冲输入

// ==================== 前向声明 ====================
void setRemoteDuty(uint16_t duty, bool force);

// ==================== PWM参数 ====================
#define PWM_FREQUENCY   25000 // 风机PWM频率 25kHz

// ==================== 测速参数 ====================
#define TACH_PULSES_PER_REV  2  // 风机每转2个脉冲
volatile uint32_t tachCount = 0;
uint32_t lastTachCount = 0;
uint16_t currentRPM = 0;
unsigned long lastRPMTime = 0;

// ==================== 调速参数 ====================
enum ControlSource { SRC_EXTERNAL, SRC_WEB, SRC_REMOTE, SRC_FORCE };
ControlSource controlSrc = SRC_EXTERNAL;
uint16_t externalDuty = 0;
uint16_t manualDuty = 0;
uint16_t remoteDuty = 0;
uint16_t currentDuty = 0;
unsigned long webControlStart = 0;     // Web/远程控制开始时间
#define WEB_OVERRIDE_TIME 3000  // Web/远程控制3秒后可被外部旋钮覆盖

// ==================== WiFi参数 ====================
IPAddress localAPIP(192, 168, 4, 1);
IPAddress localSTAIP;
unsigned long connectedTime = 0;
bool staConnected = false;
char deviceId[7];  // MAC地址派生6位唯一ID

// ==================== 固件版本 ====================
#define FW_VERSION "2.0.0"

// ==================== 生成设备唯一ID ====================
void generateDeviceId() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    // 6字节MAC转为64位整数，取模得6位十进制ID (100000~999999)
    uint64_t macNum = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
                      ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
                      ((uint64_t)mac[4] << 8)  | mac[5];
    uint32_t id = (macNum % 900000) + 100000;
    snprintf(deviceId, sizeof(deviceId), "%06u", (unsigned int)id);
    Serial.printf("[System] 设备ID: %s\n", deviceId);
}

// ==================== WiFi配置存储 ====================
#define EEPROM_SIZE 512
#define EEPROM_WIFI_OFFSET 0
#define EEPROM_MQTT_OFFSET 200
#define EEPROM_MAGIC 0xA5F1  // 校验头

struct WiFiConfig {
    uint16_t magic;
    char ssid[32];
    char password[64];
    bool valid;
};
WiFiConfig wifiConfig;

struct MQTTConfig {
    uint16_t magic;
    char server[32];
    int port;
    char topic[48];      // 发布主题（状态上报）
    char cmdTopic[48];   // 订阅主题（接收远程控制指令）
    int interval;        // 上报间隔（秒）
    bool enabled;
};
MQTTConfig mqttConfig;

// ==================== Web服务器 ====================
ESP8266WebServer server(80);

// ==================== MQTT配置 ====================

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttPublish = 0;
bool mqttConnected = false;
bool mqttEnabled = false;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long mqttReconnectInterval = 5000;  // 初始5秒重连间隔
#define MQTT_RECONNECT_MIN    5000UL   // 最小重连间隔 5秒
#define MQTT_RECONNECT_MAX    60000UL  // 最大重连间隔 60秒

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char buf[64];
    unsigned int copyLen = (length < 63) ? length : 63;
    memcpy(buf, payload, copyLen);
    buf[copyLen] = '\0';
    
    Serial.printf("[MQTT] 收到命令: %s\n", buf);
    
    // 校验设备ID：查找 "id":"XXXXXX"
    char* idPos = strstr(buf, "\"id\"");
    if (!idPos) {
        Serial.println("[MQTT] 命令中未找到id字段，忽略");
        return;
    }
    char* idColon = strchr(idPos, ':');
    if (!idColon) return;
    char* idStart = strchr(idColon, '\"');
    if (!idStart) return;
    idStart++;  // 跳过引号
    char* idEnd = strchr(idStart, '\"');
    if (!idEnd) return;
    int idLen = idEnd - idStart;
    if (idLen != 6 || strncmp(idStart, deviceId, 6) != 0) {
        Serial.printf("[MQTT] 设备ID不匹配(期望:%s 收到:%.*s)，忽略\n", deviceId, idLen, idStart);
        return;
    }
    
    // 简易JSON解析：查找 "duty" 字段
    char* dutyPos = strstr(buf, "\"duty\"");
    if (!dutyPos) {
        Serial.println("[MQTT] 命令中未找到duty字段");
        return;
    }
    
    // 找到冒号
    char* colon = strchr(dutyPos, ':');
    if (!colon) return;
    
    int duty = atoi(colon + 1);
    if (duty < 0 || duty > 100) {
        Serial.printf("[MQTT] duty值无效: %d\n", duty);
        return;
    }
    
    // 查找 force 字段
    bool force = (strstr(buf, "\"force\"") != nullptr &&
                  strstr(buf, "true") != nullptr);
    
    setRemoteDuty(duty, force);
}

void initMQTT() {
    if (!mqttConfig.enabled) {
        Serial.println("[MQTT] MQTT未启用（未配置服务器）");
        return;
    }
    mqttClient.setServer(mqttConfig.server, mqttConfig.port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setSocketTimeout(2);
    Serial.println("[MQTT] MQTT客户端初始化完成");
    Serial.printf("[MQTT] 服务器: %s:%d  发布: %s  订阅: %s\n", mqttConfig.server, mqttConfig.port, mqttConfig.topic, mqttConfig.cmdTopic);
}

bool tryMQTTConnect() {
    if (mqttClient.connected()) return true;
    if (!mqttEnabled || !mqttConfig.enabled) return false;
    
    Serial.print("[MQTT] 正在连接...");
    String clientId = "FanCtrl_" + WiFi.macAddress();
    if (mqttClient.connect(clientId.c_str())) {
        Serial.println("成功");
        // 成功连接后重置重连间隔
        mqttReconnectInterval = MQTT_RECONNECT_MIN;
        // 订阅命令主题
        if (strlen(mqttConfig.cmdTopic) > 0) {
            mqttClient.subscribe(mqttConfig.cmdTopic);
            Serial.printf("[MQTT] 已订阅命令主题: %s\n", mqttConfig.cmdTopic);
        }
        return true;
    }
    Serial.printf("失败(rc=%d)\n", mqttClient.state());
    // 失败后加倍重连间隔（最大60秒）
    mqttReconnectInterval = min(mqttReconnectInterval * 2, MQTT_RECONNECT_MAX);
    Serial.printf("[MQTT] 下次重连将在 %lu 秒后\n", mqttReconnectInterval / 1000);
    return false;
}

void publishMQTT() {
    char jsonPayload[512];
    const char* ctrlMode = (controlSrc == SRC_FORCE) ? "force" : 
                          (controlSrc == SRC_REMOTE) ? "remote" :
                          (controlSrc == SRC_WEB) ? "web" : "external";
    const char* netMode = staConnected ? "ap+sta" : "ap";
    int rssi = staConnected ? WiFi.RSSI() : 0;
    String staIPStr = staConnected ? localSTAIP.toString() : "";
    
    snprintf(jsonPayload, sizeof(jsonPayload),
        "{\"id\":\"%s\",\"rpm\":%d,\"duty\":%d,\"externalDuty\":%d,\"webDuty\":%d,\"remoteDuty\":%d,"
        "\"mode\":\"%s\",\"network\":\"%s\",\"staIP\":\"%s\",\"apIP\":\"%s\","
        "\"wifiSSID\":\"%s\",\"rssi\":%d,\"uptime\":%lu}",
        deviceId, currentRPM, currentDuty, externalDuty, manualDuty, remoteDuty,
        ctrlMode, netMode,
        staIPStr.c_str(),
        "192.168.4.1",
        wifiConfig.valid ? wifiConfig.ssid : "Fan_Control",
        rssi,
        millis() / 1000);
    
    if (mqttClient.publish(mqttConfig.topic, jsonPayload)) {
        Serial.printf("[MQTT] 已发送: %s\n", jsonPayload);
    }
}

// ==================== 外部PWM采集 (中断方式，非阻塞) ====================
volatile uint32_t extPwmRiseTime = 0;     // 最近上升沿时间(us)
volatile uint32_t extPwmHighUs = 0;        // 最新高电平脉宽(us)
volatile uint32_t extPwmPeriodUs = 0;      // 最新周期(us)
volatile bool extPwmNewData = false;       // 新数据就绪标志
unsigned long extPwmLastEdgeTime = 0;      // 最后一次边沿时间(ms)，用于无信号检测

// ==================== 速度历史数据 ====================
#define RPM_HISTORY_SIZE 60
uint16_t rpmHistory[RPM_HISTORY_SIZE];
uint8_t rpmHistoryIndex = 0;

// ==================== HTML页面 ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>风机控制系统 V2.0</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Inter','Segoe UI',sans-serif;background:#07111F;color:#C8D6E5;min-height:100vh}
::selection{background:#00D8FF33}
::-webkit-scrollbar{width:6px}
::-webkit-scrollbar-track{background:#07111F}
::-webkit-scrollbar-thumb{background:#1B2A3F;border-radius:3px}
.container{max-width:1280px;margin:0 auto;padding:16px 20px}
/* Header */
.header{display:flex;justify-content:space-between;align-items:center;padding:16px 24px;background:#0D1B2A;border-radius:12px;margin-bottom:16px;border:1px solid #1B2A3F;flex-wrap:wrap;gap:12px}
.header-left h1{font-size:20px;font-weight:700;color:#00D8FF;letter-spacing:1px}
.header-left .devid{font-size:12px;color:#5A6D80;margin-top:2px}
.header-right{display:flex;gap:10px;flex-wrap:wrap}
.tag{display:inline-flex;align-items:center;gap:6px;padding:5px 14px;border-radius:16px;font-size:12px;font-weight:600;white-space:nowrap}
.tag-online{background:#00D97E1A;color:#00D97E;border:1px solid #00D97E33}
.tag-online .dot{width:7px;height:7px;border-radius:50%;background:#00D97E;animation:pulse 2s infinite}
.tag-wifi{background:#00D8FF1A;color:#00D8FF;border:1px solid #00D8FF33}
.tag-mqtt{background:#FFC53D1A;color:#FFC53D;border:1px solid #FFC53D33}
.tag-mode{background:#A855F71A;color:#A855F7;border:1px solid #A855F733}
.tag-danger{background:#FF4D4F1A;color:#FF4D4F;border:1px solid #FF4D4F33}
.tag-uptime{background:#0D1B2A;color:#5A6D80;border:1px solid #1B2A3F;font-family:monospace}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
/* Status Cards */
.status-cards{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:16px}
.stat-card{background:#0D1B2A;border-radius:10px;padding:14px 18px;border:1px solid #1B2A3F;transition:border-color .2s}
.stat-card:hover{border-color:#00D8FF33}
.stat-card .slabel{font-size:11px;color:#5A6D80;text-transform:uppercase;letter-spacing:.5px;margin-bottom:6px}
.stat-card .sval{font-size:16px;font-weight:700;color:#C8D6E5}
.stat-card.ip .sval{font-family:monospace;font-size:14px;color:#00D8FF}
/* Main Grid */
.main-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:16px}
.panel{background:#0D1B2A;border-radius:12px;padding:22px 24px;border:1px solid #1B2A3F}
.panel h2{font-size:15px;font-weight:700;color:#00D8FF;margin-bottom:16px;display:flex;align-items:center;gap:8px}
.panel h2::before{content:'';width:3px;height:16px;background:#00D8FF;border-radius:2px;display:inline-block}
/* RPM Gauge */
.gauge-wrap{text-align:center}
#gaugeCanvas{display:block;margin:0 auto;max-width:240px;width:100%}
.gauge-info{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:16px}
.ginfo{background:#07111F;border-radius:8px;padding:10px;text-align:center}
.ginfo .glabel{font-size:10px;color:#5A6D80;margin-bottom:4px;text-transform:uppercase}
.ginfo .gval{font-size:18px;font-weight:700;color:#C8D6E5}
.ginfo .gval.accent{color:#00D8FF}
/* PWM Control */
.mode-badge{display:inline-block;padding:6px 16px;border-radius:14px;font-size:12px;font-weight:700;margin-bottom:14px}
.mode-badge.external{background:#00D8FF1A;color:#00D8FF}
.mode-badge.manual{background:#FFC53D1A;color:#FFC53D}
.mode-badge.remote{background:#A855F71A;color:#A855F7}
.mode-badge.force{background:#FF4D4F1A;color:#FF4D4F}
.slider-wrap{padding:8px 0}
.slider-wrap input[type=range]{width:100%;height:6px;-webkit-appearance:none;background:linear-gradient(90deg,#00D8FF,#00D97E);border-radius:3px;outline:none}
.slider-wrap input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:26px;height:26px;background:#00D8FF;border-radius:50%;cursor:pointer;box-shadow:0 0 16px #00D8FF66;border:2px solid #07111F}
.slider-val{text-align:center;font-size:36px;font-weight:700;color:#00D8FF;margin:8px 0}
.btn-row{display:flex;gap:10px;margin-top:12px}
.btn{flex:1;padding:11px 16px;border:none;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;transition:all .2s;letter-spacing:.5px}
.btn:hover{transform:translateY(-1px);filter:brightness(1.15)}
.btn-danger{background:#FF4D4F;color:#fff}
.btn-primary{background:#00D8FF;color:#07111F}
.btn-success{background:#00D97E;color:#07111F}
.btn-outline{background:transparent;border:1px solid #1B2A3F;color:#5A6D80}
.btn-outline:hover{border-color:#00D8FF;color:#00D8FF}
/* Chart */
.chart-panel{grid-column:1/-1;margin-bottom:16px}
#rpmChart{display:block;width:100%}
.chart-tabs{display:flex;gap:6px;margin-bottom:12px}
.chart-tab{padding:5px 14px;border-radius:14px;font-size:11px;font-weight:600;cursor:pointer;border:1px solid #1B2A3F;background:transparent;color:#5A6D80;transition:all .2s}
.chart-tab.active,.chart-tab:hover{background:#00D8FF1A;border-color:#00D8FF;color:#00D8FF}
#rpmChart{display:block;width:100%;height:240px;border-radius:8px}
/* Bottom Grid */
.bottom-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:16px}
.info-table{width:100%;font-size:13px;border-collapse:collapse}
.info-table td{padding:8px 0;border-bottom:1px solid #1B2A3F}
.info-table td:first-child{color:#5A6D80;width:90px;font-size:12px}
.info-table td:last-child{color:#C8D6E5;font-family:monospace;font-size:12px}
/* Alarm List */
.alarm-list{list-style:none;font-size:12px}
.alarm-list li{padding:8px 12px;border-radius:6px;margin-bottom:6px;display:flex;align-items:center;gap:8px;background:#07111F}
.alarm-ok{color:#00D97E}
.alarm-warn{background:#FFC53D0D;color:#FFC53D;border-left:3px solid #FFC53D}
.alarm-err{background:#FF4D4F0D;color:#FF4D4F;border-left:3px solid #FF4D4F}
/* Modals */
.modal{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(4,9,16,.85);display:none;justify-content:center;align-items:center;z-index:1000;backdrop-filter:blur(4px)}
.modal.active{display:flex}
.modal-box{background:#0D1B2A;border-radius:14px;padding:28px;max-width:420px;width:90%;border:1px solid #1B2A3F;box-shadow:0 20px 60px rgba(0,0,0,.5)}
.modal-box h2{font-size:16px;color:#00D8FF;margin-bottom:20px;display:flex;align-items:center;gap:8px}
.modal-box h2.danger{color:#FF4D4F}
.fgroup{margin-bottom:14px}
.fgroup label{display:block;font-size:12px;color:#5A6D80;margin-bottom:5px}
.fgroup input{width:100%;padding:10px 12px;background:#07111F;border:1px solid #1B2A3F;border-radius:6px;color:#C8D6E5;font-size:13px;outline:none;transition:border-color .2s}
.fgroup input:focus{border-color:#00D8FF}
.fgroup input[type=checkbox]{width:auto;margin-right:6px}
.modal-btns{display:flex;gap:10px;margin-top:20px}
.modal-btns .btn{flex:1}
/* Footer */
.footer{text-align:center;padding:16px;color:#2A3F55;font-size:11px}
/* Responsive */
@media(max-width:768px){.main-grid,.bottom-grid,.status-cards{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="container">
<!-- Header -->
<div class="header">
<div class="header-left">
<h1>风机控制系统</h1>
<div class="devid">设备ID: <span id="deviceId">---</span></div>
</div>
<div class="header-right">
<span class="tag tag-online"><span class="dot"></span> 设备在线</span>
<span class="tag tag-wifi" id="tagWiFi">WiFi 未连接</span>
<span class="tag tag-mqtt" id="tagMqtt">MQTT 未连接</span>
<span class="tag tag-mode" id="tagMode">本地控制</span>
<span class="tag tag-uptime" id="tagUptime">00:00:00</span>
</div>
</div>
<!-- Status Cards -->
<div class="status-cards">
<div class="stat-card"><div class="slabel">WiFi 状态</div><div class="sval" id="scWiFi">未连接</div></div>
<div class="stat-card ip"><div class="slabel">IP 地址</div><div class="sval" id="scIP">---</div></div>
<div class="stat-card"><div class="slabel">MQTT 状态</div><div class="sval" id="scMqtt">未连接</div></div>
<div class="stat-card"><div class="slabel">运行模式</div><div class="sval" id="scMode">外部旋钮</div></div>
</div>
<!-- Main Content -->
<div class="main-grid">
<!-- RPM Gauge -->
<div class="panel">
<h2>当前运行状态</h2>
<div class="gauge-wrap">
<canvas id="gaugeCanvas" width="240" height="180"></canvas>
</div>
<div class="gauge-info">
<div class="ginfo"><div class="glabel">PWM占空比</div><div class="gval accent" id="giDuty">0%</div></div>
<div class="ginfo"><div class="glabel">外部输入</div><div class="gval" id="giExt">0%</div></div>
<div class="ginfo"><div class="glabel">系统温度</div><div class="gval" id="giTemp">N/A</div></div>
</div>
</div>
<!-- PWM Control -->
<div class="panel">
<h2>PWM 控制</h2>
<div id="modeBadge" class="mode-badge external">外部旋钮控制</div>
<div class="slider-wrap">
<input type="range" id="speedSlider" min="0" max="100" value="0">
<div class="slider-val"><span id="sliderValue">0</span>%</div>
</div>
<div class="btn-row">
<button class="btn btn-danger" onclick="confirmStop()">紧急停机</button>
<button class="btn btn-primary" onclick="setSpeed(100,true)">全速运行</button>
</div>
<div class="btn-row" style="margin-top:8px">
<button class="btn btn-outline" onclick="openConfig()">网络配置</button>
<button class="btn btn-outline" onclick="openMQTTConfig()">MQTT配置</button>
</div>
</div>
</div>
<!-- History Chart -->
<div class="panel chart-panel">
<h2>转速实时曲线</h2>
<div class="chart-tabs"><button class="chart-tab active">实时更新</button></div>
<canvas id="rpmChart"></canvas>
<div id="chartTooltip" style="position:absolute;display:none;background:#0D1B2A;border:1px solid #00D8FF;padding:6px 10px;border-radius:6px;font-size:11px;pointer-events:none;z-index:10;color:#C8D6E5"></div>
</div>
<!-- Bottom -->
<div class="bottom-grid">
<!-- Device Info -->
<div class="panel">
<h2>设备信息</h2>
<table class="info-table">
<tr><td>设备ID</td><td id="diId">---</td></tr>
<tr><td>固件版本</td><td id="diFw">2.0.0</td></tr>
<tr><td>硬件版本</td><td>V1.0</td></tr>
<tr><td>风机型号</td><td>4线PWM风机</td></tr>
<tr><td>PWM频率</td><td>25kHz</td></tr>
<tr><td>MAC地址</td><td id="diMac">---</td></tr>
</table>
</div>
<!-- System Status -->
<div class="panel">
<h2>系统状态</h2>
<ul class="alarm-list">
<li id="alFan" class="alarm-ok"><span>风机状态</span><span style="margin-left:auto">正常</span></li>
<li id="alMqtt" class="alarm-ok"><span>MQTT连接</span><span style="margin-left:auto">未连接</span></li>
<li id="alWiFi" class="alarm-ok"><span>WiFi连接</span><span style="margin-left:auto">仅AP模式</span></li>
<li id="alPwm"><span>PWM信号</span><span style="margin-left:auto">正常</span></li>
</ul>
</div>
</div>
<div class="footer">ESP8266 Fan Controller | 25kHz PWM | V2.0</div>
</div>
<!-- WiFi Config Modal -->
<div class="modal" id="configModal">
<div class="modal-box">
<h2>网络配置</h2>
<div class="fgroup"><label>WiFi名称 (SSID)</label><input type="text" id="wifiSsid" placeholder="输入WiFi名称"></div>
<div class="fgroup"><label>WiFi密码</label><input type="password" id="wifiPassword" placeholder="输入WiFi密码"></div>
<div class="fgroup"><label>当前配置</label><div id="currentWifi" style="color:#5A6D80;font-size:12px">未配置</div></div>
<div class="modal-btns"><button class="btn btn-outline" onclick="closeConfig()">取消</button><button class="btn btn-primary" onclick="saveConfig()">保存</button></div>
</div>
</div>
<!-- MQTT Config Modal -->
<div class="modal" id="mqttModal">
<div class="modal-box">
<h2>MQTT 配置</h2>
<div class="fgroup"><label>MQTT服务器地址</label><input type="text" id="mqttServer" placeholder="192.168.1.100"></div>
<div class="fgroup"><label>端口</label><input type="number" id="mqttPort" placeholder="1883" value="1883"></div>
<div class="fgroup"><label>发布主题 (状态上报)</label><input type="text" id="mqttTopic" placeholder="fan/status" value="fan/status"></div>
<div class="fgroup"><label>订阅主题 (接收命令)</label><input type="text" id="mqttCmdTopic" placeholder="fan/command" value="fan/command"></div>
<div class="fgroup"><label>上报间隔 (秒)</label><input type="number" id="mqttInterval" placeholder="5" value="5" min="1" max="3600"></div>
<div class="fgroup"><label style="display:flex;align-items:center"><input type="checkbox" id="mqttEnabled" checked>启用MQTT上报</label></div>
<div class="modal-btns"><button class="btn btn-outline" onclick="closeMQTTConfig()">取消</button><button class="btn btn-primary" onclick="saveMQTTConfig()">保存</button></div>
</div>
</div>
<!-- Emergency Stop Confirm Modal -->
<div class="modal" id="stopModal">
<div class="modal-box">
<h2 class="danger">确认紧急停机</h2>
<p style="color:#5A6D80;margin-bottom:20px;font-size:13px">此操作将立即停止风机运行，确认继续？</p>
<div class="modal-btns"><button class="btn btn-outline" onclick="closeStop()">取消</button><button class="btn btn-danger" onclick="execStop()">确认停机</button></div>
</div>
</div>
<script>
let rpmHistory=[];
const maxPoints=60;
let isDragging=false;

// ---- RPM Gauge ----
function drawGauge(rpm){
    const c=document.getElementById('gaugeCanvas');
    const ctx=c.getContext('2d');
    const cx=120,cy=130,r=95;
    ctx.clearRect(0,0,c.width,c.height);
    // bg arc
    ctx.beginPath();ctx.arc(cx,cy,r,Math.PI,2*Math.PI);
    ctx.strokeStyle='#1B2A3F';ctx.lineWidth=18;ctx.stroke();
    // zones
    const maxR=3000;
    const pct=Math.min(rpm/maxR,1);
    const endAng=Math.PI+pct*Math.PI;
    // color gradient
    const grad=ctx.createLinearGradient(0,cy-r,0,cy+r);
    grad.addColorStop(0,'#00D8FF');
    grad.addColorStop(.35,'#00D97E');
    grad.addColorStop(.7,'#FFC53D');
    grad.addColorStop(1,'#FF4D4F');
    ctx.beginPath();ctx.arc(cx,cy,r,Math.PI,endAng);
    ctx.strokeStyle=grad;ctx.lineWidth=16;ctx.lineCap='round';ctx.stroke();
    // tick marks
    for(let i=0;i<=5;i++){
        const a=Math.PI+i*Math.PI/5;
        const x1=cx+(r-26)*Math.cos(a),y1=cy+(r-26)*Math.sin(a);
        const x2=cx+(r-10)*Math.cos(a),y2=cy+(r-10)*Math.sin(a);
        ctx.beginPath();ctx.moveTo(x1,y1);ctx.lineTo(x2,y2);
        ctx.strokeStyle='#5A6D80';ctx.lineWidth=1.5;ctx.stroke();
        const tx=cx+(r-36)*Math.cos(a),ty=cy+(r-36)*Math.sin(a);
        ctx.fillStyle='#5A6D80';ctx.font='10px monospace';ctx.textAlign='center';
        ctx.fillText(i*600,tx,ty+4);
    }
    // center text
    ctx.fillStyle='#C8D6E5';ctx.font='bold 38px Inter,sans-serif';ctx.textAlign='center';
    ctx.fillText(rpm,cx,cy-2);
    ctx.fillStyle='#5A6D80';ctx.font='12px Inter,sans-serif';ctx.fillText('RPM',cx,cy+20);
}

// ---- Chart ----
function drawChart(){
    const canvas=document.getElementById('rpmChart');
    const ctx=canvas.getContext('2d');
    const dpr=window.devicePixelRatio||1;
    const w=canvas.offsetWidth, h=240;
    canvas.width=w*dpr;canvas.height=h*dpr;canvas.style.width=w+'px';canvas.style.height=h+'px';
    ctx.setTransform(dpr,0,0,dpr,0,0);
    ctx.fillStyle='#07111F';ctx.fillRect(0,0,w,h);
    // grid
    ctx.strokeStyle='#1B2A3F';ctx.lineWidth=.5;
    for(let i=0;i<=4;i++){const y=(h/4)*i;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}
    if(rpmHistory.length<2){ctx.fillStyle='#5A6D80';ctx.font='13px Inter,sans-serif';ctx.textAlign='center';ctx.fillText('等待数据...',w/2,h/2);return}
    const maxRPM=Math.max(...rpmHistory,500);
    const stepX=w/(rpmHistory.length-1);
    const grad=ctx.createLinearGradient(0,0,w,0);
    grad.addColorStop(0,'#00D8FF');grad.addColorStop(.5,'#00D97E');grad.addColorStop(1,'#A855F7');
    ctx.strokeStyle=grad;ctx.lineWidth=2.5;ctx.lineJoin='round';
    ctx.beginPath();
    for(let i=0;i<rpmHistory.length;i++){
        const x=i*stepX,y=h-(rpmHistory[i]/maxRPM)*h;
        if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
    }
    ctx.stroke();
    // fill
    ctx.lineTo(w,h);ctx.lineTo(0,h);ctx.closePath();
    const fgrad=ctx.createLinearGradient(0,0,0,h);
    fgrad.addColorStop(0,'rgba(0,216,255,.15)');fgrad.addColorStop(1,'rgba(0,216,255,.01)');
    ctx.fillStyle=fgrad;ctx.fill();
}

// ---- Data Fetching ----
async function fetchData(){
    try{
        const res=await fetch('/data');
        const json=await res.json();
        document.getElementById('giDuty').textContent=json.currentDuty+'%';
        document.getElementById('giExt').textContent=json.externalDuty+'%';
        document.getElementById('giTemp').textContent='N/A';
        if(!isDragging){
            document.getElementById('speedSlider').value=json.currentDuty;
            document.getElementById('sliderValue').textContent=json.currentDuty;
        }
        drawGauge(json.rpm);
        rpmHistory.push(json.rpm);
        if(rpmHistory.length>maxPoints)rpmHistory.shift();
        drawChart();
        // mode badge
        const mEl=document.getElementById('modeBadge');
        const mTag=document.getElementById('tagMode');
        const mSc=document.getElementById('scMode');
        const modes={force:['强制控制','force','强制'],remote:['远程控制','remote','远程'],web:['Web手动','manual','Web手动']};
        const m=modes[json.mode]||['外部旋钮控制','external','外部旋钮'];
        mEl.className='mode-badge '+m[1];mEl.textContent=m[0];
        mSc.textContent=m[2];
    }catch(e){console.error('Data error:',e);}
}

function updateStatus(){
    fetch('/status').then(r=>r.json()).then(json=>{
        document.getElementById('deviceId').textContent=json.id||'---';
        document.getElementById('diId').textContent=json.id||'---';
        document.getElementById('diFw').textContent=json.fwVer||'2.0.0';
        document.getElementById('diMac').textContent=json.mac||'---';
        // WiFi
        const tw=document.getElementById('tagWiFi');
        document.getElementById('scWiFi').textContent=json.sta?'已连接':'未连接';
        document.getElementById('scIP').textContent=json.sta?json.staIP:json.apIP;
        tw.textContent='WiFi '+(json.sta?'已连接':'未连接');
        tw.className=json.sta?'tag tag-online':'tag tag-wifi';
        // MQTT
        const tm=document.getElementById('tagMqtt');
        document.getElementById('scMqtt').textContent=json.mqttConnected?'已连接':'未连接';
        tm.textContent='MQTT '+(json.mqttConnected?'已连接':'未连接');
        tm.className=json.mqttConnected?'tag tag-online':'tag tag-mqtt';
        document.getElementById('alMqtt').className=json.mqttConnected?'alarm-ok':'alarm-warn';
        document.getElementById('alMqtt').children[1].textContent=json.mqttConnected?'已连接':'未连接';
        document.getElementById('alWiFi').className=json.sta?'alarm-ok':'alarm-warn';
        document.getElementById('alWiFi').children[1].textContent=json.sta?'已连接':'仅AP模式';
        // uptime
        const s=json.uptime||0;
        const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
        document.getElementById('tagUptime').textContent=String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(sec).padStart(2,'0');
        document.getElementById('currentWifi').textContent=json.configSsid||'未配置';
    }).catch(e=>{});
}

// ---- Control ----
async function setSpeed(val,force){
    await fetch('/control?duty='+val+'&force='+(force?1:0));
    document.getElementById('sliderValue').textContent=val;
    document.getElementById('speedSlider').value=val;
}

function confirmStop(){
    document.getElementById('stopModal').classList.add('active');
}
function closeStop(){
    document.getElementById('stopModal').classList.remove('active');
}
function execStop(){
    setSpeed(0,true);
    closeStop();
}

// ---- Slider Events ----
document.getElementById('speedSlider').addEventListener('input',function(){
    isDragging=true;
    document.getElementById('sliderValue').textContent=this.value;
    setSpeed(this.value);
});
document.getElementById('speedSlider').addEventListener('mouseup',()=>{isDragging=false});
document.getElementById('speedSlider').addEventListener('touchend',()=>{isDragging=false});

// ---- WiFi Config ----
function openConfig(){
    fetch('/status').then(r=>r.json()).then(json=>{
        document.getElementById('wifiSsid').value=json.configSsid||'';
        document.getElementById('configModal').classList.add('active');
    }).catch(()=>{alert('读取配置失败');});
}
function closeConfig(){document.getElementById('configModal').classList.remove('active')}
async function saveConfig(){
    const ssid=document.getElementById('wifiSsid').value;
    const pw=document.getElementById('wifiPassword').value;
    if(!ssid){alert('请输入WiFi名称');return}
    const res=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pw)});
    const json=await res.json();
    alert(json.success?'配置已保存，设备将尝试连接...':'保存失败: '+json.error);
    closeConfig();
}

// ---- MQTT Config ----
function openMQTTConfig(){
    fetch('/status').then(r=>r.json()).then(json=>{
        document.getElementById('mqttServer').value=json.mqttServer||'';
        document.getElementById('mqttPort').value=json.mqttPort||1883;
        document.getElementById('mqttTopic').value=json.mqttTopic||'fan/status';
        document.getElementById('mqttCmdTopic').value=json.mqttCmdTopic||'fan/command';
        document.getElementById('mqttInterval').value=json.mqttInterval||5;
        document.getElementById('mqttEnabled').checked=!!json.mqttEnabled;
        document.getElementById('mqttModal').classList.add('active');
    }).catch(()=>{alert('读取配置失败');});
}
function closeMQTTConfig(){document.getElementById('mqttModal').classList.remove('active')}
async function saveMQTTConfig(){
    const srv=document.getElementById('mqttServer').value;
    const port=document.getElementById('mqttPort').value;
    const topic=document.getElementById('mqttTopic').value;
    const cmdt=document.getElementById('mqttCmdTopic').value;
    const ival=document.getElementById('mqttInterval').value;
    const en=document.getElementById('mqttEnabled').checked?'1':'0';
    const res=await fetch('/mqttconfig',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'server='+encodeURIComponent(srv)+'&port='+port+'&topic='+encodeURIComponent(topic)+'&cmdtopic='+encodeURIComponent(cmdt)+'&interval='+ival+'&enabled='+en});
    const json=await res.json();
    alert(json.success?'MQTT配置已保存！'+(en=='1'?'将开始上报数据':'已停止上报'):'保存失败');
    closeMQTTConfig();
}

// ---- Init ----
setInterval(fetchData,500);
setInterval(updateStatus,2000);
fetchData();updateStatus();drawGauge(0);drawChart();
</script>
</body>
</html>
)rawliteral";

// ==================== EEPROM操作 ====================
void loadWiFiConfig() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(EEPROM_WIFI_OFFSET, wifiConfig);
    if (wifiConfig.magic != EEPROM_MAGIC) {
        // 首次启动，写默认值
        memset(&wifiConfig, 0, sizeof(wifiConfig));
        wifiConfig.magic = EEPROM_MAGIC;
        wifiConfig.valid = false;
        EEPROM.put(EEPROM_WIFI_OFFSET, wifiConfig);
        EEPROM.commit();
        Serial.println("[Config] EEPROM首次初始化(WiFi)");
    } else if (wifiConfig.valid) {
        Serial.printf("[Config] 已加载WiFi配置: %s\n", wifiConfig.ssid);
    } else {
        Serial.println("[Config] 无WiFi配置");
    }
}

void saveWiFiConfig(const char* ssid, const char* password) {
    memset(&wifiConfig, 0, sizeof(wifiConfig));
    wifiConfig.magic = EEPROM_MAGIC;
    strncpy(wifiConfig.ssid, ssid, sizeof(wifiConfig.ssid) - 1);
    wifiConfig.ssid[sizeof(wifiConfig.ssid) - 1] = '\0';
    strncpy(wifiConfig.password, password, sizeof(wifiConfig.password) - 1);
    wifiConfig.password[sizeof(wifiConfig.password) - 1] = '\0';
    wifiConfig.valid = true;
    EEPROM.put(EEPROM_WIFI_OFFSET, wifiConfig);
    EEPROM.commit();
    Serial.printf("[Config] 已保存WiFi配置: %s\n", ssid);
}

void clearWiFiConfig() {
    wifiConfig.valid = false;
    wifiConfig.ssid[0] = '\0';
    wifiConfig.password[0] = '\0';
    // keep magic
    EEPROM.put(EEPROM_WIFI_OFFSET, wifiConfig);
    EEPROM.commit();
    Serial.println("[Config] WiFi配置已清除");
}

void loadMQTTConfig() {
    EEPROM.get(EEPROM_MQTT_OFFSET, mqttConfig);
    if (mqttConfig.magic != EEPROM_MAGIC) {
        // 首次启动，写默认值
        memset(&mqttConfig, 0, sizeof(mqttConfig));
        mqttConfig.magic = EEPROM_MAGIC;
        mqttConfig.port = 1883;
        mqttConfig.interval = 5;
        mqttConfig.enabled = false;
        strncpy(mqttConfig.topic, "fan/status", sizeof(mqttConfig.topic) - 1);
        mqttConfig.topic[sizeof(mqttConfig.topic) - 1] = '\0';
        strncpy(mqttConfig.cmdTopic, "fan/command", sizeof(mqttConfig.cmdTopic) - 1);
        mqttConfig.cmdTopic[sizeof(mqttConfig.cmdTopic) - 1] = '\0';
        EEPROM.put(EEPROM_MQTT_OFFSET, mqttConfig);
        EEPROM.commit();
        Serial.println("[Config] EEPROM首次初始化(MQTT，默认关闭)");
        return;
    }
    if (mqttConfig.enabled) {
        Serial.printf("[Config] 已加载MQTT配置: %s:%d 发布:%s 订阅:%s 间隔:%ds\n", 
            mqttConfig.server, mqttConfig.port, mqttConfig.topic, mqttConfig.cmdTopic, mqttConfig.interval);
    } else {
        Serial.println("[Config] MQTT上报已关闭（有配置但未启用）");
    }
}

void saveMQTTConfig(const char* server, int port, const char* topic, const char* cmdTopic, int interval, bool enabled) {
    memset(&mqttConfig, 0, sizeof(mqttConfig));
    mqttConfig.magic = EEPROM_MAGIC;
    strncpy(mqttConfig.server, server, sizeof(mqttConfig.server) - 1);
    mqttConfig.server[sizeof(mqttConfig.server) - 1] = '\0';
    mqttConfig.port = port;
    strncpy(mqttConfig.topic, topic, sizeof(mqttConfig.topic) - 1);
    mqttConfig.topic[sizeof(mqttConfig.topic) - 1] = '\0';
    strncpy(mqttConfig.cmdTopic, cmdTopic, sizeof(mqttConfig.cmdTopic) - 1);
    mqttConfig.cmdTopic[sizeof(mqttConfig.cmdTopic) - 1] = '\0';
    mqttConfig.interval = interval;
    mqttConfig.enabled = enabled;
    EEPROM.put(EEPROM_MQTT_OFFSET, mqttConfig);
    EEPROM.commit();
    Serial.printf("[Config] 已保存MQTT配置: %s:%d 发布:%s 订阅:%s 间隔:%ds 启用:%d\n", 
        server, port, topic, cmdTopic, interval, enabled);
}

// ==================== 初始化WiFi (AP+STA双模) ====================
void initWiFi() {
    Serial.println("\n[WiFi] 启动AP+STA双模模式...");
    
    // AP模式始终开启
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(localAPIP, localAPIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("Fan_Control", "12345678");
    
    Serial.println("[WiFi] AP已启动: Fan_Control");
    Serial.print("[WiFi] AP IP: ");
    Serial.println(WiFi.softAPIP());
    
    connectedTime = millis();
    
    // 如果有保存的WiFi配置，尝试连接
    if (wifiConfig.valid && strlen(wifiConfig.ssid) > 0) {
        Serial.printf("[WiFi] 尝试连接: %s\n", wifiConfig.ssid);
        WiFi.begin(wifiConfig.ssid, wifiConfig.password);
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            localSTAIP = WiFi.localIP();
            Serial.println();
            Serial.println("[WiFi] STA连接成功!");
            Serial.print("[WiFi] STA IP: ");
            Serial.println(localSTAIP);
        } else {
            Serial.println();
            Serial.println("[WiFi] STA连接失败，将保持AP模式");
        }
    }
}

// ==================== PWM输出初始化 (25kHz) ====================
void initFanPWM() {
    analogWriteRange(1023);
    analogWriteFreq(PWM_FREQUENCY);
    pinMode(PIN_FAN_PWM, OUTPUT);
    analogWrite(PIN_FAN_PWM, 0);
    Serial.println("[PWM] 风机PWM初始化完成 (25kHz, 10bit)");
}

// ==================== 外部PWM输入初始化 (中断方式) ====================
void IRAM_ATTR extPWMISR() {
    uint32_t now = micros();
    if (digitalRead(PIN_EXT_PWM) == HIGH) {
        // 上升沿：计算上一个周期长度
        uint32_t period = now - extPwmRiseTime;
        // 放宽周期容差：15~250us (4kHz~66kHz)，适应5V钳位畸变导致的边沿偏移
        if (period > 15 && period < 250) {
            extPwmPeriodUs = period;
            extPwmNewData = true;
        }
        extPwmRiseTime = now;
    } else {
        // 下降沿：记录高电平持续时间
        uint32_t highUs = now - extPwmRiseTime;
        if (highUs < 250) {
            extPwmHighUs = highUs;
        }
    }
    extPwmLastEdgeTime = millis();
}

void initExternalPWM() {
    // 设为INPUT模式 (ESP8266 GPIO13无内部下拉，建议硬件加分压电阻 10k+20k)
    pinMode(PIN_EXT_PWM, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_EXT_PWM), extPWMISR, CHANGE);
    Serial.println("[EXT] 外部PWM输入初始化完成 (GPIO13, 中断CHANGE)");
}

// ==================== TACH测速初始化 ====================
void IRAM_ATTR tachISR() {
    tachCount++;
}

void initTach() {
    pinMode(PIN_TACH, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TACH), tachISR, FALLING);
    Serial.println("[TACH] 测速初始化完成 (GPIO12, 下降沿中断)");
}

// ==================== 外部PWM采集 (中断非阻塞 + 中值 + EMA双重滤波) ====================
#define EXT_SAMPLE_INTERVAL 80  // 采样间隔(ms)

uint16_t readExternalPWMDuty() {
    static uint16_t rawSamples[3] = {0};
    static uint8_t sampleIdx = 0;
    static uint16_t lastOutputDuty = 0;
    static float emaDuty = 0;        // EMA平滑值 (浮点，保持精度)
    static unsigned long lastSample = 0;
    static unsigned long lastDebug = 0;
    static int noSignalCount = 0;
    
    // 无信号检测：超过200ms没有边沿触发，判定为无信号
    if (!extPwmNewData) {
        if (millis() - extPwmLastEdgeTime > 200) {
            noSignalCount++;
            if (noSignalCount > 5) {
                lastOutputDuty = 0;
                emaDuty = 0;
                rawSamples[0] = 0; rawSamples[1] = 0; rawSamples[2] = 0;
            }
        }
        return lastOutputDuty;
    }
    noSignalCount = 0;
    
    // 原子读取（ESP8266是32位MCU，uint32_t读写是原子的）
    uint32_t highUs = extPwmHighUs;
    uint32_t periodUs = extPwmPeriodUs;
    extPwmNewData = false;
    
    // 计算原始占空比
    uint16_t rawDuty = (periodUs > 0) ? ((highUs * 100) / periodUs) : 0;
    if (rawDuty > 100) rawDuty = 100;
    
    // 按采样间隔衰减（80ms一次），避免每次PWM周期都处理
    unsigned long now = millis();
    if (now - lastSample < EXT_SAMPLE_INTERVAL) return lastOutputDuty;
    lastSample = now;
    
    // 中值滤波：取3个样本的中位数，有效抑制偶发噪点
    rawSamples[sampleIdx] = rawDuty;
    sampleIdx = (sampleIdx + 1) % 3;
    
    uint16_t sorted[3];
    sorted[0] = rawSamples[0]; sorted[1] = rawSamples[1]; sorted[2] = rawSamples[2];
    if (sorted[0] > sorted[1]) { uint16_t t = sorted[0]; sorted[0] = sorted[1]; sorted[1] = t; }
    if (sorted[1] > sorted[2]) { uint16_t t = sorted[1]; sorted[1] = sorted[2]; sorted[2] = t; }
    if (sorted[0] > sorted[1]) { uint16_t t = sorted[0]; sorted[0] = sorted[1]; sorted[1] = t; }
    uint16_t medianDuty = sorted[1];
    
    // EMA指数平滑 (alpha=0.25)：在中值滤波之后再做一次衰减，对抗5V边沿畸变导致的残余抖动
    // 公式: ema = 0.25 * median + 0.75 * ema_old
    if (emaDuty < 0.1f && medianDuty > 0) {
        emaDuty = medianDuty;  // 首次有效信号，快速响应
    } else {
        emaDuty = 0.25f * medianDuty + 0.75f * emaDuty;
    }
    uint16_t smoothedDuty = (uint16_t)(emaDuty + 0.5f);  // 四舍五入
    if (smoothedDuty > 100) smoothedDuty = 100;
    
    // 死区2%：5V钳位畸变时波动更大，加大死区避免反复跳变
    if (abs((int)smoothedDuty - (int)lastOutputDuty) >= 2) {
        lastOutputDuty = smoothedDuty;
    }
    
    if (now - lastDebug >= 2000) {
        Serial.printf("[EXT] high=%uus period=%uus raw=%u%% med=%u%% ema=%d%% out=%u%%\n",
            highUs, periodUs, rawDuty, medianDuty, smoothedDuty, lastOutputDuty);
        lastDebug = now;
    }
    
    return lastOutputDuty;
}

// ==================== 更新转速 ====================
void updateRPM() {
    unsigned long now = millis();
    if (now - lastRPMTime >= 1000) {
        uint32_t counts = tachCount - lastTachCount;
        currentRPM = (counts * 60000UL) / (TACH_PULSES_PER_REV * (now - lastRPMTime));
        lastTachCount = tachCount;
        lastRPMTime = now;
        
        rpmHistory[rpmHistoryIndex] = currentRPM;
        rpmHistoryIndex = (rpmHistoryIndex + 1) % RPM_HISTORY_SIZE;
    }
}

// ==================== 更新控制逻辑 ====================
void updateControl() {
    uint16_t extDuty = readExternalPWMDuty();
    unsigned long now = millis();
    
    switch (controlSrc) {
    case SRC_FORCE:
        break;  // 强制模式永不自动切换
    case SRC_WEB:
    case SRC_REMOTE: {  // Web和远程控制：3秒后可被外部旋钮覆盖
        static uint16_t lastExtCheckDuty = 0;
        static uint8_t extStableCount = 0;
        
        if (now - webControlStart >= WEB_OVERRIDE_TIME) {
            int deltaExt = abs((int)extDuty - (int)externalDuty);
            if (deltaExt > 15) {
                // 要求连续5次稳定在同一值（波动<=3%），确认是人为旋钮而非噪声
                if (abs((int)extDuty - (int)lastExtCheckDuty) <= 3) {
                    extStableCount++;
                    if (extStableCount >= 5) {
                        controlSrc = SRC_EXTERNAL;
                        externalDuty = extDuty;
                        currentDuty = externalDuty;
                        analogWrite(PIN_FAN_PWM, map(currentDuty, 0, 100, 0, 1023));
                        extStableCount = 0;
                        lastExtCheckDuty = 0;
                        Serial.printf("[Control] 切换到外部控制: %d%%\n", externalDuty);
                    }
                } else {
                    extStableCount = 0;
                }
                lastExtCheckDuty = extDuty;
            } else {
                extStableCount = 0;
            }
        }
        break;
    }
    default: {
        static uint16_t lastExtDuty = 0;
        if (abs((int)extDuty - (int)lastExtDuty) > 5) {
            externalDuty = extDuty;
            lastExtDuty = extDuty;
            currentDuty = externalDuty;
            analogWrite(PIN_FAN_PWM, map(currentDuty, 0, 100, 0, 1023));
        }
        break;
    }
    }
}

void setManualDuty(uint16_t duty, bool force = false) {
    unsigned long now = millis();
    manualDuty = constrain(duty, 0, 100);
    controlSrc = force ? SRC_FORCE : SRC_WEB;
    webControlStart = now;
    currentDuty = manualDuty;
    // 记录当前外部旋钮的实际读数作为基准，避免噪声/漂移导致误切换回外部模式
    if (!force) externalDuty = readExternalPWMDuty();
    analogWrite(PIN_FAN_PWM, map(currentDuty, 0, 100, 0, 1023));
    Serial.printf("[Control] %s控制: %d%% (外部基准:%d%%)\n", force ? "强制" : "Web", manualDuty, externalDuty);
}

void setRemoteDuty(uint16_t duty, bool force = false) {
    unsigned long now = millis();
    remoteDuty = constrain(duty, 0, 100);
    controlSrc = force ? SRC_FORCE : SRC_REMOTE;
    webControlStart = now;
    currentDuty = remoteDuty;
    // 记录当前外部旋钮的实际读数作为基准
    if (!force) externalDuty = readExternalPWMDuty();
    analogWrite(PIN_FAN_PWM, map(currentDuty, 0, 100, 0, 1023));
    Serial.printf("[Control] %s控制: %d%% (外部基准:%d%%)\n", force ? "强制" : "远程", remoteDuty, externalDuty);
}

// ==================== Web服务器路由 ====================
void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
    char json[512];
    bool isManual = (controlSrc != SRC_EXTERNAL);
    const char* ctrlMode = (controlSrc == SRC_FORCE) ? "force" : 
                          (controlSrc == SRC_REMOTE) ? "remote" :
                          (controlSrc == SRC_WEB) ? "web" : "external";
    snprintf(json, sizeof(json),
        "{\"id\":\"%s\",\"rpm\":%d,\"currentDuty\":%d,\"externalDuty\":%d,\"webDuty\":%d,\"remoteDuty\":%d,\"mode\":\"%s\",\"manualControl\":%s}",
        deviceId, currentRPM, currentDuty, externalDuty, manualDuty, remoteDuty,
        ctrlMode, isManual ? "true" : "false");
    server.send(200, "application/json", json);
}

void handleControl() {
    if (server.hasArg("duty")) {
        uint16_t duty = server.arg("duty").toInt();
        bool force = server.hasArg("force") && server.arg("force") == "1";
        setManualDuty(duty, force);
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleStatus() {
    String staIPStr = staConnected ? localSTAIP.toString() : "---";
    String apIPStr = WiFi.softAPIP().toString();
    String macStr = WiFi.macAddress();
    String json = "{";
    json += "\"id\":\"" + String(deviceId) + "\",";
    json += "\"sta\":" + String(staConnected ? "true" : "false") + ",";
    json += "\"staIP\":\"" + staIPStr + "\",";
    json += "\"apIP\":\"" + apIPStr + "\",";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"configSsid\":\"" + String(wifiConfig.valid ? wifiConfig.ssid : "") + "\",";
    json += "\"mqttServer\":\"" + String(mqttConfig.server) + "\",";
    json += "\"mqttPort\":" + String(mqttConfig.port) + ",";
    json += "\"mqttTopic\":\"" + String(mqttConfig.topic) + "\",";
    json += "\"mqttCmdTopic\":\"" + String(mqttConfig.cmdTopic) + "\",";
    json += "\"mqttInterval\":" + String(mqttConfig.interval) + ",";
    json += "\"mqttEnabled\":" + String(mqttConfig.enabled ? "true" : "false") + ",";
    json += "\"fwVer\":\"" FW_VERSION "\",";
    json += "\"mac\":\"" + macStr + "\",";
    json += "\"mqttConnected\":" + String(mqttClient.connected() ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
}

void handleHistory() {
    String json = "{\"data\":[";
    for (uint8_t i = 0; i < RPM_HISTORY_SIZE; i++) {
        if (i > 0) json += ",";
        json += String(rpmHistory[i]);
    }
    json += "],\"count\":" + String(RPM_HISTORY_SIZE) + "}";
    server.send(200, "application/json", json);
}

void handleConfig() {
    if (server.method() == HTTP_POST) {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        
        if (ssid.length() == 0) {
            server.send(200, "application/json", "{\"success\":false,\"error\":\"SSID不能为空\"}");
            return;
        }
        
        saveWiFiConfig(ssid.c_str(), password.c_str());
        
        // 尝试连接新WiFi
        WiFi.begin(ssid.c_str(), password.c_str());
        
        server.send(200, "application/json", "{\"success\":true}");
        
        // 延迟后检查连接状态
        delay(3000);
        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            localSTAIP = WiFi.localIP();
        }
    } else {
        server.send(405, "text/plain", "Method Not Allowed");
    }
}

void handleClearConfig() {
    clearWiFiConfig();
    WiFi.disconnect();
    staConnected = false;
    server.send(200, "application/json", "{\"success\":true}");
}

void handleMQTTConfig() {
    if (server.method() == HTTP_POST) {
        String serverAddr = server.arg("server");
        int port = server.arg("port").toInt();
        String topic = server.arg("topic");
        String cmdTopic = server.arg("cmdtopic");
        int interval = server.arg("interval").toInt();
        bool enabled = server.arg("enabled") == "1";
        
        if (serverAddr.length() == 0) enabled = false;
        if (port <= 0) port = 1883;
        if (topic.length() == 0) topic = "fan/status";
        if (cmdTopic.length() == 0) cmdTopic = "fan/command";
        if (interval <= 0) interval = 5;
        
        saveMQTTConfig(serverAddr.c_str(), port, topic.c_str(), cmdTopic.c_str(), interval, enabled);
        
        if (mqttClient.connected()) mqttClient.disconnect();
        mqttConnected = false;
        mqttEnabled = staConnected && enabled;
        initMQTT();
        
        if (mqttEnabled) {
            mqttConnected = tryMQTTConnect();
        }
        
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(405, "text/plain", "Method Not Allowed");
    }
}

void initWebServer() {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/control", handleControl);
    server.on("/status", handleStatus);
    server.on("/config", handleConfig);
    server.on("/clearconfig", handleClearConfig);
    server.on("/mqttconfig", handleMQTTConfig);
    server.on("/api/device/status", handleStatus);
    server.on("/api/device/history", handleHistory);
    server.begin();
    Serial.println("[Web] Web服务器已启动 (端口 80)");
}

// ==================== 串口打印帮助 ====================
void printHelp() {
    Serial.println("\n========================================");
    Serial.println("       ESP8266 风机控制系统 v2.0        ");
    Serial.printf( "           设备ID: %s                \n", deviceId);
    Serial.println("========================================");
    Serial.println("引脚配置:");
    Serial.println("  GPIO14 (D5) - 风机PWM输出 (25kHz)");
    Serial.println("  GPIO13 (D7) - 外部PWM输入");
    Serial.println("  GPIO12 (D6) - 转速TACH输入");
    Serial.println("\nWiFi模式: AP + STA 双模");
    Serial.println("  AP热点: Fan_Control / 12345678");
    Serial.println("  AP IP: 192.168.4.1");
    Serial.println("  可通过Web后台配置连接外部WiFi");
    Serial.println("\n控制模式:");
    Serial.println("  - 外部旋钮优先控制");
    Serial.println("  - Web页面手动覆盖");
    Serial.println("  - MQTT远程控制 (订阅: fan/command, JSON: {\"duty\":80})");
    Serial.println("  - 旋转旋钮自动切回外部控制");
    Serial.println("\n访问 Web UI: http://192.168.4.1");
    Serial.println("========================================\n");
}

// ==================== Setup ====================
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n[System] ESP8266 风机控制系统启动...");
    
    loadWiFiConfig();
    loadMQTTConfig();
    
    initFanPWM();
    initExternalPWM();
    initTach();
    initWiFi();
    generateDeviceId();
    initWebServer();
    initMQTT();
    
    printHelp();
    
    lastRPMTime = millis();
    extPwmLastEdgeTime = millis();
}

// ==================== Loop ====================
void loop() {
    static unsigned long lastPrint = 0;
    unsigned long now = millis();
    
    server.handleClient();
    
    // 检查STA连接状态
    if (wifiConfig.valid) {
        bool currentSta = (WiFi.status() == WL_CONNECTED);
        if (currentSta != staConnected) {
            staConnected = currentSta;
            if (staConnected) {
                localSTAIP = WiFi.localIP();
                Serial.println("[WiFi] STA重新连接成功!");
                Serial.print("[WiFi] STA IP: ");
                Serial.println(localSTAIP);
            }
        }
    }
    
    // MQTT：STA连接时自动启用，STA断开时禁用
    if (!staConnected && mqttEnabled) {
        mqttEnabled = false;
        mqttConnected = false;
        mqttReconnectInterval = MQTT_RECONNECT_MIN;
    }
    if (staConnected && mqttConfig.enabled && !mqttEnabled) {
        mqttEnabled = true;
        mqttConnected = tryMQTTConnect();
    }
    
    // MQTT 主循环 + 断线重连
    if (mqttEnabled) {
        if (mqttClient.connected()) {
            mqttConnected = true;
            mqttClient.loop();
            unsigned int mqttInterval = mqttConfig.interval * 1000;  // 秒->毫秒
            if (mqttInterval < 1000) mqttInterval = 5000;
            if (now - lastMqttPublish >= mqttInterval) {
                publishMQTT();
                lastMqttPublish = now;
            }
        } else {
            mqttConnected = false;
            // 按重连间隔尝试重连
            if (now - lastMqttReconnectAttempt >= mqttReconnectInterval) {
                lastMqttReconnectAttempt = now;
                tryMQTTConnect();
            }
        }
    }
    
    updateControl();
    updateRPM();
    
    const char* modeStr = (controlSrc == SRC_FORCE) ? "强制" : 
                         (controlSrc == SRC_REMOTE) ? "远程" :
                         (controlSrc == SRC_WEB) ? "Web手动" : "外部旋钮";
    if (now - lastPrint > 1000) {
        Serial.printf("[Status] RPM: %d | 占空比: %d%% | 模式: %s | STA: %s | MQTT: %s\n",
            currentRPM, currentDuty, modeStr,
            staConnected ? "已连接" : "未连接",
            mqttConnected ? "已连接" : "未连接");
        lastPrint = now;
    }
    
    delay(10);
}
