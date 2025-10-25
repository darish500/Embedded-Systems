#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===== PIN CONFIGURATION =====
#define GSM_RX D5   // SIM800L TX -> ESP8266 D5
#define GSM_TX D6   // SIM800L RX -> ESP8266 D6 (via voltage divider)
#define OLED_SDA D2 // ESP8266 SDA
#define OLED_SCL D1 // ESP8266 SCL

// ===== OLED SETUP =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===== MODULE OBJECTS =====
SoftwareSerial gsmSerial(GSM_RX, GSM_TX); // SIM800L serial
TinyGPSPlus gps;                          // GPS object

// ===== VARIABLES =====
String phoneNumber = "+"; // replace with your number
bool smsSent = false;
const int sampleCount = 10;

double latBuffer[sampleCount];
double lngBuffer[sampleCount];
int bufferIndex = 0;
bool bufferFilled = false;
double lastStableLat = 0.0, lastStableLng = 0.0;

// ====== SETUP ======
void setup() {
  Serial.begin(9600);      // GPS on hardware serial
  gsmSerial.begin(9600);   // SIM800L on software serial
  Wire.begin(OLED_SDA, OLED_SCL);

  // === OLED INIT ===
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("❌ OLED init failed!"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(F("📡 Initializing Modules..."));
  display.display();

  Serial.println(F("\n📡 Initializing GSM and GPS..."));
  delay(2000);

  // === Test GSM ===
  gsmSerial.println("AT");
  delay(1000);
  Serial.println(F("✅ GSM Ready!"));
  display.println(F("✅ GSM Ready!"));
  display.display();
  delay(1000);

  Serial.println(F("Waiting for GPS Fix..."));
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("🔄 Waiting for GPS Fix..."));
  display.display();
}

// ====== LOOP ======
void loop() {
  // Read GPS data from hardware serial
  while (Serial.available() > 0) {
    gps.encode(Serial.read());
  }

  if (gps.location.isValid()) {
    double lat = gps.location.lat();
    double lng = gps.location.lng();
    double speed = gps.speed.kmph();
    double sat = gps.satellites.value();

    // Fill buffer for stability checking
    latBuffer[bufferIndex] = lat;
    lngBuffer[bufferIndex] = lng;
    bufferIndex = (bufferIndex + 1) % sampleCount;
    if (bufferIndex == 0) bufferFilled = true;

    // === SERIAL MONITOR ===
    Serial.println("\n============== GPS DATA ==============");
    Serial.print("Latitude  : "); Serial.println(lat, 6);
    Serial.print("Longitude : "); Serial.println(lng, 6);
    Serial.print("Speed (km/h): "); Serial.println(speed, 2);
    Serial.print("Satellites : "); Serial.println(sat);
    Serial.println("======================================");

    // === OLED DISPLAY ===
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("📍 GPS Tracker"));
    display.print(F("Lat: ")); display.println(lat, 6);
    display.print(F("Lng: ")); display.println(lng, 6);
    display.print(F("Spd: ")); display.print(speed, 1); display.println(F(" km/h"));
    display.print(F("Sat: ")); display.println(sat);
    display.display();

    // Check stability before sending SMS
    if (bufferFilled && !smsSent && isLocationStable()) {
      lastStableLat = average(latBuffer, sampleCount);
      lastStableLng = average(lngBuffer, sampleCount);

      Serial.println(F("\n✅ Stable Location Detected!"));
      Serial.print(F("Stable LAT: ")); Serial.println(lastStableLat, 6);
      Serial.print(F("Stable LNG: ")); Serial.println(lastStableLng, 6);

      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(F("✅ Stable Location"));
      display.println(F("Sending SMS..."));
      display.display();

      sendSMS(lastStableLat, lastStableLng);
      smsSent = true;

      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(F("📤 SMS Sent Successfully"));
      display.display();
    }
  } else {
    Serial.println(F("❗ Waiting for valid GPS fix..."));
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println(F("🔄 Waiting for GPS Fix..."));
    display.display();
  }

  delay(1000);
}

// ===== LOCATION STABILITY CHECK =====
bool isLocationStable() {
  double avgLat = average(latBuffer, sampleCount);
  double avgLng = average(lngBuffer, sampleCount);

  double maxLatDiff = 0, maxLngDiff = 0;
  for (int i = 0; i < sampleCount; i++) {
    maxLatDiff = max(maxLatDiff, fabs(latBuffer[i] - avgLat));
    maxLngDiff = max(maxLngDiff, fabs(lngBuffer[i] - avgLng));
  }

  bool stable = (maxLatDiff < 0.0001 && maxLngDiff < 0.0001); // ~11m

  Serial.print("📈 Stability Check → ");
  Serial.println(stable ? "✅ STABLE" : "⚠️ UNSTABLE");
  Serial.print("Lat Variation: "); Serial.println(maxLatDiff, 6);
  Serial.print("Lng Variation: "); Serial.println(maxLngDiff, 6);

  display.setCursor(0, 50);
NEW SKETCH

  display.print(F("Status: "));
  display.println(stable ? "STABLE" : "UNSTABLE");
  display.display();

  return stable;
}

// ===== AVERAGE FUNCTION =====
double average(double arr[], int size) {
  double sum = 0;
  for (int i = 0; i < size; i++) sum += arr[i];
  return sum / size;
}

// ===== SEND SMS =====
void sendSMS(double lat, double lng) {
  String mapsLink = "https://maps.google.com/?q=" + String(lat, 6) + "," + String(lng, 6);
  String message = "✅ Stable GPS Fix Obtained!\nLocation:\n" + mapsLink;

  Serial.println(F("\n📤 Sending SMS..."));
  Serial.println("Message:");
  Serial.println(message);
  Serial.println("--------------------------------------");

  gsmSerial.println("AT+CMGF=1");
  delay(1000);
  gsmSerial.print("AT+CMGS=\"");
  gsmSerial.print(phoneNumber);
  gsmSerial.println("\"");
  delay(1000);
  gsmSerial.print(message);
  delay(500);
  gsmSerial.write(26); // CTRL+Z
  delay(5000);

  Serial.println(F("✅ SMS Sent Successfully!"));
  Serial.println("======================================\n");
}
