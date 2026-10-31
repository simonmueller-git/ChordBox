// Host test: the real keypad scan (firmware/input.cpp) against a simulated
// 4x4 keypad wired as in the build sheet: the keypad's pins 1-8 (C1-C4, then
// R1-R4) on KP_LINES in order. Pins are simulated: pull-ups, lines pulled LOW,
// and keys that join two wires.
//
//   clang++ -std=c++17 -O1 -w -DCHORDBOX_HOST_GPIO -I tests/host/shim -I firmware \
//     tests/host/test_input.cpp firmware/input.cpp -o /tmp/chordbox_input && /tmp/chordbox_input
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../firmware/config.h"
#include "../../firmware/input.h"
#include "../../firmware/types.h"
#include "LittleFS.h"

std::map<std::string, std::string> g_files;
LittleFSClass LittleFS;
static int64_t g_us = 1000000;
int64_t nowUs() { return g_us; }
uint32_t hwRandom() { return 1; }
uint32_t millis() { return (uint32_t)(g_us / 1000); }

// ---- simulated pins
static std::map<int, int> g_mode, g_level;
static std::set<std::pair<int, int>> g_keysDown;   // wire pairs joined by pressed keys
static std::set<int> g_held;                       // buttons held to GND (knob push, KEY0)
static bool g_knob2 = false;                       // the second knob's board (its pull-ups) is fitted

void pinMode(uint8_t p, uint8_t m) { g_mode[p] = m; }
void digitalWrite(uint8_t p, uint8_t v) { g_level[p] = v; }
static bool pulledLow(int p) { return (g_mode[p] == OUTPUT && g_level[p] == LOW) || g_held.count(p); }
int digitalRead(uint8_t p) {                       // LOW if any wire joined to p is pulled LOW
  if (g_mode[p] == INPUT_PULLDOWN && !(g_knob2 && (p == PIN_ENC2_A || p == PIN_ENC2_B || p == PIN_ENC2_SW)))
    return LOW;                                    // nothing there to beat the pull-down
  std::set<int> seen{p};
  std::vector<int> todo{p};
  while (!todo.empty()) {
    int q = todo.back();
    todo.pop_back();
    if (pulledLow(q)) return LOW;
    for (auto& k : g_keysDown) {
      int o = k.first == q ? k.second : k.second == q ? k.first : -1;
      if (o >= 0 && seen.insert(o).second) todo.push_back(o);
    }
  }
  return HIGH;
}
void delayMicroseconds(unsigned) {}
int digitalPinToInterrupt(int p) { return p; }
void attachInterrupt(int, void (*)(), int) {}
void noInterrupts() {}
void interrupts() {}

static std::vector<Event> g_ev;
void postEvent(const Event& e) { g_ev.push_back(e); }

static int fails = 0;
#define CHECK(c, ...)                              \
  do {                                             \
    printf("%s  ", (c) ? "ok  " : "FAIL");         \
    printf(__VA_ARGS__);                           \
    printf("\n");                                  \
    if (!(c)) fails++;                             \
  } while (0)

static int g_pin8 = KP_LINES[7];                   // where keypad pin 8 (row R4) is wired
static void key(int row, int col, bool down) {    // row 0 = 1 2 3 A, col 0 = 1 4 7 *
  std::pair<int, int> k{KP_LINES[col], row == 3 ? g_pin8 : KP_LINES[4 + row]};
  if (down) g_keysDown.insert(k);
  else g_keysDown.erase(k);
}
static void runMs(int ms) {
  for (int i = 0; i < ms; i++) {
    g_us += 1000;
    input::poll(millis());
  }
}
static int count(EvType t, int a = -1) {
  int n = 0;
  for (auto& e : g_ev)
    if (e.type == t && (a < 0 || e.a == a)) n++;
  return n;
}

