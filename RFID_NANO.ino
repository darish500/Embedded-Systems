#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal.h>

#define SS_PIN 10
#define RST_PIN 9
#define BUZZER_PIN 4
#define LCD_CONTRAST 6  // PWM pin for LCD contrast
#define Re_pin 3
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal lcd(A0, A1, A2, A3, A4, A5);

// ✅ Authorized UIDs (replace with your actual card UIDs)
byte card1[4] = {0x1D, 0x2B, 0x0D, 0x06};
byte card2[4] = {0x19, 0x95, 0x73, 0xB3};
byte card3[4] = {0x98, 0x76, 0x54, 0x32};

void setup() {
  Serial.begin(9600);
  SPI.begin();
  rfid.PCD_Init();

  lcd.begin(16, 2);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LCD_CONTRAST, OUTPUT);
  analogWrite(LCD_CONTRAST, 120); // Adjust for desired contrast (0–255)
  pinMode(Re_pin, OUTPUT);
  digitalWrite(Re_pin,LOW);
  lcd.print("RFID Auth System");
  delay(2000);
  lcd.clear();
  lcd.print("Scan your card");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  Serial.print("UID: ");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();

  if (checkCard(rfid.uid.uidByte)) {
    successBeep();
    lcd.clear();
    lcd.print("Access Granted");
    Serial.println("Access Granted");
    delay(500);
    lcd.clear();
    lcd.print("Door Open");
    digitalWrite(Re_pin,HIGH);
    delay(3000);
    digitalWrite(Re_pin , LOW);
    delay(50);
    lcd.clear();
  } else {
    lcd.clear();
    lcd.print("Access Denied");
     accessDenied();
    Serial.println("Access Denied");
    delay(800);
  }

  
  lcd.clear();
  lcd.print("Scan your card");
  delay(200);
  rfid.PICC_HaltA();
}

bool checkCard(byte *uid) {
  if (compareUID(uid, card1)) return true;
  if (compareUID(uid, card2)) return true;
  if (compareUID(uid, card3)) return true;
  return false;
}

bool compareUID(byte *uid, byte *valid) {
  for (byte i = 0; i < 4; i++) {
    if (uid[i] != valid[i]) return false;
  }
  return true;
}

void successBeep() {
digitalWrite(4, HIGH);
delay(80);
digitalWrite(4,LOW);
delay(50);
digitalWrite(4, HIGH);
delay(80);
digitalWrite(4,LOW);
delay(50);
digitalWrite(4, HIGH);
delay(80);
digitalWrite(4,LOW);
delay(50);
}
void accessDenied(){
  digitalWrite(4,HIGH);
  delay(500);
  digitalWrite(4,LOW);
  delay(150);
  digitalWrite(4,HIGH);
  delay(500);
  digitalWrite(4,LOW);
  delay(150);

}
