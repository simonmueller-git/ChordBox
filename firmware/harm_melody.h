// harm_melody.h — MELODY: a line on top (Harmony Engine Spec, section 7).
// Tier 1: safe pads. Tier 2: the user taps a rhythm, the engine picks pitches.
// Tier 3: rhythm from the cell table too. One sampler, four voice presets.
#pragma once
#include <stdint.h>
#include "harm_theory.h"

namespace hm {

enum Voice : uint8_t { V_LEAD, V_FLUTE, V_PIANO, V_ARP, V_COUNT };
const char* voiceWord(uint8_t v);

struct Note {
  uint8_t step;                        // 16ths from the line's start
  uint8_t pitch, len, vel;             // len in 16ths
};

struct Line {
  uint8_t bars = 0;                    // 1..16
  uint8_t n = 0;
  Note note[128];
};

struct BarHarm {                       // the chord under one bar
  int rootPc = 0;
  uint8_t qual = ht::Q_NONE;
  uint16_t tones = 0;                  // root, third, fifth
  uint16_t full = 0;                   // with the 7th
  uint8_t shape[6];                    // the placed voicing (for ARP), MIDI
  uint8_t nShape = 0;
  bool homeChord = false;              // this is the home chord (for "avoid holding")
};

struct MCtx {
  int home = 0, coll = 0;
  bool dark = true;
  int spice = 3;                       // 0..7: the 9th joins the chord notes at 5+
  int density = 4;                     // 0..7
  int range = 2;                       // 0..4: 12, 15, 19, 24, 29 semitones around the middle
  uint8_t voice = V_LEAD;
  bool penta = true;                   // five-note scale (else the full seven)
  uint32_t seed = 1;
};

// tier 1: the 12 pads' pitches (low to high)
void safePads(const MCtx& x, uint8_t pitch[12]);
uint16_t scaleMask(const MCtx& x);
void rangeOf(const MCtx& x, int& lo, int& hi);

// tiers 2 and 3: `rhythm` = one 16-step mask per bar (nullptr: from the voice's cells).
// `bars` = the chords, one per bar; n = 1..16 bars. Form A A' A B per four bars.
bool generate(const MCtx& x, const BarHarm* bars, int n, const uint16_t* rhythm, Line& out);

// spec test 16: 0 if every on-beat note is a chord note and every note is in range
int check(const Line& l, const BarHarm* bars, const MCtx& x);

}  // namespace hm