int main() {
  input::begin();
  runMs(20);
  CHECK(g_ev.empty(), "power-up: no events with nothing pressed");

  // every key, one at a time: the right pad, once, within 2 ms of the press
  int right = 0, fast = 0;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      g_ev.clear();
      key(r, c, true);
      runMs(2);
      if (count(EV_PAD_DOWN, r * 4 + c) == 1) fast++;
      runMs(20);
      key(r, c, false);
      runMs(20);
      if (g_ev.size() == 2 && count(EV_PAD_DOWN, r * 4 + c) == 1 && count(EV_PAD_UP, r * 4 + c) == 1) right++;
    }
  CHECK(right == 16, "all 16 keys play their own pad (1 2 3 A = pads 1-4, * 0 # D = pads 13-16): %d/16", right);
  CHECK(fast == 16, "each press arrives within 2 ms: %d/16", fast);

  // two keys held together, same row and same column: exactly those two pads
  g_ev.clear();
  key(1, 1, true);
  key(1, 2, true);
  key(2, 1, false);
  runMs(20);
  bool rowPair = count(EV_PAD_DOWN) == 2 && count(EV_PAD_DOWN, 5) && count(EV_PAD_DOWN, 6);
  key(1, 1, false);
  key(1, 2, false);
  runMs(20);
  g_ev.clear();
  key(0, 3, true);
  key(3, 3, true);
  runMs(20);
  bool colPair = count(EV_PAD_DOWN) == 2 && count(EV_PAD_DOWN, 3) && count(EV_PAD_DOWN, 15);
  key(0, 3, false);
  key(3, 3, false);
  runMs(20);
  CHECK(rowPair && colPair, "two keys at once (same row, same column): only those two pads");

  // contact bounce on release doesn't retrigger
  g_ev.clear();
  key(2, 2, true);
  runMs(10);
  key(2, 2, false);
  runMs(2);
  key(2, 2, true);                                // bounce
  runMs(1);
  key(2, 2, false);
  runMs(20);
  CHECK(count(EV_PAD_DOWN) == 1 && count(EV_PAD_UP) == 1, "a bouncing release gives one press and one release");

  // KEYPAD SETUP: a key arrives as its wire pair, and a new map moves it
  g_ev.clear();
  input::calibrate(true);
  key(3, 0, true);                                // the * key
  runMs(5);
  key(3, 0, false);
  runMs(20);
  int pair = g_ev.size() == 1 && g_ev[0].type == EV_RAW_DOWN ? g_ev[0].a : -1;
  int8_t pads[16];
  memset(pads, -1, sizeof pads);
  pads[0] = pair;                                 // "press pad 1": the * key
  input::setPads(pads, 16);
  input::calibrate(false);
  g_ev.clear();
  key(3, 0, true);
  runMs(5);
  key(3, 0, false);
  runMs(20);
  CHECK(pair >= 0 && count(EV_PAD_DOWN, 0) == 1 && count(EV_PAD_DOWN) == 1,
        "KEYPAD SETUP: the * key (pair %d) now plays pad 1", pair);
  input::resetMap();

  // KEY0: debounced press and release
  g_ev.clear();
  g_held.insert(PIN_KEY0);
  runMs(10);
  g_held.erase(PIN_KEY0);
  runMs(10);
  CHECK(count(EV_KEY) == 2 && g_ev[0].b == 1 && g_ev[1].b == 0, "KEY0: one press, one release");

  // ---- wiring v1: keypad pin 8 still on GPIO 47, no second knob: row 4 still plays
  g_pin8 = KP_ALT_LINE;
  int row4 = 0;
  for (int c = 0; c < 4; c++) {
    g_ev.clear();
    key(3, c, true);
    runMs(5);
    key(3, c, false);
    runMs(20);
    if (count(EV_PAD_DOWN, 12 + c) == 1 && count(EV_PAD_DOWN) == 1) row4++;
  }
  CHECK(row4 == 4 && !input::knob2(), "v1 wiring (pin 8 on GPIO 47): * 0 # D still play pads 13-16: %d/4", row4);
  g_pin8 = KP_LINES[7];

  // ---- a keypad map saved by the old firmware (8 wires, pin 8 on 47) carries over
  {
    uint8_t v3[5 + 128];
    memset(v3, 0, sizeof v3);
    v3[0] = 3, v3[1] = 1, v3[4] = 8;
    memset(v3 + 5, 0xFF, 128);
    auto oldIdx = [](int i, int j) { return i * (2 * 8 - i - 1) / 2 + (j - i - 1); };
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) v3[5 + oldIdx(c, 4 + r)] = (uint8_t)(15 - (r * 4 + c));   // a mirrored map
    g_files["/input.bin"] = std::string((const char*)v3, sizeof v3);
    input::begin();
    runMs(20);
    int ok = 0;
    for (int pin8 : {(int)KP_LINES[7], KP_ALT_LINE}) {
      g_pin8 = pin8;
      g_ev.clear();
      key(3, 0, true);                             // the * key: pad 16 - 13 = 3 in the mirrored map
      runMs(5);
      key(3, 0, false);
      runMs(20);
      if (count(EV_PAD_DOWN, 3) == 1 && count(EV_PAD_DOWN) == 1) ok++;
    }
    g_ev.clear();
    key(0, 0, true);                               // the 1 key: pad 15
    runMs(5);
    key(0, 0, false);
    runMs(20);
    CHECK(ok == 2 && count(EV_PAD_DOWN, 15) == 1, "an old saved keypad map still works, pin 8 on GPIO 0 or 47");
    g_files.erase("/input.bin");
    g_pin8 = KP_LINES[7];
  }

  // ---- wiring v2: the second knob is found, and GPIO 47 is its S2, not a keypad wire
  g_knob2 = true;
  input::resetMap();
  input::begin();
  runMs(20);
  g_ev.clear();
  CHECK(input::knob2(), "the second knob is found by its pull-ups");
  g_pin8 = KP_ALT_LINE;
  key(3, 1, true);
  runMs(5);
  key(3, 1, false);
  runMs(20);
  CHECK(count(EV_PAD_DOWN) == 0, "with the knob fitted, GPIO 47 is no longer scanned as a keypad wire");
  g_pin8 = KP_LINES[7];
  key(3, 1, true);
  runMs(5);
  key(3, 1, false);
  runMs(20);
  CHECK(count(EV_PAD_DOWN, 13) == 1, "and keypad pin 8 on GPIO 0 plays row 4");

  // turn: one full quadrature cycle per detent, both ways
  auto phase = [](int a, int b) {
    if (a) g_held.erase(PIN_ENC2_A); else g_held.insert(PIN_ENC2_A);
    if (b) g_held.erase(PIN_ENC2_B); else g_held.insert(PIN_ENC2_B);
    runMs(1);
  };
  auto detent = [&](int dir) {
    const int cw[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    for (int i = 0; i < 4; i++) {
      int k = dir > 0 ? i : (2 - i + 4) % 4;
      if (dir < 0 && i == 3) k = 3;
      phase(cw[k][0], cw[k][1]);
    }
  };
  g_ev.clear();
  detent(1);
  detent(1);
  int fwd = 0;
  for (auto& e : g_ev)
    if (e.type == EV_TURN2) fwd += e.b;
  g_ev.clear();
  detent(-1);
  int back = 0;
  for (auto& e : g_ev)
    if (e.type == EV_TURN2) back += e.b;
  CHECK(std::abs(fwd) == 2 && back == -fwd / 2, "second knob: two detents one way (%d), one back (%d)", fwd, back);

  g_ev.clear();
  g_held.insert(PIN_ENC2_SW);
  runMs(10);
  g_held.erase(PIN_ENC2_SW);
  runMs(10);
  CHECK(count(EV_CLICK2) == 1 && count(EV_TURN2) == 0, "second knob: a tap is one click");

  g_ev.clear();
  g_held.insert(PIN_ENC2_SW);
  runMs(10);
  detent(1);
  g_held.erase(PIN_ENC2_SW);
  runMs(10);
  CHECK(count(EV_PUSH_TURN2) == 1 && count(EV_CLICK2) == 0, "second knob: push + turn, and no click after it");

  // the OLED's four buttons
  g_ev.clear();
  for (int i = 0; i < 4; i++) {
    g_held.insert(BTN_PINS[i]);
    runMs(10);
    g_held.erase(BTN_PINS[i]);
    runMs(10);
  }
  bool btns = count(EV_BTN) == 8;
  for (int i = 0; i < 8 && btns; i++) btns = g_ev[i].a == i / 2 && g_ev[i].b == (i % 2 ? 0 : 1);
  CHECK(btns, "OLED buttons K1-K4: a press and a release each, in order");

  printf("\n%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
