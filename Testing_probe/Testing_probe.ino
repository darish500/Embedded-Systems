#include <Arduino.h>

#define SOIL_PIN  39

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  Serial.println("\n  Moisture test — with discharge resistor");
  Serial.println("  Probe in air first, then dip in water\n");
}

void loop() {
  long total = 0;
  for (int i = 0; i < 20; i++) {
    total += analogRead(SOIL_PIN);
    delay(30);
  }
  int avg = total / 20;
  Serial.printf("  Raw: %d\n", avg);
  delay(1000);
}