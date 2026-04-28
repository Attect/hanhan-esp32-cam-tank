// 硬件：ESP32-COM (标准ESP32) + L298N + OV3660 + 舵机（铲子/照明模块）
// 安装依赖库：WebSockets by Markus Sattler，esp32-camera（随ESP32板包自带）
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_wifi.h>
#include <driver/gpio.h>

// =================== 用户配置区域 ===================
// 请修改为你自己的 WiFi 名称和密码
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ========== 引脚定义区 ==========
#define MOTOR_LEFT_IN1   12
#define MOTOR_LEFT_IN2   13
#define MOTOR_RIGHT_IN3  15
#define MOTOR_RIGHT_IN4  14
#define LIGHT_PIN        4
#define SERVO_PIN        2   // 铲子舵机 / 照明模块共用IO

// ========== 摄像头引脚定义 (ESP32-CAM 标准引脚) ==========
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

WebServer server(80);
WiFiServer streamServer(81);
WebSocketsServer webSocket = WebSocketsServer(82);

bool lightOn = false;
volatile int lightBrightness = 128;  // 灯亮度 0~255
int shovelAngle = 50;                // 铲子 0~100，默认中间位置

// 软件PWM定时器
#include <esp_timer.h>
esp_timer_handle_t lightTimer = NULL;
volatile int lightPwmPhase = 0;

void IRAM_ATTR lightTimerCallback(void* arg) {
  // 固定PWM周期500μs = 2kHz，人眼不可见闪烁
  const int period = 500;
  const int minPulse = 50;  // 最小脉冲宽度50μs，避免太短导致不稳定

  if (lightBrightness <= 0) {
    digitalWrite(LIGHT_PIN, LOW);
    lightPwmPhase = 0;
    esp_timer_start_once(lightTimer, 1000);
    return;
  }
  if (lightBrightness >= 255) {
    digitalWrite(LIGHT_PIN, HIGH);
    lightPwmPhase = 0;
    esp_timer_start_once(lightTimer, 1000);
    return;
  }

  // 将0~255映射到0~period，但限制最小脉冲宽度
  int onTime = (lightBrightness * period) / 255;
  if (onTime > 0 && onTime < minPulse) onTime = minPulse;
  if (onTime > period - minPulse) onTime = period;

  if (lightPwmPhase == 0) {
    digitalWrite(LIGHT_PIN, HIGH);
    esp_timer_start_once(lightTimer, onTime);
    lightPwmPhase = 1;
  } else {
    digitalWrite(LIGHT_PIN, LOW);
    int offTime = period - onTime;
    if (offTime < minPulse) offTime = minPulse;
    esp_timer_start_once(lightTimer, offTime);
    lightPwmPhase = 0;
  }
}
bool autoQuality = true;        // 自动画质开关
framesize_t manualFrameSize = FRAMESIZE_VGA;
framesize_t currentFrameSize = FRAMESIZE_VGA;
unsigned long lastMoveTime = 0;
bool isMovingFlag = false;
TaskHandle_t streamTaskHandle = NULL;

// 休眠相关
volatile int streamClientCount = 0;
unsigned long lastClientActive = 0;
bool isSleeping = false;
const unsigned long SLEEP_DELAY_MS = 5000;
const unsigned long POWER_ON_GRACE_PERIOD = 60000;  // 上电后60秒内不进入休眠
volatile bool cameraEnabled = true;

void initCamera();
void setFrameSize(framesize_t size);
void checkAutoQuality();
void initMotors();
void initLightPWM();
void initServo();
void setMotor(int left, int right);
void setShovel(int angle);
void setLightBrightness(int brightness);
void streamTask(void *pvParameters);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void handleRoot();
void handleMotor();
void handleLight();
void handleShovel();
void goToSleep();
void wakeUp();
void checkSleep();

// =================== 摄像头初始化 ===================
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = manualFrameSize;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    manualFrameSize = FRAMESIZE_QVGA;
  }
  currentFrameSize = config.frame_size;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败: 0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
}

