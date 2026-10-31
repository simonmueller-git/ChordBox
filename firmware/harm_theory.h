// harm_theory.h — the Harmony Engine's music: note sets, home, chords, shapes,
// placement and the tables of the spec (Harmony Engine Spec, 2026-10-10).
//
// Pure functions and constant tables, no chip calls, so tests/host runs them.
// Pitch classes are 0..11 from C. A chord is stored as (root, quality) where
// root counts semitones up from HOME; changing home re-spells every chord.
#pragma once
#include <stdint.h>

namespace ht {

// ------------------------------------------------------------ note sets --
// The 12 seven-note collections, indexed by position on the circle of fifths:
// collection k is the major scale with k sharps (k = 0 C, 1 G, ... 11 F).
// A note at fifths position p belongs to collections p-5 .. p+1.
int fifthsPos(int pc);                 // C 0, G 1, D 2 ... F 11
int collTonic(int k);                  // the major-scale tonic of collection k
uint16_t collMask(int k);              // bit pc = in the collection
const char* noteName(int pc, int k);   // spelled for collection k ("Bb" in F, "A#" in B)

// log-scores for each collection; HUNT votes and LISTEN notes both land here
struct NoteSet {
  float vote[12];                      // by-ear verdicts
  float listen[12];                    // from incoming MIDI (already scaled)
  float manual[12];                    // PLAY: home + bright/dark set by hand
  void clear();
  float L(int k) const { return vote[k] + listen[k] + manual[k]; }
};

// a verdict on a probe (bit mask of pitch classes): YES = fits, NO = clashes.
// errIn / errOut: chance of a wrong verdict for notes predicted in / outside the set
void vote(NoteSet& s, uint16_t probe, bool fits, float errIn = 0.10f, float errOut = 0.15f);
void posterior(const NoteSet& s, float P[12]);
float fitChance(const float P[12], uint16_t probe);   // chance the probe fits, by the posterior
// the probe whose chance of fitting is closest to 50%: a single note or a
// root + fifth pair. Returns the mask; *root = its lowest pitch class
uint16_t nextProbe(const float P[12], int* root, uint16_t rejected = 0);

struct Sure {                           // the honest readout
  int leader, runner;                   // collections
  int pct;                              // leader's chance, capped at 95
  int bars;                             // 0..5
  bool tie;                             // "two are equally likely"
  int splitPc;                          // the one note that splits them (-1 none)
  int splitAltPc;                       // the runner's version of it ("B or B flat?")
  int nEqual;                           // how many collections share the lead (pentatonic: 3)
};
Sure sure(const NoteSet& s);

// ------------------------------------------------------------------ home --
// evidence for which note is home, accumulated per pitch class
struct HomeObs {
  float bassDur[12];                   // share of bass duration (any units)
  float barStart[12];                  // bass plays it at a bar or loop start
  float first[12];                     // first bass note of the loop
  float totalDur[12];                  // duration in all parts
  float phraseEnd[12];                 // long note at a phrase end
  void clear();
};
// mode of `pc` as home in collection k: 0 ionian (bright), 1 dorian, 2 phrygian,
// 3 lydian, 4 mixolydian, 5 aeolian (dark), 6 locrian
int modeOf(int pc, int k);
const char* modeWord(int mode);        // "bright", "dark", "dark dorian" ...
bool modeDark(int mode);
// home scores for the 12 pitch classes in collection k (notes outside it: -1e9)
void homeScores(const HomeObs& o, int k, float out[12]);
// combined over collections, weighted by their posterior: P(home = pc)
void homePosterior(const HomeObs& o, const float Pset[12], float out[12]);

// ---------------------------------------------------------------- chords --
enum Qual : uint8_t { Q_DARK, Q_BRIGHT, Q_DOM, Q_HANG, Q_POWER, Q_NONE };
const char* qualSuffix(uint8_t q);     // "m", "", "7", "sus", "5", "-"
const char* qualWord(uint8_t q);       // "dark", "bright", "7", "hanging", "power", "none"

struct Chord {
  int8_t root = 0;                     // semitones above home (planed: absolute pitch class)
  uint8_t qual = Q_NONE;
  uint8_t planed = 0;                  // slide shape: absolute root, exempt from the note set
  bool operator==(const Chord& o) const { return root == o.root && qual == o.qual && planed == o.planed; }
  bool operator!=(const Chord& o) const { return !(*this == o); }
};
int chordRootPc(const Chord& c, int homePc);
uint16_t chordTones(const Chord& c, int homePc);     // triad (or power / sus) pitch classes
uint16_t chordTonesFull(const Chord& c, int homePc); // with the 7th
// "F", "Dm", "Bb7", "Csus" — spelled for collection k
void chordName(const Chord& c, int homePc, int k, char* out, int cap);
// Roman numeral relative to home: "i", "bVI", "V7", "IVsus"
void chordRoman(const Chord& c, char* out, int cap);

// the 12 one-finger chords for this home + collection: diatonic (no diminished)
// first, then the Outside / Swap chords of its family
int chordPads(int homePc, int k, Chord out[12]);

// ---------------------------------------------------------------- shapes --
// Shapes are offsets in semitones above the chord root; the bass plays the root.
enum Shape : uint8_t {
  SH_THIN, SH_FULL, SH_WIDE, SH_FLIP,          // the four on the top row
  SH_SHELL, SH_PIANO, SH_PIANO_TRIAD, SH_COLOUR,   // VALUE reaches these too
  SH_BLOCK,                                        // root-position block: slide shapes
  SH_COUNT
};
const char* shapeWord(uint8_t s);      // "thin", "full", "wide", "flip", ...
int shapeOffsets(uint8_t qual, uint8_t shape, bool onIV, int8_t out[6]);

// Register rules and placement (spec 6.4). `pcs` = pitch classes or offsets
// (any octave); `prev` = the previous voicing (sorted), n = 0 for none;
// `topTarget` = the pitch the highest voice stays near (0 = none).
// Returns the number of notes in `out` (sorted, MIDI).
int place(const int8_t* tones, int n, int rootMidiPc, const uint8_t* prev, int nPrev, int topTarget,
          uint8_t out[8]);
int placeCost(const uint8_t* v, int n, const uint8_t* prev, int nPrev, int topTarget);

// ------------------------------------------------------- next-chord ranks --
// Table 6.5: ranked successors of a chord, by family (dark/dorian, bright/mixolydian)
int nextChords(const Chord& from, bool darkHome, Chord out[8]);
float nextWeight(const Chord& from, const Chord& to, bool darkHome);   // 0.40, 0.25, 0.15, ... small if unlisted

// ------------------------------------------------------------- skeletons --
struct Skeleton {
  const char* mood;
  bool dark;                           // written for a dark (minor) home
  uint8_t len;                         // 8 or 16
  const char* bars;                    // e.g. "i9 i9 iv9 iv9 i9 i9 iv9 bVII"
};
extern const Skeleton SKELETONS[];
extern const int N_SKELETONS;
// parse one bar token ("bVImaj7", "V9sus", "bVII|IV" = halves, "i/1" = low note home)
bool parseRoman(const char* tok, Chord& c, uint8_t& shapeHint);

// ----------------------------------------------------------------- rhythm --
struct Rhythm { const char* name; const char* steps; };   // 16 chars, x = hit
extern const Rhythm STABS[];          // 6.7, plus SUSTAIN
extern const int N_STABS;
extern const Rhythm CELLS[];          // 7, melody cells
extern const int N_CELLS;
uint16_t rhythmMask(const char* steps);

// ------------------------------------------------------------ pentatonic --
uint16_t pentaMask(int homePc, bool dark);    // minor pentatonic for dark homes, major for bright

}  // namespace ht
