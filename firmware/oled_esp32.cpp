// oled_esp32.cpp — the bass OLED on I2C (GPIO 9 SCL, 10 SDA), its own task on core 0.
// Drawing lives in ui_oled.cpp. One small driver serves both usual chips: the
// SSD1306 (0.96") and the SH1106 (1.3", `oled offset 2`), both written a page
// (8 rows) at a time, and only the pages that changed.
#include "bigmem.h"
#include "config.h"
#if defined(ARDUINO_ARCH_ESP32) && HAS_EXTRAS

#include <LittleFS.h>
#include <Wire.h>
#include "app.h"
#include "ui.h"
#include "ui_oled.h"

namespace {

struct OledPrefs {
  uint8_t version = 1;
  uint8_t flip = 0;               // picture turned 180 degrees
  uint8_t offset = 0;             // first column: 0 SSD1306, 2 SH1106
  uint8_t on = 1;
  uint8_t contrast = 0xCF;
} OP;

lgfx::LGFX_Sprite spr;
UiModel* curP = nullptr;          // in PSRAM
uint8_t addr = 0;                 // 0 = no OLED found (yet)
uint8_t sent[8][128];             // what the OLED shows, per page
bool sentValid = false;
SemaphoreHandle_t lock;
uint32_t frames = 0, lastUs = 0;

void loadPrefs() {
  File f = LittleFS.open("/oled.bin", "r");
  if (!f) return;
  OledPrefs t;
  if (f.read((uint8_t*)&t, sizeof t) == sizeof t && t.version == OP.version) OP = t;
  f.close();
}

void savePrefs() {
  File f = LittleFS.open("/oled.bin", "w");
  if (!f) return;
  f.write((const uint8_t*)&OP, sizeof OP);
  f.close();
}

bool cmds(std::initializer_list<uint8_t> c) {
  Wire.beginTransmission(addr);
  Wire.write(0x00);               // control byte: commands follow
  for (uint8_t b : c) Wire.write(b);
  return Wire.endTransmission() == 0;
}

bool probe() {
  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return (addr = a), true;
  }
  addr = 0;
  return false;
}

bool oledInit() {
  sentValid = false;
  // commands both chips take (the SH1106 ignores the SSD1306-only ones)
  return cmds({0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x02,
               (uint8_t)(OP.flip ? 0xA0 : 0xA1), (uint8_t)(OP.flip ? 0xC0 : 0xC8), 0xDA, 0x12, 0x81, OP.contrast,
               0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, (uint8_t)(OP.on ? 0xAF : 0xAE)});
}

// the sprite, as the OLED's pages: byte x of page p = column x, rows 8p..8p+7 (bit 0 on top)
void pack(uint8_t out[8][128]) {
  memset(out, 0, 8 * 128);
  for (int y = 0; y < OLED_H; y++)
    for (int x = 0; x < OLED_W; x++)
      if (spr.readPixelValue(x, y)) out[y >> 3][x] |= 1 << (y & 7);
}

bool push(uint8_t frame[8][128]) {
  for (int p = 0; p < 8; p++) {
    if (sentValid && !memcmp(frame[p], sent[p], 128)) continue;
    for (int half = 0; half < 2; half++) {       // Wire's buffer holds 128 bytes: two writes a page
      int col = half * 64 + OP.offset;
      if (!cmds({(uint8_t)(0xB0 | p), (uint8_t)(col & 0x0F), (uint8_t)(0x10 | (col >> 4))})) return false;
      Wire.beginTransmission(addr);
      Wire.write(0x40);                          // control byte: data follows
      Wire.write(&frame[p][half * 64], 64);
      if (Wire.endTransmission() != 0) return false;
    }
    memcpy(sent[p], frame[p], 128);
  }
  sentValid = true;
  return true;
}

void task(void*) {
  UiModel& cur = *bigOnce(curP);
  static uint8_t frame[8][128];
  uint32_t lastSeq = 0, lastFrame = 0, lastProbe = 0;
  for (;;) {
    uint32_t now = millis();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!addr) {                                 // not there (yet): look every 2 s
      if (now - lastProbe >= 2000 || !lastProbe) {
        lastProbe = now;
        if (probe() && !oledInit()) addr = 0;
        lastSeq = 0;
      }
    } else if (OP.on) {
      modelRead(cur);
      bool anim = oledAnimating(cur, now) && now - lastFrame >= 50;
      if (cur.seq != lastSeq || anim) {
        lastSeq = cur.seq;
        lastFrame = now;
        uint32_t t0 = micros();
        oledRender(spr, cur, now);
        pack(frame);
        if (!push(frame)) addr = 0;              // unplugged: find it again
        lastUs = micros() - t0;
        frames++;
      }
    }
    xSemaphoreGive(lock);
    vTaskDelay(pdMS_TO_TICKS(addr ? 10 : 100));
  }
}

}  // namespace

void oledBegin() {
  loadPrefs();
  spr.setColorDepth(1);
  spr.createSprite(OLED_W, OLED_H);
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);
  Wire.setTimeOut(20);
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "oled", 6144, nullptr, 1, nullptr, 0);
}

void ui::oledCmd(const String& arg, Print& out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  String a = arg;
  a.trim();
  bool change = true;
  if (a == "flip") OP.flip = !OP.flip;
  else if (a.startsWith("offset")) OP.offset = constrain(a.substring(6).toInt(), 0, 4);
  else if (a == "off") OP.on = 0;
  else if (a == "on") OP.on = 1;
  else if (a.startsWith("contrast")) OP.contrast = constrain(a.substring(8).toInt(), 0, 255);
  else change = false;
  if (change) {
    savePrefs();
    if (addr && !oledInit()) addr = 0;
  }
  if (addr) out.printf("oled: found at 0x%02X  ", addr);
  else out.print("oled: not found (SCL 9, SDA 10, VCC 3V3)  ");
  out.printf("flip %d  offset %d (0 SSD1306, 2 SH1106)  %s  contrast %d  frames %lu  last %lu us\n", OP.flip,
             OP.offset, OP.on ? "on" : "off", OP.contrast, (unsigned long)frames, (unsigned long)lastUs);
  xSemaphoreGive(lock);
}

void ui::oledShot(Print& out) {                   // the picture as rows of # and .
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int y = 0; y < OLED_H; y++) {
    char row[OLED_W + 1];
    for (int x = 0; x < OLED_W; x++) row[x] = spr.readPixelValue(x, y) ? '#' : '.';
    row[OLED_W] = 0;
    out.println(row);
  }
  xSemaphoreGive(lock);
}

#endif
