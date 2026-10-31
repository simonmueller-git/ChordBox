// input.cpp — keypad, push encoder and the KEY0 button
//
// Keypad: every keypad wire is scanned against every other one. Each wire in
// turn is pulled LOW while the rest are read through pull-ups, so a key shows
// up as the pair of wires it joins, whatever the keypad's pinout. KEYPAD SETUP
// learns which pair is which pad; nothing is ever driven HIGH against LOW, so
// holding several keys can't short two outputs.
//
// Encoder gestures (one knob does a lot, so they are strict):
//   turn             EV_TURN         push + turn   EV_PUSH_TURN
//   tap (< HOLD_MS)  EV_CLICK        hold          EV_HOLD, then EV_RELEASE
// KEY0 (the button beside the knob): EV_KEY, b = 1 pressed / 0 released.
// Wiring v2 (ESP32): the second knob (EV_TURN2 / EV_PUSH_TURN2 / EV_CLICK2),
// found at power-up by its pull-ups, and the OLED's four buttons (EV_BTN).

#include "input.h"
#include <LittleFS.h>
#include "config.h"
#include "platform.h"
#include "types.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#endif

namespace {

#ifdef KP_ALT_LINE
constexpr int NLMAX = sizeof KP_LINES + 1;       // keypad wires, the alternative pin included
#else
constexpr int NLMAX = sizeof KP_LINES;
#endif
constexpr int NPAIR = NLMAX * (NLMAX - 1) / 2;   // possible keys: one per pair of wires
static_assert(NPAIR <= 128, "at most 16 keypad wires");

uint8_t lines[NLMAX];                            // the wires scanned (the alternative pin
int NL = 0;                                      // drops out when the second knob uses it)

struct Settings {
  uint8_t version = 4;
  uint8_t keypads = 1;
  uint8_t encReverse = 0;
  uint8_t screenFlip = 0;
  uint8_t nLines = NLMAX;        // the map below is only valid for this wiring
  uint8_t knob2 = 0;             // second knob: 0 found at power-up, 1 always, 2 never
  int8_t map[128];               // wire pair -> pad 0..31, -1 = not a key
};

Settings S;
bool calibrating = false;

// debounced key state per wire pair
uint32_t stable[4], lastRaw[4];
uint32_t changedAt[NPAIR];
uint32_t logicalMask = 0;

// the v2 parts
bool knob2Fitted = false;
int32_t enc2Accum = 0;
bool sw2Down = false, sw2Raw = false, turned2 = false, held2 = false;
uint32_t sw2ChangedAt = 0, sw2PressedAt = 0;
bool btnDown[4], btnRaw[4];
uint32_t btnChangedAt[4];

// encoder
int32_t encAccum = 0;           // counts not yet turned into detents
bool swDown = false, swRaw = false;
uint32_t swChangedAt = 0, swPressedAt = 0;
bool turned = false, holdSent = false;
bool bootHeld = false;

// KEY0
bool k0Down = false, k0Raw = false, k0AtBoot = false;
uint32_t k0ChangedAt = 0;

volatile uint32_t nClick = 0, nHold = 0, nKeyDown = 0, nTurn = 0, nSwDown = 0;

void post(EvType t, uint8_t a = 0, int16_t b = 0) {
  if (t == EV_CLICK) nClick++;
  if (t == EV_HOLD) nHold++;
  if (t == EV_KEY && b) nKeyDown++;
  if (t == EV_TURN || t == EV_PUSH_TURN) nTurn++;
  Event e{t, a, b, 0};
  postEvent(e);
}

// i < j. Independent of the number of wires, so adding one keeps every pair's index.
inline int pairIndex(int i, int j) { return j * (j - 1) / 2 + i; }

// keypad pin 8 may sit on its v2 pin (wire 7) or its v1 pin (wire 8): a key on
// one counts as the same key on the other
void twinPairs() {
#ifdef KP_ALT_LINE
  for (int i = 0; i < 7; i++) {
    int8_t& a = S.map[pairIndex(i, 7)];
    int8_t& b = S.map[pairIndex(i, 8)];
    if (a < 0) a = b;
    if (b < 0) b = a;
  }
#endif
}

// ---------------------------------------------------------------- encoder --
#if defined(ARDUINO_ARCH_ESP32)
pcnt_unit_handle_t unit = nullptr, unit2 = nullptr;
int lastCount = 0, lastCount2 = 0;

pcnt_unit_handle_t pcntQuad(int pinA, int pinB) {
  pcnt_unit_handle_t unit = nullptr;
  pcnt_unit_config_t ucfg = {};
  ucfg.low_limit = -30000;
  ucfg.high_limit = 30000;
  pcnt_new_unit(&ucfg, &unit);
  pcnt_glitch_filter_config_t filt = {};
  filt.max_glitch_ns = 1000;
  pcnt_unit_set_glitch_filter(unit, &filt);
  pcnt_chan_config_t ca = {};
  ca.edge_gpio_num = pinA;
  ca.level_gpio_num = pinB;
  pcnt_channel_handle_t chA, chB;
  pcnt_new_channel(unit, &ca, &chA);
  pcnt_chan_config_t cb = {};
  cb.edge_gpio_num = pinB;
  cb.level_gpio_num = pinA;
  pcnt_new_channel(unit, &cb, &chB);
  pcnt_channel_set_edge_action(chA, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
  pcnt_channel_set_level_action(chA, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  pcnt_channel_set_edge_action(chB, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
  pcnt_channel_set_level_action(chB, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  gpio_pullup_en((gpio_num_t)pinA);
  gpio_pullup_en((gpio_num_t)pinB);
  pcnt_unit_enable(unit);
  pcnt_unit_clear_count(unit);
  pcnt_unit_start(unit);
  return unit;
}

void encBegin() { unit = pcntQuad(PIN_ENC_A, PIN_ENC_B); }

int pcntTake(pcnt_unit_handle_t u, int& last) {   // counts since the last call
  int c = 0;
  pcnt_unit_get_count(u, &c);
  int d = c - last;
  last = c;
  if (c > 20000 || c < -20000) {
    pcnt_unit_clear_count(u);
    last = 0;
  }
  return d;
}

int encTake() { return pcntTake(unit, lastCount); }

#if HAS_EXTRAS
void enc2Begin() { unit2 = pcntQuad(PIN_ENC2_A, PIN_ENC2_B); }
int enc2Take() { return pcntTake(unit2, lastCount2); }
#endif

#else  // Pico: pin-change interrupts through a quadrature state table
volatile int32_t isrCount = 0;
volatile uint8_t qState = 0;
const int8_t QT[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void encIsr() {
  uint8_t s = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  qState = ((qState << 2) | s) & 0x0F;
  isrCount += QT[qState];
}

void encBegin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  qState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encIsr, CHANGE);
}

int encTake() {
  noInterrupts();
  int d = isrCount;
  isrCount = 0;
  interrupts();
  return d;
}

#if HAS_EXTRAS  // host tests: knob 2 decoded by polling
uint8_t q2 = 0;
void enc2Begin() {
  pinMode(PIN_ENC2_A, INPUT_PULLUP);
  pinMode(PIN_ENC2_B, INPUT_PULLUP);
  q2 = (digitalRead(PIN_ENC2_A) << 1) | digitalRead(PIN_ENC2_B);
}
int enc2Take() {
  uint8_t s = (digitalRead(PIN_ENC2_A) << 1) | digitalRead(PIN_ENC2_B);
  q2 = ((q2 << 2) | s) & 0x0F;
  return QT[q2];
}
#endif
#endif

void encPoll(uint32_t now) {
  encAccum += encTake() * (S.encReverse ? -1 : 1);
  int detents = encAccum / ENC_COUNTS_PER_DETENT;
  encAccum -= detents * ENC_COUNTS_PER_DETENT;

  bool raw = digitalRead(PIN_ENC_SW) == LOW;
  if (raw != swRaw) {
    swRaw = raw;
    swChangedAt = now;
  }
  if (raw != swDown && now - swChangedAt >= 4) {      // debounced edge
    swDown = raw;
    if (swDown) {
      nSwDown++;
      swPressedAt = now;
      turned = holdSent = false;
    } else if (!turned && !holdSent) {
      post(EV_CLICK);
    } else {
      post(EV_RELEASE);
    }
  }
  if (detents) {
    if (swDown) {
      turned = true;
      post(EV_PUSH_TURN, 0, detents);
    } else {
      post(EV_TURN, 0, detents);
    }
  }
  if (swDown && !turned && !holdSent && now - swPressedAt >= HOLD_MS) {
    holdSent = true;
    post(EV_HOLD);
  }
}

void keyPoll(uint32_t now) {
  bool raw = digitalRead(PIN_KEY0) == LOW;
  if (raw != k0Raw) {
    k0Raw = raw;
    k0ChangedAt = now;
  }
  if (raw != k0Down && now - k0ChangedAt >= 4) {
    k0Down = raw;
    post(EV_KEY, 0, raw ? 1 : 0);
  }
}

// ------------------------------------------------------------ the v2 parts --
#if HAS_EXTRAS
// the second knob's pull-ups (on its board, fed from 3V3) beat the chip's weak
// pull-down: S1 reads HIGH at rest when the knob is there
bool detectKnob2() {
  if (S.knob2) return S.knob2 == 1;
  pinMode(PIN_ENC2_A, INPUT_PULLDOWN);
  delayMicroseconds(300);
  bool found = digitalRead(PIN_ENC2_A) == HIGH;
  pinMode(PIN_ENC2_A, INPUT_PULLUP);
  return found;
}

void enc2Poll(uint32_t now) {
  enc2Accum += enc2Take();
  int detents = enc2Accum / ENC_COUNTS_PER_DETENT;
  enc2Accum -= detents * ENC_COUNTS_PER_DETENT;
  bool raw = digitalRead(PIN_ENC2_SW) == LOW;
  if (raw != sw2Raw) {
    sw2Raw = raw;
    sw2ChangedAt = now;
  }
  if (raw != sw2Down && now - sw2ChangedAt >= 4) {
    sw2Down = raw;
    if (sw2Down) {
      turned2 = held2 = false;
      sw2PressedAt = now;
      post(EV_KNOB2, 0, 1);
    } else {
      post(EV_KNOB2, 0, 0);
      if (!turned2 && !held2) post(EV_CLICK2);
    }
  }
  if (detents) {
    if (sw2Down) turned2 = true;
    post(sw2Down ? EV_PUSH_TURN2 : EV_TURN2, 0, detents);
  }
  if (sw2Down && !turned2 && !held2 && now - sw2PressedAt >= 500) {   // a long press
    held2 = true;
    post(EV_HOLD2);
  }
}

void btnPoll(uint32_t now) {
  for (int i = 0; i < 4; i++) {
    bool raw = digitalRead(BTN_PINS[i]) == LOW;
    if (raw != btnRaw[i]) {
      btnRaw[i] = raw;
      btnChangedAt[i] = now;
    }
    if (raw != btnDown[i] && now - btnChangedAt[i] >= 5) {
      btnDown[i] = raw;
      post(EV_BTN, i, raw ? 1 : 0);
    }
  }
}
#endif

// ---------------------------------------------------------------- keypads --
// ESP32: every wire is open-drain with a pull-up, so "release" is just a write.
// Pico (no open-drain pads): a wire is an output only while it is pulled LOW,
// and is driven HIGH for a moment on release so the wires recover quickly.
void linesBegin() {
  NL = 0;
  for (uint8_t pin : KP_LINES) lines[NL++] = pin;
#ifdef KP_ALT_LINE
  if (!knob2Fitted) lines[NL++] = KP_ALT_LINE;          // keypad pin 8 may still be on its v1 pin
#endif
  for (int i = 0; i < NL; i++) {
#if defined(ARDUINO_ARCH_ESP32)
    pinMode(lines[i], OUTPUT_OPEN_DRAIN | PULLUP);
    digitalWrite(lines[i], HIGH);
#else
    pinMode(lines[i], INPUT_PULLUP);
#endif
  }
}

inline void lineLow(int i) {
#if !defined(ARDUINO_ARCH_ESP32)
  pinMode(lines[i], OUTPUT);
#endif
  digitalWrite(lines[i], LOW);
}

inline void lineRelease(int i) {
  digitalWrite(lines[i], HIGH);
#if !defined(ARDUINO_ARCH_ESP32)
  pinMode(lines[i], INPUT_PULLUP);
#endif
}

void scanRaw(uint32_t out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0;
  for (int i = 0; i < NL - 1; i++) {             // the last wire never needs pulling
    lineLow(i);
    delayMicroseconds(5);
    for (int j = i + 1; j < NL; j++)
      if (digitalRead(lines[j]) == LOW) {
        int p = pairIndex(i, j);
        out[p >> 5] |= 1u << (p & 31);
      }
    lineRelease(i);
  }
}

void keysPoll(uint32_t now) {
  uint32_t raw[4];
  scanRaw(raw);
  for (int p = 0; p < NPAIR; p++) {
    int w = p >> 5;
    uint32_t bit = 1u << (p & 31);
    bool r = raw[w] & bit;
    if (r != (bool)(lastRaw[w] & bit)) changedAt[p] = now;
    if (r == (bool)(stable[w] & bit)) continue;
    if (now - changedAt[p] < (r ? PAD_PRESS_MS : PAD_DEBOUNCE_MS)) continue;
    stable[w] ^= bit;
    if (calibrating) {
      if (r) post(EV_RAW_DOWN, p);
      continue;
    }
    int pad = S.map[p];
    if (pad < 0 || pad >= 16 * S.keypads) continue;
    if (r) logicalMask |= 1u << pad;
    else logicalMask &= ~(1u << pad);
    post(r ? EV_PAD_DOWN : EV_PAD_UP, pad);
  }
  memcpy(lastRaw, raw, sizeof lastRaw);
}

void loadSettings() {
  File f = LittleFS.open("/input.bin", "r");
  if (!f) return;
  uint8_t buf[sizeof(Settings)];
  int n = f.read(buf, sizeof buf);
  f.close();
  if (n == (int)sizeof(Settings) && buf[0] == S.version) {
    Settings t;
    memcpy(&t, buf, sizeof t);
    if (t.nLines != NLMAX) {                     // rewired to a different line count
      t.nLines = NLMAX;
      memcpy(t.map, S.map, sizeof t.map);
    }
    S = t;
    return;
  }
  if (n == 5 + 128 && buf[0] == 3) {             // v3: 8 wires, pairs numbered row by row
    S.keypads = buf[1];
    S.encReverse = buf[2];
    S.screenFlip = buf[3];
    int old = buf[4];
    if (old == 8) {
      memset(S.map, -1, sizeof S.map);
      for (int i = 0; i < old; i++)
        for (int j = i + 1; j < old; j++) {
          int p = i * (2 * old - i - 1) / 2 + (j - i - 1);
          S.map[pairIndex(i, j)] = (int8_t)buf[5 + p];
        }
      twinPairs();
    }
  }
}
}  // namespace

void input::resetMap() {
  // KP_LINES lists the keypad's pins 1-8: columns C1-C4, then rows R1-R4.
  // KEYPAD SETUP replaces this map if the keypad is wired any other way.
  memset(S.map, -1, sizeof S.map);
  if (sizeof KP_LINES < 8) return;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) S.map[pairIndex(c, 4 + r)] = r * 4 + c;
  twinPairs();
}

void input::begin() {
  resetMap();
  loadSettings();
  if (S.keypads > KP_MAX_KEYPADS) S.keypads = KP_MAX_KEYPADS;
#if HAS_EXTRAS
  knob2Fitted = detectKnob2();                         // before the keypad claims GPIO 47
  for (int i = 0; i < 4; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    btnDown[i] = btnRaw[i] = false;
  }
#endif
  linesBegin();
  pinMode(PIN_KEY0, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encBegin();
  delayMicroseconds(100);                        // let the pull-ups settle
  if (digitalRead(PIN_ENC_SW) == LOW) {          // held while plugging in
    bootHeld = true;
    swDown = swRaw = holdSent = true;            // so letting go is not a click
  }
  k0Down = k0Raw = digitalRead(PIN_KEY0) == LOW; // a key held at power-up is not a press
  k0AtBoot = k0Down;
#if HAS_EXTRAS
  if (knob2Fitted) {
    pinMode(PIN_ENC2_SW, INPUT_PULLUP);
    enc2Begin();
    sw2Down = sw2Raw = digitalRead(PIN_ENC2_SW) == LOW;
  }
  for (int i = 0; i < 4; i++) btnDown[i] = btnRaw[i] = digitalRead(BTN_PINS[i]) == LOW;
#endif
}

bool input::knob2() { return knob2Fitted; }
int input::knob2Mode() { return S.knob2; }
void input::setKnob2Mode(int m) { S.knob2 = constrain(m, 0, 2); }

bool input::heldAtBoot() { return bootHeld; }
bool input::key0AtBoot() { return k0AtBoot; }

void input::poll(uint32_t now) {
  keysPoll(now);
  encPoll(now);
  keyPoll(now);
#if HAS_EXTRAS
  if (knob2Fitted) enc2Poll(now);
  btnPoll(now);
#endif
}

int input::keypads() { return S.keypads; }
void input::setKeypads(int n) { S.keypads = n == 2 && KP_MAX_KEYPADS >= 2 ? 2 : 1; }
bool input::encReversed() { return S.encReverse; }
void input::setEncReversed(bool r) { S.encReverse = r; }
bool input::screenFlip() { return S.screenFlip; }
void input::setScreenFlip(bool f) { S.screenFlip = f; }

void input::calibrate(bool on) {
  calibrating = on;
  memset(stable, 0, sizeof stable);
  memset(lastRaw, 0, sizeof lastRaw);
  logicalMask = 0;
}

void input::setPads(const int8_t pairOfPad[], int nPads) {
  memset(S.map, -1, sizeof S.map);
  for (int pad = 0; pad < nPads && pad < 32; pad++)
    if (pairOfPad[pad] >= 0 && pairOfPad[pad] < NPAIR) S.map[pairOfPad[pad]] = pad;
  twinPairs();
}

bool input::saveSettings() {
  File f = LittleFS.open("/input.bin", "w");
  if (!f) return false;
  f.write((const uint8_t*)&S, sizeof S);
  f.close();
  return true;
}

uint32_t input::padMask() { return logicalMask; }

void input::debug(Print& p) {
  p.printf("pins now: knob A %d  B %d  push(SW) %d  KEY0 %d   (0 = pressed / low)\n", digitalRead(PIN_ENC_A),
           digitalRead(PIN_ENC_B), digitalRead(PIN_ENC_SW), digitalRead(PIN_KEY0));
  p.printf("since power-up: knob pushes %lu  clicks %lu  holds %lu  turns %lu  KEY0 presses %lu\n",
           (unsigned long)nSwDown, (unsigned long)nClick, (unsigned long)nHold, (unsigned long)nTurn,
           (unsigned long)nKeyDown);
  p.printf("state: knob down %d  turned %d  hold sent %d  KEY0 down %d  keypad pads %08lx\n", swDown, turned, holdSent,
           k0Down, (unsigned long)logicalMask);
  p.print("keypad wires on GPIO");
  for (int i = 0; i < NL; i++) p.printf(" %d", lines[i]);
  p.println();
#if HAS_EXTRAS
  p.printf("second knob: %s (%s)", knob2Fitted ? "fitted" : "not found", S.knob2 == 0 ? "auto" : S.knob2 == 1 ? "forced on" : "forced off");
  if (knob2Fitted)
    p.printf("  S1 %d  S2 %d  KEY %d", digitalRead(PIN_ENC2_A), digitalRead(PIN_ENC2_B), digitalRead(PIN_ENC2_SW));
  p.printf("\nOLED buttons K1-K4: %d %d %d %d  (0 = pressed)\n", digitalRead(BTN_PINS[0]), digitalRead(BTN_PINS[1]),
           digitalRead(BTN_PINS[2]), digitalRead(BTN_PINS[3]));
#endif
}
