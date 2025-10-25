#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ====== WiFi Credentials ======
const char* ssid = "Traffic";
const char* password = "trafic123";

// ====== Telegram Credentials ======
#define BOTtoken "8248678690:AAHeDBU6BqSYqDH0Mu6FXTlmnhXw-wn2euA"
#define CHAT_ID  "7637859403"

// ====== Camera Pin Config for AI Thinker ======
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

// ====== Button Pin ======
#define BUTTON_PIN 12  // Connect button between GPIO12 and GND

// ====== Global Variables ======
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

int botRequestDelay = 1000;
unsigned long lastTimeBotRan = 0;
camera_fb_t * fb = NULL;
size_t fbIndex = 0;
bool buttonPressed = false;

// ====== Forward Declarations ======
bool moreDataAvailable();
byte getNextByte();
void sendPhotoTelegram();
void handleNewMessages(int numNewMessages);

// ====== Camera Setup ======
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector

  Serial.begin(115200);
  Serial.println("\nInitializing...");

  pinMode(BUTTON_PIN, INPUT_PULLUP); // Button to GND

  // Camera configuration
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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;

  // Initialize camera
  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed!");
    while (true);
  }

  // Connect WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());

  client.setInsecure();
  bot.sendMessage(CHAT_ID, "✅ ESP32-CAM online! Send /photo or press the button to capture a picture.");
}

// ====== Camera Data Functions ======
bool moreDataAvailable() {
  return (fbIndex < fb->len);
}

byte getNextByte() {
  return fb->buf[fbIndex++];
}

// ====== Send Photo to Telegram ======
void sendPhotoTelegram() {
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed!");
    bot.sendMessage(CHAT_ID, "❌ Camera capture failed!");
    return;
  }

  fbIndex = 0;
  Serial.println("Sending photo to Telegram...");

  bot.sendPhotoByBinary(
    CHAT_ID,
    "image/jpeg",
    fb->len,
    moreDataAvailable,
    getNextByte,
    nullptr,
    nullptr
  );

  esp_camera_fb_return(fb);
  Serial.println("Photo sent!");
}

// ====== Handle Telegram Commands ======
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;

    Serial.println("Received message: " + text);

    if (text == "/start") {
      String welcome = "Hello 👋\n";
      welcome += "Send /photo to take a picture.\n";
      welcome += "Or press the physical button to capture one.";
      bot.sendMessage(chat_id, welcome, "");
    }

    if (text == "/photo") {
      bot.sendMessage(chat_id, "📸 Capturing photo...");
      sendPhotoTelegram();
    }
  }
}

// ====== Loop ======
void loop() {
  // Check Telegram messages periodically
  if (millis() > lastTimeBotRan + botRequestDelay) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }

  // Check button press
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // Debounce
    if (!buttonPressed) {
      buttonPressed = true;
      Serial.println("Button pressed! Capturing photo...");
      bot.sendMessage(CHAT_ID, "📸 Button pressed! Capturing photo...");
      sendPhotoTelegram();
    }
  } else {
    buttonPressed = false;
  }
}