// =================== 动态切换分辨率 ===================
void setFrameSize(framesize_t size) {
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, size);
    currentFrameSize = size;
    Serial.printf("画质切换: %d\n", size);
  }
}

// =================== 自动画质检测 ===================
void checkAutoQuality() {
  if (!autoQuality) return;
  static unsigned long lastCheck = 0;
  static unsigned long lastSwitch = 0;
  if (millis() - lastCheck < 500) return;
  lastCheck = millis();

  bool shouldLow = isMovingFlag || (millis() - lastMoveTime < 1500);
  framesize_t target = shouldLow ? FRAMESIZE_QVGA : manualFrameSize;
  if (target != currentFrameSize && millis() - lastSwitch > 3000) {
    setFrameSize(target);
    lastSwitch = millis();
  }
}

// =================== 休眠与唤醒 ===================
void goToSleep() {
  if (isSleeping) return;
  isSleeping = true;
  Serial.println("进入休眠：停止电机、关闭车灯与摄像头、降低WiFi功率");
  setMotor(0, 0);
  setLightBrightness(0);
  esp_camera_deinit();
  WiFi.setTxPower(WIFI_POWER_7dBm);
}

void wakeUp() {
  if (!isSleeping) return;
  isSleeping = false;
  Serial.println("唤醒：恢复摄像头与WiFi功率");
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  // 卸载摄像头遗留的GPIO ISR服务，避免重复初始化导致画面卡顿
  gpio_uninstall_isr_service();
  initCamera();
  if (!autoQuality && currentFrameSize != manualFrameSize) {
    setFrameSize(manualFrameSize);
  }
}

void checkSleep() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();

  // 上电冷却期内不进入休眠，给足够时间打开页面
  if (millis() < POWER_ON_GRACE_PERIOD) return;

  bool hasClient = webSocket.connectedClients() > 0 || streamClientCount > 0;
  if (hasClient) {
    lastClientActive = millis();
  } else if (!isSleeping && millis() - lastClientActive > SLEEP_DELAY_MS) {
    goToSleep();
  }
}

// =================== 电机/灯光/舵机初始化 ===================
void initMotors() {
  pinMode(MOTOR_LEFT_IN1, OUTPUT);
  pinMode(MOTOR_LEFT_IN2, OUTPUT);
  pinMode(MOTOR_RIGHT_IN3, OUTPUT);
  pinMode(MOTOR_RIGHT_IN4, OUTPUT);
  // 单PWM调速：IN1/IN3控制方向(digital)，IN2/IN4控制速度(PWM)
  // 左右电机共用定时器1，灯使用独立的定时器2，避免PWM配置冲突
  ledcAttachChannel(MOTOR_LEFT_IN2, 1000, 8, 3);
  ledcAttachChannel(MOTOR_RIGHT_IN4, 1000, 8, 2);
  digitalWrite(MOTOR_LEFT_IN1, LOW);
  ledcWrite(MOTOR_LEFT_IN2, 0);
  digitalWrite(MOTOR_RIGHT_IN3, LOW);
  ledcWrite(MOTOR_RIGHT_IN4, 0);
}

void initLightPWM() {
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);
  const esp_timer_create_args_t timer_args = {
    .callback = &lightTimerCallback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "light_pwm"
  };
  esp_timer_create(&timer_args, &lightTimer);
  esp_timer_start_once(lightTimer, 1000);
  setLightBrightness(0);
}

#define SERVO_LEDC_CHANNEL 6

void initServo() {
  ledcAttachChannel(SERVO_PIN, 50, 10, SERVO_LEDC_CHANNEL);
  setShovel(shovelAngle);
}

