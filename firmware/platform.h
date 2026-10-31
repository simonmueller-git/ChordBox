// platform.h — the few things the engine needs from the chip
#pragma once
#include <Arduino.h>

#if defined(ARDUINO_ARCH_RP2040)
  #include <pico/time.h>
  static inline int64_t nowUs() { return (int64_t)time_us_64(); }
  static inline uint32_t hwRandom() { return rp2040.hwrand32(); }
#elif defined(ARDUINO_ARCH_ESP32)
  #include <esp_random.h>
  #include <esp_timer.h>
  static inline int64_t nowUs() { return esp_timer_get_time(); }
  static inline uint32_t hwRandom() { return esp_random(); }
#else  // host tests (tests/host/shim)
  int64_t nowUs();
  uint32_t hwRandom();
#endif

// the published UI model is written by the I/O loop and copied by the screen,
// which runs on the other core on both boards
#if defined(ARDUINO_ARCH_ESP32)
  #define MODEL_LOCK_DECL static portMUX_TYPE modelMux = portMUX_INITIALIZER_UNLOCKED;
  #define MODEL_LOCK() portENTER_CRITICAL(&modelMux)
  #define MODEL_UNLOCK() portEXIT_CRITICAL(&modelMux)
#elif defined(ARDUINO_ARCH_RP2040)
  #include <pico/critical_section.h>
  extern critical_section_t g_modelCs;           // initialised in setup(), before core 1 draws
  #define MODEL_LOCK_DECL
  #define MODEL_LOCK() critical_section_enter_blocking(&g_modelCs)
  #define MODEL_UNLOCK() critical_section_exit(&g_modelCs)
#else
  #define MODEL_LOCK_DECL
  #define MODEL_LOCK()
  #define MODEL_UNLOCK()
#endif

// hand an input event to the engine from any core/task (defined per platform in firmware.ino)
struct Event;
void postEvent(const Event& e);

// console: pretend to be the Move's MIDI clock (bpm 0 = off)
void simClock(float bpm);
void simTransport(uint8_t status);   // 0xFA start, 0xFB continue, 0xFC stop
