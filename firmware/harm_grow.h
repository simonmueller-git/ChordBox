// harm_grow.h — the loop (16 slots) and GROW: from 4 chords to 8 or 16
// (Harmony Engine Spec, section 6). Form first, then shape, then rhythm.
#pragma once
#include <stdint.h>
#include "harm_theory.h"

namespace hg {

enum SlotFlag : uint8_t {
  HF_PUSH = 1,                          // Early: starts a 16th before the bar
  HF_LOCK = 2,                          // never changed by GROW
  HF_SPLIT = 4,                         // two halves: c, then c2
  HF_SET = 8,                           // the slot holds something (else empty)
};

struct Slot {
  ht::Chord c, c2;                     // c2 = second half when split
  uint8_t shape = ht::SH_THIN, shape2 = ht::SH_THIN;
  int8_t low = -1;                     // low note, semitones above home (-1 = the chord's root)
  uint8_t flags = 0;
  uint8_t op = 0;                      // which operator made it (for the screen)
  bool operator==(const Slot& o) const {
    return c == o.c && c2 == o.c2 && shape == o.shape && shape2 == o.shape2 && low == o.low && flags == o.flags;
  }
};

enum Op : uint8_t { OP_NONE, OP_WAYBACK, OP_SWAP, OP_SPLIT, OP_OUTSIDE, OP_BASSES, OP_STUCK, OP_SLIDE, OP_CHROMA, OP_FREE };
const char* opWord(uint8_t op);

enum Form : uint8_t { FORM_AUTO, FORM_AA, FORM_AABA };

struct Loop {
  uint8_t len = 4;                     // 4, 8 or 16 slots
  uint8_t rhythm = 1;                  // index into ht::STABS
  uint8_t top = 0;                     // top-note target (MIDI), 0 = none
  uint8_t form = FORM_AUTO;
  Slot s[16];
  bool operator==(const Loop& o) const {
    if (len != o.len) return false;
    for (int i = 0; i < len; i++)
      if (!(s[i] == o.s[i])) return false;
    return true;
  }
};

struct Ctx {
  int home = 0;                        // home pitch class
  int coll = 0;                        // the note set (collection)
  bool dark = true;                    // home chord is dark
  int spice = 3;                       // 0..7
  uint32_t seed = 1;
};

bool outside(const ht::Chord& c, const Ctx& x);      // tones not all in the note set (planed: never)

// extend / vary `in` to `len` slots. Spice 0 = exactly what was entered.
void generate(const Loop& in, int len, const Ctx& x, Loop& out);
// ~20 candidates, ranked; the page'th group of `n` best into out. Returns how many.
int candidates(const Loop& in, int len, const Ctx& x, int page, Loop* out, int n);
float score(const Loop& l, const Ctx& x);
// ranked next-chord suggestions for slot i, in context
int suggest(const Loop& l, int i, const Ctx& x, ht::Chord out[8]);
// the six hard constraints; returns 0 if all hold, else the rule number broken
int violates(const Loop& out, const Loop& in, const Ctx& x);
// a skeleton from the library, transposed to this home
void skeleton(int idx, const Ctx& x, Loop& out);

// how strongly each slot leans forward (rises to slots 15-16, resets on 1), 0..1
float tension(int i, int len);

}  // namespace hg