// =================== 铲子控制（修复方向：0=放下，100=抬起） ===================
void setShovel(int angle) {
  angle = constrain(angle, 0, 100);
  shovelAngle = angle;
  // 反转映射：前端0~100 → 舵机角度135°~45°
  int realAngle = map(angle, 0, 100, 135, 45);
  realAngle = constrain(realAngle, 45, 135);
  // 10位分辨率，占空比 0.5ms~2.5ms 对应 0~180°
  int duty = map(realAngle, 0, 180, 26, 123);
  ledcWrite(SERVO_PIN, duty);
}

void setLightBrightness(int brightness) {
  brightness = constrain(brightness, 0, 255);
  lightBrightness = brightness;
  lightOn = brightness > 0;
}

// =================== 电机控制（单PWM调速） ===================
void setMotor(int left, int right) {
  left = constrain(left, -255, 255);
  right = constrain(right, -255, 255);

  // 单PWM调速：IN1/IN3 控制方向，IN2/IN4 控制速度
  // 注意：当方向引脚为 HIGH 时，PWM=255 对应刹车，PWM=0 对应全速，所以反转时需要反相PWM
  if (left > 0) {
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    ledcWrite(MOTOR_LEFT_IN2, left);
  } else if (left < 0) {
    digitalWrite(MOTOR_LEFT_IN1, HIGH);
    ledcWrite(MOTOR_LEFT_IN2, 255 + left);  // left为负数，如-255→0，-1→254
  } else {
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    ledcWrite(MOTOR_LEFT_IN2, 0);
  }

  if (right > 0) {
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    ledcWrite(MOTOR_RIGHT_IN4, right);
  } else if (right < 0) {
    digitalWrite(MOTOR_RIGHT_IN3, HIGH);
    ledcWrite(MOTOR_RIGHT_IN4, 255 + right);  // right为负数，如-255→0，-1→254
  } else {
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    ledcWrite(MOTOR_RIGHT_IN4, 0);
  }

  if (left != 0 || right != 0) {
    lastMoveTime = millis();
    isMovingFlag = true;
  } else {
    isMovingFlag = false;
  }
}

// =================== WebSocket 事件处理 ===================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      // 新客户端连接时同步当前摄像头状态
      webSocket.sendTXT(num, String("CAM,") + (cameraEnabled ? "1" : "0"));
      break;
    }
    case WStype_TEXT: {
      String msg = String((char*)payload);
      if (msg.startsWith("M,")) {
        int c1 = msg.indexOf(',');
        int c2 = msg.indexOf(',', c1 + 1);
        if (c2 != -1) {
          int left = msg.substring(c1 + 1, c2).toInt();
          int right = msg.substring(c2 + 1).toInt();
          setMotor(left, right);
        }
      }
      else if (msg.startsWith("S,")) {
        int angle = msg.substring(2).toInt();
        setShovel(angle);
      }
      else if (msg == "L") {
        // 切换开关
        if (lightOn) {
          setLightBrightness(0);
        } else {
          setLightBrightness(lightBrightness > 0 ? lightBrightness : 128);
        }
        webSocket.broadcastTXT(String("LIGHT,") + lightBrightness);
      }
      else if (msg.startsWith("L,")) {
        int brightness = msg.substring(2).toInt();
        setLightBrightness(brightness);
        webSocket.broadcastTXT(String("LIGHT,") + lightBrightness);
      }
      else if (msg.startsWith("Q,")) {
        String mode = msg.substring(2);
        if (mode == "AUTO") {
          autoQuality = true;
        } else {
          autoQuality = false;
          if (mode == "QVGA") manualFrameSize = FRAMESIZE_QVGA;
          else if (mode == "VGA") manualFrameSize = FRAMESIZE_VGA;
          else if (mode == "SVGA") manualFrameSize = FRAMESIZE_SVGA;
          if (!isMovingFlag) setFrameSize(manualFrameSize);
        }
        String resp = String("QUALITY,") + mode;
        webSocket.broadcastTXT(resp);
      }
      else if (msg.startsWith("C,")) {
        int en = msg.substring(2).toInt();
        if (en == 1 && !cameraEnabled) {
          gpio_uninstall_isr_service();
          initCamera();
          cameraEnabled = true;
        } else if (en == 0 && cameraEnabled) {
          esp_camera_deinit();
          cameraEnabled = false;
        }
        webSocket.broadcastTXT(String("CAM,") + (cameraEnabled ? "1" : "0"));
      }
      break;
    }
    default: break;
  }
}

