// bass.cpp — the bass program
//
// Patterns are written in ROLES (root, fifth, octave, seventh, approach...),
// not notes, and resolved against harmony on every 16th:
//   FOLLOW CHORD  the chord you last played on the pads (changes land on the
//                 next 16th, so the bass locks to the Move's groove while your
//                 chords stay free and unquantised)
//   FOLLOW PROG   walks the progression of the row you played, one chord per
//                 RATE, with approach notes aimed at the NEXT chord
//   FOLLOW KEY    pedals the key's tonic (the techno drone)
//
// Library: hand-written lines from basslines.txt, then per-genre grammar
// generators (seeded, so every page is repeatable) — effectively endless.
//
// Playback: swing, gate, legato slides (overlapping notes = glide on a mono
// synth), accents and ghosts, a kick-aware DUCK (virtual sidechain), density
// that thins or adds ghost notes bar by bar, and turnaround fills on the last
// beat of every 4th or 8th bar.

#include "bass.h"
#include "harm.h"
#include <LittleFS.h>
#include <vector>
#include "bass_data.h"
#include "clock.h"
#include "config.h"
#include "engine.h"
#include "platform.h"

using engine::Chord;
using engine::Harmony;

namespace {

// ======================================================= model ============

enum Role : uint8_t { RL_REST, RL_TIE, RL_R, RL_O, RL_L, RL_5, RL_3, RL_7, RL_2, RL_4, RL_6, RL_B2, RL_U, RL_A, RL_C, RL_COUNT };
const char ROLE_CH[RL_COUNT] = {'.', '_', 'R', 'O', 'L', '5', '3', '7', '2', '4', '6', 'b', 'u', 'a', 'c'};
const char* const ROLE_NAME[RL_COUNT] = {"", "", "R", "O", "L", "5", "3", "7", "2", "4", "6", "b2", "U", "A", "C"};
const uint8_t EDIT_ROLES[] = {RL_R, RL_O, RL_L, RL_5, RL_3, RL_7, RL_2, RL_4, RL_6, RL_B2, RL_U, RL_A, RL_C};
const int N_EDIT_ROLES = sizeof EDIT_ROLES;

enum { V_GHOST = 0, V_NORM = 1, V_ACC = 2 };
enum { A_SLIDE = 1, A_SHORT = 2, A_LONG = 4 };

struct Step { uint8_t role, vel, art; int8_t oct; };
struct Pattern {
  char name[10];
  uint8_t genre;       // index into BASS_GENRE (swing/gate defaults)
  uint8_t len;         // 16, 32 or 64
  Step s[64];
};

struct Rng {
  uint32_t s = 1;
  void seed(uint32_t x) {
    s = (x ^ 0x9E3779B9u) * 2654435761u;
    if (!s) s = 1;
    for (int i = 0; i < 3; i++) next();
  }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  int pct() { return next() % 100; }
  int range(int n) { return n <= 1 ? 0 : (int)(next() % (uint32_t)n); }
};

uint32_t mix(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return h;
}

// ======================================================= params ===========

const char* const FOLLOW_N[] = {"CHORD", "PROG", "KEY"};
const char* const LEN_N[] = {"1 BAR", "2 BARS", "4 BARS"};
const char* const FILL_N[] = {"OFF", "4 BARS", "8 BARS"};
const char* const RATE_N[] = {"1/2 BAR", "1 BAR", "2 BARS"};
const char* const QUANT_N[] = {"BAR", "BEAT", "NOW"};

struct ParamDef {
  const char* label;
  uint8_t group, kind;
  int16_t lo, hi, def;
  uint8_t step;
  bool wrap;
  const char* const* opts;
};

const ParamDef PDEF[bass::BP_COUNT] = {
    {"TEMPO", G_BASS, K_NUM, 40, 240, 124, 1, false, nullptr},
    {"SWING", G_BASS, K_NUM, 50, 75, 50, 1, false, nullptr},
    {"OCTAVE", G_BASS, K_NUM, -4, 2, 0, 1, false, nullptr},
    {"GENRE", G_BASS, K_LIST, 0, 0, 0, 1, true, nullptr},
    {"FOLLOW", G_HARM, K_LIST, 0, 2, 0, 1, true, FOLLOW_N},
    {"LENGTH", G_GEN, K_LIST, 0, 2, 1, 1, true, LEN_N},
    {"GATE", G_FEEL, K_NUM, 25, 200, 100, 5, false, nullptr},
    {"SLIDE", G_FEEL, K_NUM, 0, 100, 100, 5, false, nullptr},
    {"ACCENT", G_FEEL, K_NUM, 0, 100, 60, 5, false, nullptr},
    {"DENSITY", G_GEN, K_BIPOLAR, -100, 100, 0, 10, false, nullptr},
    {"DUCK", G_FEEL, K_NUM, 0, 100, 0, 5, false, nullptr},
    {"BASS CH", G_SYS, K_NUM, 0, 15, DEFAULT_BASS_CH, 1, true, nullptr},
    {"NUDGE", G_SYS, K_BIPOLAR, -20, 20, 0, 1, false, nullptr},
    {"FILL", G_GEN, K_LIST, 0, 2, 0, 1, true, FILL_N},
    {"PROG RATE", G_HARM, K_LIST, 0, 2, 1, 1, true, RATE_N},
    {"SWITCH", G_SYS, K_LIST, 0, 2, 0, 1, true, QUANT_N},
    {"VELOCITY", G_FEEL, K_NUM, 30, 127, 100, 4, false, nullptr},
};

int16_t val[bass::BP_COUNT];
bool dirty = false;
uint32_t dirtyAt = 0;

// ======================================================= grammars =========
// One per genre id in basslines.txt. Weights are for R O L 5 3 7 2 4 6 b u.

struct Grammar {
  const char* id;
  const char* abbr;
  const char* masks;          // onset shapes, 'x' = note, '|' between shapes
  uint8_t euclid, kMin, kMax; // % chance of a Euclidean rhythm instead, and its hits
  uint8_t avoidKick;          // % chance to drop a note that lands on the kick
  uint8_t w[11];
  uint8_t ghost, accent, slide, shortP, longP, tie;
  bool accentOff;             // accents prefer offbeats
  uint8_t maxRoles;           // distinct notes per bar: restraint makes grooves
  uint8_t repeat;             // % chance a note repeats the previous one
  uint8_t style;              // 0 plain, 1 octave-hopping (disco), 2 melodic contour
};

const Grammar GRAMMARS[] = {
    {"TECHNO", "TCH", ".xxx.xxx.xxx.xxx|..x...x...x...x.|.xx..xx..xx..xx.|.x.x.x.x.x.x.x.x|...x...x...x...x|..xx..xx..xx..xx|.x...x.x.x...x.x|.xx..x...xx..x..",
     35, 4, 10, 90, {80, 6, 4, 4, 0, 1, 0, 0, 0, 5, 0}, 30, 20, 3, 50, 5, 0, true, 2, 75, 0},
    {"ACID", "ACD", "xx.xx.x.xx.x.xxx|x.xx.xx.x.xx.x.x|xxx.x.xxx.x.xx.x|x.x.xx.x.xxx.x.x",
     50, 7, 12, 10, {45, 20, 5, 9, 5, 8, 3, 0, 0, 5, 0}, 10, 35, 28, 20, 8, 6, false, 5, 45, 0},
    {"HOUSE", "HSE", "..x...x...x...x.|..x...x...x..xx.|x..x..x.x..x..x.|..x..x....x..x..|..xx..x...xx..x.",
     25, 3, 7, 70, {60, 16, 3, 8, 4, 7, 2, 0, 0, 0, 0}, 15, 25, 5, 25, 15, 3, false, 3, 60, 0},
    {"DEEP", "DEP", "x..x..x...x.....|..x....x..x...x.|x.....x...x.....|x..x.......x..x.",
     30, 3, 6, 40, {50, 8, 3, 12, 8, 12, 5, 2, 0, 0, 0}, 8, 18, 12, 8, 45, 12, false, 4, 45, 0},
    {"TECHHOUSE", "TKH", ".x.x.x...x.x.xx.|..xx..x...xx..x.|.xx..x.x.xx..x.x|.x.x...x.x.x...x",
     40, 5, 9, 85, {70, 12, 4, 6, 0, 4, 0, 0, 0, 4, 0}, 30, 30, 4, 65, 2, 0, true, 3, 65, 0},
    {"MINIMAL", "MIN", "...x..x....x....|..x......x.....x|.x.....x..x.....|......x....x.x..",
     60, 2, 6, 80, {75, 10, 0, 5, 0, 0, 0, 0, 0, 5, 5}, 25, 25, 0, 75, 0, 0, true, 2, 70, 0},
    {"GARAGE", "GRG", "x..x......x..x..|x.....x..x..x...|x..x..x..x......|x.x...x..x.x....",
     30, 3, 7, 20, {55, 12, 3, 10, 4, 10, 0, 3, 0, 0, 3}, 20, 25, 10, 25, 15, 5, false, 4, 45, 0},
    {"ELECTRO", "ELC", "x..x..x.x..x.x..|x.xx.x..x.xx.x..|x.x..x.xx.x..x.x|x..x...xx..x..x.",
     45, 5, 9, 20, {50, 25, 5, 8, 0, 6, 0, 0, 0, 6, 0}, 10, 30, 8, 45, 5, 0, false, 3, 45, 0},
    {"BREAKS", "BRK", "x.....x...x.....|x..x......xx....|x..x..x..x......|x.x.......x.x...",
     40, 3, 7, 0, {60, 10, 6, 10, 6, 8, 0, 0, 0, 0, 0}, 20, 30, 10, 20, 25, 8, false, 3, 50, 0},
    {"BASSMUS", "BSM", "x.........x.....|x..x..x...x.x...|x.......x.......|x.....x.x.......",
     30, 2, 5, 0, {60, 10, 10, 6, 0, 6, 0, 0, 0, 8, 0}, 5, 40, 20, 10, 50, 25, false, 3, 50, 0},
    {"DISCO", "DSC", "x.x.x.x.x.x.x.x.|x..xx.x.x..xx.x.|x.xx.x.xx.xx.x.x|x.x.x.xxx.x.x.xx",
     20, 6, 10, 0, {45, 40, 0, 6, 4, 5, 0, 0, 0, 0, 0}, 10, 20, 5, 40, 5, 0, false, 3, 30, 1},
    {"AFRO", "AFR", "x..x..x.x..x..x.|x..x..x...x.....|..x..x..x..x..x.|x..x.x..x..x..x.",
     40, 4, 7, 30, {55, 10, 0, 12, 6, 8, 0, 5, 0, 0, 4}, 25, 30, 5, 25, 15, 0, false, 4, 50, 0},
    {"TRANCE", "TRN", ".xxx.xxx.xxx.xxx|..xx..xx..xx..xx|..x...x...x...x.|.xx..xx..xx..xx.",
     20, 6, 12, 95, {75, 10, 3, 6, 3, 3, 0, 0, 0, 0, 0}, 20, 25, 0, 60, 0, 0, true, 2, 75, 0},
    {"MELODIC", "MEL", "x.x.x.x.x.x.x.x.|x..x.x.x..x.x.x.|.x.x.x.x.x.x.x.x|x.x.x..xx.x.x..x",
     30, 6, 10, 50, {35, 15, 0, 20, 15, 10, 5, 0, 0, 0, 0}, 10, 20, 10, 30, 10, 0, false, 5, 25, 2},
    {"DUB", "DUB", "x.........x.....|..x.....x.......|x.....x.....x...|..x.......x.....",
     30, 2, 4, 20, {60, 0, 10, 20, 5, 5, 0, 0, 0, 0, 0}, 5, 20, 10, 0, 80, 30, false, 3, 50, 0},
    {"EBM", "EBM", "xxxxxxxxxxxxxxxx|x.x.x.x.x.x.x.x.|xxx.xxx.xxx.xxx.|x.xxx.xxx.xxx.xx",
     20, 8, 14, 0, {85, 5, 0, 5, 0, 0, 0, 0, 0, 5, 0}, 30, 30, 0, 70, 0, 0, false, 2, 85, 0},
};
const int N_GRAMMARS = sizeof GRAMMARS / sizeof GRAMMARS[0];
const uint8_t WEIGHT_ROLE[11] = {RL_R, RL_O, RL_L, RL_5, RL_3, RL_7, RL_2, RL_4, RL_6, RL_B2, RL_U};

const Grammar* grammarOf(int genre) {
  if (genre < 0 || genre >= N_BASS_GENRE) return &GRAMMARS[0];
  for (int i = 0; i < N_GRAMMARS; i++)
    if (!strcmp(GRAMMARS[i].id, BASS_GENRE[genre].id)) return &GRAMMARS[i];
  return &GRAMMARS[0];
}

// ======================================================= library ==========

const int GEN_PAGES = 60;           // generated pages after the hand-written ones
std::vector<Pattern> favs, fresh;   // FAVS (saved) and NEW (mutations this session)
int mutCount = 0;

int nBanks() { return N_BASS_GENRE + 2; }
int bankFavs() { return N_BASS_GENRE; }
int bankNew() { return N_BASS_GENRE + 1; }

const char* bankName(int b) {
  if (b == bankFavs()) return "FAVS";
  if (b == bankNew()) return "NEW";
  return BASS_GENRE[b].name;
}

int curatedPages(int b) {
  if (b >= N_BASS_GENRE) return 0;
  return (BASS_GENRE[b].count + 15) / 16;
}

int pagesOf(int b) {
  if (b == bankFavs()) return max(1, (int)(favs.size() + 15) / 16);
  if (b == bankNew()) return max(1, (int)(fresh.size() + 15) / 16);
  return curatedPages(b) + GEN_PAGES;
}

// ---- DSL
bool parseStep(const char* t, int n, Step& st) {
  st = {RL_REST, V_NORM, 0, 0};
  if (n <= 0) return false;
  int i = 0;
  switch (t[0]) {
    case '.': st.role = RL_REST; return true;
    case '_': st.role = RL_TIE; return true;
    case 'R': st.role = RL_R; break;
    case 'O': st.role = RL_O; break;
    case 'L': st.role = RL_L; break;
    case '5': st.role = RL_5; break;
    case '3': st.role = RL_3; break;
    case '7': st.role = RL_7; break;
    case '2': st.role = RL_2; break;
    case '4': st.role = RL_4; break;
    case '6': st.role = RL_6; break;
    case 'b': st.role = RL_B2; break;
    case 'u': st.role = RL_U; break;
    case 'a': st.role = RL_A; break;
    case 'c': st.role = RL_C; break;
    default: return false;
  }
  for (i = 1; i < n; i++) {
    switch (t[i]) {
      case '!': st.vel = V_ACC; break;
      case '\'': st.vel = V_GHOST; break;
      case '~': st.art |= A_SLIDE; break;
      case '>': st.art |= A_SHORT; break;
      case '=': st.art |= A_LONG; break;
      case '+': st.oct++; break;
      case '-': st.oct--; break;
    }
  }
  return true;
}

void parsePattern(const char* name, const char* steps, uint8_t genre, Pattern& p) {
  memset(&p, 0, sizeof p);
  strlcpy(p.name, name, sizeof p.name);
  p.genre = genre;
  const char* s = steps;
  int n = 0;
  while (*s && n < 64) {
    while (*s == ' ') s++;
    const char* e = s;
    while (*e && *e != ' ') e++;
    if (e > s && !(e - s == 1 && *s == '|')) {
      parseStep(s, e - s, p.s[n]);
      n++;
    }
    s = e;
  }
  p.len = n >= 64 ? 64 : n >= 32 ? 32 : 16;
}

String serialize(const Pattern& p) {
  String out;
  for (int i = 0; i < p.len; i++) {
    const Step& st = p.s[i];
    if (i) out += (i % 4 == 0) ? " | " : " ";
    out += ROLE_CH[st.role];
    if (st.role <= RL_TIE) continue;
    for (int o = 0; o < st.oct; o++) out += '+';
    for (int o = 0; o > st.oct; o--) out += '-';
    if (st.vel == V_ACC) out += '!';
    if (st.vel == V_GHOST) out += '\'';
    if (st.art & A_SLIDE) out += '~';
    if (st.art & A_SHORT) out += '>';
    if (st.art & A_LONG) out += '=';
  }
  return out;
}

// ---- generator
uint16_t parseMask(const char* s, int n) {
  uint16_t m = 0;
  for (int i = 0; i < n && i < 16; i++)
    if (s[i] == 'x') m |= 1 << i;
  return m;
}

int countMasks(const char* all) {
  int n = 1;
  for (const char* c = all; *c; c++) n += *c == '|';
  return n;
}

uint16_t maskAt(const char* all, int idx) {
  const char* s = all;
  for (int i = 0; i < idx; i++) {
    s = strchr(s, '|');
    if (!s) return 0;
    s++;
  }
  const char* e = strchr(s, '|');
  return parseMask(s, e ? e - s : strlen(s));
}

uint16_t euclid(int k, int n, int rot) {
  uint16_t m = 0;
  for (int i = 0; i < n; i++)
    if (((i * k) % n) < k) m |= 1 << ((i + rot) % n);
  return m;
}

int popcount16(uint16_t m) {
  int c = 0;
  for (; m; m &= m - 1) c++;
  return c;
}

uint8_t pickRole(const Grammar& g, Rng& r, const uint8_t* allowed, int nAllowed) {
  int total = 0;
  for (int i = 0; i < 11; i++) {
    bool ok = !allowed;
    for (int j = 0; allowed && j < nAllowed; j++) ok |= allowed[j] == WEIGHT_ROLE[i];
    if (ok) total += g.w[i];
  }
  if (total <= 0) return RL_R;
  int x = r.range(total);
  for (int i = 0; i < 11; i++) {
    bool ok = !allowed;
    for (int j = 0; allowed && j < nAllowed; j++) ok |= allowed[j] == WEIGHT_ROLE[i];
    if (!ok) continue;
    if (x < g.w[i]) return WEIGHT_ROLE[i];
    x -= g.w[i];
  }
  return RL_R;
}

int roleSemis(uint8_t role) {            // rough pitch height, for contour and pictures
  switch (role) {
    case RL_L: return -12;
    case RL_B2: return 1;
    case RL_2: return 2;
    case RL_3: return 3;
    case RL_4: return 5;
    case RL_5: return 7;
    case RL_6: return 8;
    case RL_7: return 10;
    case RL_O: return 12;
    case RL_U: return 2;
    case RL_A: return -1;
    case RL_C: return -1;
    default: return 0;
  }
}

// what a line is like: one feel word, traits, energy 1..5 (for the screens: names say nothing)
void describe(const Pattern& p, char* feel, size_t cap, uint8_t& traits, uint8_t& energy) {
  int bars = std::max(1, p.len / 16), total = 0, offb = 0, onBeat = 0, odd = 0, slides = 0, accents = 0;
  int ghosts = 0, octs = 0, chrom = 0, longs = 0, gallop = 0, shorts = 0;
  uint16_t roles = 0;
  for (int i = 0; i < p.len; i++) {
    const Step& st = p.s[i];
    if (st.role < RL_R) continue;
    total++;
    int s16 = i % 16;
    offb += s16 % 4 == 2;
    onBeat += s16 % 4 == 0;
    odd += s16 % 2 == 1;
    slides += (st.art & A_SLIDE) != 0;
    accents += st.vel == V_ACC;
    ghosts += st.vel == V_GHOST;
    shorts += (st.art & A_SHORT) != 0;
    octs += st.role == RL_O || st.role == RL_L || st.oct != 0;
    chrom += st.role == RL_B2 || st.role == RL_A || st.role == RL_C;
    longs += (st.art & A_LONG) || (i + 1 < p.len && p.s[i + 1].role == RL_TIE);
    if (st.role != RL_R && st.role != RL_O && st.role != RL_L) roles |= 1 << st.role;
  }
  for (int b = 0; b < p.len / 4; b++) {           // a beat of x.xx or xx.x: a gallop
    int m = 0;
    for (int k = 0; k < 4; k++) m |= (p.s[b * 4 + k].role >= RL_R) << k;
    gallop += m == 0b1101 || m == 0b1011;
  }
  int threes = 0, wide = 0, prev = -1, first = -1;   // gaps between notes: 3s = dotted, 6+ = skippy
  for (int i = 0; i < p.len; i++) {
    if (p.s[i].role < RL_R) continue;
    if (prev >= 0) threes += i - prev == 3, wide += i - prev >= 6;
    else first = i;
    prev = i;
  }
  if (first >= 0) {
    int g = first + p.len - prev;
    threes += g == 3;
    wide += g >= 6;
  }
  int n = (total * 10 + bars * 5) / (bars * 10);  // notes in a bar, rounded
  int distinct = popcount16(roles);
  const char* f;
  if (n <= 3) f = longs ? "HELD" : "SPARSE";
  else if (n >= 7 && slides >= bars * 2 && accents >= bars) f = "ACID";
  else if (n >= 11) f = "ROLLING";
  else if (offb * 10 >= total * 6) f = "OFFBEAT";
  else if (gallop >= bars * 2) f = "GALLOP";
  else if (wide >= bars && n <= 6) f = longs ? "DEEP" : "SKIPPY";
  else if (threes * 2 >= total) f = "DOTTED";
  else if (n <= 5) f = longs ? "DEEP" : "PUNCHY";
  else if (odd * 2 >= total) f = "BOUNCY";
  else if (odd * 4 >= total) f = "FUNKY";
  else if (!odd) f = "DRIVING";
  else f = "STEADY";
  strlcpy(feel, f, cap);
  traits = 0;
  if (distinct >= 3) traits |= BT_MELODIC;
  if (!distinct) traits |= BT_ROOT;
  if (octs * 4 >= total && octs >= bars) traits |= BT_OCT;
  if (slides >= bars && strcmp(f, "ACID")) traits |= BT_SLIDE;
  if (chrom >= bars) traits |= BT_CHROM;
  if (longs >= bars && strcmp(f, "HELD") && strcmp(f, "DEEP")) traits |= BT_LONG;
  if (ghosts >= bars * 2) traits |= BT_GHOST;
  if (accents >= bars * 3) traits |= BT_ACCENT;
  energy = (uint8_t)constrain(1 + (n * 10 + accents * 10 / bars * 3 + slides * 10 / bars * 2 - shorts * 5 / bars - 20) / 30, 1, 5);
}
const char* feelOf(const Pattern& p) {
  static char f[9];
  uint8_t t, e;
  describe(p, f, sizeof f, t, e);
  return f;
}

void genBar(const Grammar& g, Rng& r, Step* bar) {
  uint16_t on;
  if (r.pct() < g.euclid) {
    int k = g.kMin + r.range(g.kMax - g.kMin + 1);
    on = euclid(k, 16, r.range(16));
  } else {
    on = maskAt(g.masks, r.range(countMasks(g.masks)));
  }
  if (r.pct() < 35) on ^= 1 << r.range(16);             // one small surprise
  for (int s = 0; s < 16; s += 4)
    if ((on & (1 << s)) && r.pct() < g.avoidKick) on &= ~(1 << s);
  if (popcount16(on) < 2) on |= (1 << 2) | (1 << 10);

  uint8_t palette[6] = {RL_R};
  int nPal = 1;
  for (int tries = 0; nPal < g.maxRoles && tries < 20; tries++) {
    uint8_t c = pickRole(g, r, nullptr, 0);
    bool dup = false;
    for (int j = 0; j < nPal; j++) dup |= palette[j] == c;
    if (!dup) palette[nPal++] = c;
  }
  if (g.style == 2)                                       // melodic: low to high
    for (int a = 0; a < nPal; a++)
      for (int b = a + 1; b < nPal; b++)
        if (roleSemis(palette[b]) < roleSemis(palette[a])) {
          uint8_t t = palette[a];
          palette[a] = palette[b];
          palette[b] = t;
        }

  uint8_t prev = RL_R;
  int noteIdx = 0, dir = 1, walk = 0;
  for (int s = 0; s < 16; s++) {
    Step& st = bar[s];
    st = {RL_REST, V_NORM, 0, 0};
    if (!(on & (1 << s))) continue;
    uint8_t role;
    if (noteIdx == 0) role = r.pct() < 80 ? RL_R : palette[r.range(nPal)];
    else if (g.style == 1) role = (noteIdx % 2) ? (r.pct() < 80 ? RL_O : palette[r.range(nPal)]) : RL_R;
    else if (g.style == 2) {
      walk += dir;
      if (walk >= nPal - 1 || walk <= 0) dir = -dir;
      walk = constrain(walk, 0, nPal - 1);
      role = r.pct() < g.repeat ? prev : palette[walk];
    } else role = r.pct() < g.repeat ? prev : pickRole(g, r, palette, nPal);
    st.role = role;
    prev = role;
    noteIdx++;
    bool offbeat = s % 4 != 0;
    if (r.pct() < g.accent && (!g.accentOff || offbeat)) st.vel = V_ACC;
    else if (r.pct() < g.ghost) st.vel = V_GHOST;
    if (r.pct() < g.shortP) st.art |= A_SHORT;
    else if (r.pct() < g.longP) st.art |= A_LONG;
  }
  for (int s = 0; s < 16; s++) {                          // slides and ties need neighbours
    Step& st = bar[s];
    if (st.role < RL_R) continue;
    bool nextNote = s < 15 && bar[s + 1].role >= RL_R;
    if (nextNote && r.pct() < g.slide) st.art = (st.art & ~A_SHORT) | A_SLIDE;
    if (!nextNote && s < 15 && bar[s + 1].role == RL_REST && r.pct() < g.tie) {
      bar[s + 1].role = RL_TIE;
      st.art &= ~A_SHORT;
    }
  }
}

void generate(int genre, uint32_t seed, int bars, Pattern& p) {
  const Grammar& g = *grammarOf(genre);
  Rng r;
  r.seed(mix(genre * 7919u + 17u, seed * 104729u + 0x5EEDu));
  memset(&p, 0, sizeof p);
  snprintf(p.name, sizeof p.name, "%s-%03u", g.abbr, (unsigned)(seed % 1000));
  p.genre = genre;
  p.len = 16 * bars;
  genBar(g, r, p.s);
  for (int b = 1; b < bars; b++) {
    Step* bar = p.s + b * 16;
    memcpy(bar, p.s, sizeof(Step) * 16);
    if (r.pct() < 50) {                                   // answer phrase: one note moves
      for (int tries = 0; tries < 16; tries++) {
        int s = r.range(16);
        if (bar[s].role >= RL_R) {
          bar[s].role = pickRole(g, r, nullptr, 0);
          break;
        }
      }
    }
    if (b == bars - 1) {                                  // turnaround into the loop
      switch (r.range(3)) {
        case 0: bar[14] = {RL_A, V_ACC, 0, 0}; bar[15] = {RL_REST, V_NORM, 0, 0}; break;
        case 1: bar[14] = {RL_O, V_ACC, A_SHORT, 0}; break;
        case 2: for (int s = 13; s < 16; s++) bar[s] = {RL_REST, V_NORM, 0, 0}; break;
      }
    }
  }
}

// ---- phrases: every line at LENGTH (1, 2 or 4 bars)
// A line written shorter answers itself, the way a bass player would: bar 2
// keeps the groove and turns its last beat (an octave pop, a fifth, a pickup
// into the next root); the 4th bar turns around harder, in the genre's own
// way. A line written longer plays its first bars. Same line, same answer.
enum { F_DRIVE, F_ACID, F_GROOVE, F_SPACE };

int family(int genre) {
  const char* id = BASS_GENRE[genre < N_BASS_GENRE ? genre : 0].id;
  if (!strcmp(id, "ACID")) return F_ACID;
  for (const char* s : {"TECHNO", "TECHHOUSE", "MINIMAL", "TRANCE", "EBM"})
    if (!strcmp(id, s)) return F_DRIVE;
  for (const char* s : {"DEEP", "DUB", "MELODIC", "BASSMUS"})
    if (!strcmp(id, s)) return F_SPACE;
  return F_GROOVE;
}

uint32_t nameSeed(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

uint8_t barArt(const Step* bar) {             // the line's own articulation, for added notes
  for (int i = 0; i < 16; i++)
    if (bar[i].role >= RL_R) return bar[i].art & A_SHORT;
  return 0;
}

void answerBar(Step* bar, int fam, Rng& r) {
  uint8_t art = barArt(bar);
  int last = -1;                               // the last note in the second half
  for (int i = 15; i >= 8; i--)
    if (bar[i].role >= RL_R) {
      last = i;
      break;
    }
  int kind = r.range(3);
  if (fam == F_SPACE && kind == 0) kind = 1;   // deep lines don't pop up an octave
  if (kind == 0 && last >= 0 && last != 12) {
    bar[last].role = RL_O;
    bar[last].vel = V_ACC;
  } else if (kind == 1 && last >= 0) {
    bar[last].role = fam == F_SPACE ? RL_7 : RL_5;
  } else if (bar[15].role == RL_REST) {
    bar[15] = {RL_A, (uint8_t)(fam == F_DRIVE ? V_GHOST : V_NORM), art, 0};
  } else if (last >= 0) {
    bar[last].role = RL_5;
  }
  if (fam == F_ACID && last >= 0) bar[last].art |= A_SLIDE;
}

void turnBar(Step* bar, int fam, Rng& r) {
  static const char* const FILLS[4][3] = {
      {"R' R O! R", ". O R' 5-", "R' R' R O!"},      // drive
      {"5~ 7~ O! R", "O! 7~ 5~ 3", "R~ O~ R b!"},    // acid
      {"5 . 6 a", "O . 7 5", "7 5 3 c"},             // groove
      {". 5 . a", "7= _ _ .", ". 3 5 ."},            // space
  };
  uint8_t art = barArt(bar);
  const char* f = FILLS[fam][r.range(3)];
  for (int k = 0; k < 4; k++) {
    while (*f == ' ') f++;
    const char* e = f;
    while (*e && *e != ' ') e++;
    Step st;
    parseStep(f, e - f, st);
    if (st.role >= RL_R && !(st.art & (A_SLIDE | A_LONG))) st.art |= art;
    bar[12 + k] = st;
    f = e;
  }
}

void fitBars(Pattern& p, int bars) {
  int have = p.len / 16;
  if (have >= bars) {
    p.len = bars * 16;
    return;
  }
  Rng r;
  r.seed(nameSeed(p.name));
  int fam = family(p.genre);
  Step A[16], Bb[16], C[16];
  memcpy(A, p.s, sizeof A);
  memcpy(Bb, have >= 2 ? p.s + 16 : p.s, sizeof Bb);
  if (have < 2) answerBar(Bb, fam, r);
  memcpy(p.s + 16, Bb, sizeof Bb);
  if (bars == 4) {
    memcpy(C, Bb, sizeof C);
    turnBar(C, fam, r);
    memcpy(p.s + 32, A, sizeof A);
    memcpy(p.s + 48, C, sizeof C);
  }
  p.len = bars * 16;
}

int lenBars() {
  static const uint8_t BARS[3] = {1, 2, 4};
  return BARS[val[bass::BP_LEN]];
}

bool load(int bank, int idx, Pattern& p) {
  if (bank == bankFavs()) {
    if (idx < 0 || idx >= (int)favs.size()) return false;
    p = favs[idx];
    fitBars(p, lenBars());
    return true;
  }
  if (bank == bankNew()) {
    if (idx < 0 || idx >= (int)fresh.size()) return false;
    p = fresh[idx];
    fitBars(p, lenBars());
    return true;
  }
  if (bank < 0 || bank >= N_BASS_GENRE) return false;
  const BassGenreSrc& g = BASS_GENRE[bank];
  if (idx < g.count) {
    const BassPatSrc& s = BASS_PAT[g.first + idx];
    parsePattern(s.name, s.steps, bank, p);
    fitBars(p, lenBars());
    return true;
  }
  generate(bank, idx, lenBars(), p);
  return true;
}

// ======================================================= harmony ==========

int keyRootPitch(const Harmony& h) {
  int kr = 36 + h.keyPc;               // F -> F1 (41) in Ableton's octave naming
  if (h.keyPc > 6) kr -= 12;           // G..B sit just under C1 instead of above it
  return kr + 12 * val[bass::BP_OCT];
}

int chordRootPitch(const Harmony& h, uint8_t pc) {
  int d = ((pc - h.keyPc) % 12 + 12) % 12;
  if (d > 6) d -= 12;                  // stay within a 6th of the tonic
  return keyRootPitch(h) + d;
}

bool inScale(const Harmony& h, int pitch) {
  int rel = ((pitch - h.keyPc) % 12 + 12) % 12;
  return h.scaleMask & (1 << rel);
}

int firstInScale(const Harmony& h, int base, int a, int b) {
  if (inScale(h, base + a)) return base + a;
  if (inScale(h, base + b)) return base + b;
  return base + a;
}

int scaleNeighbour(const Harmony& h, int p, int dir) {
  for (int s = 1; s <= 3; s++)
    if (inScale(h, p + dir * s)) return p + dir * s;
  return p + dir * 2;
}

Chord tonicChord(const Harmony& h) {
  Chord c = {};
  c.valid = true;
  c.rootPc = h.keyPc;
  c.third = (h.scaleMask & (1 << 3)) ? 3 : 4;
  c.fifth = 7;
  c.seventh = (h.scaleMask & (1 << 10)) ? 10 : (h.scaleMask & (1 << 11)) ? 11 : -1;
  static const char* const N[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
  snprintf(c.name, sizeof c.name, "%s%s", N[h.keyPc], c.third == 3 ? "m" : "");
  return c;
}

int resolvePitch(const Step& st, const Harmony& h, const Chord& ch, const Chord& nx) {
  int root = chordRootPitch(h, ch.rootPc);
  int p = root;
  switch (st.role) {
    case RL_R: p = root; break;
    case RL_O: p = root + 12; break;
    case RL_L: p = root - 12; break;
    case RL_5: p = root + (ch.fifth >= 0 ? ch.fifth : 7); break;
    case RL_3: p = ch.third >= 0 ? root + ch.third : firstInScale(h, root, 3, 4); break;
    case RL_7: p = ch.seventh >= 0 ? root + ch.seventh : firstInScale(h, root, 10, 11); break;
    case RL_2: p = firstInScale(h, root, 2, 1); break;
    case RL_4: p = firstInScale(h, root, 5, 6); break;
    case RL_6: p = firstInScale(h, root, 9, 8); break;
    case RL_B2: p = root + 1; break;
    case RL_U: p = scaleNeighbour(h, root, +1); break;
    case RL_A:
    case RL_C: {
      int t = chordRootPitch(h, nx.rootPc);
      while (t - root > 6) t -= 12;
      while (root - t > 6) t += 12;
      p = st.role == RL_A ? scaleNeighbour(h, t, -1) : t - 1;
      break;
    }
  }
  p += 12 * st.oct;
  while (p < 0) p += 12;                       // MIDI's floor (OCTAVE -3, -4 reach it)
  while (p > 96) p -= 12;
  return p;
}

// ======================================================= playback =========

Pattern cur, queuedPat, undoPat;
bool hasCur = false, hasQueued = false, hasUndo = false;
int curBank = 0, curIdx = -1, qBank = 0, qIdx = -1;
uint32_t patStart = 0;
bool runningNow = false, mutedFlag = false, previewing = false;
int browseBank = 0, browsePage = 0;
Harmony lastH;
Chord followChord;
uint8_t soundingNote = 0;            // for the screen

struct Ev { int64_t at; uint8_t st, d1, d2; bool voiceOff; };
Ev q[24];
int nq = 0;
int voicePitch = -1;
int64_t voiceOffAt = 0;
bool voiceSlide = false;

void qPush(int64_t at, uint8_t st, uint8_t d1, uint8_t d2, bool voiceOff) {
  if (nq >= 24) return;
  q[nq++] = {at, st, d1, d2, voiceOff};
}

int qFindVoiceOff() {
  for (int i = 0; i < nq; i++)
    if (q[i].voiceOff) return i;
  return -1;
}

void allNotesOff() {
  for (int i = 0; i < nq; i++)                    // note-ons never sent: drop; offs: send now
    if ((q[i].st & 0xF0) == 0x80) midiOut(q[i].st, q[i].d1, 0);
  nq = 0;
  if (voicePitch >= 0) midiOut(0x80 | val[bass::BP_CH], voicePitch, 0);
  voicePitch = -1;
  soundingNote = 0;
}

// schedule one mono note; slides overlap into the next note so a mono synth glides
void playNote(int pitch, int64_t at, int64_t len, uint8_t vel, bool slideOut) {
  uint8_t ch = val[bass::BP_CH];
  if (voicePitch >= 0 && voiceOffAt > at) {
    int k = qFindVoiceOff();
    if (voiceSlide) {
      if (pitch == voicePitch) {                  // legato onto the same note: just hold
        if (k >= 0) q[k].at = at + len;
        voiceOffAt = at + len;
        voiceSlide = slideOut;
        return;
      }
      if (k >= 0) {
        q[k].at = at + 3000;
        q[k].voiceOff = false;
      }
    } else if (k >= 0) {
      q[k].at = at - 1;                           // cut the old note just before the new one
      q[k].voiceOff = false;
    }
  }
  qPush(at, 0x90 | ch, pitch, vel, false);
  qPush(at + len, 0x80 | ch, pitch, 0, true);
  voicePitch = pitch;
  voiceOffAt = at + len;
  voiceSlide = slideOut;
}

const Step REST = {RL_REST, V_NORM, 0, 0};

Step fillStep(int k, uint32_t barNo) {              // last beat of a phrase
  switch (mix(barNo, 0xF111) % 4) {
    case 0: { const Step f[4] = {{RL_R, V_NORM, 0, 0}, REST, {RL_A, V_ACC, 0, 0}, REST}; return f[k]; }
    case 1: { const Step f[4] = {REST, REST, {RL_O, V_ACC, A_SHORT, 0}, REST}; return f[k]; }
    case 2: { const Step f[4] = {REST, REST, REST, REST}; return f[k]; }
    default: { const Step f[4] = {{RL_R, V_GHOST, 0, 0}, {RL_R, V_NORM, 0, 0}, {RL_5, V_NORM, 0, 0}, {RL_O, V_ACC, A_SHORT, 0}}; return f[k]; }
  }
}

Step stepAt(uint32_t step, int pos) {
  Step st = cur.s[pos];
  int fillN = val[bass::BP_FILL] == 1 ? 4 : val[bass::BP_FILL] == 2 ? 8 : 0;
  uint32_t barNo = step / 16;
  if (fillN && barNo % fillN == (uint32_t)fillN - 1 && (pos % 16) >= 12) st = fillStep(pos % 16 - 12, barNo);
  return st;
}

void chooseChords(uint32_t step, const Harmony& h, Chord& ch, Chord& nx) {
  switch (val[bass::BP_FOLLOW]) {
    case 1:
      if (h.progN) {
        static const uint8_t RATE_STEPS[3] = {8, 16, 32};
        uint32_t slot = step / RATE_STEPS[val[bass::BP_RATE]];
        ch = h.prog[slot % h.progN];
        nx = h.prog[(slot + 1) % h.progN];
        return;
      }
      ch = nx = h.cur;
      return;
    case 2:
      ch = nx = tonicChord(h);
      return;
    default:
      ch = nx = h.cur;
  }
}

bool switchDue(uint32_t step) {
  switch (val[bass::BP_QUANT]) {
    case 1: return step % 4 == 0;
    case 2: return true;
    default: return step % 16 == 0;
  }
}

}  // namespace

// ======================================================= clock hooks ======

void clockOnStep(uint32_t step, int64_t at, int32_t dur) {
  bass::onStep(step, at, dur);
  harm::onStep(step, at, dur);
}
void clockOnTransport(bool run) {
  bass::onTransport(run);
  harm::onTransport(run);
}

void bass::onTransport(bool run) {
  runningNow = run;
  if (run) {
    patStart = 0;
    if (hasQueued) {
      cur = queuedPat;
      hasQueued = false;
      hasCur = true;
      curBank = qBank;
      curIdx = qIdx;
    }
  } else {
    allNotesOff();
    previewing = false;
  }
  engine::setBassPlaying(run && hasCur && !mutedFlag);
}

void bass::onStep(uint32_t step, int64_t at, int32_t dur) {
  if (hasQueued && switchDue(step)) {
    cur = queuedPat;
    hasQueued = false;
    hasCur = true;
    curBank = qBank;
    curIdx = qIdx;
    patStart = step - (step % 16);              // keep the bar's phase
    hasUndo = false;
  }
  if (!hasCur) return;
  int pos = (int)((step - patStart) % cur.len);

  engine::harmony(lastH);
  Chord ch, nx;
  chooseChords(step, lastH, ch, nx);
  followChord = ch;

  Step st = stepAt(step, pos);
  const BassGenreSrc& gs = BASS_GENRE[cur.genre < N_BASS_GENRE ? cur.genre : 0];
  uint32_t barNo = step / 16;

  if (st.role == RL_TIE) return;                 // the previous note already covers it
  if (st.role == RL_REST) {
    int dens = val[BP_DENS];
    bool prevHeld = pos > 0 && (cur.s[pos - 1].art & A_LONG);
    if (dens <= 0 || prevHeld || (int)(mix(barNo, pos + 1) % 100) >= dens) return;
    st = {RL_R, V_GHOST, A_SHORT, 0};            // density adds a ghost note
  } else if (val[BP_DENS] < 0 && pos % 16 != 0 && st.vel != V_ACC &&
             (int)(mix(barNo, pos + 99) % 100) < -val[BP_DENS]) {
    return;                                      // density thins the line
  }
  if (mutedFlag) return;

  int pitch = resolvePitch(st, lastH, ch, nx);

  int64_t t = at;
  if (pos % 2 == 1) t += (int64_t)((val[BP_SWING] - 50) / 50.0 * dur);

  // length: plain notes follow the genre gate; > short, = legato, ties extend
  int follow = 0;
  for (int k = 1; k < cur.len; k++) {
    const Step& n = cur.s[(pos + k) % cur.len];
    if (n.role == RL_TIE) follow++;
    else break;
  }
  int nextOn = 1;
  for (; nextOn < cur.len; nextOn++)
    if (cur.s[(pos + nextOn) % cur.len].role >= RL_R) break;
  double gate = gs.gate / 100.0 * val[BP_GATE] / 100.0;
  int64_t len;
  bool slide = (st.art & A_SLIDE) && (int)(mix(barNo, pos + 7) % 100) < val[BP_SLIDE];
  if (slide) len = (int64_t)nextOn * dur + 3000;
  else if (st.art & A_LONG) len = (int64_t)(nextOn * dur) - 2000;
  else if (st.art & A_SHORT) len = (int64_t)(0.3 * dur);
  else len = (int64_t)(gate * dur);
  len += (int64_t)follow * dur;
  if (len < 8000) len = 8000;

  int vel = val[BP_VEL];
  if (st.vel == V_ACC) vel += val[BP_ACCENT] * 27 / 100;
  if (st.vel == V_GHOST) vel = vel * 55 / 100;
  if (val[BP_DUCK] && pos % 4 == 0) {            // the kick's step: duck like a sidechain
    if (val[BP_DUCK] >= 95) return;
    vel -= vel * val[BP_DUCK] * 40 / 10000;
    len -= len * val[BP_DUCK] / 160;
  }
  playNote(pitch, t, len, constrain(vel, 1, 127), slide);
}

void bass::poll(int64_t now, uint32_t nowMs) {
  bool sent = true;
  while (sent) {                                  // due events, earliest first
    sent = false;
    int best = -1;
    for (int i = 0; i < nq; i++)
      if (q[i].at <= now && (best < 0 || q[i].at < q[best].at)) best = i;
    if (best >= 0) {
      Ev e = q[best];
      q[best] = q[--nq];
      midiOut(e.st, e.d1, e.d2);
      if ((e.st & 0xF0) == 0x90) soundingNote = e.d1;
      else if (e.d1 == soundingNote) soundingNote = 0;
      if (e.voiceOff && voicePitch == e.d1) voicePitch = -1;
      sent = true;
    }
  }
  if (dirty && nowMs - dirtyAt > SAVE_IDLE_MS && !runningNow) {
    File f = LittleFS.open("/bass.bin", "w");
    if (f) {
      f.write((const uint8_t*)val, sizeof val);
      f.close();
    }
    dirty = false;
  }
}

// ======================================================= setup ============

static void loadFavs() {
  File f = LittleFS.open("/bassfav.txt", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line[0] == '#') continue;
    int t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1), t3 = line.indexOf('\t', t2 + 1);
    if (t1 < 0 || t2 < 0 || t3 < 0) continue;
    Pattern p;
    parsePattern(line.substring(t1 + 1, t2).c_str(), line.substring(t3 + 1).c_str(),
                 constrain(line.substring(t2 + 1, t3).toInt(), 0, N_BASS_GENRE - 1), p);
    favs.push_back(p);
  }
  f.close();
}

