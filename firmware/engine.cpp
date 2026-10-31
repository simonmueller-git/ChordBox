// engine.cpp — the chord program: ChordBox v5's chord engine
//
// Rules carried over from the Pico firmware:
//   * NOTHING IS QUANTISED. A chord sounds the instant its pad goes down; the
//     Move's clock drives only the bass program, never these pads. Strum and
//     humanize use the wall clock.
//   * Same pad + same seed = same chord, so a progression stays repeatable
//     until you move VAR (turn it back and the old chords return).
//
// The chord math, voicings and generator are unchanged from v5 (and checked
// against code.py by tests/host). This file adds the API the app drives and
// the Harmony snapshot the bass program follows.

#include "engine.h"
#include "harm.h"
#include <LittleFS.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "clock.h"
#include "config.h"
#include "platform.h"
#include "chords_data.h"
#include "progs_data.h"

using namespace engine;

// ======================================================= tables ===========

static const char* const NOTE_NAMES[12] = {"C", "Db", "D", "Eb", "E", "F",
                                           "Gb", "G", "Ab", "A", "Bb", "B"};
static const char* const SCALE_N[] = {"OFF", "MAJ", "MIN", "DOR",
                                      "PHR", "MIX", "HMIN", "LYD"};
static const uint16_t SCALE_MASK[] = {
    0x0FFF,
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),
};
static const uint16_t NATURAL_MINOR = SCALE_MASK[2];
static const char* const VOIC_N[] = {"CLOSE", "OPEN", "SHELL", "ST9"};
static const char* const LEN_N[] = {"4", "8", "16"};
static const uint8_t LEN_V[] = {4, 8, 16};
static const char* const ONOFF_N[] = {"OFF", "ON"};
static const char* const WEIGHT_N[] = {"EVEN", "PIANO"};
static const char* const SDIR_N[] = {"AS SET", "ALTERNATE", "RANDOM", "OUTWARD"};
static const char* const ARP_N[] = {"OFF", "UP", "DOWN", "UP-DOWN", "RANDOM", "CHORD"};
enum { ARP_OFF, ARP_UP, ARP_DOWN, ARP_UPDN, ARP_RANDOM, ARP_CHORD };
static const char* const RATE_N[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float RATE_STEPS[] = {4, 2, 4.0f / 3, 1, 2.0f / 3, 0.5f};   // in 16ths
static const char* const RHY_N[] = {"ALL", "8THS", "OFFBEAT", "HOUSE", "3-3-2", "DUB", "RANDOM"};
enum { RHY_ALL, RHY_RANDOM = 6 };
static const uint16_t RHY_MASK[] = {                 // bit k = arp step k of the bar
    0xFFFF,
    0b0101010101010101,                              // x.x.x.x.x.x.x.x.
    0b0100010001000100,                              // ..x...x...x...x.
    0b0010010000100100,                              // ..x..x....x..x..
    0b0100100101001001,                              // x..x..x.x..x..x.
    0b0001000000010000,                              // ....x.......x...
    0xFFFF,
};
static const char* const ECHO_N[] = {"OFF", "1/8", "3/16", "1/4"};
static const uint8_t ECHO_STEPS[] = {0, 2, 3, 4};
static const char* const LOOPLEN_N[] = {"1/2 BAR", "1 BAR", "2 BARS"};
static const uint8_t LOOP_STEPS[] = {8, 16, 32};

struct ParamDef {
  const char* label;
  uint8_t group, kind;
  int16_t lo, hi, def;
  uint8_t step;     // value change per encoder detent
  bool wrap;
  const char* const* opts;
};

static const ParamDef PDEF[P_COUNT] = {
    {"ROOT", G_KEY, K_LIST, 0, 11, 5, 1, true, NOTE_NAMES},
    {"SCALE", G_KEY, K_LIST, 0, 7, 2, 1, true, SCALE_N},
    {"OCTAVE", G_KEY, K_NUM, -2, 2, 0, 1, false, nullptr},
    {"BANK", G_KEY, K_LIST, 0, 0, 0, 1, true, nullptr},
    {"DICE", G_HARM, K_NUM, 0, 100, 0, 5, false, nullptr},
    {"VAR", G_HARM, K_NUM, 0, 99, 0, 1, true, nullptr},
    {"VOICING", G_HARM, K_LIST, 0, 3, 0, 1, true, VOIC_N},
    {"STRUM", G_FEEL, K_BIPOLAR, -100, 100, 0, 10, false, nullptr},
    {"HUMAN", G_FEEL, K_NUM, 0, 100, 0, 5, false, nullptr},
    {"VELOCITY", G_FEEL, K_NUM, 20, 127, 100, 4, false, nullptr},
    {"VOICELEAD", G_SYS, K_LIST, 0, 1, 1, 1, true, ONOFF_N},
    {"CHORD CH", G_SYS, K_NUM, 0, 15, DEFAULT_CHORD_CH, 1, true, nullptr},
    {"GEN DARK", G_GEN, K_NUM, 0, 100, 70, 5, false, nullptr},
    {"GEN TENS", G_GEN, K_NUM, 0, 100, 30, 5, false, nullptr},
    {"GEN LEN", G_GEN, K_LIST, 0, 2, 0, 1, true, LEN_N},
    {"MUTATE N", G_GEN, K_NUM, 1, 4, 1, 1, false, nullptr},
    {"SOFT", G_FEEL, K_NUM, 0, 100, 0, 5, false, nullptr},
    {"VEL RAND", G_FEEL, K_NUM, 0, 100, 15, 5, false, nullptr},
    {"WEIGHT", G_FEEL, K_LIST, 0, 1, 1, 1, true, WEIGHT_N},
    {"STRUM DIR", G_FEEL, K_LIST, 0, 3, 0, 1, true, SDIR_N},
    {"ARP", G_GEN, K_LIST, 0, 5, 0, 1, true, ARP_N},
    {"ARP RATE", G_GEN, K_LIST, 0, 5, 3, 1, false, RATE_N},
    {"ARP GATE", G_FEEL, K_NUM, 10, 200, 80, 10, false, nullptr},
    {"ARP OCT", G_GEN, K_NUM, 1, 4, 1, 1, false, nullptr},
    {"RHYTHM", G_GEN, K_LIST, 0, 6, 0, 1, true, RHY_N},
    {"REPEAT", G_GEN, K_LIST, 0, 5, 3, 1, false, RATE_N},
    {"RPT RAMP", G_FEEL, K_BIPOLAR, -100, 100, 0, 10, false, nullptr},
    {"ECHO", G_GEN, K_LIST, 0, 3, 0, 1, true, ECHO_N},
    {"ECHO FB", G_FEEL, K_NUM, 10, 90, 55, 5, false, nullptr},
    {"LOOP LEN", G_GEN, K_LIST, 0, 2, 1, 1, true, LOOPLEN_N},
};

static const int ROWS_PER_PAGE = 4;   // 4 progressions x 4 chords = one 4x4 keypad

// ======================================================= rng ==============

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
  float f() { return (next() >> 8) * (1.0f / 16777216.0f); }
  int range(int n) { return n <= 1 ? 0 : (int)(next() % (uint32_t)n); }
};

static Rng live;   // feel: timing / velocity jitter (never seeded per pad)

// ======================================================= library ==========

struct Prog {
  String name;
  std::vector<uint8_t> ids;
};
struct Bank {
  String name;
  std::vector<uint32_t> rows;   // prog * 2 + half
};

static std::vector<Prog> PROGS;
static std::vector<Bank> BANKS;
static int favBank = -1;
static int newBank = -1;
static const SubDef* subsFor[N_CHORDS];

static int chordId(const char* s, int len) {
  for (int i = 0; i < N_CHORDS; i++) {
    const char* n = CHORD_DEFS[i].name;
    if ((int)strlen(n) == len && strncmp(n, s, len) == 0) return i;
  }
  return -1;
}
static int chordId(const String& s) { return chordId(s.c_str(), s.length()); }

