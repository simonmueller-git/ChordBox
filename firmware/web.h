// web.h — the phone / iPad page (ESP32 only; on the Pico these do nothing)
//
// At power-up ChordBox joins the saved Wi-Fi network and serves its page at
// http://chordbox.local. With no network saved, or none in reach within 15 s,
// it opens its own hotspot "ChordBox" (password HOTSPOT_PASSWORD, config.h) at 192.168.4.1.
// The page talks over a WebSocket: state out ~10x a second, commands in as
// console-style lines ("pad 3 1", "act play", "set STRM 40", "scene 2"...),
// which become the same events the keypad and knob send.
#pragma once
#include <Arduino.h>

namespace web {

void begin();                               // starts the Wi-Fi task (core 0)
const char* status();                       // for MORE: "chordbox.local", "192.168.4.1", "JOINING", "OFF"
bool takeNotice(char* out, size_t cap);     // a message for the screen (connected, hotspot...)
void setCredentials(const char* ssid, const char* pass);   // saved; used from the next restart
void printInfo(Print& p);

struct Info { char ssid[33], url[32], pass[16]; bool hotspot, on; uint8_t phones; };
void info(Info& out);                       // for the PHONE LINK screen

}  // namespace web