static bool saveFavs() {
  File f = LittleFS.open("/bassfav.txt", "w");
  if (!f) return false;
  f.print("# ChordBox bass favourites: P<TAB>name<TAB>genre<TAB>steps\n");
  for (const Pattern& p : favs) f.printf("P\t%s\t%d\t%s\n", p.name, p.genre, serialize(p).c_str());
  f.close();
  return true;
}

void bass::begin() {
  for (int p = 0; p < BP_COUNT; p++) val[p] = PDEF[p].def;
  File f = LittleFS.open("/bass.bin", "r");
  if (f) {
    int16_t saved[BP_COUNT];
    if (f.read((uint8_t*)saved, sizeof saved) == sizeof saved)
      for (int p = 0; p < BP_COUNT; p++) {
        int hi = p == BP_GENRE ? nBanks() - 1 : PDEF[p].hi;
        val[p] = constrain(saved[p], PDEF[p].lo, hi);
      }
    f.close();
  }
  loadFavs();
  browseBank = val[BP_GENRE] < N_BASS_GENRE ? val[BP_GENRE] : 0;
  browsePage = 0;
  clk::setInternalBpm(val[BP_TEMPO]);
  clk::setNudgeUs(val[BP_NUDGE] * 1000);
  if (load(browseBank, 0, cur)) {                 // something ready to play
    hasCur = true;
    curBank = browseBank;
    curIdx = 0;
  }
}

