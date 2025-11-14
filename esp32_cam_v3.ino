#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <ArduinoJson.h>
#include <Base64.h>

// WiFi credentials
const char* ssid = "Tuan Anh";
const char* password = "tuananh1309@";

// Server configuration
const char* ws_host = "10.75.122.249";
const uint16_t ws_port = 4000;
const char* ws_path = "/";

// Camera pins for AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WebSocketsClient wsClient;
bool wsConnected = false;

// Flash LED pin
#define FLASH_LED_PIN 4

// Streaming control
bool streamingEnabled = false;
unsigned long lastStreamFrame = 0;
const unsigned long STREAM_INTERVAL = 100; // 10 FPS

void setup() {
  Serial.begin(9600);
  Serial.println("ESP32-CAM License Plate Recognition Starting...");
  
  // Disable brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  // Setup flash LED
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  
  // Initialize camera
  if (!initCamera()) {
    Serial.println("Camera init failed!");
    ESP.restart();
  }
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  // Initialize WebSocket
  initWebSocket();
  
  Serial.println("ESP32-CAM ready!");
}

bool initCamera() {
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Init with high specs for quality
  if(psramFound()){
    config.frame_size = FRAMESIZE_SVGA; // 800x600 for better streaming
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA; // 640x480
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }
  
  // Sensor settings for better license plate capture
  sensor_t * s = esp_camera_sensor_get();
  
  // Tăng contrast và sharpness cho chữ rõ hơn
  s->set_brightness(s, 0);     // -2 to 2
  s->set_contrast(s, 2);       // -2 to 2 - TĂNG CONTRAST
  s->set_saturation(s, 0);     // -2 to 2
  s->set_sharpness(s, 2);      // -2 to 2 - TĂNG SHARPNESS
  s->set_denoise(s, 1);        // 0 to 8 - Giảm noise
  
  // Auto white balance
  s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
  s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
  s->set_wb_mode(s, 0);        // 0 to 4 - Auto
  
  // Auto exposure
  s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
  s->set_aec2(s, 1);           // 0 = disable , 1 = enable
  s->set_ae_level(s, 0);       // -2 to 2
  s->set_aec_value(s, 300);    // 0 to 1200
  
  // Auto gain
  s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
  s->set_agc_gain(s, 0);       // 0 to 30
  s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
  
  // Other settings
  s->set_bpc(s, 0);            // 0 = disable , 1 = enable
  s->set_wpc(s, 1);            // 0 = disable , 1 = enable
  s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable
  s->set_lenc(s, 1);           // 0 = disable , 1 = enable
  s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
  s->set_vflip(s, 0);          // 0 = disable , 1 = enable
  s->set_dcw(s, 1);            // 0 = disable , 1 = enable
  s->set_colorbar(s, 0);       // 0 = disable , 1 = enable
  
  Serial.println("✅ Camera initialized with OCR-optimized settings");
  
  return true;
}

void initWebSocket() {
  wsClient.begin(ws_host, ws_port, ws_path);
  wsClient.onEvent(handleWebSocketEvent);
  wsClient.setReconnectInterval(5000);
  wsClient.enableHeartbeat(15000, 3000, 2);
}

void handleWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("WebSocket disconnected");
      wsConnected = false;
      streamingEnabled = false;
      break;
      
    case WStype_CONNECTED:
      Serial.println("WebSocket connected");
      wsConnected = true;
      
      // Send identification
      {
        DynamicJsonDocument doc(256);
        doc["type"] = "camera_connect";
        doc["deviceId"] = "ESP32_CAM_PARKING";
        doc["capabilities"] = "license_plate_recognition,streaming";
        
        String msg;
        serializeJson(doc, msg);
        wsClient.sendTXT(msg);
      }
      break;
      
    case WStype_TEXT:
      {
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, (char*)payload);
        
        if (!error) {
          String type = doc["type"];
          
          if (type == "capture_request") {
            String action = doc["data"]["action"];
            String rfidCode = doc["data"]["rfidCode"];
            String gateType = doc["data"]["gateType"];
            
            Serial.printf("Capture request: %s for RFID %s\n", 
                         action.c_str(), rfidCode.c_str());
            
            captureAndSendImage(rfidCode, action, gateType);
          }
          else if (type == "start_stream") {
            Serial.println("Starting video stream");
            streamingEnabled = true;
          }
          else if (type == "stop_stream") {
            Serial.println("Stopping video stream");
            streamingEnabled = false;
          }
        }
      }
      break;
      
    default:
      break;
  }
}

