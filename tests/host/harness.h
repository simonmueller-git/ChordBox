// harness.h — run the real firmware logic on the Mac: time, flash, MIDI out
// and the keypad hardware are simulated here. Include once per test program.
#pragma once
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "../../firmware/app.h"
#include "../../firmware/bass.h"
#include "../../firmware/clock.h"
#include "../../firmware/engine.h"
#include "../../firmware/input.h"
#include "../../firmware/types.h"
#include "LittleFS.h"

std::map<std::string, std::string> g_files, g_prefs;
LittleFSClass LittleFS;

static int64_t g_us = 1000000;
uint32_t hwRandom() { return 0xC0FFEEu; }
int64_t nowUs() { return g_us; }
uint32_t millis() { return (uint32_t)(g_us / 1000); }

struct Msg { int64_t t; uint8_t st, d1, d2; };
static std::vector<Msg> g_midi;
void midiOut(uint8_t st, uint8_t d1, uint8_t d2) { g_midi.push_back({g_us, st, d1, d2}); }

void postEvent(const Event& e) { app::handle(e); }
void simClock(float) {}
void simTransport(uint8_t) {}

// ---- keypad hardware stand-in
static int g_keypads = 1;
static bool g_encRev = false, g_calib = false, g_flip = false;
static int8_t g_pairOfPad[32];
namespace input {
void begin() {}
void poll(uint32_t) {}
int keypads() { return g_keypads; }
void setKeypads(int n) { g_keypads = n; }
bool encReversed() { return g_encRev; }
void setEncReversed(bool r) { g_encRev = r; }
void calibrate(bool on) { g_calib = on; }
void setPads(const int8_t p[], int n) { memset(g_pairOfPad, -1, sizeof g_pairOfPad); memcpy(g_pairOfPad, p, n); }
void resetMap() {}
bool screenFlip() { return g_flip; }
void setScreenFlip(bool f) { g_flip = f; }
bool saveSettings() { g_files["/input.bin"] = "x"; return true; }
uint32_t padMask() { return 0; }
bool heldAtBoot() { return false; }
bool knob2() { return true; }
bool key0AtBoot() { return false; }
int knob2Mode() { return 0; }
void setKnob2Mode(int) {}
}  // namespace input

// ---- driving helpers
static void runFor(int64_t us) {                 // advance time in 250 us steps
  int64_t end = g_us + us;
  while (g_us < end) {
    g_us += 250;
    clk::poll(g_us);
    engine::tick(g_us, millis());
    bass::poll(g_us, millis());
    app::tick(g_us, millis());
  }
}

static void ev(EvType t, int a = 0, int b = 0) {
  Event e{t, (uint8_t)a, (int16_t)b, 0};
  app::handle(e);
  runFor(1000);
}

// a Move clock at `bpm` for `us`, starting with START when asked
static int64_t g_nextTick = 0;
static void moveClock(float bpm, int64_t us, bool start = false) {
  int64_t per = (int64_t)(60e6 / (bpm * 24));
  if (start) {
    clk::midiRealtime(0xFA, g_us);
    g_nextTick = g_us;
  }
  if (!g_nextTick) g_nextTick = g_us;
  int64_t end = g_us + us;
  while (g_us < end) {
    if (g_us >= g_nextTick) {
      clk::midiRealtime(0xF8, g_nextTick);
      g_nextTick += per;
    }
    g_us += 250;
    clk::poll(g_us);
    engine::tick(g_us, millis());
    bass::poll(g_us, millis());
    app::tick(g_us, millis());
  }
}

static void bootAll() {
  if (!g_files.count("/buttons2.bin")) g_files["/buttons2.bin"] = std::string("\0\1\2\3", 4);   // K1..K4 top to bottom
  engine::begin();
  clk::begin();
  bass::begin();
  app::begin();
  runFor(2000);
}

static std::vector<Msg> notesOn(int ch) {
  std::vector<Msg> v;
  for (auto& m : g_midi)
    if ((m.st & 0xF0) == 0x90 && m.d2 && (m.st & 15) == ch) v.push_back(m);
  return v;
}
static std::vector<Msg> notesOff(int ch) {
  std::vector<Msg> v;
  for (auto& m : g_midi)
    if (((m.st & 0xF0) == 0x80 || ((m.st & 0xF0) == 0x90 && !m.d2)) && (m.st & 15) == ch) v.push_back(m);
  return v;
}
static UiModel model() {
  UiModel m;
  modelRead(m);
  return m;
}

static int g_fails = 0;
#define CHECK(c, ...) do { if (c) printf("ok    "); else { printf("FAIL  "); g_fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