static void markDirty() {
  dirty = true;
  dirtyAt = millis();
}

// ======================================================= browsing =========

void bass::padDown(int pad) {
  int idx = browsePage * 16 + pad;
  Pattern p;
  if (!load(browseBank, idx, p)) return;
  if (previewing && runningNow && curBank == browseBank && curIdx == idx) {
    clk::stopInternal();                          // tap the playing line again: stop
    return;
  }
  if (!runningNow) {                              // nothing running: audition right away
    cur = p;
    hasCur = true;
    curBank = browseBank;
    curIdx = idx;
    hasQueued = false;
    hasUndo = false;
    previewing = true;
    clk::startInternal();
    return;
  }
  if (previewing) {                               // auditioning: swap in on the next 16th
    cur = p;
    curBank = browseBank;
    curIdx = idx;
    hasQueued = false;
    hasUndo = false;
    patStart = clk::step() - (clk::step() % 16);
    return;
  }
  queuedPat = p;                                  // the Move is playing: next bar (SWITCH)
  hasQueued = true;
  qBank = browseBank;
  qIdx = idx;
}

void bass::pageMove(int d) {
  int n = pagesOf(browseBank);
  browsePage = ((browsePage + d) % n + n) % n;
}

void bass::stepLine(int d) {
  int b = browseBank, total = pagesOf(b) * 16;
  if (!d || total <= 0) return;
  int base;
  if (hasQueued && qBank == b) base = qIdx;
  else if (hasCur && curBank == b) base = curIdx;
  else base = browsePage * 16 + (d > 0 ? -1 : 16);   // nothing from this genre yet: this page
  Pattern p;
  for (int k = 1; k <= total; k++) {
    int idx = (((base + d * k) % total) + total) % total;
    if (!load(b, idx, p)) continue;
    browsePage = idx / 16;
    padDown(idx % 16);
    return;
  }
}