static std::vector<uint8_t> parseToks(const char* toks) {
  std::vector<uint8_t> ids;
  const char* p = toks;
  while (*p) {
    while (*p == ' ') p++;
    const char* q = p;
    while (*q && *q != ' ') q++;
    if (q > p) {
      int id = chordId(p, q - p);
      ids.push_back(id >= 0 ? id : 0);   // unknown -> I (never happens: gen_data checks)
    }
    p = q;
  }
  return ids;
}

static String tokString(const std::vector<uint8_t>& ids) {
  String s;
  for (size_t i = 0; i < ids.size(); i++) {
    if (i) s += ' ';
    s += CHORD_DEFS[ids[i]].name;
  }
  return s;
}

// ChordBox's _base_triad: strip one known suffix, same order as code.py
static int baseTriad(uint8_t id) {
  static const char* const SUF[] = {"maj9", "maj7", "add9", "sus2", "sus4",
                                    "dim7", "69", "07", "9", "7", "6", "5",
                                    "-5", "0"};
  const char* n = CHORD_DEFS[id].name;
  int len = strlen(n);
  for (const char* s : SUF) {
    int sl = strlen(s);
    if (len > sl && strcmp(n + len - sl, s) == 0) return chordId(n, len - sl);
  }
  return id;
}

static const SubDef* findSubs(int id) {
  for (int i = 0; i < N_SUBS; i++)
    if (SUB_DEFS[i].from == id) return &SUB_DEFS[i];
  return nullptr;
}

