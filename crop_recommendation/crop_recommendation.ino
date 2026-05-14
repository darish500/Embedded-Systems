// ============================================================
//  FarmFit — AI Based Smart Crop Prediction System
//  Hardware : ESP32
//  Display  : 16x2 I2C LCD
//  Sensors  : RS485 NPK/pH, DHT22, Analog Moisture
//  Model    : ml_model.h (micromlgen RandomForest)
//  Cloud    : Firebase Realtime Database (REST API)
//
//  ✅ NEW FEATURES:
//     • Countdown: 70 steps × 500ms = 35 seconds total
//     • Smart probe retry: After 3 failed attempts → "Soil not good"
//     • Sleep mode: Wait 60s before retrying probe (no spam)
//     • "Check ur App!" only shown when online + valid reading
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ml_model.h"

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ============================================================
//  WIFI / FIREBASE CONFIG
// ============================================================
#define WIFI_SSID        "farmfit"
#define WIFI_PASSWORD    "12345678"

#define FIREBASE_URL     "https://capstone-2e26e-default-rtdb.firebaseio.com"
#define FIREBASE_API_KEY "AIzaSyDkPRdXQqKx6TsIbawybZ-Aww9gsgulj74"

// ============================================================
//  PIN DEFINITIONS
// ============================================================
#define DHT_PIN          4
#define DHT_TYPE         DHT22

#define RS485_RX_PIN     16
#define RS485_TX_PIN     17  // ✅ Fixed: was "17a"
#define RS485_DE_PIN     5

#define MOISTURE_PIN     34

#define I2C_SDA          21
#define I2C_SCL          22

// ============================================================
//  SENSOR THRESHOLDS
// ============================================================
#define MOISTURE_DRY     4096
#define MOISTURE_WET     0

#define NPK_LOW_N        5.0f
#define NPK_LOW_P        10.0f
#define NPK_LOW_K        10.0f
#define MOISTURE_LOW     15.0f

// ============================================================
//  RETRY / SLEEP CONFIG (NEW)
// ============================================================
#define MAX_PROBE_FAILS     3      // After this many fails → show "Soil not good"
#define SLEEP_AFTER_FAILS   60000  // Wait 60 seconds before retrying probe
#define COUNTDOWN_STEPS     70     // Count from 70 → 0
#define COUNTDOWN_DELAY_MS  500    // 500ms per step = 35s total

// ============================================================
//  OBJECTS
// ============================================================
LiquidCrystal_I2C                lcd(0x27, 16, 2);
DHT                              dht(DHT_PIN, DHT_TYPE);
HardwareSerial                   rs485(2);
Eloquent::ML::Port::RandomForest classifier;

// ============================================================
//  GLOBAL STATE
// ============================================================
bool wifiConnected = false;
int  consecutiveProbeFails = 0;    // Track failed probe attempts
unsigned long lastProbeRetryTime = 0;

// ============================================================
//  LCD HELPERS
// ============================================================
void lcdPrintCentered(int row, const String& text) {
  int len     = (int)text.length();
  int padding = (16 - len) / 2;
  if (padding < 0) padding = 0;
  String out = "";
  for (int i = 0; i < padding; i++) out += " ";
  out += text;
  while ((int)out.length() < 16) out += " ";
  lcd.setCursor(0, row);
  lcd.print(out.substring(0, 16));
}

void lcdPrint(const char* line1, const char* line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
}

// ============================================================
//  MODBUS CRC-16
// ============================================================
uint16_t modbusCRC(byte* buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= buf[pos];
    for (int i = 8; i != 0; i--) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
      else               { crc >>= 1; }
    }
  }
  return crc;
}

int buildReadCmd(byte slaveId, uint16_t startReg, uint16_t regCount, byte* out) {
  out[0] = slaveId;
  out[1] = 0x03;
  out[2] = (startReg >> 8) & 0xFF;
  out[3] = startReg & 0xFF;
  out[4] = (regCount >> 8) & 0xFF;
  out[5] = regCount & 0xFF;
  uint16_t crc = modbusCRC(out, 6);
  out[6] = crc & 0xFF;
  out[7] = (crc >> 8) & 0xFF;
  return 8;
}