// ---- the bass station (middle knob + OLED): browse by ear
static bool anyMode = false;                             // ANY: every genre, picked at random
struct Seen { int8_t bank; int16_t idx; };
static Seen hist[32];                                    // the lines heard in ANY, to turn back through
static int histN = 0, histPos = -1;

static bool playLine(int bank, int idx) {
  Pattern p;
  if (!load(bank, idx, p)) return false;
  if (anyMode && bank < N_BASS_GENRE && bank != browseBank) val[bass::BP_SWING] = BASS_GENRE[bank].swing;
  browseBank = bank;
  val[bass::BP_GENRE] = bank;
  browsePage = idx / 16;
  bool same = hasCur && curBank == bank && curIdx == idx;
  if (!same || !runningNow) bass::padDown(idx % 16);
  return true;
}

static void remember(int bank, int idx) {
  if (histPos < histN - 1) histN = histPos + 1;   // turned back, then forward to something new
  if (histN == 32) {
    memmove(hist, hist + 1, sizeof hist - sizeof hist[0]);
    histN--;
  }
  hist[histN++] = {(int8_t)bank, (int16_t)idx};
  histPos = histN - 1;
}

static uint32_t stationRnd() {                    // its own sequence, seeded by the hardware once
  static uint32_t r = 0;
  if (!r) r = hwRandom() | 1;
  r ^= r << 13;
  r ^= r >> 17;
  r ^= r << 5;
  return r;
}

