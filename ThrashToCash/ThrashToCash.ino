#include <MAX30105.h>
#include <heartRate.h>
#include <spo2_algorithm.h>

#include <ESP32_Servo.h>

/* main_esp32_modified.ino
   Modified Smart Waste Sorter with:
   - '*' = backspace
   - Auto-stop on no-weight-change -> stop camera -> close -> upload -> deep sleep
   - '#' during sorting = manual finish
   - CAM triggered repeatedly while dumping
   - Deep sleep wake on keypad rows (ext1)
*/

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include "HX711.h"
#include <ArduinoJson.h>
#include "esp_sleep.h" // for deep sleep

// ========== WiFi & Supabase ==========
const char* ssid = "REDMI 15C";
const char* password = "khalidi123";

const char* CHECK_USER_URL = "https://nkrirpjmixfrunhmrawz.supabase.co/functions/v1/check-user";
const char* SUBMIT_WASTE_URL = "https://nkrirpjmixfrunhmrawz.supabase.co/functions/v1/submit-waste";
const char* SUPABASE_API_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5rcmlycGptaXhmcnVuaG1yYXd6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTk5NjQ4NTgsImV4cCI6MjA3NTU0MDg1OH0.qeFGqAauX_wPGHX29hiLphUXdFKA6_s2VuMU8lGYUFs";

// ========== HX711 ==========
#define DOUT 19
#define CLK 18
HX711 scale;
float calibration_factor = 873320.0; // adjust if necessary

// ========== I2C LCD ==========
#define I2C_SDA 21
#define I2C_SCL 22
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ========== Keypad ==========
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
 {'1','2','3'},
 {'4','5','6'},
 {'7','8','9'},
 {'*','0','#'}
};
// Ensure these are the same pins you used (RTC-capable for deep sleep)
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] 
// ========== WiFi & Supabase ==========
const char* ssid = "REDMI 15C";
const char* password = "khalidi123";

const char* CHECK_USER_URL = "https://nkrirpjmixfrunhmrawz.supabase.co/functions/v1/check-user";
const char* SUBMIT_WASTE_URL = "https://nkrirpjmixfrunhmrawz.supabase.co/functions/v1/submit-waste";
const char* SUPABASE_API_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5rcmlycGptaXhmcnVuaG1yYXd6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTk5NjQ4NTgsImV4cCI6MjA3NTU0MDg1OH0.qeFGqAauX_wPGHX29hiLphUXdFKA6_s2Vu
= {26, 25, 33};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ========== Servos ==========
Servo lidServo;
Servo sorterServo;
const int LID_SERVO_PIN = 5;
const int SORTER_SERVO_PIN = 4;
int lidClosedDeg = 150;
int lidOpenDeg = 50;
int sorterBottleDeg = 0;
int sorterNylonDeg = 180;
int sorterNeutralDeg = 90;

// ========== Serial2 for CAM ==========
HardwareSerial CamSerial(2);
String camBuffer = "";
String lastPrediction = "unknown";
float lastConfidence = 0.0;
volatile bool newPredictionAvailable = false;

// ========== State variables ==========
String inputCode = "";
bool waitingForPrediction = false;

// ✅ Fixed naming conflict here
enum SessionState {
 STATE_IDLE,
STATE_AUTHENTICATED,
 STATE_SORTING,
 STATE_UPLOADING,
STATE_SLEEP
};
SessionState state = STATE_IDLE;

// Sorting/weight logic
float baselineWeight = 0.0;
float lastWeightSample = 0.0;
unsigned long lastWeightChangeAt = 0;
const float weightChangeThreshold = 0.01; // kg
const unsigned long noChangeTimeoutMs = 4000UL; // 4 seconds
const unsigned long camTriggerIntervalMs = 1000UL;
unsigned long lastCamTriggerAt = 0;

// helper: allow a small moving average sample for active weight reading
float readWeightAverage(int samples=10){
 float sum = 0;
 for (int i=0;i<samples;i++){
  float u = scale.get_units(1);
  sum += u;
  delay(8);
 }
 return sum / samples;
}

// ========== Helpers ==========
void softMoveServo(Servo &s, int fromDeg, int toDeg, int stepDelay=12, int stepSize=4){
 if (fromDeg < toDeg){
 for (int d = fromDeg; d <= toDeg; d += stepSize) {
 s.write(d);
delay(stepDelay);
 }
 } else {
 for (int d = fromDeg; d >= toDeg; d -= stepSize) {
 s.write(d);
 delay(stepDelay);
 }
 } s.write(toDeg);
}

