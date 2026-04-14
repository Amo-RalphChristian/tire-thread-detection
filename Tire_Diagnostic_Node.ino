#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "esp_camera.h"
#include <Wire.h>
#include "Adafruit_VL53L0X.h"

// TensorFlow Lite Micro Libraries
#include <TensorFlowLite_ESP32.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Your downloaded AI Model
#include "tire_model_data.h"

// --- HARDWARE PIN DEFINITIONS (ESP32-S3 CAM) ---
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// I2C Pins for ToF Laser Sensor
#define I2C_SDA 40
#define I2C_SCL 39

// --- GLOBALS ---
AsyncWebServer server(80);
Adafruit_VL53L0X lox = Adafruit_VL53L0X();

// TensorFlow Globals
tflite::ErrorReporter* error_reporter = nullptr;
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;

// Allocate RAM for the AI (2MB is plenty)
constexpr int kTensorArenaSize = 2000 * 1024; 
uint8_t* tensor_arena = nullptr;

void setup() {
  Serial.begin(115200);
  tflite::InitializeTarget();

  // --- PSRAM ALLOCATION FOR AI ---
  if (psramInit()) {
    Serial.println("PSRAM found and initialized.");
    tensor_arena = (uint8_t*)ps_malloc(kTensorArenaSize);
    if (tensor_arena == nullptr) {
      Serial.println("Fatal Error: Could not allocate PSRAM for AI!");
      return;
    }
  } else {
    Serial.println("Fatal Error: PSRAM not found! Check your Arduino Tools menu.");
    return;
  }
  
  // 1. Initialize LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed");
    return;
  }

  // 2. Initialize Wi-Fi
  WiFi.softAP("Tire_Diagnostic_Node", "12345678"); 
  Serial.print("AP Started. IP: ");
  Serial.println(WiFi.softAPIP());

  // 3. Initialize ToF Sensor
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X"));
  }

  // 4. Initialize Camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_240X240; 
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed!");
    return;
  }

  // 5. Initialize AI Model
  static tflite::MicroErrorReporter micro_error_reporter;
  error_reporter = &micro_error_reporter;

  model = tflite::GetModel(_content_tire_model_tflite);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema mismatch!");
    return;
  }
  
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
  interpreter = &static_interpreter;
  
  interpreter->AllocateTensors();
  input = interpreter->input(0);
  output = interpreter->output(0);

  // 6. Web Server Routing
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/style.css", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/script.js", "text/javascript");
  });

  // 7. Diagnostic Logic
  server.on("/run-diagnostic", HTTP_GET, [](AsyncWebServerRequest *request){
    VL53L0X_RangingMeasurementData_t measure;
    lox.rangingTest(&measure, false);
    float depth_mm = (measure.RangeStatus != 4) ? measure.RangeMilliMeter : 0.0;

    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      request->send(500, "text/plain", "Camera Failed");
      return;
    }
    
    // AI Invoke
    if (interpreter->Invoke() != kTfLiteOk) {
      request->send(500, "text/plain", "AI Failed");
      esp_camera_fb_return(fb);
      return;
    }

    float defective_prob = output->data.f[0];
    float healthy_prob = output->data.f[1];
    
    String ai_class = (healthy_prob > defective_prob) ? "HEALTHY" : "DEFECTIVE";
    float confidence = max(healthy_prob, defective_prob) * 100.0;

    String sys_status = "safe";
    if (ai_class == "DEFECTIVE" || depth_mm < 1.6) {
      sys_status = "critical";
    } else if (depth_mm < 3.0) {
      sys_status = "warning";
    }

    String json = "{";
    json += "\"ai_class\":\"" + ai_class + "\",";
    json += "\"confidence\":\"" + String(confidence, 1) + "\",";
    json += "\"tof_mm\":\"" + String(depth_mm, 1) + "\",";
    json += "\"system_status\":\"" + sys_status + "\"";
    json += "}";

    request->send(200, "application/json", json);
    esp_camera_fb_return(fb); 
  });

  server.begin();
  Serial.println("Server listening...");
}

void loop() {
  delay(1000);
}