static int nPages(int b) {
  int n = BANKS[b].rows.size();
  return max(1, (n + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
}

static bool rowRef(int b, int r, int& prog, int& half) {
  if (b < 0 || b >= (int)BANKS.size() || r < 0 || r >= (int)BANKS[b].rows.size()) return false;
  uint32_t v = BANKS[b].rows[r];
  prog = v >> 1;
  half = v & 1;
  return true;
}

// the (up to) 4 chords of one pad row
static int rowIds(int b, int r, uint8_t out[4]) {
  int prog, half;
  if (!rowRef(b, r, prog, half)) return 0;
  const std::vector<uint8_t>& ids = PROGS[prog].ids;
  int start = ids.size() > 4 ? half * 4 : 0;
  int n = 0;
  for (int i = start; i < (int)ids.size() && n < 4; i++) out[n++] = ids[i];
  return n;
}

// add a token list as a new progression; 8 chords per prog, 4 per pad row
static int install(const std::vector<uint8_t>& toks, const String& name,
                   const char* bankName, int* firstPage) {
  std::vector<uint32_t> rows;
  for (size_t gi = 0, part = 0; gi < toks.size(); gi += 8, part++) {
    Prog p;
    p.name = part == 0 ? name : name + char('a' + part - 1);
    p.ids.assign(toks.begin() + gi, toks.begin() + min(gi + 8, toks.size()));
    uint32_t idx = PROGS.size();
    rows.push_back(idx * 2);
    if (p.ids.size() > 4) rows.push_back(idx * 2 + 1);
    PROGS.push_back(p);
  }
  int bi = -1;
  for (size_t i = 0; i < BANKS.size(); i++)
    if (BANKS[i].name == bankName) bi = i;
  if (bi < 0) {
    BANKS.push_back({bankName, {}});
    bi = BANKS.size() - 1;
  }
  int first = BANKS[bi].rows.size();
  BANKS[bi].rows.insert(BANKS[bi].rows.end(), rows.begin(), rows.end());
  if (firstPage) *firstPage = first / ROWS_PER_PAGE;
  return bi;
}

static int findBank(const char* name) {
  for (size_t i = 0; i < BANKS.size(); i++)
    if (BANKS[i].name == name) return i;
  return -1;
}

// ======================================================= state ============

static int16_t val[P_COUNT];
static int page = 0;
static uint32_t bootSeed = 1;
static uint32_t sessionSeed = 1;    // bootSeed mixed with VAR
static bool latch = false;
static bool bassPlaying = false;    // ST9 drops its root while the bass plays
static int genCount = 0, mutCount = 0, favCount = 0;
static int recallBank = -1, recallPage = 0;   // RECALL jumps to Favs and back

// last played (for the hero, STAR, MUTATE and the bass)
static int lastBank = -1, lastRow = -1, lastCol = -1;
static std::vector<int> lastVoicing;
static char lastSym[14] = "", lastRn[10] = "";
static uint8_t lastNotes[10], lastNNotes = 0;
static bool played = false;
static int8_t lastIv[8];            // the chord as resolved, key-relative
static uint8_t lastIvN = 0;

// sounding notes, per pad
struct Voice { uint8_t note, ch; bool sent; };   // v is kept low to high
struct Held {
  bool on = false;
  std::vector<Voice> v;
  uint8_t rootPc = 0;
  int8_t ivRoot = 0, qual = -1, inv = 0;           // for QUALITY and re-voicing
};
static const int LOOP = 16;         // held[LOOP]: the chord the loop recorder is playing
static Held held[17];
static int8_t heldOrder[16];        // most recent first, for STAR targeting
static uint8_t noteRef[16][128];    // per channel refcount (overlapping pads)

struct Sched { int64_t due; uint8_t note, vel, ch, pad; };
static std::vector<Sched> sched;

static bool dirty = false;
static uint32_t dirtyAt = 0;

static inline int bank() { return val[P_BANK]; }
static void reseed() { sessionSeed = bootSeed ^ ((uint32_t)val[P_VAR] * 0x9E3779B1u); }
static void markDirty() {
  dirty = true;
  dirtyAt = millis();
}

// ======================================================= chord math =======

static void chordSymbol(int keyPc, const int8_t* iv, int n, char* out, size_t cap) {
  int r = ((keyPc + iv[0]) % 12 + 12) % 12;
  bool has[12] = {};
  for (int i = 0; i < n; i++) has[((iv[i] - iv[0]) % 12 + 12) % 12] = true;
  int third = has[3] ? 3 : has[4] ? 4 : -1;
  const char* q;
  if (third == 3) q = (has[6] && !has[7]) ? "dim" : "m";
  else if (third == 4) q = (has[8] && !has[7]) ? "aug" : "";
  else q = has[2] ? "sus2" : has[5] ? "sus4" : "5";
  if (strcmp(q, "dim") == 0) {
    snprintf(out, cap, "%s%s", NOTE_NAMES[r], has[10] ? "m7b5" : has[9] ? "dim7" : "dim");
    return;
  }
  const char* e = "";
  if (has[11]) e = has[2] ? "maj9" : "maj7";
  else if (has[10]) e = has[2] ? "9" : "7";
  else if (has[9] && third >= 0) e = "6";
  else if (has[2] && third >= 0) e = "add9";
  snprintf(out, cap, "%s%s%s", NOTE_NAMES[r], q, e);
}

struct Res {
  int8_t iv[8];
  uint8_t n, id;
  bool diced;
  char sym[14];
  char rn[10];
};

// dice: substitution + added extension, seeded per pad (same pad + seed = same chord)
static void resolve(uint8_t id, uint32_t mix, Rng& rng, Res& out) {
  float r = val[P_DICE] / 100.0f;
  rng.seed(sessionSeed ^ mix);
  uint8_t cid = id;
  out.diced = false;
  if (r > 0 && rng.f() < r) {
    const SubDef* s = subsFor[cid];
    if (s) {
      cid = s->to[rng.range(s->n)];
      out.diced = true;
    }
  }
  const ChordDef& d = CHORD_DEFS[cid];
  out.n = d.n;
  memcpy(out.iv, d.iv, d.n);
  if (r > 0 && rng.f() < r * 0.7f) {
    static const int8_t EXT[3] = {10, 11, 14};
    int8_t e = out.iv[0] + EXT[rng.range(3)];
    bool dup = false;
    for (int i = 0; i < out.n; i++)
      if (((out.iv[i] - e) % 12 + 12) % 12 == 0) dup = true;
    if (!dup && out.n < 8) {
      out.iv[out.n++] = e;
      out.diced = true;
    }
  }
  out.id = cid;
  chordSymbol(val[P_ROOT], out.iv, out.n, out.sym, sizeof out.sym);
  strlcpy(out.rn, d.name, sizeof out.rn);
}

static bool borrowed(const Res& r) {
  if (val[P_SCALE] == 0) return false;
  uint16_t mask = SCALE_MASK[val[P_SCALE]];
  for (int i = 0; i < r.n; i++)
    if (!(mask & (1 << (((r.iv[i] % 12) + 12) % 12)))) return true;
  return false;
}

static std::vector<int> applyVoicing(const Res& r) {
  std::vector<int> iv(r.iv, r.iv + r.n);
  int m = val[P_VOIC];
  if (m == 0 || iv.size() < 3) return iv;
  if (m == 1) {                                   // open: drop 2nd from top
    iv[iv.size() - 2] -= 12;
    return iv;
  }
  int third = iv[1] - iv[0];
  int seventh = iv.size() > 3 ? iv[3] - iv[0] : (third == 3 ? 10 : 11);
  if (m == 2) return {iv[0], iv[0] + third, iv[0] + seventh};   // shell 1-3-7
  std::vector<int> out = {iv[0] + third, iv[0] + seventh, iv[0] + 14};   // rootless 3-7-9
  if (!bassPlaying) out.insert(out.begin(), iv[0]);   // the bass covers the root
  return out;
}

static void randomizeVoicing(std::vector<int>& n, float r, bool allowJump, Rng& rng) {
  for (size_t i = 1; i < n.size(); i++)
    if (rng.f() < r * 0.4f) n[i] += 12;
  if (n.size() > 2 && rng.f() < r * 0.3f) n.back() -= 12;
  if (allowJump && rng.f() < r * 0.25f) {
    int sh = rng.range(2) ? 12 : -12;
    for (int& x : n) x += sh;
  }
}

static std::vector<int> voiceLead(std::vector<int> cur, const std::vector<int>& prev, int anchor) {
  std::sort(cur.begin(), cur.end());
  std::vector<int> best = cur;
  double bestScore = 1e9;
  std::vector<int> pv = prev;
  std::sort(pv.begin(), pv.end());
  int n = cur.size();
  for (int rot = 0; rot < n; rot++) {
    std::vector<int> v;
    for (int i = rot; i < n; i++) v.push_back(cur[i]);
    for (int i = 0; i < rot; i++) v.push_back(cur[i] + 12);
    std::sort(v.begin(), v.end());
    for (int sh : {-24, -12, 0, 12, 24}) {
      std::vector<int> c = v;
      for (int& x : c) x += sh;
      if (c.front() < 30 || c.back() > 100) continue;
      double mean = 0;
      for (int x : c) mean += x;
      mean /= c.size();
      if (fabs(mean - anchor) > 6) continue;
      double score = 0;
      if (!pv.empty()) {
        size_t m = min(c.size(), pv.size());
        for (size_t i = 0; i < m; i++) score += abs(c[i] - pv[i]);
        score += 3 * abs((int)c.size() - (int)pv.size());
      }
      score += 0.35 * fabs(mean - anchor);
      if (score < bestScore) {
        best = c;
        bestScore = score;
      }
    }
  }
  return best;
}

static uint32_t padMix(int prog, int half, int col) {
  return (uint32_t)prog * 977u + (uint32_t)half * 131u + (uint32_t)col * 29u;
}

// ======================================================= MIDI out =========

static void noteOn(uint8_t ch, uint8_t n, uint8_t v) {
  if (noteRef[ch][n]) midiOut(0x80 | ch, n, 0);     // retrigger, keep one voice
  if (noteRef[ch][n] < 255) noteRef[ch][n]++;
  midiOut(0x90 | ch, n, v);
}

static void noteOff(uint8_t ch, uint8_t n) {
  if (!noteRef[ch][n]) return;
  if (--noteRef[ch][n] == 0) midiOut(0x80 | ch, n, 0);
}

// velocity of chord note `rank` (0 = lowest) of n: SOFT lowers it, WEIGHT
// voices it like a pianist (top note sings, inner notes back off), VEL RAND varies it
static uint8_t noteVel(int rank, int n) {
  float v = val[P_VEL] * (1.0f - 0.55f * val[P_SOFT] / 100.0f);
  if (val[P_WEIGHT] && n >= 3) v += rank == n - 1 ? 12 : rank == 0 ? 4 : -10;
  float r = val[P_VRND] / 100.0f;
  if (r > 0) v += (live.f() - 0.5f) * r * 40;
  return constrain((int)v, 1, 127);
}

static int soundingCount() {
  int n = 0;
  for (auto& h : held) n += h.on;
  return n;
}

static void release(int pad) {
  Held& h = held[pad];
  if (!h.on) return;
  for (size_t i = 0; i < sched.size();)
    if (sched[i].pad == pad) sched.erase(sched.begin() + i);
    else i++;
  for (auto& v : h.v)
    if (v.sent) noteOff(v.ch, v.note);
  h.v.clear();
  h.on = false;
  for (int i = 0; i < 16; i++)
    if (heldOrder[i] == pad) heldOrder[i] = -1;
}

static void releaseAll() {
  for (int p = 0; p < 16; p++) release(p);
}

static void resetMotion();

void engine::panic() {
  resetMotion();
  sched.clear();
  for (auto& h : held) {
    h.v.clear();
    h.on = false;
  }
  memset(noteRef, 0, sizeof noteRef);
  int ch = val[P_CHCH];
  midiOut(0xB0 | ch, 123, 0);                   // all notes off
  for (int n = 0; n < 128; n++) midiOut(0x80 | ch, n, 0);
  memset(heldOrder, -1, sizeof heldOrder);
}

// ======================================================= sounding =========

static float walk = 0.5f;           // HUMAN: a slow random walk, not white noise
static bool altFlip = false;        // STRUM DIR = ALTERNATE

// play a stored chord now: STRUM spreads it (first note at once), HUMAN only
// ever makes notes late, velocities from noteVel
static void sound(int pad, int64_t now, bool strum) {
  Held& h = held[pad];
  int n = h.v.size();
  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  bool down = val[P_STRM] < 0;
  switch (val[P_SDIR]) {
    case 1: down = altFlip = !altFlip; break;
    case 2:
      for (int i = n - 1; i > 0; i--) std::swap(order[i], order[live.range(i + 1)]);
      down = false;
      break;
    case 3:                                        // from the middle outwards
      for (int k = 0, lo = (n - 1) / 2, hi = lo + 1; k < n; k++)
        order[k] = (k % 2 == 0 && lo >= 0) || hi >= n ? lo-- : hi++;
      down = false;
      break;
  }
  if (down) std::reverse(order.begin(), order.end());
  float spreadUs = strum ? abs(val[P_STRM]) * 450.0f : 0;   // 0 .. 45 ms per note
  float hum = val[P_HUM] / 100.0f;
  walk = walk * 0.85f + live.f() * 0.15f;
  for (int k = 0; k < n; k++) {
    int i = order[k];
    float d = k * spreadUs;
    if (hum > 0 && k > 0) d += hum * (walk * 8000.0f + live.f() * 4000.0f);
    uint8_t v = noteVel(i, n);
    if (d < 1000) {
      noteOn(h.v[i].ch, h.v[i].note, v);
      h.v[i].sent = true;
    } else {
      sched.push_back({now + (int64_t)d, h.v[i].note, v, h.v[i].ch, (uint8_t)pad});
    }
  }
}

// ======================================================= motion ===========
// The arp, REPEAT, ECHO and the loop recorder: timed on the Move's clock while
// it runs, else free-running at the internal tempo from the first press.

struct Off { int64_t at; uint8_t note, ch; };
struct Echo { int64_t at, len; uint8_t note, vel, ch; };
static std::vector<Off> offs;       // notes the motion engine turned on
static std::vector<Echo> echoes;
static const int64_t NONE = INT64_MIN;
static int64_t lastIdx = NONE;      // last arp / repeat grid index fired
static int64_t freeT0 = 0;
static int arpPos = 0, rptCount = 0;
static bool repeatOn = false;

enum { LOOP_IDLE, LOOP_REC, LOOP_PLAY };
struct Slot { std::vector<int> notes; char sym[14], rn[10]; int8_t iv[8]; uint8_t ivN; };
static Slot slots[8];
static int nSlots = 0, loopState = LOOP_IDLE, loopSlot = -1;
static int64_t loopBase = 0;

static bool motionOn() { return val[P_ARP] != ARP_OFF || repeatOn; }
static double stepLenUs() { return clk::running() ? (double)clk::stepUs() : 60e6 / clk::internalBpm() / 4; }
static double posSteps(int64_t now) {                 // 16ths since bar 0
  if (clk::running()) return clk::step() + (double)(now - clk::stepAtUs()) / std::max(1, (int)clk::stepUs());
  return (double)(now - freeT0) / (60e6 / clk::internalBpm() / 4);
}
static double rateSteps() { return RATE_STEPS[repeatOn ? val[P_RPT] : val[P_ARATE]]; }

static int activePad() {            // what the arp / repeat plays: the newest pad, else the loop
  for (int i = 0; i < 16; i++)
    if (heldOrder[i] >= 0 && held[heldOrder[i]].on) return heldOrder[i];
  if (loopState == LOOP_PLAY && held[LOOP].on) return LOOP;
  return -1;
}

static void motionNote(uint8_t ch, uint8_t note, uint8_t vel, int64_t offAt) {
  noteOn(ch, note, vel);
  offs.push_back({offAt, note, ch});
}

static void fireStep(int pad, int64_t idx, int64_t now, bool first) {
  Held& h = held[pad];
  int n = h.v.size();
  if (!n) return;
  double durUs = rateSteps() * stepLenUs();
  if (repeatOn) {                                    // the whole chord, fading in or out
    float r = val[P_RRAMP] / 100.0f, f = 1;
    if (r > 0) f = std::min(1.0f, 0.35f + 0.65f * rptCount * r / 8);
    else if (r < 0) f = std::max(0.08f, 1 + rptCount * r / 8);
    rptCount++;
    for (int i = 0; i < n; i++)
      motionNote(h.v[i].ch, h.v[i].note, std::max(1, (int)(noteVel(i, n) * f)), now + (int64_t)(durUs * 0.5));
    return;
  }
  if (!first) {                                      // RHYTHM: which grid steps play
    int perBar = std::max(1, (int)lround(16 / rateSteps()));
    int k = (int)(((idx % perBar) + perBar) % perBar) % 16;
    if (val[P_RHY] == RHY_RANDOM ? live.f() < 0.45f : !(RHY_MASK[val[P_RHY]] & (1u << k))) return;
  }
  int64_t offAt = now + (int64_t)(durUs * val[P_AGATE] / 100.0);
  if (val[P_ARP] == ARP_CHORD) {                     // stabs
    for (int i = 0; i < n; i++) motionNote(h.v[i].ch, h.v[i].note, noteVel(i, n), offAt);
    return;
  }
  std::vector<int> list;
  for (int o = 0; o < val[P_AOCT]; o++)
    for (int i = 0; i < n; i++)
      if (h.v[i].note + 12 * o <= 127) list.push_back(h.v[i].note + 12 * o);
  int N = list.size(), j = 0;
  switch (val[P_ARP]) {
    case ARP_UP: j = arpPos % N; break;
    case ARP_DOWN: j = N - 1 - arpPos % N; break;
    case ARP_UPDN: {
      int period = std::max(1, 2 * N - 2), p = arpPos % period;
      j = p < N ? p : period - p;
      break;
    }
    default: j = live.range(N);
  }
  arpPos++;
  motionNote(h.v[0].ch, list[j], noteVel(j % n, n), offAt);
}

// a new chord while the arp / repeat runs: the press sounds at once (unless a
// RHYTHM is set: stabs wait for their step), then the grid takes over
static void motionStart(int64_t now) {
  int pad = activePad();
  if (pad < 0) return;
  if (lastIdx == NONE && !clk::running()) freeT0 = now;
  arpPos = 0;
  double q = posSteps(now) / rateSteps();
  int64_t idx = (int64_t)floor(q);
  if (repeatOn || val[P_RHY] == RHY_ALL) {
    fireStep(pad, idx, now, true);
    lastIdx = q - idx > 0.75 ? idx + 1 : idx;
  } else if (lastIdx == NONE) {
    lastIdx = q - idx < 0.02 ? idx - 1 : idx;
  }
}

static void scheduleEcho(int pad, int64_t now) {
  if (!val[P_ECHO]) return;
  Held& h = held[pad];
  int n = h.v.size();
  int64_t gap = (int64_t)(ECHO_STEPS[val[P_ECHO]] * stepLenUs());
  float g = 1;
  for (int k = 1; k <= 8; k++) {
    g *= val[P_EFB] / 100.0f;
    if (noteVel(n - 1, n) * g < 8) break;
    for (int i = 0; i < n; i++)
      echoes.push_back({now + k * gap, gap * 6 / 10, h.v[i].note, (uint8_t)std::max(1, (int)(noteVel(i, n) * g)), h.v[i].ch});
  }
}

static void recordSlot(const Res& res) {
  if (loopState != LOOP_REC || nSlots >= 8) return;
  Slot& s = slots[nSlots++];
  s.notes.assign(lastNotes, lastNotes + lastNNotes);
  strlcpy(s.sym, res.sym, sizeof s.sym);
  strlcpy(s.rn, res.rn, sizeof s.rn);
  s.ivN = res.n;
  memcpy(s.iv, res.iv, res.n);
  uiToast("REC %d/8  %s", nSlots, res.sym);
}

static void loopTick(int64_t now) {
  if (loopState != LOOP_PLAY || !nSlots) return;
  int64_t idx = (int64_t)floor(posSteps(now) / LOOP_STEPS[val[P_LOOPLEN]]) - loopBase;
  int s = (int)(((idx % nSlots) + nSlots) % nSlots);
  if (s == loopSlot) return;
  loopSlot = s;
  release(LOOP);
  const Slot& sl = slots[s];
  Held& h = held[LOOP];
  h.v.clear();
  for (int x : sl.notes) h.v.push_back({(uint8_t)x, (uint8_t)val[P_CHCH], false});
  h.on = true;
  h.qual = -1;
  strlcpy(lastSym, sl.sym, sizeof lastSym);
  strlcpy(lastRn, sl.rn, sizeof lastRn);
  lastNNotes = min((int)sl.notes.size(), 10);
  for (int i = 0; i < lastNNotes; i++) lastNotes[i] = sl.notes[i];
  lastIvN = sl.ivN;
  memcpy(lastIv, sl.iv, sl.ivN);
  played = true;
  if (motionOn()) motionStart(now);
  else sound(LOOP, now, true);
}

static void motionTick(int64_t now) {
  for (size_t i = 0; i < offs.size();)
    if (now >= offs[i].at) {
      noteOff(offs[i].ch, offs[i].note);
      offs.erase(offs.begin() + i);
    } else {
      i++;
    }
  for (size_t i = 0; i < echoes.size();)
    if (now >= echoes[i].at) {
      Echo e = echoes[i];
      echoes.erase(echoes.begin() + i);
      motionNote(e.ch, e.note, e.vel, now + e.len);
    } else {
      i++;
    }
  loopTick(now);
  int pad = motionOn() ? activePad() : -1;
  if (pad < 0) {
    lastIdx = NONE;
    return;
  }
  int64_t idx = (int64_t)floor(posSteps(now) / rateSteps());
  if (idx == lastIdx) return;
  if (lastIdx != NONE && idx < lastIdx - 1) lastIdx = idx - 1;   // the clock restarted
  lastIdx = idx;
  fireStep(pad, idx, now, false);
}

static void resetMotion() {
  for (auto& o : offs) noteOff(o.ch, o.note);
  offs.clear();
  echoes.clear();
  repeatOn = false;
  lastIdx = NONE;
  loopState = LOOP_IDLE;
  loopSlot = -1;
  held[LOOP].v.clear();
  held[LOOP].on = false;
}

// ======================================================= playing ==========

// inversions: each step up lifts the lowest note an octave, each step down drops the highest
static void invert(std::vector<int>& ns, int steps) {
  if (ns.empty()) return;
  std::sort(ns.begin(), ns.end());
  for (int k = steps; k > 0 && ns.back() + 12 <= 127; k--) {
    ns[0] += 12;
    std::sort(ns.begin(), ns.end());
  }
  for (int k = steps; k < 0 && ns[0] - 12 >= 0; k++) {
    ns.back() -= 12;
    std::sort(ns.begin(), ns.end());
  }
}

static int8_t invBias = 0;          // the second knob: every chord is played in this inversion

static void playPad(int pad, int64_t nowUs) {
  if (held[pad].on) {                     // re-tap while latched = release
    release(pad);
    if (latch) return;
  }
  if (latch && soundingCount()) releaseAll();

  int row = page * ROWS_PER_PAGE + pad / 4, col = pad % 4;
  uint8_t ids[4];
  int n = rowIds(bank(), row, ids);
  if (!n) return;
  int prog, half;
  rowRef(bank(), row, prog, half);

  Rng rng;
  Res res;
  resolve(ids[col % n], padMix(prog, half, col), rng, res);
  int base = 60 + val[P_OCT] * 12 + val[P_ROOT];
  std::vector<int> notes = applyVoicing(res);
  for (int& x : notes) x += base;
  float r = val[P_DICE] / 100.0f;
  if (r > 0) randomizeVoicing(notes, r, !val[P_VLD], rng);
  if (val[P_VLD]) notes = voiceLead(notes, lastVoicing, base + 5);
  std::sort(notes.begin(), notes.end());
  notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
  if (invBias) invert(notes, invBias);
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [](int x) { return x < 0 || x > 127; }),
              notes.end());
  if (notes.empty()) return;
  lastVoicing = notes;

  Held& h = held[pad];
  h.v.clear();
  uint8_t ch = val[P_CHCH];
  for (int x : notes) h.v.push_back({(uint8_t)x, ch, false});
  h.rootPc = ((val[P_ROOT] + res.iv[0]) % 12 + 12) % 12;
  h.ivRoot = res.iv[0];
  h.qual = -1;
  h.inv = invBias;
  h.on = true;
  memmove(heldOrder + 1, heldOrder, 15);
  heldOrder[0] = pad;

  strlcpy(lastSym, res.sym, sizeof lastSym);
  strlcpy(lastRn, res.rn, sizeof lastRn);
  lastNNotes = min((int)notes.size(), 10);
  for (int i = 0; i < lastNNotes; i++) lastNotes[i] = notes[i];
  lastIvN = res.n;
  memcpy(lastIv, res.iv, res.n);
  lastBank = bank();
  lastRow = row;
  lastCol = col;
  played = true;

  recordSlot(res);
  if (motionOn()) {
    motionStart(nowUs);
  } else {
    sound(pad, nowUs, true);
    scheduleEcho(pad, nowUs);
  }
}

