// harm.h — the Harmony Engine (v2.0): one auto-saved Song Profile feeding
// HUNT (chords by ear), LISTEN (from MIDI), PLAY, GROW and MELODY, a 16-slot
// loop played on the Move's clock, a persistent history of takes, a capture
// buffer and Print to Move. The app routes the controls; see app.cpp for the map.
#pragma once
#include <Arduino.h>
#include "engine.h"
#include "types.h"

namespace harm {

void begin(bool liveSafe);                 // load the profile and takes
void enter(uint8_t hmode);                 // the screen switched to a harmony mode
void leave();                              // back to CHORD / BASS (the loop keeps playing)
uint8_t mode();
bool liveSafe();
bool playing();                            // the loop is playing
bool ownsHarmony();                        // the bass follows this engine now
bool harmony(engine::Harmony& h);          // what the bass follows (false: not active)

void tick(int64_t nowUs, uint32_t nowMs);  // scheduled notes, held-NO undo, saves
void onStep(uint32_t step, int64_t atUs, int32_t stepUs);
void onTransport(bool running);
void midiIn(uint8_t status, uint8_t d1, uint8_t d2, int64_t nowUs);   // LISTEN

// controls
void pad(int pad, bool down);              // the sound grid, 0..15
void slotLayer(bool on);                   // main knob held: the pads are the 16 slots
bool inSlotLayer();
void slotPad(int slot, bool down, bool shift);
void yes();
void no(bool down);                        // tap = no / reject; held = step back through undo
void value(int d);                         // MAIN turn: the mode's value; slots view: suggestions
void click();                              // MAIN click: play / stop (TAKES: load)
int setCount();                            // the SET knob's list for this mode
void setInfo(int i, char* label, int lcap, char* val, int vcap);
void setAdjust(int i, int d);              // SET turn
void tapOne();                             // tap "1": the next 16th is the bar start
void yesNoWords(char* yes, char* no, int cap);   // what YES and NO do right now (OLED labels)
void shiftYes();                           // the capture buffer -> a take
void shiftNo();                            // A/B: flip to the previous take on the bar line
void print();                              // Print to Move: play the loop once, in sync
void takes(bool open);                     // the TAKES list
bool takesOpen();
void stopAll();                            // silence the loop and the melody
void newSong();                            // forget the votes and what was heard (takes stay)

void view(HarmView& v);
void printState(Print& p);
void setMelodyCh(int ch);
int melodyCh();

}  // namespace harm
