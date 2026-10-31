// ui_draw.h — render one frame of the ChordBox screen into a sprite
#pragma once
#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include <LovyanGFX.hpp>
#include <stdint.h>
#include "types.h"

static const int W = 320, H = 240;

// `between` (optional) runs between screen sections, so input is never held
// up by a whole frame of drawing (the Pico uses it; the ESP32 draws on its own core)
void uiRender(lgfx::LGFX_Sprite& sprite, const UiModel& m, uint32_t nowMs, void (*between)() = nullptr);

// true while something moves on its own (playhead, blinking), so the screen
// keeps redrawing without a model change
bool uiAnimating(const UiModel& m, uint32_t nowMs);

// 4-bit palette sprite (38 KB); both targets draw into one of these
void uiInitSprite(lgfx::LGFX_Sprite& sprite);
extern const uint8_t UI_PALETTE[][3];
extern const int UI_PALETTE_N;
