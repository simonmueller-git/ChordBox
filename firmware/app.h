// app.h — modes, the function layer and the UI model: one knob + 16 pads
#pragma once
#include <Arduino.h>
#include "types.h"

namespace app {

void begin();
void handle(const Event& e);                 // input + console events
void tick(int64_t nowUs, uint32_t nowMs);    // publishes the model when it changed
void setMoveUp(bool up);
void midiIn(uint8_t status, uint8_t d1, uint8_t d2);   // channel messages from the Move
uint8_t scenesUsed();                        // bit i = scene i+1 is saved (reads flash)
uint8_t sceneSelected();
void printHelp(Print& p);
int actionIndex(const String& name);         // console: action id, -1 if unknown

}  // namespace app

void modelRead(UiModel& out);                // thread-safe copy for the screen