// ======================================================= generator ========

static const char* const EXT_SUF[] = {"9", "maj7", "7", "add9"};

static int withExtension(uint8_t id) {
  String base = CHORD_DEFS[id].name;
  for (const char* s : EXT_SUF) {
    int e = chordId(base + s);
    if (e >= 0) return e;
  }
  return id;
}

static void vary(std::vector<uint8_t>& out, Rng& rng) {
  int n = out.size();
  if (!n) return;
  int start = rng.f() < 0.6f ? n - 1 : rng.range(n);
  for (int k = 0; k < n; k++) {
    int i = (start + k) % n;
    const SubDef* s = subsFor[out[i]];
    if (s) {
      out[i] = s->to[rng.range(s->n)];
      return;
    }
  }
}

static std::vector<uint8_t> genProg() {
  Rng rng;
  rng.seed(sessionSeed ^ (genCount * 7717u) ^ 0x517u);
  bool minor = rng.f() < val[P_DARK] / 100.0f;
  const uint8_t(*pool)[4] = minor ? TEMPL_MIN : TEMPL_MAJ;
  int np = minor ? N_TEMPL_MIN : N_TEMPL_MAJ;
  const uint8_t* head = pool[rng.range(np)];
  std::vector<uint8_t> toks(head, head + 4);
  int len = LEN_V[val[P_LEN]];
  while ((int)toks.size() < len) {
    if (rng.f() < 0.55f) {
      std::vector<uint8_t> v(head, head + 4);
      vary(v, rng);
      toks.insert(toks.end(), v.begin(), v.end());
    } else {
      const uint8_t* t = pool[rng.range(np)];
      toks.insert(toks.end(), t, t + 4);
    }
  }
  toks.resize(len);
  float tens = val[P_TENS] / 100.0f;
  for (auto& t : toks)
    if (rng.f() < tens) t = withExtension(t);
  return toks;
}

