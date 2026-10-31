// engine.h — the chord program (ChordBox v5 engine)
#pragma once
#include <Arduino.h>
#include "types.h"

namespace engine {

enum P : uint8_t {
  P_ROOT, P_SCALE, P_OCT, P_BANK,
  P_DICE, P_VAR, P_VOIC, P_STRM,
  P_HUM, P_VEL, P_VLD, P_CHCH,
  P_DARK, P_TENS, P_LEN, P_MUT,
  P_SOFT, P_VRND, P_WEIGHT, P_SDIR,            // feel
  P_ARP, P_ARATE, P_AGATE, P_AOCT, P_RHY,      // arpeggiator and stabs (on the clock)
  P_RPT, P_RRAMP, P_ECHO, P_EFB, P_LOOPLEN,    // repeat, echo, loop recorder
  P_COUNT
};

// a chord as the bass hears it
struct Chord {
  bool valid;
  uint8_t rootPc;                  // absolute pitch class
  int8_t third, fifth, seventh;    // semitones above the root, -1 = not in the chord
  char name[12];
};

struct Harmony {
  uint8_t keyPc;
  uint16_t scaleMask;              // bit i = key root + i semitones is in the scale
  Chord cur;                       // the chord you played last (or the tonic)
  Chord prog[8];                   // the progression it came from
  uint8_t progN;
  int8_t progPos;                  // where `cur` sits in it
};

void begin();
void tick(int64_t nowUs, uint32_t nowMs);    // strum tails, deferred saves

void padDown(int pad);             // 0..15 on this page: sounds now, never quantised
void padUp(int pad);
void selectRow(int pad);           // silent: aim STAR / MUTATE at a row
void allOff();
void panic();

void pageMove(int detents);        // one page per detent, rolling through banks
void paramAdjust(int p, int detents);
void paramSet(int p, int v);
int paramGet(int p);
void paramView(int p, ParamView& out);
int paramIndex(const String& label);

void star();
void unstar();
void gen();
void mutate();
void recall();                     // Favs <-> where you were
void toggleLatch();
bool latched();
void plain();
void setBassPlaying(bool on);      // ST9 voicing drops its root while the bass plays

// performing (the arp, repeat, echo and loop run on the Move's clock, or on the
// internal tempo when it's stopped; the pads themselves still sound at once)
void revoice(int pad, int steps);  // hold a pad + turn: walk its inversion
void quality(int pad, int steps);  // hold a pad + push + turn: major, minor, 7, maj7, 9, sus...
void shapeTurn(int steps);         // second knob: the inversion of every chord (and the one sounding)
void shapeQuality(int steps);      // second knob pushed + turned: the sounding chord's quality
int inversion();
void setRepeat(bool on);           // pad held, then KEY0: the chord repeats at REPEAT
bool repeating();
void loopButton();                 // REC: record chords -> loop them -> stop
void stopAll();                    // silence: chords, latch, arp, repeat, echo, loop

// where you are (bank by name, page), kept across restarts by the app
void place(char* bankName, size_t cap, int& page);
void setPlace(const char* bankName, int page);

void chordView(ChordView& v);
void harmony(Harmony& h);
const char* keyName();
const char* scaleName();           // "" when SCALE is off

void printState(Print& p);
void printFavs(Print& p);

}  // namespace engine

// provided by the sketch / app
void midiOut(uint8_t status, uint8_t d1, uint8_t d2);
void uiToast(const char* fmt, ...);