void captureAndSendImage(String rfidCode, String action, String gateType) {
  Serial.println("=== CAPTURE START ===");
  Serial.print("RFID: ");
  Serial.println(rfidCode);
  Serial.print("Action: ");
  Serial.println(action);
  
  const int MAX_RETRIES = 3;
  const int NUM_IMAGES = 6; // Capture 6 images
  camera_fb_t * fb = NULL;
  
  // Turn on flash LED
  digitalWrite(FLASH_LED_PIN, HIGH);
  Serial.println("Flash LED ON");
  delay(150); // Stabilize lighting
  
  // Set camera to higher quality for capture
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_UXGA); // 1600x1200
  s->set_quality(s, 10);
  
  // Capture 6 images, only keep the 6th
  for (int image_num = 1; image_num <= NUM_IMAGES; image_num++) {
    Serial.print("Capturing image ");
    Serial.print(image_num);
    Serial.print("/");
    Serial.println(NUM_IMAGES);
    
    // Attempt capture with retries
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
      Serial.print("Attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.println(MAX_RETRIES);
      
      fb = esp_camera_fb_get();
      
      if (fb) {
        Serial.println("Capture successful!");
        // If this is not the 6th image, return the buffer and continue
        if (image_num < NUM_IMAGES) {
          esp_camera_fb_return(fb);
          fb = NULL;
        }
        break; // Success, move to next image
      } else {
        Serial.print("Capture failed on attempt ");
        Serial.println(attempt);
        
        if (attempt < MAX_RETRIES) {
          Serial.println("Retrying...");
          delay(500); // Wait before retry
        }
      }
    }
    
    if (!fb && image_num == NUM_IMAGES) {
      Serial.println("All capture attempts failed for final image!");
      digitalWrite(FLASH_LED_PIN, LOW);
      s->set_framesize(s, FRAMESIZE_SVGA);
      s->set_quality(s, 12);
      sendCaptureError(rfidCode, action, "Camera capture failed after retries");
      return;
    }
    
    delay(50); // Small delay between captures
  }
  
  // Turn off flash
  digitalWrite(FLASH_LED_PIN, LOW);
  Serial.println("Flash LED OFF");
  
  // Restore streaming quality
  s->set_framesize(s, FRAMESIZE_SVGA);
  s->set_quality(s, 12);
  
  // Process the 6th image if captured
  if (fb) {
    Serial.print("Final image captured: ");
    Serial.print(fb->len);
    Serial.println(" bytes");
    
    // Validate image size
    if (fb->len < 1000) {
      Serial.println("Image too small, likely corrupted");
      esp_camera_fb_return(fb);
      sendCaptureError(rfidCode, action, "Image corrupted");
      return;
    }
    
    // Convert to Base64
    Serial.println("Converting to Base64...");
    String base64Image = base64::encode(fb->buf, fb->len);
    Serial.print("Base64 length: ");
    Serial.println(base64Image.length());
    
    // Send to server
    Serial.println("Sending to server...");
    sendImageToServer(rfidCode, action, gateType, base64Image);
    
    // Return frame buffer
    esp_camera_fb_return(fb);
  }
  
  Serial.println("=== CAPTURE END ===");
}

void sendImageToServer(String rfidCode, String action, String gateType, String base64Image) {
  // Split large images into chunks
  const size_t CHUNK_SIZE = 8000; // 8KB chunks
  size_t totalLength = base64Image.length();
  size_t chunks = (totalLength + CHUNK_SIZE - 1) / CHUNK_SIZE;
  
  Serial.printf("Sending image in %d chunks\n", chunks);
  
  for (size_t i = 0; i < chunks; i++) {
    size_t start = i * CHUNK_SIZE;
    size_t end = min(start + CHUNK_SIZE, totalLength);
    String chunk = base64Image.substring(start, end);
    
    DynamicJsonDocument doc(10000);
    doc["type"] = "camera_image";
    doc["data"]["rfidCode"] = rfidCode;
    doc["data"]["action"] = action;
    doc["data"]["gateType"] = gateType;
    doc["data"]["chunk"] = i;
    doc["data"]["totalChunks"] = chunks;
    doc["data"]["imageData"] = chunk;
    
    String msg;
    serializeJson(doc, msg);
    wsClient.sendTXT(msg);

    Serial.print("📦 Sent chunk ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(chunks);
    Serial.print(" (");
    Serial.print((i + 1) * 100 / chunks);
    Serial.println("%)");
    
    delay(50); // Small delay between chunks
  }
  
  Serial.println("Image sent successfully");
}

void sendStreamFrame() {
  if (!streamingEnabled || !wsConnected) return;
  
  unsigned long currentTime = millis();
  if (currentTime - lastStreamFrame < STREAM_INTERVAL) return;
  
  // Capture frame
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Frame capture failed");
    return;
  }
  
  // Convert to Base64
  String base64Frame = base64::encode(fb->buf, fb->len);
  
  // Send via WebSocket
  DynamicJsonDocument doc(base64Frame.length() + 200);
  doc["type"] = "stream_frame";
  doc["data"] = base64Frame;
  doc["timestamp"] = currentTime;
  
  String msg;
  serializeJson(doc, msg);
  wsClient.sendTXT(msg);
  
  esp_camera_fb_return(fb);
  lastStreamFrame = currentTime;
}

void sendCaptureError(String rfidCode, String action, String error) {
  DynamicJsonDocument doc(512);
  doc["type"] = "camera_error";
  doc["data"]["rfidCode"] = rfidCode;
  doc["data"]["action"] = action;
  doc["data"]["error"] = error;
  
  String msg;
  serializeJson(doc, msg);
  wsClient.sendTXT(msg);
}

void loop() {
  wsClient.loop();
  
  // Send stream frames if enabled
  if (streamingEnabled) {
    sendStreamFrame();
  }
  
  delay(10);
}