// ======================================================= commands =========

static void jumpTo(int b, int p) {
  val[P_BANK] = b;
  page = constrain(p, 0, nPages(b) - 1);
}

void engine::gen() {
  genCount++;
  char nm[8];
  snprintf(nm, sizeof nm, "G%02d", genCount);
  int pg;
  newBank = install(genProg(), nm, "New", &pg);
  jumpTo(newBank, pg);
  uiToast("NEW  %s  %d CHORDS", nm, LEN_V[val[P_LEN]]);
}

// MUTATE: breed the last progression you played — swap MUTATE N chords for relatives
void engine::mutate() {
  int prog, half;
  if (!rowRef(lastBank, lastRow, prog, half)) {
    uiToast("PLAY A ROW FIRST");
    return;
  }
  std::vector<uint8_t> toks = PROGS[prog].ids;
  mutCount++;
  Rng rng;
  rng.seed(sessionSeed ^ (mutCount * 104729u) ^ 0x3A3Au);
  for (int k = 0; k < val[P_MUT]; k++) {
    int n = toks.size();
    int i = (k == 0 && rng.f() < 0.6f) ? n - 1 : rng.range(n);
    const SubDef* s = subsFor[toks[i]];
    if (s) toks[i] = s->to[rng.range(s->n)];
  }
  if (rng.f() < val[P_TENS] / 100.0f) {
    int i = rng.range(toks.size());
    toks[i] = withExtension(toks[i]);
  }
  char nm[8];
  snprintf(nm, sizeof nm, "M%02d", mutCount);
  int pg;
  newBank = install(toks, nm, "New", &pg);
  jumpTo(newBank, pg);
  uiToast("MUTATE  %s < %s", nm, PROGS[prog].name.c_str());
}

static int targetRow(int& b) {
  for (int i = 0; i < 16; i++)
    if (heldOrder[i] >= 0 && held[heldOrder[i]].on) {
      b = bank();
      return page * ROWS_PER_PAGE + heldOrder[i] / 4;
    }
  b = lastBank;
  return lastRow;
}

static bool saveFavFile() {
  File f = LittleFS.open("/favs.txt", "w");
  if (!f) return false;
  f.print("# ChordBox favourites: P<TAB>name<TAB>chords\n");
  if (favBank >= 0) {
    int lastProg = -1;
    for (uint32_t r : BANKS[favBank].rows) {
      int p = r >> 1;
      if (p == lastProg) continue;
      lastProg = p;
      f.printf("P\t%s\t%s\n", PROGS[p].name.c_str(), tokString(PROGS[p].ids).c_str());
    }
  }
  f.close();
  return true;
}

