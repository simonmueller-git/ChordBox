// ui_pico.cpp — ST7789 on the Raspberry Pi Pico (SPI0), drawn on core 1 so
// the I/O loop on core 0 (USB-MIDI, keypads, clock, both programs) never
// waits for a frame. A mutex guards the framebuffer against console screenshots
// taken from core 0; display setting changes are handed to core 1 to apply.
#if defined(ARDUINO_ARCH_RP2040)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <pico/mutex.h>
#include "app.h"
#include "config.h"
#include "engine.h"
#include "types.h"
#include "ui.h"
#include "ui_common.h"

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
    p.invert = true;
    p.rgb_order = bgr;
    p.dlen_16bit = false;
    p.bus_shared = false;
    panel.config(p);
    auto l = light.config();          // BLK: backlight, PWM for brightness
    l.pin_bl = PIN_LCD_BL;
    l.invert = false;
    l.freq = 12000;
    light.config(l);
    panel.setLight(&light);
    setPanel(&panel);
  }
};

static LGFX lcd;
static LGFX_Sprite fb(&lcd);
static LcdPrefs lp;
static UiModel cur;
static mutex_t fbMutex;
static volatile bool ready = false, lcdPending = false;
static bool flipApplied = false;
static uint32_t lastSeq = 0, nextDeadline = 0, lastFrame = 0;
static uint32_t tRender = 0, tPush = 0;

static void applyLcd() {
  lcd.setup(lp.bgr, lp.mhz * 1000000u);
  lcd.init();
  lcd.setRotation(lcdRotation(lp, flipApplied));
  lcd.invertDisplay(lp.invert);
  lcd.fillScreen(0);
  lcd.setBrightness(lp.bright);
}

void ui::begin() {                              // core 1
  mutex_init(&fbMutex);
  lcdPrefsLoad(lp);
  applyLcd();
  uiInitSprite(fb);
  ready = true;
}

void ui::service() {                            // core 1, forever
  uint32_t now = millis();
  if (lcdPending) {
    lcdPending = false;
    applyLcd();
    lastSeq = 0;
  }
  modelRead(cur);
  if (cur.screenFlip != flipApplied) {           // MORE: SCREEN FLIP
    flipApplied = cur.screenFlip;
    mutex_enter_blocking(&fbMutex);
    lcd.setRotation(lcdRotation(lp, flipApplied));
    mutex_exit(&fbMutex);
    lastSeq = 0;
  }
  bool timed = nextDeadline && now >= nextDeadline;
  bool anim = uiAnimating(cur, now) && now - lastFrame >= UI_ANIM_MS;
  if ((cur.seq == lastSeq || now - lastFrame < UI_FRAME_MS) && !timed && !anim) {
    delay(1);
    return;
  }
  lastFrame = now;
  lastSeq = cur.seq;
  mutex_enter_blocking(&fbMutex);
  uint32_t t0 = micros();
  uiRender(fb, cur, now);
  uint32_t t1 = micros();
  lcd.startWrite();
  fb.pushSprite(0, 0);
  lcd.endWrite();
  tRender = t1 - t0;
  tPush = micros() - t1;
  mutex_exit(&fbMutex);
  nextDeadline = 0;
  for (uint32_t t : {cur.focusUntil, cur.toastUntil})
    if (t > now && (!nextDeadline || t < nextDeadline)) nextDeadline = t;
}

void ui::shot(Print& out) {                     // core 0 (console): wait for a whole frame
  if (!ready) return;
  mutex_enter_blocking(&fbMutex);
  shotSprite(fb, out);
  mutex_exit(&fbMutex);
}

void ui::lcdCmd(const String& arg, Print& out) {
  if (lcdPrefsCommand(lp, arg, out)) lcdPending = true;
}

extern uint32_t maxWaitUs;

void ui::oledCmd(const String&, Print& out) { out.println("oled: ESP32 only"); }
void ui::oledShot(Print& out) { out.println("oled: ESP32 only"); }

void ui::perf(Print& out) {
  out.printf("screen (core 1): render %lu us  push %lu us   input wait (core 0) worst %lu us\n",
             (unsigned long)tRender, (unsigned long)tPush, (unsigned long)maxWaitUs);
  maxWaitUs = 0;
}

#endif  // ARDUINO_ARCH_RP2040