// ========== Supabase: check user ==========
bool checkUser(String phone) {
if (WiFi.status() != WL_CONNECTED) {
 lcd.clear(); lcd.print("WiFi Lost!"); Serial.println("WiFi not connected."); return false;
 }

HTTPClient http;
 http.begin(CHECK_USER_URL);
 http.addHeader("Content-Type", "application/json");
 http.addHeader("apikey", SUPABASE_API_KEY);
 http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
 DynamicJsonDocument doc(200);
 doc["unique_code"] = phone;
 String body; serializeJson(doc, body);

 int httpCode = http.POST(body);
 String response = http.getString();
 Serial.println("Check HTTP Code: " + String(httpCode));
 Serial.println("Check Response: " + response);
 Serial.println("HTTP Code: " + String(httpCode));
Serial.println("Response: ");
Serial.println(response);


 bool exists = false;
 if (httpCode == 200 || httpCode == 201) {
 DynamicJsonDocument resp(512);
 DeserializationError err = deserializeJson(resp, response);
 if (!err && resp.containsKey("exists")) exists = resp["exists"];
 }

 http.end();
 return exists;
}

// ========== Supabase: submit waste ==========
bool sendWasteData(String phone, float weight, String wtype) {
 if (WiFi.status() != WL_CONNECTED) {
  lcd.clear(); lcd.print("WiFi Lost!"); return false;
 }

 HTTPClient http;
 http.begin(SUBMIT_WASTE_URL);
 http.addHeader("Content-Type", "application/json");
 http.addHeader("apikey", SUPABASE_API_KEY); http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);

 DynamicJsonDocument doc(512);
 doc["unique_code"] = phone;
 doc["weight_kg"] = weight;



 String body; serializeJson(doc, body);
 Serial.println("Sending to Supabase: " + body);

 int httpCode = http.POST(body);
 String response = http.getString();
 Serial.println("HTTP Code: " + String(httpCode));
 Serial.println("Response: " + response);

 lcd.clear();
 if (httpCode == 200 || httpCode == 201) {
  lcd.print("Upload Success!");
 lcd.setCursor(0,1); lcd.print("Points Added!");
  http.end();
  delay(2000);
 return true;
 } else {
  if (response.indexOf("No account found") != -1) {
 lcd.print("No account found");
  lcd.setCursor(0,1); lcd.print("Create online!");
 } else {
lcd.print("Upload Failed!");
 }
 http.end();
 delay(2000);
 return false;
 }
}

// ================= Setup =================
void setup() {
 Serial.begin(115200);
 delay(50);

 CamSerial.begin(115200, SERIAL_8N1, 16, 17);
 Serial.println("Serial2 (CAM) started on RX=16 TX=17");

 Wire.begin(I2C_SDA, I2C_SCL);
 lcd.begin();
 lcd.backlight(); lcd.clear(); lcd.print("Connecting WiFi");

WiFi.begin(ssid, password);
unsigned long start = millis();
 while (WiFi.status() != WL_CONNECTED) {
  delay(300);
 Serial.print(".");
if (millis() - start > 20000) break;
 }
 if (WiFi.status() == WL_CONNECTED) {
 lcd.clear(); lcd.print("WiFi Connected");
 } else {
  lcd.clear(); lcd.print("WiFi Err"); }
 delay(800);

 scale.begin(DOUT, CLK);
 scale.set_scale(calibration_factor);
 scale.tare();

 lidServo.attach(LID_SERVO_PIN);
 sorterServo.attach(SORTER_SERVO_PIN);
 lidServo.write(lidClosedDeg);
 sorterServo.write(sorterNeutralDeg);

 lcd.clear(); lcd.print("Enter Phone ID:");
 lcd.setCursor(0,1);

 state = STATE_IDLE;
}

// ================= Serial2 handling =================
void handleCamSerial() {
     while (CamSerial.available()) {
  char c = CamSerial.read();
if (c == '\n') {
 String line = camBuffer; camBuffer = "";
 line.trim();
 if (line.length() > 0) {
Serial.println("CAM -> " + line);
 DynamicJsonDocument doc(256);
 DeserializationError err = deserializeJson(doc, line);
 if (!err) {
 if (doc.containsKey("prediction")) lastPrediction = String((const char*)doc["prediction"].as<const char*>());
 if (doc.containsKey("confidence")) lastConfidence = doc["confidence"].as<float>();
 newPredictionAvailable = true;
 waitingForPrediction = false;
 } else {
 String label = line;
 lastPrediction = label;
 lastConfidence = 0.0;
 newPredictionAvailable = true;
 waitingForPrediction = false;
 }
 }
 } else {
 camBuffer += c;
 if (camBuffer.length() > 800) camBuffer = "";
 }
 }
}

// ================= deep sleep helper =================
void goToDeepSleepAwaitKeypadWake() {
 lcd.clear(); lcd.print("Sleeping...");
 Serial.println("Going to deep sleep. Wake on keypad.");

 lidServo.detach();
 sorterServo.detach();

 uint64_t mask = 0;
 for (int i = 0; i < ROWS; ++i) {
 mask |= (1ULL << rowPins[i]);
 }
esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);


 delay(200);
 esp_deep_sleep_start();
}