void engine::star() {
  int b;
  int r = targetRow(b);
  uint8_t ids[4];
  int n = rowIds(b, r, ids);
  if (!n) {
    uiToast("PLAY A ROW FIRST");
    return;
  }
  std::vector<uint8_t> toks(ids, ids + n);
  favCount++;
  char nm[8];
  snprintf(nm, sizeof nm, "F%02d", favCount);
  favBank = install(toks, nm, "Favs", nullptr);
  bool ok = saveFavFile();
  uiToast(ok ? "* SAVED  %s" : "* %s  (NOT SAVED)", nm);
}

void engine::unstar() {
  int b;
  int r = targetRow(b);
  if (b != favBank || favBank < 0 || r < 0 || r >= (int)BANKS[favBank].rows.size()) {
    uiToast("OPEN FAVS TO UNSTAR");
    return;
  }
  int prog = BANKS[favBank].rows[r] >> 1;
  auto& rows = BANKS[favBank].rows;
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [prog](uint32_t v) { return (int)(v >> 1) == prog; }),
             rows.end());
  saveFavFile();
  if (lastBank == favBank) lastRow = -1;
  page = constrain(page, 0, nPages(favBank) - 1);
  uiToast("UNSTARRED  %s", PROGS[prog].name.c_str());
}

void engine::recall() {
  if (favBank < 0 || BANKS[favBank].rows.empty()) {
    uiToast("NO FAVS YET - STAR SAVES");
    return;
  }
  if (bank() != favBank) {
    recallBank = bank();
    recallPage = page;
    jumpTo(favBank, 0);
    uiToast("FAVS  %d", (int)BANKS[favBank].rows.size());
  } else if (recallBank >= 0) {
    jumpTo(recallBank, recallPage);
    uiToast("BACK  %s", BANKS[bank()].name.c_str());
  }
}

void engine::plain() {
  val[P_DICE] = 0;
  val[P_HUM] = 0;
  val[P_STRM] = 0;
  val[P_VOIC] = 0;
  markDirty();
  uiToast("PLAIN");
}

void engine::toggleLatch() {
  latch = !latch;
  if (!latch) releaseAll();
  uiToast(latch ? "LATCH ON" : "LATCH OFF");
}

bool engine::latched() { return latch; }

void engine::allOff() {
  stopAll();
  lastVoicing.clear();
}

void engine::stopAll() {
  resetMotion();
  releaseAll();
  release(LOOP);
  sched.clear();
  latch = false;
  midiOut(0xB0 | val[P_CHCH], 123, 0);             // all notes off, for anything left
}

bool engine::repeating() { return repeatOn; }

void engine::setRepeat(bool on) {
  int64_t now = nowUs();
  int pad = activePad();
  if (on) {
    if (pad < 0 || repeatOn) return;
    Held& h = held[pad];
    for (auto& v : h.v)
      if (v.sent) {
        noteOff(v.ch, v.note);
        v.sent = false;
      }
    for (size_t i = 0; i < sched.size();)
      if (sched[i].pad == pad) sched.erase(sched.begin() + i);
      else i++;
    repeatOn = true;
    rptCount = 0;
    lastIdx = NONE;
    motionStart(now);
  } else if (repeatOn) {
    repeatOn = false;
    lastIdx = NONE;
    if (pad >= 0 && !motionOn()) sound(pad, now, false);   // still held: back to the chord
  }
}

void engine::loopButton() {
  int64_t now = nowUs();
  if (loopState == LOOP_IDLE) {
    nSlots = 0;
    loopState = LOOP_REC;
    uiToast("REC: PLAY UP TO 8 CHORDS");
  } else if (loopState == LOOP_REC) {
    if (!nSlots) {
      loopState = LOOP_IDLE;
      uiToast("REC CANCELLED");
      return;
    }
    if (!clk::running() && lastIdx == NONE) freeT0 = now;
    loopBase = (int64_t)floor(posSteps(now) / LOOP_STEPS[val[P_LOOPLEN]]);
    loopSlot = -1;
    loopState = LOOP_PLAY;
    uiToast("LOOP  %d CHORDS", nSlots);
  } else {
    loopState = LOOP_IDLE;
    release(LOOP);
    uiToast("LOOP OFF");
  }
}

// new notes for a sounding chord (re-voiced or re-qualified), sounded at once
static void resound(int pad, std::vector<int> ns) {
  Held& h = held[pad];
  std::sort(ns.begin(), ns.end());
  ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
  uint8_t ch = h.v.empty() ? val[P_CHCH] : h.v[0].ch;
  for (auto& v : h.v)
    if (v.sent) noteOff(v.ch, v.note);
  for (size_t i = 0; i < sched.size();)
    if (sched[i].pad == pad) sched.erase(sched.begin() + i);
    else i++;
  h.v.clear();
  for (int x : ns) h.v.push_back({(uint8_t)x, ch, false});
  if (!motionOn())
    for (size_t i = 0; i < h.v.size(); i++) {
      noteOn(ch, h.v[i].note, noteVel(i, h.v.size()));
      h.v[i].sent = true;
    }
  lastVoicing = ns;
  lastNNotes = min((int)ns.size(), 10);
  for (int i = 0; i < lastNNotes; i++) lastNotes[i] = ns[i];
}

void engine::revoice(int pad, int steps) {
  if (pad < 0 || pad > 15 || !held[pad].on || !steps) return;
  Held& h = held[pad];
  std::vector<int> ns;
  for (auto& v : h.v) ns.push_back(v.note);
  invert(ns, steps);
  h.inv += steps;
  resound(pad, ns);
  uiToast("INVERSION %+d", h.inv);
}

static int soundingPad() {                  // the chord sounding now, most recent first
  for (int k = 0; k < 16; k++) {
    int p = heldOrder[k];
    if (p >= 0 && p < 16 && held[p].on) return p;
  }
  return -1;
}

void engine::shapeTurn(int steps) {
  int was = invBias;
  invBias = constrain(invBias + steps, -4, 4);
  int d = invBias - was;
  int p = soundingPad();
  if (p >= 0 && d) {
    Held& h = held[p];
    std::vector<int> ns;
    for (auto& v : h.v) ns.push_back(v.note);
    invert(ns, d);
    h.inv += d;
    resound(p, ns);
  }
  uiToast(invBias ? "INVERSION %+d" : "INVERSION 0 (ROOT)", invBias);
}

void engine::shapeQuality(int steps) {
  int p = soundingPad();
  if (p < 0) {
    uiToast("PLAY A CHORD FIRST");
    return;
  }
  quality(p, steps);
}

int engine::inversion() { return invBias; }

static const struct { const char* suf; int8_t iv[5]; uint8_t n; } QUAL[] = {
    {"", {0, 4, 7}, 3},       {"m", {0, 3, 7}, 3},          {"7", {0, 4, 7, 10}, 4},
    {"maj7", {0, 4, 7, 11}, 4}, {"m7", {0, 3, 7, 10}, 4},   {"9", {0, 4, 7, 10, 14}, 5},
    {"m9", {0, 3, 7, 10, 14}, 5}, {"sus2", {0, 2, 7}, 3},   {"sus4", {0, 5, 7}, 3},
    {"6", {0, 4, 7, 9}, 4},     {"dim", {0, 3, 6}, 3},      {"aug", {0, 4, 8}, 3},
};
static const int N_QUAL = sizeof QUAL / sizeof QUAL[0];

void engine::quality(int pad, int steps) {
  if (pad < 0 || pad > 15 || !held[pad].on || !steps) return;
  Held& h = held[pad];
  int q = h.qual < 0 ? (steps > 0 ? steps - 1 : N_QUAL + steps) : h.qual + steps;
  h.qual = ((q % N_QUAL) + N_QUAL) % N_QUAL;
  int lowest = 127;
  for (auto& v : h.v) lowest = min(lowest, (int)v.note);
  int r = lowest - ((lowest - h.rootPc) % 12 + 12) % 12;       // the root at or below it
  if (lowest - r > 6) r += 12;
  std::vector<int> ns;
  for (int i = 0; i < QUAL[h.qual].n; i++) ns.push_back(r + QUAL[h.qual].iv[i]);
  resound(pad, ns);
  snprintf(lastSym, sizeof lastSym, "%s%s", NOTE_NAMES[h.rootPc], QUAL[h.qual].suf);
  lastIvN = QUAL[h.qual].n;
  for (int i = 0; i < lastIvN; i++) lastIv[i] = h.ivRoot + QUAL[h.qual].iv[i];
  uiToast("%s", lastSym);
}

