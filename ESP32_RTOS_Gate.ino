#include <WiFi.h>
#include <Wire.h>
#include <OV7670.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>

// 👇 记得换成你自己的 WiFi 和 IP
const char* ssid = "你的WiFi名字";
const char* password = "你的WiFi密码";
const char* serverUrl = "http://你的电脑IP:5000/api/recognize"; 

#define MODE   QQVGA
#define COLOR  RGB565

const camera_config_t cam_conf = {
  .D0 = 23,  .D1 = 34,  .D2 = 35,  .D3 = 32,
  .D4 = 33,  .D5 = 25,  .D6 = 26,  .D7 = 27,
  .XCLK = 4, .PCLK = 5, .VSYNC = 13,
  .xclk_freq_hz = 8'000'000,
  .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0
};

OV7670 cam;
uint16_t w = 160, h = 120;
Servo myServo;
const int servoPin = 15; 

// 🔥 核心：定义 FreeRTOS 的队列（用于在两个任务之间传递图像数据指针）
QueueHandle_t frameQueue;

// ================= 任务1：图像采集任务 =================
void Task_Camera(void *pvParameters) {
  Serial.println("Task_Camera 运行在核心: " + String(xPortGetCoreID()));
  
  uint16_t* frameBuf = (uint16_t*)malloc(w * h * 2); // 动态分配内存
  
  while(1) {
    // 采集一帧图像
    for (uint16_t y = 0; y < h; y++) {
      uint16_t* line = cam.getLine(y + 1);
      memcpy(frameBuf + y*w, line, w*2);
    }
    
    // 将图像缓冲区的指针发送到队列中（如果队列满了就丢弃本帧）
    if (xQueueSend(frameQueue, &frameBuf, 0) != pdTRUE) {
      Serial.println("⚠️ 队列已满，丢弃当前帧");
    }
    
    // 模拟车辆检测，每5秒抓拍一次
    vTaskDelay(pdMS_TO_TICKS(5000)); 
  }
}

// ================= 任务2：云端通信与执行任务 =================
void Task_Network(void *pvParameters) {
  Serial.println("Task_Network 运行在核心: " + String(xPortGetCoreID()));
  
  uint16_t* receivedBuf = nullptr;
  
  while(1) {
    // 阻塞等待队列中的图像数据指针（等待时间不限）
    if (xQueueReceive(frameQueue, &receivedBuf, portMAX_DELAY) == pdTRUE) {
      Serial.println("📸 收到一帧图像，准备发送至服务器...");
      
      HTTPClient http;
      http.begin(serverUrl);
      http.addHeader("Content-Type", "application/octet-stream");
      
      // 只发 2000 字节作为演示
      int httpResponseCode = http.POST((uint8_t*)receivedBuf, 2000); 
      
      if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("收到服务器回复: " + response);
        
        if (response.indexOf("200") > 0) {
          Serial.println("🚦 识别成功！道闸开启！");
          myServo.write(90);  
          vTaskDelay(pdMS_TO_TICKS(3000)); // 非阻塞延时
          myServo.write(0);   
          Serial.println("道闸关闭。");
        }
      } else {
        Serial.println("❌ 请求失败，错误码: " + String(httpResponseCode));
      }
      http.end();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  myServo.attach(servoPin);
  myServo.write(0); 

  Serial.print("正在连接WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi连接成功!");

  Wire.begin(21, 22);   
  Wire.setClock(400000);
  if (cam.init(&cam_conf, MODE, COLOR) != ESP_OK) {
    Serial.println("❌ 摄像头初始化失败!");
    // 注意：为了没硬件也能跑通框架，这里不写 while(true) 卡死
  }

  // 🔥 创建队列，最多存储 2 个指针
  frameQueue = xQueueCreate(2, sizeof(uint16_t*));

  // 🔥 核心：创建 FreeRTOS 任务，并绑定到不同核心
  // ESP32 是双核的，核心 0 跑网络（WiFi底层也在这），核心 1 跑图像采集
  xTaskCreatePinnedToCore(Task_Camera, "Camera Task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_Network, "Network Task", 8192, NULL, 1, NULL, 0);
}

void loop() {
  // 留空，因为任务都在 FreeRTOS 里独立运行
  vTaskDelay(pdMS_TO_TICKS(1000));
}