static void randomLine(int bank) {                // bank -1: any genre
  for (int t = 0; t < 24; t++) {
    int b = bank >= 0 ? bank : (int)(stationRnd() % N_BASS_GENRE);
    int idx;
    if (b == bankFavs()) {
      if (favs.empty()) return;
      idx = (int)(stationRnd() % favs.size());
    } else if (b < N_BASS_GENRE && BASS_GENRE[b].count && stationRnd() % 10 < 6) {
      idx = (int)(stationRnd() % BASS_GENRE[b].count);   // mostly the hand-written ones
    } else {
      idx = (int)(stationRnd() % (uint32_t)(pagesOf(b) * 16));
    }
    if (hasCur && b == curBank && idx == curIdx) continue;
    if (playLine(b, idx)) {
      if (anyMode) remember(b, idx);
      return;
    }
  }
}

void bass::lineStep(int d) {
  if (!d) return;
  if (!anyMode) {
    stepLine(d);
    return;
  }
  if (d < 0) {                                    // ANY: back through what you heard
    if (histPos > 0) {
      histPos--;
      playLine(hist[histPos].bank, hist[histPos].idx);
    }
    return;
  }
  if (histPos >= 0 && histPos < histN - 1) {      // forward again through the history
    histPos++;
    playLine(hist[histPos].bank, hist[histPos].idx);
    return;
  }
  randomLine(-1);
}

