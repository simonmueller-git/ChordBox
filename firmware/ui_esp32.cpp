// ui_esp32.cpp — ST7789 on the ESP32-S3: panel on SPI3, UI task on core 0.
// Drawing lives in ui_draw.cpp; settings and screenshots in ui_common.cpp.
#if defined(ARDUINO_ARCH_ESP32)

#define LGFX_USE_V1
#include "bigmem.h"
#include <LovyanGFX.hpp>
#include "app.h"
#include "config.h"
#include "engine.h"
#include "types.h"
#include "ui.h"
#include "ui_common.h"

namespace console { void poll(); }
#if HAS_EXTRAS
#include "input.h"
void oledBegin();
#endif

class LGFX : public lgfx::LGFX_Device {
 public:
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  void setup(bool bgr, uint32_t hz) {
    auto b = bus.config();
    b.spi_host = LCD_SPI_HOST;
    b.spi_mode = 0;
    b.freq_write = hz;
    b.freq_read = 16000000;
    b.spi_3wire = true;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = PIN_LCD_SCK;
    b.pin_mosi = PIN_LCD_MOSI;
    b.pin_miso = -1;
    b.pin_dc = PIN_LCD_DC;
    bus.config(b);
    panel.setBus(&bus);
    auto p = panel.config();
    p.pin_cs = PIN_LCD_CS;
    p.pin_rst = PIN_LCD_RST;
    p.pin_busy = -1;
    p.panel_width = 240;
    p.panel_height = 320;
    p.offset_x = 0;
    p.offset_y = 0;
    p.readable = false;
    p.invert = true;          // IPS ST7789 panels want inversion on
    p.rgb_order = bgr;
    p.dlen_16bit = false;
    p.bus_shared = false;
    panel.config(p);
    auto l = light.config();          // BLK: backlight, PWM for brightness
    l.pin_bl = PIN_LCD_BL;
    l.invert = false;
    l.freq = 12000;
    l.pwm_channel = 7;
    light.config(l);
    panel.setLight(&light);
    setPanel(&panel);
  }
};

static LGFX lcd;
static LGFX_Sprite fb(&lcd);
static SemaphoreHandle_t fbLock;
static LcdPrefs lp;
static UiModel* curP = nullptr;                    // in PSRAM
static uint32_t tRender = 0, tPush = 0;
static bool flipApplied = false;

static void uiTask(void*) {
  UiModel& cur = *bigOnce(curP);
  uint32_t lastSeq = 0, nextDeadline = 0, lastFrame = 0;
  uint8_t ledState = 0xFF;
#if HAS_EXTRAS
  bool ledOff = input::knob2();                 // the second knob's push sits on the light's pin
#else
  bool ledOff = false;
#endif
  for (;;) {
    console::poll();
    uint32_t now = millis();
    modelRead(cur);
    if (cur.screenFlip != flipApplied) {         // MORE: SCREEN FLIP
      flipApplied = cur.screenFlip;
      xSemaphoreTake(fbLock, portMAX_DELAY);
      lcd.setRotation(lcdRotation(lp, flipApplied));
      xSemaphoreGive(fbLock);
      lastSeq = 0;
    }
    bool timed = nextDeadline && now >= nextDeadline;
    bool anim = uiAnimating(cur, now) && now - lastFrame >= UI_ANIM_MS;
    if (cur.seq != lastSeq || timed || anim) {
      lastFrame = now;
      lastSeq = cur.seq;
      xSemaphoreTake(fbLock, portMAX_DELAY);
      uint32_t t0 = micros();
      uiRender(fb, cur, now);
      uint32_t t1 = micros();
      lcd.startWrite();
      fb.pushSprite(0, 0);
      lcd.endWrite();
      tRender = t1 - t0;
      tPush = micros() - t1;
      xSemaphoreGive(fbLock);
      nextDeadline = 0;
      for (uint32_t t : {cur.focusUntil, cur.toastUntil})
        if (t > now && (!nextDeadline || t < nextDeadline)) nextDeadline = t;
    }
#ifdef RGB_BUILTIN
    uint8_t s = (cur.moveUp ? 1 : 0) | (cur.clk.running ? 2 : 0);
    if (s != ledState && !ledOff) {
      ledState = s;
      rgbLedWrite(RGB_BUILTIN, (s & 2) ? 8 : 0, (s & 1) ? 6 : 0, 0);
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(UI_FRAME_MS / 2));
  }
}

static void applyLcd() {
  lcd.setup(lp.bgr, lp.mhz * 1000000u);
  lcd.init();
  lcd.setRotation(lcdRotation(lp, flipApplied));
  lcd.invertDisplay(lp.invert);
  lcd.fillScreen(0);
  lcd.setBrightness(lp.bright);
}

void ui::begin() {
  lcdPrefsLoad(lp);
  applyLcd();
  uiInitSprite(fb);
  fbLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 1, nullptr, 0);
#if HAS_EXTRAS
  oledBegin();
#endif
}

void ui::service() {}

void ui::shot(Print& out) {
  xSemaphoreTake(fbLock, portMAX_DELAY);
  shotSprite(fb, out);
  xSemaphoreGive(fbLock);
}

void ui::lcdCmd(const String& arg, Print& out) {
  if (!lcdPrefsCommand(lp, arg, out)) return;
  xSemaphoreTake(fbLock, portMAX_DELAY);
  applyLcd();
  xSemaphoreGive(fbLock);
}

void ui::perf(Print& out) { out.printf("render %lu us  push %lu us\n", (unsigned long)tRender, (unsigned long)tPush); }

#endif  // ARDUINO_ARCH_ESP32
