// harm_listen.h — LISTEN: chords, note set and home from a live MIDI stream
// (Harmony Engine Spec, section 5). Pure: notes in, results out.
//
// Times are in 16ths since the Move's song start (bar n = 16n .. 16n+16), so
// folding repeats lines up with the Move's own bars. Without a clock,
// detectTempo() finds the grid first.
#pragma once
#include <stdint.h>
#include "harm_theory.h"

namespace hl {

struct Ev {
  float on, dur;                       // 16ths
  uint8_t pitch, vel, ch;
};

enum Ext : uint8_t { X_7 = 1, X_9 = 2, X_SUS = 4 };

struct BarChord {
  int8_t root = -1;                    // absolute pitch class, -1 = no chord
  uint8_t qual = ht::Q_NONE;
  uint8_t ext = 0;                     // annotations: 7th, 9th, sus
  int8_t low = -1;                     // bass pitch class under it, -1 = none
};

struct Result {
  int bars = 0;                        // bars heard
  int loop = 0;                        // loop length found: 1, 2, 4 or 8
  int firstBar = 0;                    // the song bar the analysis starts at
  float listenL[12];                   // note-set scores, ready for NoteSet.listen
  ht::HomeObs home;                    // evidence for home
  int coll = 0;                        // the note set used for chord qualities
  BarChord chord[8];                   // per loop bar, smoothed
  bool bassOnly = false;               // only a bassline: plain + twist instead
  BarChord plain[8], twist[8];
  uint16_t kept[8];                    // pitch classes that survived folding, per loop bar
};

// bassCh: the bass channel (0-15), or -1 = the lowest sustained note below MIDI 55.
// forceColl: the note set to fill qualities from (-1 = the best one heard).
// msPer16th: to drop notes shorter than 60 ms.
bool analyze(const Ev* ev, int n, int bassCh, int forceColl, float msPer16th, Result& r);

// No clock: the tempo (115-128 BPM) and 16th phase that best fit the onsets
// (microseconds). `beatOneLead` = how far the best downbeat guess leads the
// next one (0..1); under 0.2, ask for a tap on "1".
bool detectTempo(const uint32_t* onsetUs, const uint8_t* weight, int n, float& bpm, float& phaseUs,
                 int& downbeat16th, float& beatOneLead);

}  // namespace hl
