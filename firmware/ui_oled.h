// ui_oled.h — render the bass OLED (128x64, one bit) into a sprite
#pragma once
#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include <LovyanGFX.hpp>
#include <stdint.h>
#include "types.h"

static const int OLED_W = 128, OLED_H = 64;

void oledRender(lgfx::LGFX_Sprite& sprite, const UiModel& m, uint32_t nowMs);   // sprite: 1-bit, 128x64
bool oledAnimating(const UiModel& m, uint32_t nowMs);
