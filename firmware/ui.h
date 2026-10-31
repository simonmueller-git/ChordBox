// ui.h — screen: ST7789 320x240 landscape
#pragma once
#include <Arduino.h>

namespace ui {

void begin();                 // LCD + framebuffer (ESP32: also starts the UI task)
void service();               // Pico: call from loop(); draws when something changed
void shot(Print& out);        // dump the framebuffer (RGB565, big endian)
void lcdCmd(const String& arg, Print& out);   // invert / bgr / rot / mhz N
void perf(Print& out);        // last render and push times
void oledCmd(const String& arg, Print& out);  // the bass OLED: status, flip, offset N, on/off
void oledShot(Print& out);    // the OLED's picture as 128x64 bits (8 KB of 0/1 rows)

}  // namespace ui