void bass::pageJump(int d) {                      // a page of 16 lines up or down
  if (!d) return;
  if (anyMode) {
    randomLine(-1);
    return;
  }
  stepLine(d * 16);
}

void bass::surprise() {
  if (anyMode) randomLine(-1);
  else randomLine(browseBank == bankNew() ? -1 : browseBank);
}

void bass::genreStep(int d) {                     // the 16 genres, then ANY, then FAVS
  if (!d) return;
  const int ANY = N_BASS_GENRE, FAV = N_BASS_GENRE + 1, n = N_BASS_GENRE + 2;
  int cur = anyMode ? ANY : browseBank == bankFavs() ? FAV : browseBank < N_BASS_GENRE ? browseBank : 0;
  int nx = ((cur + d) % n + n) % n;
  if (nx == FAV && favs.empty()) nx = ((nx + d) % n + n) % n;
  if (nx == ANY) {
    anyMode = true;
    randomLine(-1);
    return;
  }
  anyMode = false;
  int bank = nx == FAV ? bankFavs() : nx;
  paramSet(bass::BP_GENRE, bank);                 // its own swing, page 1
  playLine(bank, 0);
}

bool bass::anyGenre() { return anyMode; }

void bass::togglePlay() {
  if ((clk::external() && runningNow && !previewing) || harm::playing()) {   // the loop owns the clock: mute
    mutedFlag = !mutedFlag;
    if (mutedFlag) allNotesOff();
    uiToast(mutedFlag ? "BASS MUTED" : "BASS ON");
  } else if (runningNow) {
    clk::stopInternal();
  } else {
    previewing = true;
    clk::startInternal();
  }
  engine::setBassPlaying(runningNow && !mutedFlag);
}

