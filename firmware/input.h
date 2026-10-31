// input.h — 4x4 keypad + push encoder + KEY0 -> Events (see types.h)
#pragma once
#include <Arduino.h>

namespace input {

void begin();                         // pins, encoder counter, saved key map
void poll(uint32_t nowMs);            // call every ~1 ms: scan, debounce, gestures

int keypads();                        // keypads in use (at most KP_MAX_KEYPADS)
void setKeypads(int n);
bool encReversed();
void setEncReversed(bool r);
bool screenFlip();                    // picture turned 180 degrees
void setScreenFlip(bool f);

// calibration: while on, presses arrive as EV_RAW_DOWN (a = the pair of keypad
// wires the key joins, 0..35 for 9 wires)
void calibrate(bool on);
void setPads(const int8_t pairOfPad[], int nPads);   // pad -> wire pair, -1 = skipped
void resetMap();
bool saveSettings();

uint32_t padMask();                   // logical pads held now (bit 0..31)
bool knob2();                         // the second knob was found at power-up (wiring v2)
int knob2Mode();                      // 0 auto, 1 always on, 2 never (console `input knob2 ...`)
void setKnob2Mode(int m);             // takes effect at the next power-up
bool heldAtBoot();
bool key0AtBoot();                    // KEY0 held at power-up: live-safe (PLAY only)                    // knob was pressed at power-up: open keypad setup
void debug(Print& p);                 // pin levels and gesture counts (console `input`, /diag)

}  // namespace input