void engine::setBassPlaying(bool on) { bassPlaying = on; }

// ======================================================= playing API ======

void engine::padDown(int pad) {
  if (pad < 0 || pad > 15) return;
  playPad(pad, nowUs());
}

void engine::padUp(int pad) {
  if (pad < 0 || pad > 15) return;
  if (!latch) release(pad);
}

void engine::selectRow(int pad) {
  int row = page * ROWS_PER_PAGE + pad / 4;
  uint8_t ids[4];
  if (!rowIds(bank(), row, ids)) return;
  lastBank = bank();
  lastRow = row;
  lastCol = pad % 4;
  uiToast("ROW %d SELECTED", row + 1);
}

// ======================================================= params ===========

void engine::paramAdjust(int p, int detents) {
  if (p < 0 || p >= P_COUNT || !detents) return;
  const ParamDef& d = PDEF[p];
  int hi = p == P_BANK ? (int)BANKS.size() - 1 : d.hi;
  int v = val[p] + detents * d.step;
  if (d.wrap) {
    int span = hi - d.lo + 1;
    v = ((v - d.lo) % span + span) % span + d.lo;
  } else {
    v = constrain(v, (int)d.lo, hi);
  }
  if (v == val[p]) return;
  val[p] = v;
  if (p == P_BANK) page = 0;
  if (p == P_VAR) {
    reseed();
    lastVoicing.clear();
  }
  if (p != P_BANK) markDirty();
}

void engine::paramSet(int p, int v) {
  if (p < 0 || p >= P_COUNT) return;
  int hi = p == P_BANK ? (int)BANKS.size() - 1 : PDEF[p].hi;
  val[p] = constrain(v, (int)PDEF[p].lo, hi);
  if (p == P_BANK) page = 0;
  if (p == P_VAR) reseed();
  if (p != P_BANK) markDirty();
}

int engine::paramGet(int p) { return p >= 0 && p < P_COUNT ? val[p] : 0; }

void engine::pageMove(int detents) {
  for (; detents > 0; detents--) {
    if (++page >= nPages(bank())) {
      page = 0;
      val[P_BANK] = (bank() + 1) % BANKS.size();
    }
  }
  for (; detents < 0; detents++) {
    if (--page < 0) {
      val[P_BANK] = (bank() - 1 + BANKS.size()) % BANKS.size();
      page = nPages(bank()) - 1;
    }
  }
}

static void fmtValue(int p, char* out, size_t cap) {
  const ParamDef& d = PDEF[p];
  int v = val[p];
  switch (p) {
    case P_BANK: strlcpy(out, BANKS[v].name.c_str(), cap); return;
    case P_OCT: snprintf(out, cap, "%+d", v); return;
    case P_DICE: case P_HUM: case P_DARK: case P_TENS: case P_SOFT: case P_VRND:
    case P_AGATE: case P_EFB:
      snprintf(out, cap, "%d%%", v);
      return;
    case P_AOCT: snprintf(out, cap, "%d OCT", v); return;
    case P_RRAMP:
      if (v == 0) strlcpy(out, "FLAT", cap);
      else snprintf(out, cap, "%s %d", v > 0 ? "IN" : "OUT", abs(v));
      return;
    case P_VAR: snprintf(out, cap, "%02d", v); return;
    case P_STRM:
      if (v == 0) strlcpy(out, "OFF", cap);
      else snprintf(out, cap, "%s %d", v > 0 ? "UP" : "DOWN", abs(v));
      return;
    case P_CHCH: snprintf(out, cap, "%d", v + 1); return;
  }
  if (d.opts) strlcpy(out, d.opts[v - d.lo], cap);
  else snprintf(out, cap, "%d", v);
}

void engine::paramView(int p, ParamView& k) {
  memset(&k, 0, sizeof k);
  if (p < 0 || p >= P_COUNT) return;
  const ParamDef& d = PDEF[p];
  strlcpy(k.label, d.label, sizeof k.label);
  fmtValue(p, k.value, sizeof k.value);
  k.group = d.group;
  k.kind = d.kind;
  k.pos = val[p];
  k.lo = d.lo;
  k.hi = p == P_BANK ? BANKS.size() - 1 : d.hi;
  k.opts = d.opts;
  k.nOpts = d.opts ? d.hi - d.lo + 1 : 0;
}

int engine::paramIndex(const String& label) {
  for (int p = 0; p < P_COUNT; p++) {
    String l = PDEF[p].label;
    l.replace(" ", "");
    if (label.equalsIgnoreCase(l)) return p;
  }
  static const char* const SHORT[P_COUNT] = {"ROOT", "SCALE", "OCT", "BANK", "DICE", "VAR", "VOIC", "STRM",
                                             "HUM", "VEL", "VLEAD", "CH", "DARK", "TENS", "LEN", "MUT",
                                             "SOFT", "VRND", "WEIGHT", "SDIR", "ARP", "ARATE", "AGATE",
                                             "AOCT", "RHY", "RPT", "RRAMP", "ECHO", "EFB", "LOOPLEN"};
  for (int p = 0; p < P_COUNT; p++)
    if (label.equalsIgnoreCase(SHORT[p])) return p;
  return -1;
}

const char* engine::keyName() { return NOTE_NAMES[val[P_ROOT]]; }
const char* engine::scaleName() { return val[P_SCALE] ? SCALE_N[val[P_SCALE]] : ""; }

// ======================================================= views ============

static bool rowIsFav(int b, int r) {
  if (favBank < 0) return false;
  uint8_t ids[4];
  int n = rowIds(b, r, ids);
  if (!n) return false;
  if (b == favBank) return true;
  for (int fr = 0; fr < (int)BANKS[favBank].rows.size(); fr++) {
    uint8_t f[4];
    int fn = rowIds(favBank, fr, f);
    if (fn == n && memcmp(f, ids, n) == 0) return true;
  }
  return false;
}

static void splitSym(const char* sym, PadCell& c) {
  int rl = (sym[1] == 'b') ? 2 : 1;
  memcpy(c.root, sym, rl);
  c.root[rl] = 0;
  strlcpy(c.qual, sym + rl, sizeof c.qual);
}

void engine::place(char* bankName, size_t cap, int& pg) {
  strlcpy(bankName, BANKS[bank()].name.c_str(), cap);
  pg = page;
}

void engine::setPlace(const char* bankName, int pg) {
  for (int b = 0; b < (int)BANKS.size(); b++)
    if (BANKS[b].name == bankName) {
      val[P_BANK] = b;
      page = constrain(pg, 0, nPages(b) - 1);
      return;
    }
}