// =================== HTTP 兼容端点 ===================
void handleLight() {
  if (lightOn) {
    setLightBrightness(0);
  } else {
    setLightBrightness(128);
  }
  server.send(200, "text/plain", String(lightBrightness));
}

void handleMotor() {
  if (server.hasArg("left") && server.hasArg("right")) {
    int l = constrain(server.arg("left").toInt(), -255, 255);
    int r = constrain(server.arg("right").toInt(), -255, 255);
    setMotor(l, r);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleShovel() {
  if (server.hasArg("angle")) {
    int angle = constrain(server.arg("angle").toInt(), 0, 100);
    setShovel(angle);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}


// =================== 根页面（内嵌 Web 控制逻辑） ===================
void handleRoot() {
  if (isSleeping) wakeUp();
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>坦克遥控</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;touch-action:none;-webkit-user-select:none;user-select:none}
body{background:#0a0a0a;color:#fff;font-family:sans-serif;height:100vh;overflow:hidden;display:flex;flex-direction:column}
#top{height:40px;display:flex;align-items:center;justify-content:center;gap:8px;background:#1a1a1a;font-size:13px;border-bottom:1px solid #333}
#top button{padding:4px 10px;border:1px solid #555;background:#222;color:#fff;border-radius:4px;font-size:12px;cursor:pointer}
#top button.active{background:#0a6;border-color:#0c7;color:#000}
#main{flex:1;display:flex;min-height:0}
.side{width:110px;display:flex;flex-direction:column;align-items:center;justify-content:space-between;padding:6px 2px}
#videoWrap{flex:1;display:flex;align-items:center;justify-content:center;background:#000;position:relative}
#video{width:100%;height:100%;object-fit:contain}
.trackBtn{width:76px;height:52px;border:1px solid #444;background:#222;color:#fff;border-radius:8px;font-size:22px;line-height:52px;text-align:center;cursor:pointer;touch-action:manipulation}
.trackBtn:active{background:#444}
#joyWrap{width:100px;height:100px;border:2px solid rgba(255,255,255,0.25);border-radius:50%;position:relative;touch-action:none}
#joyKnob{width:40px;height:40px;background:rgba(255,255,255,0.5);border-radius:50%;position:absolute;left:30px;top:30px;touch-action:none}
#shovelWrap{width:50px;height:140px;border:2px solid rgba(255,255,255,0.25);border-radius:8px;position:relative;background:#151515}
#shovelThumb{width:44px;height:28px;background:rgba(255,255,255,0.5);border-radius:6px;position:absolute;left:1px;bottom:1px;touch-action:none}
#lightBtn{width:56px;height:56px;border-radius:50%;border:2px solid #aa0;background:rgba(170,170,0,0.25);font-size:22px;color:#fff;text-align:center;line-height:52px;cursor:pointer}
#lightBtn.on{background:rgba(255,255,0,0.6);border-color:#ff0}
#lightWrap{display:flex;flex-direction:column;align-items:center;gap:4px}
#lightSlider{width:60px;height:6px;-webkit-appearance:none;appearance:none;background:#333;border-radius:3px;outline:none}
#lightSlider::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:50%;background:#fff;cursor:pointer}
#hint{position:absolute;bottom:4px;left:50%;transform:translateX(-50%);font-size:11px;color:#888;pointer-events:none}
</style>
</head>
<body>
<div id="top">
<span>画质:</span>
<button id="qAuto" class="active" onclick="setQ('AUTO')">自动</button>
<button id="qQVGA" onclick="setQ('QVGA')">240P</button>
<button id="qVGA" onclick="setQ('VGA')">480P</button>
<button id="qSVGA" onclick="setQ('SVGA')">高清</button>
<span id="qLabel">自动</span>
<button id="camBtn" class="active" onclick="toggleCam()">📷</button>
</div>
<div id="main">
<div class="side">
<div class="trackBtn" id="blU">▲</div>
<div id="joyWrap"><div id="joyKnob"></div></div>
<div class="trackBtn" id="blD">▼</div>
</div>
<div id="videoWrap">
<img id="video" alt="视频流">
<div id="hint">WSAD移动 ↑↓铲子 空格灯</div>
</div>
<div class="side">
<div class="trackBtn" id="brU">▲</div>
<div id="shovelWrap"><div id="shovelThumb"></div></div>
<div id="lightWrap"><div id="lightBtn">💡</div><input type="range" id="lightSlider" min="0" max="255" value="128"></div>
<div class="trackBtn" id="brD">▼</div>
</div>
</div>
<script>
const video=document.getElementById('video');
const STREAM_URL='http://'+location.hostname+':81/stream';
video.src=STREAM_URL;
video.onerror=()=>{
  console.log('视频流断开，3秒后重试');
  setTimeout(()=>{if(camEnabled){video.src='';video.src=STREAM_URL;}},3000);
};

let ws;
function connectWS(){
  ws=new WebSocket('ws://'+location.hostname+':82');
  ws.onopen=()=>{
    console.log('WS连接');
    if(camEnabled){video.src='';video.src=STREAM_URL;}
  };
  ws.onclose=()=>{console.log('WS断开，3秒重连');setTimeout(connectWS,3000);};
  ws.onmessage=(e)=>{
    const d=e.data;
    if(d.startsWith('LIGHT,')){document.getElementById('lightBtn').classList.toggle('on',d[6]=='1');}
    else if(d.startsWith('QUALITY,')){document.getElementById('qLabel').textContent=d.slice(8);}
    else if(d.startsWith('CAM,')){camEnabled=d[4]=='1';updateCamUI();}
  };
}
connectWS();
function wsSend(s){if(ws&&ws.readyState==1)ws.send(s);}

// === 状态机 ===
let motorL=0,motorR=0,shovelVal=50;
let keys={};
let lastSent='';
let lastInputSrc=null;

function applySteeringCurve(dx,dy){
  const d=Math.hypot(dx,dy);
  if(d<0.001)return{dx:0,dy:0};
  const deg=Math.atan2(Math.abs(dx),dy)*180/Math.PI;
  let mappedDeg;
  if(deg<=70){mappedDeg=deg*45/70;}
  else{mappedDeg=45+(deg-70)*45/20;}
  const sign=dx>=0?1:-1;
  return{
    dx:sign*d*Math.sin(mappedDeg*Math.PI/180),
    dy:d*Math.cos(mappedDeg*Math.PI/180)
  };
}

function sendState(){
  if(lastInputSrc===null && (keys['w']||keys['a']||keys['s']||keys['d'])){
    updateKeyMotor();
  }
  const s=`M,${motorR},${motorL}|S,${shovelVal}`;
  if(s!==lastSent){lastSent=s;wsSend(`M,${motorR},${motorL}`);wsSend(`S,${shovelVal}`);}
}
setInterval(sendState,50);

// === 摇杆 ===
const joyWrap=document.getElementById('joyWrap');
const joyKnob=document.getElementById('joyKnob');
let joyId=null;
function joyReset(){joyKnob.style.left='30px';joyKnob.style.top='30px';if(lastInputSrc==='joy'){motorL=0;motorR=0;lastInputSrc=null;}}
function joyMove(cx,cy){
  const rect=joyWrap.getBoundingClientRect();
  let x=cx-rect.left-50,y=cy-rect.top-50;
  const dist=Math.hypot(x,y),max=35;
  if(dist>max){x=x/dist*max;y=y/dist*max;}
  joyKnob.style.left=(30+x)+'px';joyKnob.style.top=(30+y)+'px';
  const rawDx=x/max,rawDy=-y/max;
  const curved=applySteeringCurve(rawDx,rawDy);
  let left=Math.round((curved.dy+curved.dx)*255),right=Math.round((curved.dy-curved.dx)*255);
  left=Math.max(-255,Math.min(255,left));
  right=Math.max(-255,Math.min(255,right));
  motorL=left;motorR=right;
  lastInputSrc='joy';
}
joyWrap.addEventListener('touchstart',e=>{e.preventDefault();const t=e.changedTouches[0];if(joyId==null)joyId=t.identifier;joyMove(t.clientX,t.clientY);}, {passive:false});
joyWrap.addEventListener('touchmove',e=>{e.preventDefault();for(let t of e.changedTouches){if(t.identifier==joyId)joyMove(t.clientX,t.clientY);}}, {passive:false});
joyWrap.addEventListener('touchend',e=>{for(let t of e.changedTouches){if(t.identifier==joyId){joyId=null;joyReset();}}}, {passive:false});
joyWrap.addEventListener('mousedown',e=>{joyId='mouse';joyMove(e.clientX,e.clientY);});
window.addEventListener('mousemove',e=>{if(joyId=='mouse')joyMove(e.clientX,e.clientY);});
window.addEventListener('mouseup',()=>{if(joyId=='mouse'){joyId=null;joyReset();}});

// === 铲子滑杆 ===
const sw=document.getElementById('shovelWrap');
const st=document.getElementById('shovelThumb');
st.style.bottom=Math.round(shovelVal/100*112)+'px';
let shovelId=null;
function shovelMove(cy){
  const rect=sw.getBoundingClientRect();
  let y=rect.bottom-cy-14;
  y=Math.max(0,Math.min(y,112));
  st.style.bottom=y+'px';
  shovelVal=Math.round(y/112*100);
}
function shovelReset(){}
sw.addEventListener('touchstart',e=>{e.preventDefault();const t=e.changedTouches[0];if(shovelId==null)shovelId=t.identifier;shovelMove(t.clientY);}, {passive:false});
sw.addEventListener('touchmove',e=>{e.preventDefault();for(let t of e.changedTouches){if(t.identifier==shovelId)shovelMove(t.clientY);}}, {passive:false});
sw.addEventListener('touchend',e=>{for(let t of e.changedTouches){if(t.identifier==shovelId)shovelId=null;}}, {passive:false});
sw.addEventListener('mousedown',e=>{shovelId='mouse';shovelMove(e.clientY);});
window.addEventListener('mousemove',e=>{if(shovelId=='mouse')shovelMove(e.clientY);});
window.addEventListener('mouseup',()=>{if(shovelId=='mouse')shovelId=null;});

// === 单侧履带按钮 ===
function btnSetup(id,side,dir){
  const el=document.getElementById(id);
  let val=0,active=false;
  function set(v){val=v;if(side=='L')motorL=val;else motorR=val;}
  function start(){if(!active){active=true;set(dir*255);lastInputSrc='btn';}}
  function end(){if(active){active=false;set(0);if(lastInputSrc==='btn')lastInputSrc=null;}}
  el.addEventListener('touchstart',e=>{e.preventDefault();start();},{passive:false});
  el.addEventListener('touchend',e=>{e.preventDefault();end();},{passive:false});
  el.addEventListener('touchcancel',e=>{end();});
  el.addEventListener('mousedown',()=>start());
  el.addEventListener('mouseup',()=>end());
  el.addEventListener('mouseleave',()=>end());
}
btnSetup('blU','L',1);btnSetup('blD','L',-1);
btnSetup('brU','R',1);btnSetup('brD','R',-1);

// === 灯光 ===
const lightBtn=document.getElementById('lightBtn');
const lightSlider=document.getElementById('lightSlider');
let lightOn=false;
let lightBrightness=128;
function sendLight(){
  wsSend('L,'+(lightOn?lightBrightness:0));
}
function updateLightUI(){
  lightBtn.classList.toggle('on',lightOn);
  lightSlider.value=lightBrightness;
}
lightBtn.addEventListener('click',()=>{lightOn=!lightOn;updateLightUI();sendLight();});
lightBtn.addEventListener('touchstart',e=>{e.preventDefault();lightOn=!lightOn;updateLightUI();sendLight();}, {passive:false});
lightSlider.addEventListener('input',e=>{lightBrightness=parseInt(e.target.value);if(lightOn)sendLight();});

// === 画质切换 ===
function setQ(mode){
  wsSend('Q,'+mode);
  ['Auto','QVGA','VGA','SVGA'].forEach(m=>{
    document.getElementById('q'+m).classList.toggle('active',m==mode);
  });
  document.getElementById('qLabel').textContent=mode;
}
let camEnabled=true;
function toggleCam(){
  camEnabled=!camEnabled;
  wsSend('C,'+(camEnabled?'1':'0'));
  updateCamUI();
}
function updateCamUI(){
  document.getElementById('camBtn').classList.toggle('active',camEnabled);
  const v=document.getElementById('video');
  v.style.display=camEnabled?'':'none';
  v.src=camEnabled?('http://'+location.hostname+':81/stream'):'';
}

// === 键盘 ===
window.addEventListener('keydown',e=>{
  if(e.repeat)return;
  const k=e.key.toLowerCase();
  keys[k]=true;
  if(['w','a','s','d','arrowup','arrowdown',' '].includes(k))e.preventDefault();
  if(k==' ')wsSend('L');
  updateKeyMotor();
});
window.addEventListener('keyup',e=>{
  keys[e.key.toLowerCase()]=false;
  updateKeyMotor();
});
function updateKeyMotor(){
  const w=keys['w'],s=keys['s'],a=keys['a'],d=keys['d'];
  let dy=(w?1:0)-(s?1:0),dx=(d?1:0)-(a?1:0);
  if(keys['arrowup']){shovelVal=Math.min(100,shovelVal+3);st.style.bottom=Math.round(shovelVal/100*112)+'px';}
  if(keys['arrowdown']){shovelVal=Math.max(0,shovelVal-3);st.style.bottom=Math.round(shovelVal/100*112)+'px';}
  motorL=Math.round((dy+dx)*255);
  motorR=Math.round((dy-dx)*255);
  motorL=Math.max(-255,Math.min(255,motorL));
  motorR=Math.max(-255,Math.min(255,motorR));
  if(motorL!==0||motorR!==0||keys['arrowup']||keys['arrowdown']){
    lastInputSrc='key';
  } else if(lastInputSrc==='key'){
    lastInputSrc=null;
  }
}

// === 手柄 (Gamepad API) ===
let padConnected=false;
let padIndex=-1;
window.addEventListener('gamepadconnected',e=>{
  padConnected=true;
  padIndex=e.gamepad.index;
  console.log('手柄连接 index='+padIndex);
});
window.addEventListener('gamepaddisconnected',e=>{
  padConnected=false;
  padIndex=-1;
});
function pollPad(){
  try{
    let bestPad=null,bestAct=0;
    for(const g of navigator.getGamepads()){
      if(!g)continue;
      let act=0;
      for(let i=0;i<Math.min(4,g.axes.length);i++)act+=Math.abs(g.axes[i]||0);
      for(const b of g.buttons||[])if(b&&b.pressed)act+=1;
      if(act>bestAct){bestAct=act;bestPad=g;}
    }
    if(!bestPad||bestAct<0.01){requestAnimationFrame(pollPad);return;}
    const axes=bestPad.axes||[];
    const buttons=bestPad.buttons||[];
    function dz(v){if(v==null)return 0;const a=Math.abs(v);return a<0.15?0:(a-0.15)/0.85*Math.sign(v);}
    const lx=dz(axes[0]),ly=dz(axes[1]);
    const ry=dz(axes[3]);
    const curved=applySteeringCurve(lx,-ly);
    let left=Math.round((curved.dy+curved.dx)*255),right=Math.round((curved.dy-curved.dx)*255);
    left=Math.max(-255,Math.min(255,left));
    right=Math.max(-255,Math.min(255,right));
    const anyBtn=buttons.length?buttons.some(b=>b&&b.pressed):false;
    const hasInput=Math.abs(lx)>0.01||Math.abs(ly)>0.01||Math.abs(ry)>0.01||anyBtn;
    if(hasInput&&joyId==null&&shovelId==null){
      motorL=left;motorR=right;
      if(Math.abs(ry)>0.01){shovelVal=Math.max(0,Math.min(100,shovelVal-Math.round(ry*3)));st.style.bottom=Math.round(shovelVal/100*112)+'px';}
      lastInputSrc='pad';
    }else if(lastInputSrc==='pad'){
      if(joyId==null&&shovelId==null){motorL=0;motorR=0;}
      lastInputSrc=null;
    }
    let b0=buttons[0]&&buttons[0].pressed;
    if(b0&&!window.padBtn0Prev)wsSend('L');
    window.padBtn0Prev=b0;
  }catch(err){console.error('pollPad err',err);}
  requestAnimationFrame(pollPad);
}
pollPad();
</script>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", html);
}

// =================== 视频流任务 ===================
void streamTask(void *pvParameters) {
  while (1) {
    WiFiClient client = streamServer.available();
    if (client) {
      if (!cameraEnabled) {
        client.stop();
        delay(10);
        continue;
      }
      streamClientCount++;
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
      client.println();
      while (client.connected()) {
        if (!cameraEnabled) break;
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
          delay(10);
          continue;
        }
        if (!client.connected()) {
          esp_camera_fb_return(fb);
          break;
        }
        client.print("--frame\r\n");
        client.print("Content-Type: image/jpeg\r\n");
        client.print("Content-Length: ");
        client.print(fb->len);
        client.print("\r\n\r\n");
        client.write(fb->buf, fb->len);
        client.print("\r\n");
        esp_camera_fb_return(fb);
        delay(5);
      }
      client.stop();
      streamClientCount--;
    }
    delay(10);
  }
}

// =================== 主程序 ===================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 遥控坦克启动");

  initMotors();
  initLightPWM();
  initServo();
  initCamera();
  Serial.println("硬件初始化完毕");

  WiFi.begin(ssid, password);
  Serial.print("连接Wi-Fi");
  unsigned long wifiStartTime = millis();
  bool wifiTimeout = false;

  while (WiFi.status() != WL_CONNECTED) {
    if (!wifiTimeout && millis() - wifiStartTime > 30000) {
      wifiTimeout = true;
      Serial.println("\nWiFi 连接超时");
    }
    if (wifiTimeout) {
      setLightBrightness(8);   // 超时后微亮常亮
    } else {
      setLightBrightness(10);  // 连接中极暗闪烁
      delay(150);
      setLightBrightness(0);
      delay(650);
    }
    Serial.print(".");
  }
  Serial.println("\nWiFi 连接成功");
  Serial.print("IP 地址: ");
  Serial.println(WiFi.localIP());
  setLightBrightness(0);  // 连接成功后关灯

  server.on("/", handleRoot);
  server.on("/motor", handleMotor);
  server.on("/light", handleLight);
  server.on("/servo", handleShovel);
  server.begin();

  streamServer.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  xTaskCreatePinnedToCore(streamTask, "StreamTask", 8192, NULL, 1, &streamTaskHandle, 1);
}

void loop() {
  server.handleClient();
  webSocket.loop();
  if (!isSleeping) {
    checkAutoQuality();
    checkSleep();
    delay(10);
  } else {
    if (webSocket.connectedClients() > 0 || streamClientCount > 0) {
      wakeUp();
    }
    delay(100);
  }
}