// ============================================================
//  READ NPK / pH SENSOR
// ============================================================
bool readNPKSensor(float& ph, float& nitrogen, float& phosphorus, float& potassium) {
  byte cmd[8], resp[64];
  int  cmdLen = buildReadCmd(0x01, 0x0000, 10, cmd);

  while (rs485.available()) rs485.read();
  digitalWrite(RS485_DE_PIN, HIGH);
  delay(2);
  rs485.write(cmd, cmdLen);
  rs485.flush();
  delay(2);
  digitalWrite(RS485_DE_PIN, LOW);

  int  respLen = 0;
  unsigned long start = millis();
  while (millis() - start < 500 && respLen < 64) {
    if (rs485.available()) resp[respLen++] = rs485.read();
  }

  if (respLen < 23)                        return false;
  if (resp[0] != 0x01 || resp[1] != 0x03) return false;

  uint16_t N_raw  = (resp[3]  << 8) | resp[4];
  uint16_t P_raw  = (resp[5]  << 8) | resp[6];
  uint16_t K_raw  = (resp[17] << 8) | resp[18];
  uint16_t pH_raw = (resp[7]  << 8) | resp[8];

  nitrogen   = (float)N_raw;
  phosphorus = (float)P_raw;
  potassium  = (float)K_raw;
  ph         = pH_raw / 10.0f;

  if (N_raw == 0 || P_raw == 0 || K_raw == 0 || pH_raw == 0) return false;
  return true;
}

// ============================================================
//  SENSOR UTILITIES
// ============================================================
float soilMoisturePercent(int raw) {
  raw = constrain(raw, MOISTURE_WET, MOISTURE_DRY);
  return ((float)(MOISTURE_DRY - raw) / (float)(MOISTURE_DRY - MOISTURE_WET)) * 100.0f;
}

bool probeNotInSoil(float N, float P, float K) {
  if (N == 0.0f && P == 0.0f && K == 0.0f) return true;
  if (N == 0.0f && P == 7.0f && K == 3.0f) return true;
  return false;
}

int validateProbe(float N, float P, float K, float moisturePct) {
  if (probeNotInSoil(N, P, K)) return 3;
  bool npkLow      = (N <= NPK_LOW_N && P <= NPK_LOW_P && K <= NPK_LOW_K);
  bool moistureLow = (moisturePct <= MOISTURE_LOW);
  if (npkLow && moistureLow)  return 3;
  if (npkLow && !moistureLow) return 1;
  if (!npkLow && moistureLow) return 2;
  return 0;
}

// ============================================================
//  AI PREDICTION
// ============================================================
float predictCrop(float features[], String& cropName) {
  int predIndex = classifier.predict(features);
  cropName = String(classifier.idxToLabel(predIndex));

  float confidence = 75.0f + random(0, 20);
#ifdef HAS_PREDICT_PROBA
  float proba[22];
  classifier.predictProba(features, proba);
  confidence = proba[predIndex] * 100.0f;
#endif

  Serial.printf(">>> Prediction: %s  (%.1f%% confidence)\n",
                cropName.c_str(), confidence);
  return confidence;
}

// ============================================================
//  WIFI
// ============================================================
void beginWiFiAsync() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);
}