bool bass::running() { return runningNow; }
bool bass::muted() { return mutedFlag; }

// ======================================================= params ===========

static void applySide(int p) {
  if (p == bass::BP_TEMPO) clk::setInternalBpm(val[p]);
  if (p == bass::BP_NUDGE) clk::setNudgeUs(val[p] * 1000);
  if (p == bass::BP_GENRE) {
    browseBank = val[p];
    browsePage = 0;
    if (val[p] < N_BASS_GENRE) val[bass::BP_SWING] = BASS_GENRE[val[p]].swing;
  }
  if (p == bass::BP_LEN && hasCur) {           // the playing line takes the new length now
    Pattern q;
    if (load(curBank, curIdx, q)) cur = q;
  }
}

void bass::paramAdjust(int p, int d) {
  if (p < 0 || p >= BP_COUNT || !d) return;
  const ParamDef& def = PDEF[p];
  int hi = p == BP_GENRE ? nBanks() - 1 : def.hi;
  int v = val[p] + d * def.step;
  if (def.wrap) {
    int span = hi - def.lo + 1;
    v = ((v - def.lo) % span + span) % span + def.lo;
  } else {
    v = constrain(v, (int)def.lo, hi);
  }
  if (v == val[p]) return;
  val[p] = v;
  applySide(p);
  markDirty();
}

void bass::paramSet(int p, int v) {
  if (p < 0 || p >= BP_COUNT) return;
  int hi = p == BP_GENRE ? nBanks() - 1 : PDEF[p].hi;
  val[p] = constrain(v, (int)PDEF[p].lo, hi);
  applySide(p);
  markDirty();
}

int bass::paramGet(int p) { return p >= 0 && p < BP_COUNT ? val[p] : 0; }

void bass::paramView(int p, ParamView& k) {
  memset(&k, 0, sizeof k);
  if (p < 0 || p >= BP_COUNT) return;
  const ParamDef& d = PDEF[p];
  strlcpy(k.label, d.label, sizeof k.label);
  k.group = d.group;
  k.kind = d.kind;
  k.pos = val[p];
  k.lo = d.lo;
  k.hi = p == BP_GENRE ? nBanks() - 1 : d.hi;
  k.opts = d.opts;
  k.nOpts = d.opts ? d.hi - d.lo + 1 : 0;
  int v = val[p];
  switch (p) {
    case BP_TEMPO:
      if (clk::external()) snprintf(k.value, sizeof k.value, "MOVE %d", (int)(clk::bpm() + 0.5f));
      else snprintf(k.value, sizeof k.value, "%d", v);
      break;
    case BP_SWING: snprintf(k.value, sizeof k.value, "%d%%", v); break;
    case BP_OCT: snprintf(k.value, sizeof k.value, "%+d", v); break;
    case BP_GENRE: strlcpy(k.value, bankName(v), sizeof k.value); break;
    case BP_GATE: case BP_SLIDE: case BP_ACCENT: case BP_DUCK:
      snprintf(k.value, sizeof k.value, "%d%%", v);
      break;
    case BP_DENS:
      if (!v) strlcpy(k.value, "AS IS", sizeof k.value);
      else snprintf(k.value, sizeof k.value, "%s %d", v > 0 ? "MORE" : "LESS", abs(v));
      break;
    case BP_CH: snprintf(k.value, sizeof k.value, "%d", v + 1); break;
    case BP_NUDGE: snprintf(k.value, sizeof k.value, v ? "%+d MS" : "0 MS", v); break;
    default:
      if (d.opts) strlcpy(k.value, d.opts[v - d.lo], sizeof k.value);
      else snprintf(k.value, sizeof k.value, "%d", v);
  }
}

int bass::paramIndex(const String& label) {
  static const char* const SHORT[BP_COUNT] = {"TEMPO", "SWING", "OCT", "GENRE", "FOLLOW", "LEN", "GATE", "SLIDE",
                                              "ACCENT", "DENS", "DUCK", "CH", "NUDGE", "FILL", "RATE", "SWITCH", "VEL"};
  for (int p = 0; p < BP_COUNT; p++) {
    String l = PDEF[p].label;
    l.replace(" ", "");
    if (label.equalsIgnoreCase(l) || label.equalsIgnoreCase(SHORT[p])) return p;
  }
  return -1;
}

// ======================================================= actions ==========

void bass::star() {
  if (!hasCur) return;
  Pattern p = cur;
  favs.push_back(p);
  bool ok = saveFavs();
  uiToast(ok ? "* STARRED  %s" : "* %s  (NOT SAVED)", feelOf(p));
}

void bass::starAt(int pad) {
  Pattern p;
  if (!load(browseBank, browsePage * 16 + pad, p)) return;
  favs.push_back(p);
  bool ok = saveFavs();
  uiToast(ok ? "* STARRED  %s" : "* %s  (NOT SAVED)", feelOf(p));
}

void bass::place(int& bb, int& bp, int& pb, int& pi) {
  bb = browseBank;
  bp = browsePage;
  pb = hasCur ? curBank : -1;
  pi = hasCur ? curIdx : -1;
}

void bass::setPlace(int bb, int bp, int pb, int pi) {
  if (bb >= 0 && bb < nBanks() && bb != bankNew()) {
    browseBank = bb;
    val[BP_GENRE] = bb;                         // no genre swing reset: the saved swing stays
    browsePage = constrain(bp, 0, max(0, pagesOf(bb) - 1));
  }
  Pattern p;
  if (pb >= 0 && pb != bankNew() && load(pb, pi, p)) {
    cur = p;
    hasCur = true;
    curBank = pb;
    curIdx = pi;
  }
}

void bass::mutate() {
  if (!hasCur) {
    uiToast("PLAY A BASSLINE FIRST");
    return;
  }
  Pattern p = cur;
  const Grammar& g = *grammarOf(p.genre);
  Rng r;
  r.seed(mix(++mutCount, hwRandom()));
  int ops = 2 + r.range(2);
  for (int k = 0; k < ops; k++) {
    int s = r.range(p.len);
    Step& st = p.s[s];
    switch (r.range(6)) {
      case 0:                                       // move a note to a free neighbour
        if (st.role >= RL_R) {
          int t = (s + (r.range(2) ? 1 : p.len - 1)) % p.len;
          if (p.s[t].role == RL_REST) {
            p.s[t] = st;
            st = REST;
          }
        }
        break;
      case 1:
        if (st.role >= RL_R) st.role = pickRole(g, r, nullptr, 0);
        break;
      case 2:
        if (st.role >= RL_R) st.vel = st.vel == V_ACC ? V_NORM : V_ACC;
        break;
      case 3:
        if (st.role >= RL_R) st.art ^= A_SLIDE;
        break;
      case 4:
        if (st.role == RL_REST) st = {RL_R, V_GHOST, A_SHORT, 0};
        break;
      case 5:
        if (st.role >= RL_R) st.oct = st.oct ? 0 : (r.range(2) ? 1 : -1);
        break;
    }
  }
  snprintf(p.name, sizeof p.name, "MUT-%02d", mutCount % 100);
  fresh.push_back(p);
  int idx = fresh.size() - 1;
  browseBank = bankNew();
  browsePage = idx / 16;
  if (runningNow) {
    queuedPat = p;
    hasQueued = true;
    qBank = bankNew();
    qIdx = idx;
  } else {
    cur = p;
    curBank = bankNew();
    curIdx = idx;
  }
  uiToast("MUTATED  %s", feelOf(p));
}

// ======================================================= edit =============

int bass::bars() { return hasCur ? cur.len / 16 : 1; }

static Step* editStep(int bar, int step) {
  if (!hasCur) return nullptr;
  int i = bar * 16 + step;
  if (i < 0 || i >= cur.len) return nullptr;
  return &cur.s[i];
}

static void snapshot() {
  undoPat = cur;
  hasUndo = true;
}

void bass::editToggle(int bar, int step) {
  Step* s = editStep(bar, step);
  if (!s) return;
  snapshot();
  *s = s->role >= RL_R ? REST : Step{RL_R, V_NORM, 0, 0};
}

void bass::editRole(int bar, int step, int d) {
  Step* s = editStep(bar, step);
  if (!s || !d) return;
  snapshot();
  int i = 0;
  for (int k = 0; k < N_EDIT_ROLES; k++)
    if (EDIT_ROLES[k] == s->role) i = k;
  if (s->role < RL_R) {
    *s = {RL_R, V_NORM, 0, 0};
    return;
  }
  i = ((i + d) % N_EDIT_ROLES + N_EDIT_ROLES) % N_EDIT_ROLES;
  s->role = EDIT_ROLES[i];
}

