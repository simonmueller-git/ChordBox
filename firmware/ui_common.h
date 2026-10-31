// ui_common.h — pieces both display back-ends share
#pragma once
#include <Arduino.h>
#include "ui_draw.h"

struct LcdPrefs {
  uint8_t rot = 3;       // 1 or 3: landscape either way up (3: screen above the knob)
  bool invert = false;   // the Estardyn module's panel is not inverted: black stays black
  bool bgr = false;
  uint8_t mhz = 40;
  uint8_t bright = 255;  // backlight PWM on BLK
};

// the rotation to use: the saved one, turned over by MORE: SCREEN FLIP
inline uint8_t lcdRotation(const LcdPrefs& p, bool flip) { return flip ? (p.rot ^ 2) : p.rot; }

void lcdPrefsLoad(LcdPrefs& p);
void lcdPrefsSave(const LcdPrefs& p);
// parse `lcd ...`; returns true if a setting changed (caller re-inits the panel)
bool lcdPrefsCommand(LcdPrefs& p, const String& arg, Print& out);
void shotSprite(lgfx::LGFX_Sprite& s, Print& out);
