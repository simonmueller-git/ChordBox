// bass.h — the bass program: a clock-synced, chord-following bass sequencer
#pragma once
#include <Arduino.h>
#include "types.h"

namespace bass {

enum BP : uint8_t {
  BP_TEMPO, BP_SWING, BP_OCT, BP_GENRE,
  BP_FOLLOW, BP_LEN, BP_GATE, BP_SLIDE,
  BP_ACCENT, BP_DENS, BP_DUCK, BP_CH,
  BP_NUDGE, BP_FILL, BP_RATE, BP_QUANT,
  BP_VEL,
  BP_COUNT
};

void begin();
void poll(int64_t nowUs, uint32_t nowMs);   // send due notes, deferred saves
void onStep(uint32_t step, int64_t atUs, int32_t stepUs);
void onTransport(bool running);

// playing and browsing
void padDown(int pad);             // pattern `pad` of this page: queued for the next bar,
                                   // or starts a preview when nothing is running
void pageMove(int detents);        // within the genre: hand-written pages, then generated
void stepLine(int d);
void lineStep(int d);              // the station's knob: next / previous line (ANY: random, back = history)
void surprise();                   // a random line: this genre, or any genre in ANY
void pageJump(int d);              // 16 lines up / down (ANY: a new random line)
void genreStep(int d);             // the 16 genres, then ANY, then FAVS; plays a line from it
bool anyGenre();              // the next / previous line of the genre (OLED < >), played like a pad
void togglePlay();                 // preview on/off; mute while the Move is playing
bool running();
bool muted();

void paramAdjust(int p, int detents);
void paramSet(int p, int v);
int paramGet(int p);
void paramView(int p, ParamView& out);
int paramIndex(const String& label);

void star();
void starAt(int pad);              // keep line `pad` of this page (hold it + click)
void mutate();

// where you are: the genre page on screen and the line playing (kept across restarts)
void place(int& browseBank, int& browsePage, int& playBank, int& playIdx);
void setPlace(int browseBank, int browsePage, int playBank, int playIdx);

// EDIT: the playing pattern, one bar at a time (step = 0..15 within the bar)
int bars();
void editToggle(int bar, int step);
void editRole(int bar, int step, int delta);
void editAccent(int bar, int step);   // normal -> accent -> ghost
void editSlide(int bar, int step);
void editTie(int bar, int step);
void editOctave(int bar, int step, int d);
void editRotate(int d);
void editClear();
void editRandom();
bool editUndo();

void bassView(BassView& v);
void printState(Print& p);
void printPattern(Print& p);

}  // namespace bass
