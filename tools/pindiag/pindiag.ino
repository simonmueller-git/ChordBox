// Pin diagnostic: what is each Pico GPIO wired to?
#include <Arduino.h>
const int PINS[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,26,27,28};
const int N = sizeof(PINS)/sizeof(PINS[0]);
int readWith(int p, int mode) { pinMode(p, mode); delay(4); return digitalRead(p); }
void report() {
  Serial.println("=== pull test (U = reads with pull-up, D = with pull-down) ===");
  for (int i = 0; i < N; i++) {
    int p = PINS[i];
    int u = readWith(p, INPUT_PULLUP), d = readWith(p, INPUT_PULLDOWN);
    const char* what = (u && !d) ? "floating" : (!u && !d) ? "LOW: wired to GND or a load" : (u && d) ? "HIGH: driven/pulled high" : "odd";
    Serial.printf("GP%-2d U%d D%d  %s\n", p, u, d, what);
    pinMode(p, INPUT);
  }
  Serial.println("=== links (drive one pin, see which others follow) ===");
  for (int i = 0; i < N; i++) {
    int p = PINS[i];
    for (int j = 0; j < N; j++) if (j != i) pinMode(PINS[j], INPUT_PULLDOWN);
    pinMode(p, OUTPUT); digitalWrite(p, HIGH); delay(3);
    String hi;
    for (int j = 0; j < N; j++) if (j != i && digitalRead(PINS[j])) hi += " GP" + String(PINS[j]);
    for (int j = 0; j < N; j++) if (j != i) pinMode(PINS[j], INPUT_PULLUP);
    digitalWrite(p, LOW); delay(3);
    String lo;
    for (int j = 0; j < N; j++) if (j != i && !digitalRead(PINS[j])) lo += " GP" + String(PINS[j]);
    pinMode(p, INPUT);
    if (hi.length() || lo.length()) Serial.printf("GP%-2d high->%s | low->%s\n", p, hi.c_str(), lo.c_str());
  }
  for (int j = 0; j < N; j++) pinMode(PINS[j], INPUT);
  Serial.printf("VSYS %.2f V\n", analogRead(A3) * 3.3f * 3 / 1023.0f);
  Serial.println("=== end ===");
}
void setup() { Serial.begin(115200); }
void loop() {
  if (Serial.available()) { while (Serial.available()) Serial.read(); report(); }
  delay(10);
}