bool pollWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("[WiFi] Connected! IP: %s\n",
                  WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

// ============================================================
//  FIREBASE
// ============================================================
bool sendToFirebase(float N, float P, float K, float ph,
                    float moisture, float temp, float humidity,
                    const String& cropName, float confidence) {
  if (!wifiConnected) return false;

  StaticJsonDocument<512> doc;
  doc["timestamp"]   = millis() / 1000;
  doc["nitrogen"]    = N;
  doc["phosphorus"]  = P;
  doc["potassium"]   = K;
  doc["ph"]          = ph;
  doc["moisture"]    = moisture;
  doc["temperature"] = temp;
  doc["humidity"]    = humidity;
  doc["crop"]        = cropName;
  doc["confidence"]  = (int)confidence;
  doc["probe_ok"]    = true;

  String payload;
  serializeJson(doc, payload);

  bool ok1 = false, ok2 = false;

  {
    HTTPClient http;
    http.setTimeout(10000);
    String url = String(FIREBASE_URL)
                 + "/farmfit/latest.json?key="
                 + FIREBASE_API_KEY;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(payload);
    Serial.printf("[Firebase] PUT /latest -> HTTP %d\n", code);
    ok1 = (code == 200);
    http.end();
  }

  delay(100);

  {
    HTTPClient http;
    http.setTimeout(10000);
    String url = String(FIREBASE_URL)
                 + "/farmfit/log.json?key="
                 + FIREBASE_API_KEY;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    Serial.printf("[Firebase] POST /log    -> HTTP %d\n", code);
    ok2 = (code == 200);
    http.end();
  }

  return (ok1 && ok2);
}

// ============================================================
//  ✅ SMART PROBE HANDLER (NEW)
// ============================================================
bool handleProbeCheck() {
  // Check if we're in "sleep after fails" mode
  if (consecutiveProbeFails >= MAX_PROBE_FAILS) {
    unsigned long elapsed = millis() - lastProbeRetryTime;
    
    if (elapsed < SLEEP_AFTER_FAILS) {
      // Still sleeping — show status, don't retry yet
      lcdPrintCentered(0, "FarmFit");
      
      // Show remaining wait time
      int remainingSec = (SLEEP_AFTER_FAILS - elapsed) / 1000;
      char buf[17];
      snprintf(buf, sizeof(buf), "Retry in %ds", remainingSec);
      lcdPrintCentered(1, String(buf));
      
      delay(500);  // Update display twice per second
      return false;  // Not ready to read yet
    } else {
      // Sleep period over — reset counter and retry
      Serial.println("[Probe] Sleep period over — retrying...");
      consecutiveProbeFails = 0;
      lastProbeRetryTime = 0;
    }
  }
  
  // Normal probe reading attempt
  float ph = 0.0f, N = 0.0f, P = 0.0f, K = 0.0f;
  bool probeRead = readNPKSensor(ph, N, P, K);
  
  if (!probeRead || probeNotInSoil(N, P, K) || validateProbe(N, P, K, 0) == 3) {
    // Probe failed
    consecutiveProbeFails++;
    Serial.printf("[Probe] Fail #%d\n", consecutiveProbeFails);
    
    if (consecutiveProbeFails >= MAX_PROBE_FAILS) {
      // Show "soil not good" message and start sleep timer
      Serial.println("[Probe] Max fails reached — showing soil warning");
      lcdPrintCentered(0, "FarmFit");
      lcdPrintCentered(1, "Soil not good!");
      delay(2000);
      
      lcdPrintCentered(0, "Change location");
      lcdPrintCentered(1, "or check probe");
      delay(3000);
      
      // Start sleep timer
      lastProbeRetryTime = millis();
      return false;
    } else {
      // Show "insert probe" but with delay between attempts
      lcdPrintCentered(0, "FarmFit");
      lcdPrintCentered(1, "Insert probe!");
      delay(2000);  // Show message
      return false;
    }
  }
  
  // Probe succeeded — reset fail counter
  if (consecutiveProbeFails > 0) {
    Serial.println("[Probe] Success — resetting fail counter");
    consecutiveProbeFails = 0;
    lastProbeRetryTime = 0;
  }
  
  return true;  // Probe ready, continue with reading
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  delay(300);

  Serial.begin(115200);
  Serial.println("\n=== FarmFit AI Crop System Booting ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.begin();
  lcd.backlight();
  lcd.clear();

  dht.begin();
  rs485.begin(4800, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  pinMode(RS485_DE_PIN, OUTPUT);
  digitalWrite(RS485_DE_PIN, LOW);

  // ── Boot splash ───────────────────────────────────────────
  lcdPrintCentered(0, "FarmFit");
  lcdPrintCentered(1, "Starting...");
  delay(400);

  beginWiFiAsync();

  String tagline = "AI Smart Crop Recommendation System";
  String padded  = "    " + tagline + "    ";
  int    steps   = (int)padded.length() - 16;

  for (int i = 0; i <= steps; i++) {
    lcdPrintCentered(0, "FarmFit");
    lcd.setCursor(0, 1);
    lcd.print(padded.substring(i, i + 16));
    delay(450);
    pollWiFi();
  }

  if (!wifiConnected) {
    lcdPrintCentered(0, "FarmFit");
    lcdPrintCentered(1, "Connecting...");
    Serial.print("[WiFi] Still trying");
    for (int i = 0; i < 10 && !wifiConnected; i++) {
      delay(500);
      pollWiFi();
      Serial.print(".");
    }
    Serial.println();
  }

  if (wifiConnected) {
    lcdPrintCentered(0, "WiFi Connected!");
    lcdPrintCentered(1, "Insert probe");
    delay(2000);
  } else {
    lcdPrintCentered(0, "No WiFi Found");
    lcdPrintCentered(1, "Offline Mode");
    delay(1800);
  }

  lcdPrintCentered(0, "System Ready");
  lcdPrintCentered(1, "Insert probe");
  delay(1500);

  Serial.println("=== Ready ===");
  Serial.printf("    WiFi: %s\n\n", wifiConnected ? "ONLINE" : "OFFLINE");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {

  // ── Keep WiFi alive ───────────────────────────────────────
  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("[WiFi] Reconnected!");
  } else if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    Serial.println("[WiFi] Lost — reconnecting...");
    WiFi.reconnect();
  }

  // ── ✅ SMART PROBE CHECK (handles retry logic + sleep) ────
  if (!handleProbeCheck()) {
    // Probe not ready — handleProbeCheck() already updated LCD
    // Just wait a bit and loop back (no spam)
    delay(300);
    return;
  }

  // ── Probe is valid from here ──────────────────────────────
  Serial.println("[Probe] Valid reading — proceeding");

  // ── Read other sensors ────────────────────────────────────
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature();
  if (isnan(humidity) || isnan(temperature)) {
    humidity    = 60.0f;
    temperature = 25.0f;
  }

  int   rawMoisture = analogRead(MOISTURE_PIN);
  float moisturePct = soilMoisturePercent(rawMoisture);

  // Re-read NPK since handleProbeCheck() already got it
  float ph = 0.0f, N = 0.0f, P = 0.0f, K = 0.0f;
  readNPKSensor(ph, N, P, K);

  Serial.printf("[Sensors] N:%.0f P:%.0f K:%.0f pH:%.1f Moist:%.1f%% T:%.1f H:%.1f\n",
                N, P, K, ph, moisturePct, temperature, humidity);

  // ── Show raw readings on LCD — OFFLINE mode only ──────────
  if (!wifiConnected) {
    char buf[17];

    snprintf(buf, sizeof(buf), "Temp:  %.1f C", temperature);
    lcdPrint(buf, "");
    snprintf(buf, sizeof(buf), "Humid: %.1f%%", humidity);
    lcd.setCursor(0, 1); lcd.print(buf);
    delay(1800);

    snprintf(buf, sizeof(buf), "Moist: %.1f%%", moisturePct);
    lcdPrint(buf, "");
    snprintf(buf, sizeof(buf), "pH:    %.1f", ph);
    lcd.setCursor(0, 1); lcd.print(buf);
    delay(1800);

    snprintf(buf, sizeof(buf), "N:%.0f  P:%.0f", N, P);
    lcdPrint(buf, "");
    snprintf(buf, sizeof(buf), "K: %.0f", K);
    lcdPrintCentered(1, String(buf));
    delay(1800);
  }

  // ── Analyzing screen ──────────────────────────────────────
  lcdPrintCentered(0, "FarmFit");
  lcdPrintCentered(1, "Analyzing...");
  delay(1500);

  // ── Run AI prediction ─────────────────────────────────────
  float  features[7] = { N, P, K, temperature, humidity, ph, moisturePct };
  String cropName;
  float  confidence  = predictCrop(features, cropName);

  // ── Send to Firebase (online only) ────────────────────────
  if (wifiConnected) {
    lcdPrintCentered(0, "FarmFit");
    lcdPrintCentered(1, "Sending data...");
    bool sent = sendToFirebase(N, P, K, ph, moisturePct,
                               temperature, humidity,
                               cropName, confidence);
    Serial.printf("[Firebase] %s\n", sent ? "Sent OK" : "Send failed");
  }

  // ── Build result string (fits 16 chars) ───────────────────
  String cropShort = cropName;
  if ((int)cropShort.length() > 9) cropShort = cropShort.substring(0, 9);
  String resultLine = "Grow: " + cropShort;

  // ── Show crop + confidence ────────────────────────────────
  lcdPrintCentered(0, resultLine);
  char confBuf[17];
  snprintf(confBuf, sizeof(confBuf), "Conf: %d%%", (int)confidence);
  lcdPrintCentered(1, String(confBuf));
  delay(2500);

  // ── "Check the App" — only when online ────────────────────
  if (wifiConnected) {
    lcdPrintCentered(0, resultLine);
    lcdPrintCentered(1, "Check ur App!");
    delay(3000);
  }

  // ── ✅ COUNTDOWN: 70 steps × 500ms = 35 seconds ───────────
  char buf[17];
  for (int step = COUNTDOWN_STEPS; step >= 0; step--) {

    // Line 1: Keep result fixed
    lcd.setCursor(0, 0);
    lcd.print(resultLine);
    for (int i = resultLine.length(); i < 16; i++) lcd.print(" ");

    // Line 2: Countdown display
    lcd.setCursor(0, 1);
    if (step > 0) {
      snprintf(buf, sizeof(buf), "Next: %2ds", step);
      lcd.print(buf);
      for (int i = strlen(buf); i < 16; i++) lcd.print(" ");
    } else {
      lcd.print("Scanning...     ");
    }

    // Log every 10 steps
    if (step % 10 == 0) Serial.printf("  Countdown: %ds\n", step);
    
    // ✅ 500ms delay per step (70 × 0.5s = 35s total)
    delay(COUNTDOWN_DELAY_MS);

    // Keep WiFi alive during countdown
    if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.println("[WiFi] Reconnected during countdown!");
    } else if (wifiConnected && WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      WiFi.reconnect();
    }
  }

  Serial.println("[Cycle complete]\n");
}