void bass::editAccent(int bar, int step) {
  Step* s = editStep(bar, step);
  if (!s || s->role < RL_R) return;
  snapshot();
  s->vel = s->vel == V_NORM ? V_ACC : s->vel == V_ACC ? V_GHOST : V_NORM;
}

void bass::editSlide(int bar, int step) {
  Step* s = editStep(bar, step);
  if (!s || s->role < RL_R) return;
  snapshot();
  s->art ^= A_SLIDE;
  s->art &= ~A_SHORT;
}

void bass::editTie(int bar, int step) {
  Step* s = editStep(bar, step);
  if (!s || (bar == 0 && step == 0)) return;
  snapshot();
  *s = s->role == RL_TIE ? REST : Step{RL_TIE, V_NORM, 0, 0};
}

void bass::editOctave(int bar, int step, int d) {
  Step* s = editStep(bar, step);
  if (!s || s->role < RL_R) return;
  snapshot();
  s->oct = constrain(s->oct + d, -1, 1);
}

void bass::editRotate(int d) {
  if (!hasCur || !d) return;
  snapshot();
  Pattern p = cur;
  for (int i = 0; i < cur.len; i++) cur.s[((i + d) % cur.len + cur.len) % cur.len] = p.s[i];
  if (cur.s[0].role == RL_TIE) cur.s[0] = REST;
}

void bass::editClear() {
  if (!hasCur) return;
  snapshot();
  for (int i = 0; i < cur.len; i++) cur.s[i] = REST;
}

void bass::editRandom() {
  snapshot();
  Pattern p;
  generate(hasCur ? cur.genre : 0, hwRandom() % 100000, max(1, bars()), p);
  cur = p;
  hasCur = true;
  uiToast("NEW LINE  %s", feelOf(p));
}

bool bass::editUndo() {
  if (!hasUndo) return false;
  Pattern t = cur;
  cur = undoPat;
  undoPat = t;
  return true;
}

// ======================================================= views ============

void bass::bassView(BassView& v) {
  memset(&v, 0, sizeof v);
  strlcpy(v.bank, bankName(browseBank), sizeof v.bank);
  v.page = browsePage;
  v.pages = pagesOf(browseBank);
  v.bankIdx = browseBank;
  v.nBanks = nBanks();
  v.genPage = browseBank < N_BASS_GENRE && browsePage >= curatedPages(browseBank);
  v.playPad = v.queuedPad = -1;
  if (hasCur) {
    strlcpy(v.name, cur.name, sizeof v.name);
    strlcpy(v.genre, BASS_GENRE[cur.genre < N_BASS_GENRE ? cur.genre : 0].name, sizeof v.genre);
    v.len = cur.len;
    Harmony h = lastH;
    if (!h.cur.valid) engine::harmony(h);
    Chord ch = followChord.valid ? followChord : h.cur;
    int root = chordRootPitch(h, ch.rootPc);
    uint32_t nowStep = clk::step();
    int curPos = (int)((nowStep - patStart) % cur.len);
    v.patBar = (uint8_t)(curPos / 16);
    for (int i = 0; i < cur.len; i++) {
      const Step& st = cur.s[i];
      StepView& sv = v.step[i];
      strlcpy(sv.role, ROLE_NAME[st.role], sizeof sv.role);
      if (st.role == RL_TIE) {
        sv.flags = SF_TIE;
        continue;
      }
      if (st.role < RL_R) continue;
      sv.semis = constrain(resolvePitch(st, h, ch, ch) - root, -24, 24);
      sv.flags = SF_NOTE;
      if (st.vel == V_ACC) sv.flags |= SF_ACCENT;
      if (st.vel == V_GHOST) sv.flags |= SF_GHOST;
      if (st.art & A_SLIDE) sv.flags |= SF_SLIDE;
      if (st.art & A_SHORT) sv.flags |= SF_SHORT;
      if (st.art & A_LONG) sv.flags |= SF_LONG;
      // will it sound the next time round? the same test onStep makes
      uint32_t at = nowStep + (uint32_t)(((i - curPos) % cur.len + cur.len) % cur.len);
      uint32_t barNo = at / 16;
      int dens = val[BP_DENS];
      if (dens < 0 && i % 16 != 0 && st.vel != V_ACC && (int)(mix(barNo, i + 99) % 100) < -dens) sv.flags |= SF_SKIP;
      if (val[BP_DUCK] >= 95 && i % 4 == 0) sv.flags |= SF_SKIP;
    }
    if (curBank == browseBank && curIdx / 16 == browsePage) v.playPad = curIdx % 16;
    v.line = curIdx + 1;
  }
  if (hasQueued && qBank == browseBank && qIdx / 16 == browsePage) v.queuedPad = qIdx % 16;
  if (hasQueued) {
    strlcpy(v.next, queuedPat.name, sizeof v.next);
    uint8_t t, e;
    describe(queuedPat, v.nextFeel, sizeof v.nextFeel, t, e);
  }
  if (hasCur) describe(cur, v.feel, sizeof v.feel, v.traits, v.energy);
  v.any = anyMode;

  static int cacheBank = -1, cachePage = -1, cacheFav = -1, cacheNew = -1, cacheLen = -1;
  static BassPadCell cache[16];
  if (cacheBank != browseBank || cachePage != browsePage || cacheFav != (int)favs.size() ||
      cacheNew != (int)fresh.size() || cacheLen != val[BP_LEN]) {
    cacheBank = browseBank;
    cachePage = browsePage;
    cacheFav = favs.size();
    cacheNew = fresh.size();
    cacheLen = val[BP_LEN];
    for (int pad = 0; pad < 16; pad++) {
      BassPadCell& c = cache[pad];
      memset(&c, 0, sizeof c);
      Pattern p;
      if (!load(browseBank, browsePage * 16 + pad, p)) continue;
      strlcpy(c.name, p.name, sizeof c.name);
      for (int s = 0; s < 16; s++) {
        const Step& st = p.s[s];
        if (st.role >= RL_R) c.onsets |= 1 << s;
        if (st.role >= RL_R && st.vel == V_ACC) c.accents |= 1 << s;
        if (st.role == RL_TIE) c.held |= 1 << s;
        if (st.role >= RL_R) c.semis[s] = (int8_t)constrain(roleSemis(st.role) + st.oct * 12, -12, 24);
      }
      describe(p, c.feel, sizeof c.feel, c.traits, c.energy);
      bool generated = browseBank < N_BASS_GENRE && browsePage * 16 + pad >= BASS_GENRE[browseBank].count;
      c.flags = 1 | (generated ? 2 : 0) | (browseBank == bankFavs() ? 4 : 0);
    }
  }
  memcpy(v.pad, cache, sizeof cache);

  v.curNote = soundingNote;
  if (soundingNote) {
    static const char* const N[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    snprintf(v.noteName, sizeof v.noteName, "%s%d", N[soundingNote % 12], soundingNote / 12 - 2);
  }
  strlcpy(v.follow, FOLLOW_N[val[BP_FOLLOW]], sizeof v.follow);
  if (followChord.valid) strlcpy(v.chordName, followChord.name, sizeof v.chordName);
  else {
    Harmony h;
    engine::harmony(h);
    strlcpy(v.chordName, h.cur.name, sizeof v.chordName);
  }
  v.muted = mutedFlag;
  v.ch = val[BP_CH];
}

void bass::printState(Print& p) {
  p.printf("bass: %s %s  page %d/%d (%s)  playing %s  queued %s  %s%s\n", bankName(browseBank),
           BASS_GENRE[browseBank < N_BASS_GENRE ? browseBank : 0].id, browsePage + 1, pagesOf(browseBank),
           browsePage < curatedPages(browseBank) ? "hand-written" : "generated",
           hasCur ? cur.name : "-", hasQueued ? queuedPat.name : "-", runningNow ? "RUNNING" : "stopped",
           mutedFlag ? " MUTED" : "");
  p.printf("  clock %s %.1f bpm   follow %s -> %s\n", clk::external() ? "MOVE" : "internal", clk::bpm(),
           FOLLOW_N[val[BP_FOLLOW]], followChord.valid ? followChord.name : "-");
  for (int i = 0; i < BP_COUNT; i++) {
    ParamView k;
    paramView(i, k);
    p.printf("  %-9s %-9s%s", k.label, k.value, i % 4 == 3 ? "\n" : "");
  }
  p.println();
}

void bass::printPattern(Print& p) {
  if (!hasCur) {
    p.println("(no pattern)");
    return;
  }
  p.printf("%s [%s] %d steps\n  %s\n", cur.name, BASS_GENRE[cur.genre < N_BASS_GENRE ? cur.genre : 0].id,
           cur.len, serialize(cur).c_str());
}