// ================= Main loop =================
void loop() {
 handleCamSerial();

 if (state == STATE_IDLE) {
 char key = keypad.getKey();
 if (key) {
 if (key == '#') {
 if (inputCode.length() < 5) {
 lcd.clear(); lcd.print("Invalid Number!");
 delay(1200);
 lcd.clear(); lcd.print("Enter Phone ID:");
 inputCode = "";
 } else {
 lcd.clear(); lcd.print("Checking user...");
 bool exists = checkUser(inputCode);
 if (!exists) {
 lcd.clear(); lcd.print("No account found");
 lcd.setCursor(0,1); lcd.print("Create online!");
 delay(2000);
 lcd.clear(); lcd.print("Enter Phone ID:");
 inputCode = "";
 } else {
 state = STATE_AUTHENTICATED;
 lcd.clear(); lcd.print("Welcome!");
 delay(600);
 lcd.clear(); lcd.print("Opening lid...");
 softMoveServo(lidServo, lidClosedDeg, lidOpenDeg, 12, 4);
 delay(300);
 lcd.clear(); lcd.print("Place waste...");
 lcd.setCursor(0,1); lcd.print("Press # when done");
 baselineWeight = readWeightAverage(20);
 Serial.printf("Baseline weight: %.4f\n", baselineWeight);
 lastWeightSample = baselineWeight;
 lastWeightChangeAt = millis();
 lastCamTriggerAt = 0;
 newPredictionAvailable = false;
 waitingForPrediction = true;
 lastPrediction = "unknown";
 lastConfidence = 0.0;
 state = STATE_SORTING;
 }
 }
 }
 else if (key == '*') {
 if (inputCode.length() > 0) inputCode.remove(inputCode.length()-1);
 lcd.clear(); lcd.print("Enter Phone ID:");
 lcd.setCursor(0,1);
 lcd.print(inputCode);
 }
 else {
 inputCode += key;
 lcd.setCursor(0,1);
 lcd.print(inputCode);
 }
 }
 }
 else if (state == STATE_SORTING) {
 unsigned long now = millis();
 if (now - lastCamTriggerAt > camTriggerIntervalMs) {
 lastCamTriggerAt = now;
 CamSerial.println("CAP");
 waitingForPrediction = true;
 }

 if (newPredictionAvailable) {
 newPredictionAvailable = false;
 lcd.clear(); lcd.print("Sorting:");
 lcd.print(lastPrediction);
 if (lastPrediction.equalsIgnoreCase("nylon")) {
 softMoveServo(sorterServo, sorterServo.read(), sorterNylonDeg, 10, 6);
 } else if (lastPrediction.equalsIgnoreCase("bottle")) {
 softMoveServo(sorterServo, sorterServo.read(), sorterBottleDeg, 10, 6);
 } else {
 softMoveServo(sorterServo, sorterServo.read(), sorterNeutralDeg, 10, 6);
 }
}

 static unsigned long lastWeightCheck = 0;
 if (millis() - lastWeightCheck > 400) {
 lastWeightCheck = millis();
 float w = readWeightAverage(5);
 float diff = fabs(w - lastWeightSample);
 if (diff >= weightChangeThreshold) {
 lastWeightChangeAt = millis();
 Serial.printf("Weight changed: now %.4f (diff %.4f)\n", w, diff);
 lastWeightSample = w;
 }
 }

 char k = keypad.getKey();
 if (k) {
 if (k == '#') {
 Serial.println("Manual finish requested.");
 lcd.clear(); lcd.print("Finishing...");
state = STATE_UPLOADING;
 } else if (k == '*') {
 lcd.clear(); lcd.print("Cancelled");
 delay(800);
 softMoveServo(lidServo, lidOpenDeg, lidClosedDeg, 12, 4);
 delay(300);
 lcd.clear(); lcd.print("Enter Phone ID:");
 inputCode = "";
 state = STATE_IDLE;
 }
 }

 if ((millis() - lastWeightChangeAt) >= noChangeTimeoutMs) {
 Serial.println("Auto-stop: no weight change detected");
 lcd.clear(); lcd.print("No activity.");
 delay(400);
 state = STATE_UPLOADING;
 }
 }
 else if (state == STATE_UPLOADING) {
 lcd.clear(); lcd.print("Closing...");
 softMoveServo(lidServo, lidOpenDeg, lidClosedDeg, 12, 4);
 delay(300);

 float finalVal = readWeightAverage(20);
 float weightAdded = finalVal - baselineWeight;
if (weightAdded < 0.0) weightAdded = 0.0;
 Serial.printf("Final: %.4f, Added: %.4f\n", finalVal, weightAdded);

 lcd.clear(); lcd.print("Uploading...");
 bool ok = sendWasteData(inputCode, weightAdded, lastPrediction);
 if (ok) {
 lcd.clear(); lcd.print("Done ✅");
 } else {
 lcd.clear(); lcd.print("Upload Err");
 }
 delay(1500);

 state = STATE_SLEEP;
 }
 else if (state == STATE_SLEEP) {
 goToDeepSleepAwaitKeypadWake();
 }

 delay(5);
}
