// harm_theory.cpp — see harm_theory.h. Tables are the spec's (sections 4, 6, 7).
// Scores are floats: the S3 has an FPU, and every loop here is tiny.
#include "harm_theory.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ht {

static inline int mod12(int x) { return ((x % 12) + 12) % 12; }
static inline int bit(int pc) { return 1 << mod12(pc); }
static int popc(uint16_t m) {
  int n = 0;
  for (; m; m &= m - 1) n++;
  return n;
}

// ================================================================ note sets
int fifthsPos(int pc) { return mod12(pc * 7); }
int collTonic(int k) { return mod12(k * 7); }

uint16_t collMask(int k) {
  uint16_t m = 0;
  for (int p = k - 1; p <= k + 5; p++) m |= bit(p * 7);
  return m;
}

static const char* const SHARP[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* const FLAT[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

const char* noteName(int pc, int k) {
  k = mod12(k);
  return (k >= 1 && k <= 6) ? SHARP[mod12(pc)] : FLAT[mod12(pc)];
}

void NoteSet::clear() {
  memset(vote, 0, sizeof vote);
  memset(listen, 0, sizeof listen);
  memset(manual, 0, sizeof manual);
}

void vote(NoteSet& s, uint16_t probe, bool fits, float errIn, float errOut) {
  float wIn = logf((1 - errIn) / errIn), wOut = logf((1 - errOut) / errOut);
  for (int k = 0; k < 12; k++) {
    bool predicted = (collMask(k) & probe) == probe;
    if (predicted && !fits) s.vote[k] -= wIn;
    if (!predicted && fits) s.vote[k] -= wOut;
  }
}

void posterior(const NoteSet& s, float P[12]) {
  float mx = -1e30f;
  for (int k = 0; k < 12; k++) mx = fmaxf(mx, s.L(k));
  float sum = 0;
  for (int k = 0; k < 12; k++) sum += (P[k] = expf(s.L(k) - mx));
  for (int k = 0; k < 12; k++) P[k] /= sum;
}

float fitChance(const float P[12], uint16_t probe) {
  float c = 0;
  for (int k = 0; k < 12; k++)
    if ((collMask(k) & probe) == probe) c += P[k];
  return c;
}

uint16_t nextProbe(const float P[12], int* root, uint16_t rejected) {
  uint16_t best = 0;
  float bestD = 2;
  int bestRoot = 0;
  for (int pair = 1; pair >= 0; pair--)            // pairs first: they split 6/6
    for (int i = 0; i < 12; i++) {
      int pc = mod12(i * 7);                       // around the circle, so ties go to fifths
      if (rejected & bit(pc)) continue;
      uint16_t m = bit(pc) | (pair ? bit(pc + 7) : 0);
      float d = fabsf(fitChance(P, m) - 0.5f);
      if (d < bestD - 1e-4f) bestD = d, best = m, bestRoot = pc;
    }
  if (root) *root = bestRoot;
  return best;
}

Sure sure(const NoteSet& s) {
  float P[12];
  posterior(s, P);
  Sure r{};
  r.leader = 0;
  for (int k = 1; k < 12; k++)
    if (P[k] > P[r.leader]) r.leader = k;
  r.runner = r.leader == 0 ? 1 : 0;
  for (int k = 0; k < 12; k++)
    if (k != r.leader && P[k] > P[r.runner]) r.runner = k;
  float pl = P[r.leader], pr = P[r.runner];
  r.pct = (int)(pl * 100 + 0.5f);
  if (r.pct > 95) r.pct = 95;
  r.bars = r.pct >= 95 ? 5 : r.pct / 19;
  r.tie = pl < 0.60f || pl < 2 * pr;
  r.nEqual = 0;
  for (int k = 0; k < 12; k++)
    if (P[k] >= pl * 0.9f) r.nEqual++;
  uint16_t a = collMask(r.leader), b = collMask(r.runner);
  r.splitPc = r.splitAltPc = -1;
  if (popc(a & ~b) == 1 && popc(b & ~a) == 1) {    // neighbours: one note apart
    for (int pc = 0; pc < 12; pc++) {
      if ((a & ~b) & bit(pc)) r.splitPc = pc;
      if ((b & ~a) & bit(pc)) r.splitAltPc = pc;
    }
  }
  return r;
}

// ===================================================================== home
void HomeObs::clear() {
  memset(this, 0, sizeof *this);
}

int modeOf(int pc, int k) {
  switch (mod12(pc - collTonic(k))) {
    case 0: return 0;
    case 2: return 1;
    case 4: return 2;
    case 5: return 3;
    case 7: return 4;
    case 9: return 5;
    case 11: return 6;
  }
  return -1;
}

const char* modeWord(int m) {
  static const char* const W[7] = {"bright", "dark dorian", "dark phrygian", "bright lydian",
                                   "bright mixo", "dark", "locrian"};
  return m >= 0 && m < 7 ? W[m] : "?";
}

bool modeDark(int m) { return m == 1 || m == 2 || m == 5 || m == 6; }

// genre prior (spec 5): natural minor 4, dorian 3, mixolydian 2, major 2, phrygian 1, lydian 0, locrian -8
static const float MODE_PRIOR[7] = {2, 3, 1, 0, 2, 4, -8};

static float share(const float* v, int pc) {        // v[pc] as 0..8 of the largest
  float mx = 0;
  for (int i = 0; i < 12; i++) mx = fmaxf(mx, v[i]);
  return mx > 0 ? 8 * v[pc] / mx : 0;
}

static float shareOfTotal(const float* v, int pc) {  // v[pc] as 0..8 of the sum
  float sum = 0;
  for (int i = 0; i < 12; i++) sum += v[i];
  return sum > 0 ? 8 * v[pc] / sum : 0;
}

void homeScores(const HomeObs& o, int k, float out[12]) {
  uint16_t m = collMask(k);
  for (int pc = 0; pc < 12; pc++) {
    if (!(m & bit(pc))) {
      out[pc] = -1e9f;
      continue;
    }
    float fifth = share(o.totalDur, pc + 7);
    out[pc] = 5 * shareOfTotal(o.bassDur, pc) + 4 * share(o.barStart, pc) + 3 * share(o.first, pc) +
              2 * share(o.totalDur, pc) + 2 * fifth + 1 * share(o.phraseEnd, pc) +
              MODE_PRIOR[modeOf(pc, k)];
  }
}

void homePosterior(const HomeObs& o, const float Pset[12], float out[12]) {
  memset(out, 0, 12 * sizeof(float));
  for (int k = 0; k < 12; k++) {
    if (Pset[k] < 1e-4f) continue;
    float s[12], mx = -1e30f, sum = 0, e[12];
    homeScores(o, k, s);
    for (int pc = 0; pc < 12; pc++) mx = fmaxf(mx, s[pc]);
    for (int pc = 0; pc < 12; pc++) sum += (e[pc] = s[pc] < -1e8f ? 0 : expf((s[pc] - mx) / 4));
    for (int pc = 0; pc < 12; pc++) out[pc] += Pset[k] * e[pc] / sum;
  }
}

// =================================================================== chords
const char* qualSuffix(uint8_t q) {
  static const char* const S[6] = {"m", "", "7", "sus", "5", "-"};
  return q < 6 ? S[q] : "?";
}

const char* qualWord(uint8_t q) {
  static const char* const S[6] = {"dark", "bright", "7", "hanging", "power", "none"};
  return q < 6 ? S[q] : "?";
}

int chordRootPc(const Chord& c, int homePc) { return c.planed ? mod12(c.root) : mod12(homePc + c.root); }

uint16_t chordTones(const Chord& c, int homePc) {
  int r = chordRootPc(c, homePc);
  switch (c.qual) {
    case Q_DARK: return bit(r) | bit(r + 3) | bit(r + 7);
    case Q_BRIGHT:
    case Q_DOM: return bit(r) | bit(r + 4) | bit(r + 7);
    case Q_HANG: return bit(r) | bit(r + 5) | bit(r + 7);
    case Q_POWER: return bit(r) | bit(r + 7);
  }
  return 0;
}

uint16_t chordTonesFull(const Chord& c, int homePc) {
  int r = chordRootPc(c, homePc);
  uint16_t t = chordTones(c, homePc);
  if (c.qual == Q_DARK || c.qual == Q_DOM || c.qual == Q_HANG) t |= bit(r + 10);
  if (c.qual == Q_BRIGHT) t |= bit(r + 11);
  return t;
}

void chordName(const Chord& c, int homePc, int k, char* out, int cap) {
  if (c.qual == Q_NONE) {
    snprintf(out, cap, "-");
    return;
  }
  snprintf(out, cap, "%s%s", noteName(chordRootPc(c, homePc), k), qualSuffix(c.qual));
}

void chordRoman(const Chord& c, char* out, int cap) {
  static const char* const R[12] = {"I", "bII", "II", "bIII", "III", "IV", "bV", "V", "bVI", "VI", "bVII", "VII"};
  if (c.qual == Q_NONE) {
    snprintf(out, cap, "-");
    return;
  }
  char r[8];
  strncpy(r, R[mod12(c.root)], sizeof r);
  r[7] = 0;
  if (c.qual == Q_DARK)
    for (char* p = r; *p; p++)
      if (*p == 'I' || *p == 'V') *p = (char)(*p + 32);
  const char* suf = c.qual == Q_DOM ? "7" : c.qual == Q_HANG ? "sus" : c.qual == Q_POWER ? "5" : "";
  snprintf(out, cap, "%s%s%s", c.planed ? "~" : "", r, suf);
}

static bool hasPad(const Chord* p, int n, int root, int q) {
  for (int i = 0; i < n; i++)
    if (p[i].root == root && p[i].qual == q) return true;
  return false;
}

int chordPads(int homePc, int k, Chord out[12]) {
  uint16_t m = collMask(k);
  int n = 0;
  bool dark = false;
  for (int d = 0; d < 12; d++) {                   // diatonic, upwards from home
    int pc = mod12(homePc + d);
    if (!(m & bit(pc))) continue;
    bool m3 = m & bit(pc + 3), M3 = m & bit(pc + 4), p5 = m & bit(pc + 7);
    if (!p5) continue;                             // diminished: never (spec 6.2, rule 1)
    uint8_t q = M3 ? Q_BRIGHT : m3 ? Q_DARK : Q_POWER;
    if (d == 0) dark = q == Q_DARK;
    out[n++] = {(int8_t)d, q, 0};
  }
  static const int8_t DARK_OUT[][2] = {{5, Q_BRIGHT}, {7, Q_DOM}, {1, Q_BRIGHT}, {7, Q_HANG}, {2, Q_DARK},
                                       {8, Q_BRIGHT}, {10, Q_BRIGHT}, {0, Q_POWER}, {3, Q_BRIGHT}};
  static const int8_t BRIGHT_OUT[][2] = {{10, Q_BRIGHT}, {5, Q_DARK}, {8, Q_BRIGHT}, {7, Q_HANG},
                                         {3, Q_BRIGHT}, {0, Q_HANG}, {7, Q_BRIGHT}, {0, Q_POWER}};
  const int8_t(*X)[2] = dark ? DARK_OUT : BRIGHT_OUT;
  int nx = dark ? 9 : 8;
  for (int i = 0; i < nx && n < 12; i++)
    if (!hasPad(out, n, X[i][0], X[i][1])) out[n++] = {X[i][0], (uint8_t)X[i][1], 0};
  return n;
}

// =================================================================== shapes
const char* shapeWord(uint8_t s) {
  static const char* const W[SH_COUNT] = {"thin", "full", "wide", "flip", "shell", "piano", "piano 8va", "colour", "block"};
  return s < SH_COUNT ? W[s] : "?";
}

int shapeOffsets(uint8_t q, uint8_t s, bool onIV, int8_t out[6]) {
  // [quality][shape] = offsets, -1 ends
  static const int8_t T[5][SH_COUNT][6] = {
      // dark
      {{3, 7, 10, 14, -1}, {0, 3, 7, 10, 14, -1}, {5, 10, 15, 19, -1}, {10, 14, 15, 19, -1}, {3, 10, -1},
       {0, 7, 12, 15, -1}, {7, 12, 15, 19, -1}, {3, 7, 9, 14, -1}, {0, 3, 7, 10, -1}},
      // bright
      {{4, 7, 11, 14, -1}, {0, 4, 7, 11, 14, -1}, {4, 9, 14, 19, -1}, {11, 14, 16, 19, -1}, {4, 11, -1},
       {0, 7, 14, 16, -1}, {7, 12, 16, 19, -1}, {4, 7, 9, 14, -1}, {0, 4, 7, 11, -1}},
      // dominant
      {{10, 14, 16, 21, -1}, {0, 4, 7, 10, 14, -1}, {10, 16, 21, 26, -1}, {4, 10, 14, 21, -1}, {10, 16, -1},
       {0, 7, 10, 16, -1}, {7, 10, 16, 19, -1}, {10, 14, 16, 21, -1}, {0, 4, 7, 10, -1}},
      // hanging
      {{7, 10, 14, 17, -1}, {0, 5, 7, 10, 14, -1}, {5, 10, 14, 19, -1}, {10, 14, 17, 19, -1}, {5, 10, -1},
       {0, 7, 14, 17, -1}, {7, 12, 17, 19, -1}, {0, 2, 7, -1}, {0, 5, 7, 10, -1}},
      // power
      {{0, 7, 12, -1}, {0, 7, 12, 19, -1}, {0, 7, 14, -1}, {7, 12, 19, -1}, {0, 7, -1},
       {0, 7, 12, 19, -1}, {7, 12, 19, -1}, {0, 2, 7, -1}, {0, 7, 12, -1}},
  };
  if (q > Q_POWER || s >= SH_COUNT) return 0;
  const int8_t* src = T[q][s];
  if (q == Q_BRIGHT && s == SH_COLOUR && onIV) {   // Bright 9 #11: on IV only
    static const int8_t S11[] = {4, 11, 14, 18, -1};
    src = S11;
  }
  int n = 0;
  while (n < 6 && src[n] >= 0) out[n] = src[n], n++;
  return n;
}

// ---- placement (spec 6.4)
static void sort8(uint8_t* v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      uint8_t t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
}

int placeCost(const uint8_t* v, int n, const uint8_t* prev, int nPrev, int topTarget) {
  if (n <= 0) return 0;
  int cost = 0, top = v[n - 1];
  // movement between sorted voices, matched from the top
  if (nPrev > 0) {
    int k = n < nPrev ? n : nPrev;
    for (int i = 0; i < k; i++) cost += abs((int)v[n - 1 - i] - (int)prev[nPrev - 1 - i]);
    for (int i = 0; i < n - k; i++) {              // extra voices: to the nearest old one
      int best = 99;
      for (int j = 0; j < nPrev; j++) best = abs((int)v[i] - (int)prev[j]) < best ? abs((int)v[i] - (int)prev[j]) : best;
      cost += best;
    }
    if (abs(top - (int)prev[nPrev - 1]) > 4) cost += 4;   // the top voice leaps
  }
  // register
  int out = 0;
  for (int i = 0; i < n; i++) {
    if (v[i] < 48) out++;
    else if (v[i] < 52 && i + 1 < n && v[i + 1] - v[i] <= 4) out++;   // no seconds/thirds below 52
  }
  if (top < 60 || top > 79) out++;
  int span = top - v[0];
  if (span > 19) out++;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (v[j] - v[i] == 13) out++;                 // no minor ninths between voices
  cost += 3 * out;
  if (span > 12) cost += 2;
  if (n >= 2 && top - v[n - 2] == 1) cost += 5;     // semitone directly under the top voice
  if (top < 64 || top > 72) cost += 1;              // best 64..72
  if (topTarget) {
    int d = abs(top - topTarget);
    if (d > 2) cost += d - 2;
  }
  return cost;
}

int place(const int8_t* tones, int n, int rootPc, const uint8_t* prev, int nPrev, int topTarget, uint8_t out[8]) {
  if (n <= 0) return 0;
  if (n > 8) n = 8;
  int mx = 0;
  for (int i = 0; i < n; i++) mx = tones[i] > mx ? tones[i] : mx;
  // base: the root octave that puts the top note nearest the middle of 64..72
  int want = topTarget ? topTarget : 68, base = -1, bd = 999;
  for (int o = 2; o <= 6; o++) {
    int r = o * 12 + mod12(rootPc);
    int d = abs(r + mx - want);
    if (d < bd) bd = d, base = r;
  }
  uint8_t v0[8];
  for (int i = 0; i < n; i++) v0[i] = (uint8_t)(base + tones[i]);
  sort8(v0, n);
  int bestCost = 1 << 30;
  for (int f = 0; f < n; f++)
    for (int oct = -12; oct <= 12; oct += 12) {
      uint8_t v[8];
      bool ok = true;
      for (int i = 0; i < n; i++) {
        int x = v0[i] + (i < f ? 12 : 0) + oct;
        if (x < 24 || x > 108) ok = false;
        v[i] = (uint8_t)x;
      }
      if (!ok) continue;
      sort8(v, n);
      bool dup = false;
      for (int i = 1; i < n; i++) dup |= v[i] == v[i - 1];
      if (dup) continue;
      int c = placeCost(v, n, prev, nPrev, topTarget);
      if (c < bestCost) {
        bestCost = c;
        memcpy(out, v, n);
      }
    }
  return bestCost == (1 << 30) ? 0 : n;
}

// ======================================================== next-chord ranks
struct Row { int8_t from, fq; int8_t to[6][2]; };
#define C2(r, q) {r, q}
static const Row DARK_ROWS[] = {
    {0, Q_DARK, {C2(8, Q_BRIGHT), C2(10, Q_BRIGHT), C2(5, Q_DARK), C2(7, Q_DARK), C2(3, Q_BRIGHT), C2(5, Q_BRIGHT)}},
    {3, Q_BRIGHT, {C2(10, Q_BRIGHT), C2(5, Q_DARK), C2(8, Q_BRIGHT), C2(0, Q_DARK), C2(-1, 0), C2(-1, 0)}},
    {5, Q_DARK, {C2(0, Q_DARK), C2(10, Q_BRIGHT), C2(7, Q_DARK), C2(8, Q_BRIGHT), C2(-1, 0), C2(-1, 0)}},
    {5, Q_BRIGHT, {C2(0, Q_DARK), C2(10, Q_BRIGHT), C2(3, Q_BRIGHT), C2(-1, 0), C2(-1, 0), C2(-1, 0)}},
    {7, Q_DARK, {C2(0, Q_DARK), C2(8, Q_BRIGHT), C2(5, Q_DARK), C2(10, Q_BRIGHT), C2(-1, 0), C2(-1, 0)}},
    {8, Q_BRIGHT, {C2(10, Q_BRIGHT), C2(0, Q_DARK), C2(3, Q_BRIGHT), C2(7, Q_DARK), C2(-1, 0), C2(-1, 0)}},
    {10, Q_BRIGHT, {C2(0, Q_DARK), C2(8, Q_BRIGHT), C2(3, Q_BRIGHT), C2(7, Q_DARK), C2(-1, 0), C2(-1, 0)}},
    {2, Q_DARK, {C2(0, Q_DARK), C2(7, Q_DARK), C2(5, Q_BRIGHT), C2(-1, 0), C2(-1, 0), C2(-1, 0)}},
};
static const Row BRIGHT_ROWS[] = {
    {0, Q_BRIGHT, {C2(5, Q_BRIGHT), C2(9, Q_DARK), C2(7, Q_BRIGHT), C2(2, Q_DARK), C2(10, Q_BRIGHT), C2(4, Q_DARK)}},
    {2, Q_DARK, {C2(5, Q_BRIGHT), C2(7, Q_HANG), C2(4, Q_DARK), C2(0, Q_BRIGHT), C2(-1, 0), C2(-1, 0)}},
    {4, Q_DARK, {C2(9, Q_DARK), C2(5, Q_BRIGHT), C2(2, Q_DARK), C2(-1, 0), C2(-1, 0), C2(-1, 0)}},
    {5, Q_BRIGHT, {C2(0, Q_BRIGHT), C2(7, Q_BRIGHT), C2(9, Q_DARK), C2(5, Q_DARK), C2(-1, 0), C2(-1, 0)}},
    {7, Q_BRIGHT, {C2(9, Q_DARK), C2(5, Q_BRIGHT), C2(0, Q_BRIGHT), C2(-1, 0), C2(-1, 0), C2(-1, 0)}},
    {9, Q_DARK, {C2(5, Q_BRIGHT), C2(2, Q_DARK), C2(7, Q_BRIGHT), C2(4, Q_DARK), C2(-1, 0), C2(-1, 0)}},
    {10, Q_BRIGHT, {C2(5, Q_BRIGHT), C2(0, Q_BRIGHT), C2(-1, 0), C2(-1, 0), C2(-1, 0), C2(-1, 0)}},
};
#undef C2

// which table row describes `c`: V7 / Vsus share the V row, power chords their root's
static const Row* rowOf(const Chord& c, bool dark) {
  const Row* rows = dark ? DARK_ROWS : BRIGHT_ROWS;
  int n = dark ? 8 : 7;
  int r = mod12(c.root);
  for (int i = 0; i < n; i++)
    if (rows[i].from == r && rows[i].fq == c.qual) return &rows[i];
  for (int i = 0; i < n; i++)                       // same root, other quality
    if (rows[i].from == r) return &rows[i];
  return nullptr;
}

int nextChords(const Chord& from, bool dark, Chord out[8]) {
  const Row* row = rowOf(from, dark);
  if (!row) {                                       // not in the table: head home
    out[0] = {0, (uint8_t)(dark ? Q_DARK : Q_BRIGHT), 0};
    return 1;
  }
  int n = 0;
  for (int i = 0; i < 6 && row->to[i][0] >= 0; i++) {
    uint8_t q = (uint8_t)row->to[i][1];
    if (!dark && row->to[i][0] == 7 && q == Q_BRIGHT && from.qual == Q_HANG) q = Q_BRIGHT;
    out[n++] = {row->to[i][0], q, 0};
  }
  return n;
}

float nextWeight(const Chord& from, const Chord& to, bool dark) {
  if (from.root == to.root && from.qual == to.qual) return 0.15f;   // staying: house repeats chords
  Chord nx[8];
  int n = nextChords(from, dark, nx);
  static const float W[3] = {0.40f, 0.25f, 0.15f};
  for (int i = 0; i < n; i++) {
    bool same = nx[i].root == mod12(to.root) &&
                (nx[i].qual == to.qual || (nx[i].root == 7 && (to.qual == Q_DOM || to.qual == Q_HANG)));
    if (same) return i < 3 ? W[i] : 0.20f / (n - 3);
  }
  return 0.02f;
}

// ================================================================ skeletons
const Skeleton SKELETONS[] = {
    {"hypnotic deep", true, 8, "i9 i9 iv9 iv9 i9 i9 iv9 bVII"},
    {"melancholic drive", true, 8, "i9 i9 bVImaj7 bVII i9 i9 bVImaj7 v7"},
    {"euphoric, never lands", true, 8, "bVImaj7 bVII i7 i7 bVImaj7 bVII v7 i7"},
    {"warm dorian funk", true, 8, "i7 IV9 i7 IV9 i7 IV9 i7 bVII|IV"},
    {"dusty 90s memory", true, 8, "~i7 ~bIIIm7 ~iv7 ~bVIIm7 ~i7 ~bIIIm7 ~iv7 ~bVIIm7"},
    {"piano house, hands up", false, 8, "vi IV I V vi IV I Vsus|V"},
    {"gospel lift", false, 8, "ii7 iii7 IVmaj7 V9sus ii7 iii7 IVmaj7 IV|iv"},
    {"sunset nostalgia", false, 8, "IVmaj7 iii7 vi9 vi9 IVmaj7 iii7 vi9 vi9"},
    {"sunny, french disco", false, 8, "I bVII IV I I bVII IV I"},
    {"emotive anthem", true, 16, "i bVI bIII bVII i bVI bIII bVII i bVI bIII bVII iv iv bVI bVII"},
    {"widescreen rise", true, 8, "i bIII bVII IV i bIII bVII IV"},
    {"lament, dramatic", true, 16, "i i bVII bVII bVI bVI v v i i bVII bVII bVI bVI bVI bVII"},
    {"journey and return", true, 16,
     "i9 i9 bVImaj7 bVII i9 i9 bVImaj7 bVII iv9 iv9 bIIImaj7 bVIIsus i9 i9 bVImaj7 bVII"},
    {"techno tension", true, 8, "i/1 bVII/1 bVI/1 bVII/1 i/1 bVII/1 bVI/1 bVII/1"},
};
const int N_SKELETONS = sizeof SKELETONS / sizeof SKELETONS[0];

bool parseRoman(const char* t, Chord& c, uint8_t& shape) {
  c = Chord();
  shape = SH_THIN;
  if (*t == '~') {
    c.planed = 1;
    shape = SH_BLOCK;
    t++;
  }
  int acc = 0;
  if (*t == 'b') acc = -1, t++;
  else if (*t == '#') acc = 1, t++;
  static const struct { const char* s; int v; } N[] = {{"VII", 11}, {"III", 4}, {"IV", 5}, {"VI", 9}, {"II", 2},
                                                      {"V", 7}, {"I", 0}};
  bool lower = *t == 'i' || *t == 'v';
  int deg = -1;
  for (auto& n : N) {
    size_t L = strlen(n.s);
    bool match = true;
    for (size_t i = 0; i < L; i++)
      if ((t[i] & ~32) != n.s[i]) match = false;
    if (match) {
      deg = n.v;
      t += L;
      break;
    }
  }
  if (deg < 0) return false;
  c.root = (int8_t)mod12(deg + acc);
  c.qual = lower ? Q_DARK : Q_BRIGHT;
  if (!strncmp(t, "maj7", 4)) t += 4;
  else if (!strncmp(t, "m7", 2)) c.qual = Q_DARK, t += 2;
  else if (*t == 'm') c.qual = Q_DARK, t++;
  if (*t == '9' || *t == '7') {
    if (!lower && c.qual == Q_BRIGHT && (c.root == 7 || (c.root == 5 && *t == '9'))) c.qual = Q_DOM;
    t++;
  }
  if (!strncmp(t, "sus", 3)) c.qual = Q_HANG, t += 3;
  return true;
}

// =================================================================== rhythm
const Rhythm STABS[] = {
    {"SUSTAIN", "x..............."}, {"OFFBEAT", "..x...x...x...x."}, {"DOTTED", "x..x..x..x..x..."},
    {"TRESILLO", "x..x..x.x..x..x."}, {"DEEP", "...x..x...x....."},  {"M1 PIANO", "x..x..x...x.x.x."},
    {"GARAGE", ".x..x..x..x..x.."},  {"PAD PUSH", "...............x"}, {"TECHNO", "..x............."},
};
const int N_STABS = sizeof STABS / sizeof STABS[0];

const Rhythm CELLS[] = {
    {"OFFBEAT", "..x...x...x...x."}, {"TRESILLO", "x..x..x.x..x..x."}, {"HOOK", "x..x..x...x.x..."},
    {"SKIP", "..x..x..x.x....."},    {"PUSH", "...x..x....x..x."},     {"PIANO", "x.x..x.x..x.x..."},
    {"FLUTE LONG", "x.........x.x..."}, {"FLUTE PICKUP", ".............xxx"}, {"CALL", "x.x..x.........."},
    {"RESPONSE", "......x.x..x...."},
};
const int N_CELLS = sizeof CELLS / sizeof CELLS[0];

uint16_t rhythmMask(const char* s) {
  uint16_t m = 0;
  for (int i = 0; i < 16 && s[i]; i++)
    if (s[i] == 'x') m |= 1 << i;
  return m;
}

uint16_t pentaMask(int homePc, bool dark) {
  static const int8_t D[5] = {0, 3, 5, 7, 10}, B[5] = {0, 2, 4, 7, 9};
  uint16_t m = 0;
  for (int i = 0; i < 5; i++) m |= bit(homePc + (dark ? D[i] : B[i]));
  return m;
}

}  // namespace ht
