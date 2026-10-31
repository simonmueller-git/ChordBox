// ChordBox — chord + bass instrument for the Ableton Move
//   4x4 keypad (a second one optional) + push encoder + 2.0" ST7789
//   plays into the Move as a class-compliant USB-MIDI device
//
//   CHORD  find progressions, play them from the pads, never quantised
//   BASS   basslines that follow your chords, locked to the Move's clock
//
// Both boards split the work the same way: one core runs the I/O loop (USB-MIDI
// in/out, keypads, clock, both programs) and never draws; the other core draws
// the screen. ESP32-S3: I/O on core 1, screen on core 0. Pico: I/O on core 0
// (where USB lives), screen on core 1.
//
// Build/flash: ./build.sh [esp32|pico] flash   (see README.md)

#include <Adafruit_TinyUSB.h>
#include <LittleFS.h>
#include "app.h"
#include "bass.h"
#include "clock.h"
#include "config.h"
#include "engine.h"
#include "harm.h"
#include "input.h"
#include "platform.h"
#include "ui.h"

namespace console { void poll(); }

Adafruit_USBD_MIDI usbMidi;

void midiOut(uint8_t st, uint8_t d1, uint8_t d2) {
  uint8_t pk[4] = {(uint8_t)(st >> 4), st, d1, d2};
  usbMidi.writePacket(pk);
}

// ---- simulated Move clock (console `clock 124`, `clock start`) --------------
static volatile float simBpmReq = 0;
static volatile uint8_t simTrReq = 0;
static float simBpm = 0;
static int64_t simNext = 0;

void simClock(float bpm) { simBpmReq = bpm; }
void simTransport(uint8_t st) { simTrReq = st; }

static void simService(int64_t now) {
  if (simTrReq) {
    clk::midiRealtime(simTrReq, now);
    simTrReq = 0;
  }
  if (simBpmReq != simBpm) {
    simBpm = simBpmReq;
    simNext = now;
  }
  if (simBpm > 0) {
    int64_t per = (int64_t)(60e6 / (simBpm * 24));
    while (now >= simNext) {
      clk::midiRealtime(0xF8, simNext);
      simNext += per;
    }
  }
}

// ---- MIDI from the Move: clock and transport only (notes are ignored) -------
static void readMidiIn(int64_t now) {
  uint8_t pk[4];
  while (usbMidi.readPacket(pk)) {
    uint8_t cin = pk[0] & 0x0F;
    if (cin == 0x0F || cin == 0x05) {
      uint8_t b = pk[1];
      if (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC) clk::midiRealtime(b, now);
    } else if (cin == 0x03 && pk[1] == 0xF2) {
      clk::songPosition(pk[2] | (pk[3] << 7));
    } else if (cin == 0x09 || cin == 0x0C) {           // note on, program change
      app::midiIn(pk[1], pk[2], pk[3]);
      if (cin == 0x09) harm::midiIn(pk[1], pk[2], pk[3], now);
    } else if (cin == 0x08) {                          // note off: LISTEN needs the lengths
      harm::midiIn(pk[1], pk[2], pk[3], now);
    }
  }
}

static void startUsbDevice() {
  TinyUSBDevice.setManufacturerDescriptor("ChordBox");
  TinyUSBDevice.setProductDescriptor("ChordBox chords");
  usbMidi.setStringDescriptor("ChordBox");
  usbMidi.begin();
  if (TinyUSBDevice.mounted()) {             // re-enumerate so the host sees MIDI
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

static void drainEvents();

// one pass of everything time-critical
static void ioStep() {
  int64_t now = nowUs();
  uint32_t ms = millis();
  readMidiIn(now);
  simService(now);
  clk::poll(now);
  static uint32_t lastScan = 0;
  if (ms != lastScan) {                      // keypads + knob at 1 kHz
    lastScan = ms;
    input::poll(ms);
  }
  drainEvents();
  engine::tick(now, ms);
  bass::poll(nowUs(), ms);
  app::setMoveUp(TinyUSBDevice.mounted());
  app::tick(now, ms);
}

// =================================================================== Pico ==
#if defined(ARDUINO_ARCH_RP2040)
#include <pico/util/queue.h>

static queue_t evq;
critical_section_t g_modelCs;
static volatile bool uiGo = false;

void postEvent(const Event& e) {
  Event q = e;
  q.tUs = micros();
  queue_try_add(&evq, &q);
}

uint32_t maxWaitUs = 0;                       // worst input -> handling delay, for `perf`

static void drainEvents() {
  Event e;
  while (queue_try_remove(&evq, &e)) {
    uint32_t wait = micros() - e.tUs;
    if (wait > maxWaitUs) maxWaitUs = wait;
    app::handle(e);
  }
}

void setup() {
  critical_section_init(&g_modelCs);
  queue_init(&evq, sizeof(Event), 64);
  startUsbDevice();
  Serial.begin(115200);
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  engine::begin();
  clk::begin();
  bass::begin();
  input::begin();
  app::begin();
  uiGo = true;                               // core 1 may start drawing
}

void loop() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif
  ioStep();
  console::poll();
}

// core 1: the screen, nothing else
void setup1() {
  while (!uiGo) delay(1);
  ui::begin();
}

void loop1() { ui::service(); }

// ================================================================== ESP32 ==
#elif defined(ARDUINO_ARCH_ESP32)

static QueueHandle_t evq;
uint32_t maxWaitUs = 0;

void postEvent(const Event& e) {
  Event q = e;
  q.tUs = micros();
  xQueueSend(evq, &q, 0);
}

static void drainEvents() {
  Event e;
  while (xQueueReceive(evq, &e, 0) == pdTRUE) {
    uint32_t wait = micros() - e.tUs;
    if (wait > maxWaitUs) maxWaitUs = wait;
    app::handle(e);
  }
}

static void ioTask(void*) {
  for (;;) {
    ioStep();
    vTaskDelay(1);                           // 1 kHz; USB-MIDI flushes every frame
  }
}

void setup() {
  Serial0.begin(115200);                     // COM port console
  Serial.setTxTimeoutMs(5);                  // never stall when nobody reads USB serial
  startUsbDevice();
  evq = xQueueCreate(64, sizeof(Event));
  LittleFS.begin(true);
  engine::begin();
  clk::begin();
  bass::begin();
  input::begin();
  app::begin();
  ui::begin();                               // UI task on core 0
  Serial0.println("ChordBox up");
  xTaskCreatePinnedToCore(ioTask, "io", 12288, nullptr, 5, nullptr, 1);
}

void loop() { vTaskDelay(portMAX_DELAY); }
#endif
