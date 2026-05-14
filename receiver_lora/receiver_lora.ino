
/*
 * LoRa Gateway — Receiver + TFLite Inference + Firebase + Google Sheets
 * Hardware: ESP32 + UART LoRa Module
 *
 * Libraries required (Arduino Library Manager):
 *   - "Firebase ESP32 Client" by Mobizt
 *   - "ArduTFLite" by Albert Kragl
 *
 * ─── WIRING ─────────────────────────────────────────────────
 *  LoRa Module     ESP32
 *  VCC          -> 3.3V
 *  GND          -> GND
 *  TX           -> GPIO 16
 *  RX           -> GPIO 17
 *  MDO          -> GND or floating
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <FirebaseESP32.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "capstone.h"

// ── ArduTFLite — single header, that's all you need ──────────
#include <ArduTFLite.h>

// ════════════════════════════════════════════════════════════
//   CONFIGURATION — fill in all fields marked  ← HERE
// ════════════════════════════════════════════════════════════
#define WIFI_SSID          "capstone_400"                       // ← HERE
#define WIFI_PASSWORD      "capstone_400"                   // ← HERE
#define FIREBASE_API_KEY    "AIzaSyDkPRdXQqKx6TsIbawybZ-Aww9gsgulj74"               // ← HERE
#define FIREBASE_DATABASE_URL "https://capstone-2e26e-default-rtdb.firebaseio.com" // ← HERE
#define SHEETS_WEB_APP_URL "https://script.google.com/macros/s/AKfycbwBRtX-gDdjmQSBwWaBi42mLP7BW_aUaW1GPw57blItOPiHp3sayB9PaCyxHza76eTf/exec" // ← HERE
// ════════════════════════════════════════════════════════════
#define RXD2         16
#define TXD2         17
#define MOISTURE_DRY 30
#define MOISTURE_WET 70

// ── TFLite setup ─────────────────────────────────────────────
// Arena size — increase if you get memory errors at runtime
constexpr int kTensorArenaSize = 8 * 1024;
uint8_t tensor_arena[kTensorArenaSize];

const char* CLASS_LABELS[] = {
  "Very Poor Soil", "Poor Soil", "Moderate Soil", "Good Soil", "Excellent Soil"
};
const int NUM_CLASSES = 5;

struct NormRange { float min; float max; };
NormRange normRanges[5] = {
  {0.0f, 100.0f},   // [0] Moisture %
  {0.0f,  50.0f},   // [1] Temperature °C
  {0.0f, 100.0f},   // [2] Humidity %
  {0.0f, 200.0f},   // [3] Nitrogen mg/kg
  {0.0f, 200.0f},   // [4] Phosphorus mg/kg
};

// ── Firebase ─────────────────────────────────────────────────
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
bool firebaseReady = false;

// ─────────────────────────────────────────────────────────────
float normalise(float v, float mn, float mx) {
  if (mx == mn) return 0.0f;
  float n = (v - mn) / (mx - mn);
  return (n < 0.0f) ? 0.0f : (n > 1.0f) ? 1.0f : n;
}

// ─────────────────────────────────────────────────────────────
void setupTFLite() {
  Serial.println("[TFLite] Loading model...");

  // ArduTFLite API: modelInit(modelArray, arenaBuffer, arenaSize)
  if (!modelInit(model_tflite, tensor_arena, kTensorArenaSize)) {
    Serial.println("[TFLite] modelInit() failed — halting.");
    Serial.println("         Try increasing kTensorArenaSize.");
    while (true);
  }

  Serial.println("[TFLite] Model loaded successfully.");
}

// ─────────────────────────────────────────────────────────────
int runInference(float moisture, float temp, float hum,
                 float nitrogen, float phosphorus) {

  // Build normalised input array
  float input[5] = {
    normalise(moisture,   normRanges[0].min, normRanges[0].max),
    normalise(temp,       normRanges[1].min, normRanges[1].max),
    normalise(hum,        normRanges[2].min, normRanges[2].max),
    normalise(nitrogen,   normRanges[3].min, normRanges[3].max),
    normalise(phosphorus, normRanges[4].min, normRanges[4].max)
  };

  // ArduTFLite API: modelSetInput(value, index)
  for (int i = 0; i < 5; i++) {
    modelSetInput(input[i], i);
  }

  // ArduTFLite API: modelRunInference()
  if (!modelRunInference()) {
    Serial.println("[TFLite] Inference failed.");
    return -1;
  }

  // ArduTFLite API: modelGetOutput(index)
  int   best = 0;
  float top  = modelGetOutput(0);
  for (int i = 1; i < NUM_CLASSES; i++) {
    float score = modelGetOutput(i);
    if (score > top) { top = score; best = i; }
  }
  return best;
}

// ─────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Connecting to " WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[WiFi] Connected — " + WiFi.localIP().toString());
}

void setupFirebase() {
  config.api_key               = FIREBASE_API_KEY;
  config.database_url          = FIREBASE_DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  auth.user.email              = "";
  auth.user.password           = "";
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  firebaseReady = true;
  Serial.println("[Firebase] Initialised.");
}

// ─────────────────────────────────────────────────────────────
void pushToFirebase(const String& nodeId,
                    float moisture, float temp, float hum,
                    float nitrogen, float phosphorus, float potassium,
                    const String& lat, const String& lng,
                    const String& alt, const String& sats,
                    int prediction) {

  if (!firebaseReady || !Firebase.ready()) {
    Serial.println("[Firebase] Not ready — skipping.");
    return;
  }

  String basePath = "/nodes/" + nodeId;
  float  confidence = (prediction >= 0) ? modelGetOutput(prediction) * 100.0f : 0.0f;

  FirebaseJson json;
  json.set("timestamp",   (int)millis());
  json.set("moisture",    moisture);
  json.set("temperature", temp);
  json.set("humidity",    hum);
  json.set("nitrogen",    nitrogen);
  json.set("phosphorus",  phosphorus);
  json.set("potassium",   potassium);
  json.set("latitude",    lat);
  json.set("longitude",   lng);
  json.set("altitude",    alt);
  json.set("satellites",  sats);
  json.set("prediction",  prediction >= 0 ? CLASS_LABELS[prediction] : "N/A");
  json.set("confidence",  confidence);

  if (Firebase.RTDB.setJSON(&fbdo, basePath + "/latest", &json))
    Serial.println("[Firebase] Latest updated.");
  else
    Serial.println("[Firebase] Error: " + fbdo.errorReason());

  if (Firebase.RTDB.pushJSON(&fbdo, basePath + "/history", &json))
    Serial.println("[Firebase] History pushed.");
  else
    Serial.println("[Firebase] History error: " + fbdo.errorReason());
}

// ─────────────────────────────────────────────────────────────
void pushToSheets(const String& nodeId,
                  float moisture, float temp, float hum,
                  float nitrogen, float phosphorus, float potassium,
                  int prediction) {

  if (WiFi.status() != WL_CONNECTED) return;

  float confidence = (prediction >= 0) ? modelGetOutput(prediction) * 100.0f : 0.0f;

  String url = String(SHEETS_WEB_APP_URL) +
    "?node="       + nodeId               +
    "&moisture="   + String(moisture,  1) +
    "&temp="       + String(temp,      1) +
    "&humidity="   + String(hum,       1) +
    "&nitrogen="   + String(nitrogen,  0) +
    "&phosphorus=" + String(phosphorus,0) +
    "&potassium="  + String(potassium, 0) +
    "&prediction=" + (prediction >= 0 ? String(CLASS_LABELS[prediction]) : "N/A") +
    "&confidence=" + String(confidence, 1);

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  Serial.printf("[Sheets] Response: %d\n", code);
  http.end();
}

// ─────────────────────────────────────────────────────────────
String extractField(const String& msg, const String& key) {
  int start = msg.indexOf(key + ":");
  if (start == -1) return "ERR";
  start += key.length() + 1;
  int end = msg.indexOf('|', start);
  return (end == -1) ? msg.substring(start) : msg.substring(start, end);
}

// ─────────────────────────────────────────────────────────────
void processPacket(const String& msg) {
  Serial.println("\n========================================");
  Serial.println("  PACKET RECEIVED");
  Serial.println("========================================");
  Serial.println("  Raw: " + msg);
  Serial.println("----------------------------------------");

  String nodeId     = extractField(msg, "ID");
  float  moisture   = extractField(msg, "Moist").toFloat();
  String tempStr    = extractField(msg, "Temp");
  String humStr     = extractField(msg, "Hum");
  float  nitrogen   = extractField(msg, "N").toFloat();
  float  phosphorus = extractField(msg, "P").toFloat();
  float  potassium  = extractField(msg, "K").toFloat();
  String latStr     = extractField(msg, "Lat");
  String lngStr     = extractField(msg, "Lng");
  String altStr     = extractField(msg, "Alt");
  String satsStr    = extractField(msg, "Sats");

  float temp = (tempStr == "ERR") ? -1 : tempStr.toFloat();
  float hum  = (humStr  == "ERR") ? -1 : humStr.toFloat();

  // ── Print decoded values ─────────────────────────────────
  Serial.println("  Node ID     : " + nodeId);
  Serial.printf( "  Moisture    : %.0f%%\n",     moisture);
  Serial.printf( "  Temperature : %.1f C\n",     temp);
  Serial.printf( "  Humidity    : %.1f%%\n",     hum);
  Serial.printf( "  Nitrogen    : %.0f mg/kg\n", nitrogen);
  Serial.printf( "  Phosphorus  : %.0f mg/kg\n", phosphorus);
  Serial.printf( "  Potassium   : %.0f mg/kg\n", potassium);

  if (latStr == "ERR") {
    Serial.println("  GPS         : No fix");
  } else {
    Serial.println("  GPS Lat     : " + latStr);
    Serial.println("  GPS Lng     : " + lngStr);
    Serial.println("  Altitude    : " + altStr + " m");
    Serial.println("  Satellites  : " + satsStr);
    Serial.println("  Maps        : https://maps.google.com/?q=" + latStr + "," + lngStr);
  }

  Serial.print("  Soil Status : ");
  if      (moisture < MOISTURE_DRY) Serial.println("DRY — Consider watering!");
  else if (moisture > MOISTURE_WET) Serial.println("WET — Reduce watering!");
  else                              Serial.println("OPTIMAL");

  // ── TFLite inference ─────────────────────────────────────
  int predicted = -1;
  if (temp >= 0 && hum >= 0) {
    predicted = runInference(moisture, temp, hum, nitrogen, phosphorus);
    if (predicted >= 0) {
      Serial.println("----------------------------------------");
      for (int i = 0; i < NUM_CLASSES; i++) {
        Serial.printf("  [%d] %-20s : %5.1f%%%s\n",
                      i, CLASS_LABELS[i],
                      modelGetOutput(i) * 100.0f,
                      i == predicted ? " << PREDICTED" : "");
      }
      Serial.printf("\n  >> Prediction : %s\n",    CLASS_LABELS[predicted]);
      Serial.printf(  "  >> Confidence : %.1f%%\n", modelGetOutput(predicted) * 100.0f);
    }
  } else {
    Serial.println("  [TFLite] Skipped — sensor error.");
  }

  Serial.println("========================================");

  // ── Push to cloud ────────────────────────────────────────
  pushToFirebase(nodeId, moisture, temp, hum, nitrogen, phosphorus, potassium,
                 latStr, lngStr, altStr, satsStr, predicted);

  pushToSheets(nodeId, moisture, temp, hum, nitrogen, phosphorus, potassium,
               predicted);
}

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("\n--- LoRa Gateway Started ---");

  connectWiFi();
  setupFirebase();
  setupTFLite();

  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println("\n[Gateway] Listening for LoRa packets...\n");
}

// ─────────────────────────────────────────────────────────────
void loop() {
  if (Serial2.available()) {
    String msg = Serial2.readStringUntil('\n');
    msg.trim();
    if (msg.length() > 0) processPacket(msg);
  }
}