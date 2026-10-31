// bigmem.h — large work buffers go to the 8 MB PSRAM on the ESP32-S3, keeping
// the internal RAM for Wi-Fi, USB and the screens. Allocated once, never freed.
#pragma once
#include <stdlib.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
static inline void* bigAlloc(size_t n) {
  void* p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
  return p ? p : calloc(1, n);
}
#else
static inline void* bigAlloc(size_t n) { return calloc(1, n); }
#endif
template <class T> static inline T* bigOnce(T*& p) {
  if (!p) p = (T*)bigAlloc(sizeof(T));
  return p;
}