void engine::chordView(ChordView& v) {
  strlcpy(v.bank, BANKS[bank()].name.c_str(), sizeof v.bank);
  v.page = page;
  v.pages = nPages(bank());
  v.bankIdx = bank();
  v.nBanks = BANKS.size();
  v.played = played;
  v.latch = latch;
  strlcpy(v.sym, lastSym, sizeof v.sym);
  strlcpy(v.rn, lastRn, sizeof v.rn);
  if (lastBank >= 0 && lastBank < (int)BANKS.size())
    snprintf(v.rowTag, sizeof v.rowTag, "%s  %d", BANKS[lastBank].name.c_str(), lastRow + 1);
  else
    v.rowTag[0] = 0;
  v.nNotes = lastNNotes;
  memcpy(v.notes, lastNotes, lastNNotes);
  v.sounding = soundingCount();
  v.motion[0] = 0;
  if (val[P_ARP]) snprintf(v.motion, sizeof v.motion, "%s %s", ARP_N[val[P_ARP]], RATE_N[val[P_ARATE]]);
  v.repeat = repeatOn;
  v.echo = val[P_ECHO] != 0;
  v.loopState = loopState;
  v.loopN = nSlots;
  v.loopPos = loopSlot < 0 ? 0 : loopSlot + 1;
  for (int pad = 0; pad < 16; pad++) {
    PadCell& c = v.pad[pad];
    memset(&c, 0, sizeof c);
    int row = page * ROWS_PER_PAGE + pad / 4, col = pad % 4;
    uint8_t ids[4];
    int n = rowIds(bank(), row, ids);
    if (!n) continue;
    int prog, half;
    rowRef(bank(), row, prog, half);
    Rng rng;
    Res res;
    resolve(ids[col % n], padMix(prog, half, col), rng, res);
    splitSym(res.sym, c);
    strlcpy(c.rn, res.rn, sizeof c.rn);
    c.flags = PF_HAS;
    if (col >= n) c.flags |= PF_WRAP;
    if (held[pad].on) c.flags |= PF_SOUND;
    if (lastBank == bank() && lastRow == row && lastCol == col) c.flags |= PF_LAST;
    if (borrowed(res)) c.flags |= PF_BORROW;
    if (res.diced) c.flags |= PF_DICED;
  }
  for (int r = 0; r < 4; r++) v.rowFav[r] = rowIsFav(bank(), page * ROWS_PER_PAGE + r);
}

// ======================================================= harmony ==========

static void chordFromIv(const int8_t* iv, int n, Chord& c) {
  memset(&c, 0, sizeof c);
  if (n <= 0) return;
  c.valid = true;
  int root = iv[0];
  c.rootPc = ((val[P_ROOT] + root) % 12 + 12) % 12;
  bool has[12] = {};
  for (int i = 0; i < n; i++) has[((iv[i] - root) % 12 + 12) % 12] = true;
  c.third = has[3] ? 3 : has[4] ? 4 : has[2] ? 2 : has[5] ? 5 : -1;   // sus: its 2 or 4
  c.fifth = has[7] ? 7 : has[6] ? 6 : has[8] ? 8 : -1;
  c.seventh = has[10] ? 10 : has[11] ? 11 : (has[9] && c.fifth == 6) ? 9 : -1;
  chordSymbol(val[P_ROOT], iv, n, c.name, sizeof c.name);
}

void engine::harmony(Harmony& h) {
  if (harm::harmony(h)) return;                   // the Harmony Engine leads while it's on
  memset(&h, 0, sizeof h);
  h.keyPc = val[P_ROOT];
  h.scaleMask = val[P_SCALE] ? SCALE_MASK[val[P_SCALE]] : NATURAL_MINOR;
  if (played) {
    chordFromIv(lastIv, lastIvN, h.cur);
  } else {                                        // nothing played yet: the tonic
    int8_t tonic[3] = {0, (int8_t)((h.scaleMask & (1 << 3)) ? 3 : 4), 7};
    chordFromIv(tonic, 3, h.cur);
  }
  h.progPos = -1;
  int prog, half;
  if (!rowRef(lastBank, lastRow, prog, half)) return;
  const std::vector<uint8_t>& ids = PROGS[prog].ids;
  int n = min((int)ids.size(), 8);
  bool twoRows = ids.size() > 4;
  for (int i = 0; i < n; i++) {
    Rng rng;
    Res r;
    resolve(ids[i], padMix(prog, twoRows ? i / 4 : 0, twoRows ? i % 4 : i), rng, r);
    chordFromIv(r.iv, r.n, h.prog[i]);
  }
  h.progN = n;
  int pos = (twoRows ? half * 4 : 0) + lastCol;
  h.progPos = pos < n ? pos : pos % n;
}

// ======================================================= setup ============

static void loadFavs() {
  File f = LittleFS.open("/favs.txt", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line[0] == '#') continue;
    int t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1);
    if (t1 < 0 || t2 < 0) continue;
    String name = line.substring(t1 + 1, t2);
    std::vector<uint8_t> ids = parseToks(line.substring(t2 + 1).c_str());
    if (ids.empty()) continue;
    favBank = install(ids, name, "Favs", nullptr);
    favCount++;
  }
  f.close();
}

static void loadPrefs() {
  for (int p = 0; p < P_COUNT; p++) val[p] = PDEF[p].def;
  File f = LittleFS.open("/chord.bin", "r");
  if (!f) return;
  int16_t saved[P_COUNT];
  if (f.read((uint8_t*)saved, sizeof saved) == sizeof saved)
    for (int p = 0; p < P_COUNT; p++)
      if (p != P_BANK && p != P_VAR) val[p] = constrain(saved[p], PDEF[p].lo, PDEF[p].hi);
  f.close();
}

static void savePrefs() {
  File f = LittleFS.open("/chord.bin", "w");
  if (f) {
    f.write((const uint8_t*)val, sizeof val);
    f.close();
  }
  dirty = false;
}

void engine::begin() {
  for (int i = 0; i < N_CHORDS; i++) {
    const SubDef* s = findSubs(i);
    if (!s) s = findSubs(baseTriad(i));
    subsFor[i] = s;
  }
  PROGS.reserve(N_PROG_SRC + 64);
  for (int i = 0; i < N_PROG_SRC; i++)
    PROGS.push_back({PROG_SRC[i].name, parseToks(PROG_SRC[i].toks)});
  for (int i = 0; i < N_BANK_SRC; i++)
    BANKS.push_back({BANK_SRC[i].name,
                     std::vector<uint32_t>(BANK_SRC[i].rows, BANK_SRC[i].rows + BANK_SRC[i].n)});

  loadFavs();                         // LittleFS is mounted by the sketch
  loadPrefs();

  // land somewhere different every session: the key is kept, the bank is not
  bootSeed = hwRandom();
  live.seed(hwRandom());
  val[P_BANK] = bootSeed % N_BANK_SRC;
  page = (bootSeed >> 8) % nPages(val[P_BANK]);
  val[P_VAR] = 0;
  reseed();
  memset(heldOrder, -1, sizeof heldOrder);
}

void engine::tick(int64_t nowUs, uint32_t nowMs) {
  for (size_t i = 0; i < sched.size();) {
    if (nowUs >= sched[i].due) {
      Sched s = sched[i];
      sched.erase(sched.begin() + i);
      for (auto& v : held[s.pad].v)
        if (!v.sent && v.note == s.note && v.ch == s.ch) {
          v.sent = true;
          break;
        }
      noteOn(s.ch, s.note, s.vel);
    } else {
      i++;
    }
  }
  motionTick(nowUs);
  if (dirty && nowMs - dirtyAt > SAVE_IDLE_MS && !soundingCount() && sched.empty())
    savePrefs();
}

// ======================================================= console ==========

void engine::printState(Print& p) {
  char v[14];
  p.printf("key %s  scale %s  bank %d/%d %s  page %d/%d  latch %d\n",
           NOTE_NAMES[val[P_ROOT]], SCALE_N[val[P_SCALE]], bank() + 1,
           (int)BANKS.size(), BANKS[bank()].name.c_str(), page + 1, nPages(bank()), latch);
  for (int k = 0; k < P_COUNT; k++) {
    fmtValue(k, v, sizeof v);
    p.printf("  %-9s %-9s%s", PDEF[k].label, v, k % 4 == 3 ? "\n" : "");
  }
  for (int r = 0; r < ROWS_PER_PAGE; r++) {
    int row = page * ROWS_PER_PAGE + r;
    uint8_t ids[4];
    int n = rowIds(bank(), row, ids);
    p.printf("  row %d:", row + 1);
    for (int c = 0; c < 4 && n; c++) {
      int prog, half;
      rowRef(bank(), row, prog, half);
      Rng rng;
      Res res;
      resolve(ids[c % n], padMix(prog, half, c), rng, res);
      p.printf("  %-8s", res.sym);
    }
    p.println();
  }
}

void engine::printFavs(Print& p) {
  File f = LittleFS.open("/favs.txt", "r");
  if (!f) {
    p.println("(no favs.txt)");
    return;
  }
  while (f.available()) p.write(f.read());
  f.close();
}
