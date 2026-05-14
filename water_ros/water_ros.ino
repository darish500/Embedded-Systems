void setup() {
  pinMode(D2,INPUT);
  pinMode(D3,OUTPUT);
Serial.begin(115200);
}

void loop() {
  // put your main code here, to run repeatedly:
  Serial.print(digitalRead(D2));
if (digitalRead(D2) == 1){
  digitalWrite(D3 , HIGH);
  delay(50);
}
else{
  digitalWrite(D3 , LOW);
delay(50);
}
}
