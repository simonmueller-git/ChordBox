// harm.cpp — the Harmony Engine's instrument: see harm.h.
//
// Governing decisions (spec section 1) and where they live here:
//   chords first, key as by-product ... deriveKey(): the key comes from votes and
//                                        what was heard; shown, never asked for
//   identity and shape apart ........... Slot = (chord, shape); voicing() places it
//   one layout ......................... pads 0-3 are always the four shapes; the
//                                        12 below always sound (notes or chords)
//   nothing is ever lost ............... every change pushes a take (32, starred
//                                        ones never age out); held NO steps back
//   tune before hunting ................ knob 2 in HUNT, sent as pitch bend
//   thin by default .................... SH_THIN, extensions fitted into the note set
#include "harm.h"
#include "bigmem.h"
#include <LittleFS.h>
#include "bass.h"
#include "clock.h"
#include "config.h"
#include "harm_grow.h"
#include "harm_listen.h"
#include "harm_melody.h"
#include "harm_theory.h"
#include "platform.h"

using namespace ht;

namespace harm {
namespace {

inline int mod12(int x) { return ((x % 12) + 12) % 12; }

// ================================================================ profile
enum Mood : uint8_t { MOOD_BRIGHT, MOOD_MIXO, MOOD_DARK, MOOD_DORIAN, MOOD_N };
const char* const MOOD_WORD[MOOD_N] = {"bright", "bright mixo", "dark", "dark dorian"};

struct Profile {
  uint8_t version = 4;
  NoteSet ns;
  HomeObs hunt, heard;               // evidence for home: by ear, from MIDI
  int8_t manualHome = 9;             // set by hand in PLAY; -1 = inferred
  uint8_t mood = MOOD_DARK;
  int8_t tune = 0;                   // cents
  int8_t home = 9;                   // the home the loop's chords count from
  uint8_t spice = 3;
  hg::Loop loop;
  hm::MCtx mel;
  uint8_t melodyOn = 0;
  hm::Line line;
  uint8_t melCh = 2;                 // MIDI ch 3
  uint8_t oct = 2;                   // HUNT: the note pads' octave
  uint8_t shape = SH_THIN;           // the shape new chords get
  uint8_t listenBassCh = 0;          // LISTEN: 0 = auto, else the bass channel 1-16
  uint8_t skel = 0;
  uint8_t topPick = 0;               // top-note target: 0 none, 1 fifth, 2 third, 3 ninth
  int8_t selSlot = 0;
};
Profile P;
bool live = false;                   // live-safe: PLAY only, nothing rerolls
uint8_t hmode = HM_PLAY;
bool inHarm = false;

// the key, derived
int K_home = 9, K_coll = 0;
bool K_dark = true, K_manual = true;

int collForMood(int h, int mood) {
  switch (mood) {
    case MOOD_BRIGHT: return fifthsPos(h);
    case MOOD_MIXO: return fifthsPos(h + 5);
    case MOOD_DORIAN: return fifthsPos(h - 2);
    default: return fifthsPos(h + 3);
  }
}

void combinedObs(HomeObs& o) {
  o.clear();
  for (int pc = 0; pc < 12; pc++) {
    o.bassDur[pc] = P.hunt.bassDur[pc] + P.heard.bassDur[pc];
    o.barStart[pc] = P.hunt.barStart[pc] + P.heard.barStart[pc];
    o.first[pc] = P.hunt.first[pc] + P.heard.first[pc];
    o.totalDur[pc] = P.hunt.totalDur[pc] + P.heard.totalDur[pc];
    o.phraseEnd[pc] = P.hunt.phraseEnd[pc] + P.heard.phraseEnd[pc];
  }
}

bool anyEvidence() {
  for (int k = 0; k < 12; k++)
    if (P.ns.vote[k] != 0 || P.ns.listen[k] != 0) return true;
  return false;
}

void deriveKey() {
  if (P.manualHome >= 0 || !anyEvidence()) {
    int h = P.manualHome >= 0 ? P.manualHome : P.home;
    K_home = mod12(h);
    K_coll = collForMood(K_home, P.mood);
    K_manual = true;
  } else {
    float Ps[12], H[12];
    posterior(P.ns, Ps);
    int k = 0;
    for (int i = 1; i < 12; i++)
      if (Ps[i] > Ps[k]) k = i;
    HomeObs o;
    combinedObs(o);
    homePosterior(o, Ps, H);
    uint16_t m = collMask(k);
    int h = -1;
    for (int pc = 0; pc < 12; pc++)
      if ((m & (1 << pc)) && (h < 0 || H[pc] > H[h])) h = pc;
    float total = 0;
    for (int pc = 0; pc < 12; pc++) total += H[pc];
    if (total <= 0) h = mod12(collTonic(k) + 9);   // no evidence for home yet: its dark home
    K_home = h;
    K_coll = k;
    K_manual = false;
  }
  K_dark = modeDark(modeOf(K_home, K_coll));
}

// chords count from home: when the inferred home moves, keep every slot's sound
void rebase() {
  deriveKey();
  if (K_home == P.home) return;
  int d = P.home - K_home;
  for (int i = 0; i < 16; i++) {
    hg::Slot& s = P.loop.s[i];
    if (!s.c.planed) s.c.root = (int8_t)mod12(s.c.root + d);
    if (!s.c2.planed) s.c2.root = (int8_t)mod12(s.c2.root + d);
    if (s.low >= 0) s.low = (int8_t)mod12(s.low + d);
  }
  P.home = (int8_t)K_home;
}

hg::Ctx ctx(uint32_t seed = 1) {
  hg::Ctx x;
  x.home = K_home;
  x.coll = K_coll;
  x.dark = K_dark;
  x.spice = P.spice;
  x.seed = seed;
  return x;
}

hm::MCtx mctx() {
  hm::MCtx x = P.mel;
  x.home = K_home;
  x.coll = K_coll;
  x.dark = K_dark;
  x.spice = P.spice;
  return x;
}

// ===================================================================== midi
uint8_t ref[16][128];
struct Q { int64_t at; uint8_t st, d1, d2; };
Q q[160];
int nq = 0;

int chordCh() { return engine::paramGet(engine::P_CHCH) & 15; }
int bassCh() { return bass::paramGet(bass::BP_CH) & 15; }

void noteOnNow(uint8_t ch, uint8_t n, uint8_t v) {
  if (ref[ch][n]++ == 0) midiOut(0x90 | ch, n, v);
}
void noteOffNow(uint8_t ch, uint8_t n) {
  if (ref[ch][n] && --ref[ch][n] == 0) midiOut(0x80 | ch, n, 0);
}
void at(int64_t t, uint8_t st, uint8_t d1, uint8_t d2) {
  if (nq < (int)(sizeof q / sizeof q[0])) q[nq++] = {t, st, d1, d2};
  else if ((st & 0xF0) == 0x80) noteOffNow(st & 15, d1);   // never lose a note-off
}
void flushQueue(int64_t now) {
  bool any = true;
  while (any) {
    any = false;
    int best = -1;
    for (int i = 0; i < nq; i++)
      if (q[i].at <= now && (best < 0 || q[i].at < q[best].at)) best = i;
    if (best < 0) break;
    Q e = q[best];
    q[best] = q[--nq];
    any = true;
    if ((e.st & 0xF0) == 0x90) noteOnNow(e.st & 15, e.d1, e.d2);
    else noteOffNow(e.st & 15, e.d1);
  }
}
void silenceCh(uint8_t ch) {
  for (int i = 0; i < nq;)
    if ((q[i].st & 15) == ch) q[i] = q[--nq];
    else i++;
  for (int n = 0; n < 128; n++)
    if (ref[ch][n]) {
      ref[ch][n] = 0;
      midiOut(0x80 | ch, n, 0);
    }
}

void sendTune() {
  int v = 8192 + P.tune * 4096 / 100;            // a +-2 semitone bend range: 100 cents = 4096
  v = v < 0 ? 0 : v > 16383 ? 16383 : v;
  for (int ch : {chordCh(), bassCh(), (int)P.melCh}) midiOut(0xE0 | ch, v & 127, v >> 7);
}

// ================================================================ voicing
// shape -> notes: extensions outside the note set move to the nearest note in it
int voicing(const Chord& c, uint8_t shape, const uint8_t* prev, int nPrev, uint8_t out[8]) {
  if (c.qual == Q_NONE) return 0;
  int8_t off[6];
  int n = shapeOffsets(c.qual, shape, c.root == 5 && !c.planed, off);
  int root = chordRootPc(c, K_home);
  if (!c.planed) {
    uint16_t set = collMask(K_coll), tones = chordTones(c, K_home);
    for (int i = 0; i < n; i++) {
      int pc = mod12(root + off[i]);
      if ((tones & (1 << pc)) || (set & (1 << pc))) continue;
      for (int d : {-1, 1, -2, 2})
        if (set & (1 << mod12(pc + d))) {
          off[i] = (int8_t)(off[i] + d);
          break;
        }
    }
    for (int i = 0; i < n; i++)                   // no doubled notes after moving
      for (int j = i + 1; j < n; j++)
        if (off[j] == off[i]) off[j] = off[--n], j--;
  }
  static const uint8_t TOPS[4] = {0, 7, 3, 14};
  int top = 0;
  if (P.topPick) {
    int iv = TOPS[P.topPick & 3];
    if (P.topPick == 2 && !K_dark) iv = 4;
    top = 60 + mod12(K_home + iv);
    while (top < 64) top += 12;
    while (top > 72) top -= 12;
  }
  if (P.loop.top && !top) top = P.loop.top;
  return place(off, n, root, prev, nPrev, top, out);
}

// ======================================================= takes & capture
struct Take {
  uint8_t used = 0, star = 0;
  uint16_t serial = 0;
  int8_t home = 0;
  uint8_t mood = 0;
  hg::Loop loop;
};
const int N_TAKES = 32;
Take* T = nullptr;                   // [N_TAKES], in PSRAM
uint16_t serialNext = 1;
int lastTakeIdx = -1;
uint32_t lastTakeMs = 0;
uint8_t lastTakeKind = 0;
int undoSerial = -1;                 // stepping back: the take on show
bool takesDirty = false, songDirty = false;
uint32_t changedAt = 0;
bool takesUp = false;
int takeSel = 0;

enum Kind : uint8_t { TK_EDIT = 1, TK_SHAPE, TK_SUGGEST, TK_LEN, TK_GROW, TK_HUNT, TK_LISTEN, TK_CAPTURE, TK_SKEL };

void markSong() {
  songDirty = true;
  changedAt = millis();
}

void pushTake(uint8_t kind) {
  uint32_t now = millis();
  undoSerial = -1;
  markSong();
  takesDirty = true;
  bool gesture = kind == TK_SHAPE || kind == TK_SUGGEST || kind == TK_LEN;   // turning: one take per gesture
  if (gesture && lastTakeIdx >= 0 && kind == lastTakeKind && now - lastTakeMs < 1500 && T[lastTakeIdx].used) {
    T[lastTakeIdx].loop = P.loop;                  // the same gesture still going: one take
    lastTakeMs = now;
    return;
  }
  int slot = -1;
  for (int i = 0; i < N_TAKES && slot < 0; i++)
    if (!T[i].used) slot = i;
  if (slot < 0)                                    // full: the oldest that isn't starred
    for (int i = 0; i < N_TAKES; i++)
      if (!T[i].star && (slot < 0 || T[i].serial < T[slot].serial)) slot = i;
  if (slot < 0) slot = 0;
  Take& t = T[slot];
  t.used = 1;
  t.star = 0;
  t.serial = serialNext++;
  t.home = (int8_t)K_home;
  t.mood = P.mood;
  t.loop = P.loop;
  lastTakeIdx = slot;
  lastTakeKind = kind;
  lastTakeMs = now;
}

int takesByAge(int* idx) {                         // newest first
  int n = 0;
  for (int i = 0; i < N_TAKES; i++)
    if (T[i].used) idx[n++] = i;
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (T[idx[b]].serial > T[idx[a]].serial) {
        int t = idx[a];
        idx[a] = idx[b];
        idx[b] = t;
      }
  return n;
}

void loadTake(int i) {
  P.loop = T[i].loop;
  markSong();
}

// capture: the chord heard in each of the last 64 bars
hg::Slot cap[64];
int capHead = 0;                                   // next write
hg::Slot capCur;
bool capSeen = false;

// ========================================================= the sound grid
struct Held { bool on = false; uint8_t n = 0, ch = 0; uint8_t note[8]; };
Held held[16], slotHeldNotes;
Chord lastChord = {0, Q_NONE, 0};
int8_t lastLow = -1;
uint8_t lastV[8], nLastV = 0;
int8_t lastPad = -1, lastFromSlot = -1;
int8_t slotHeld = -1, slotHeld2 = -1;
bool slotLayerOn = false;
int suggestIdx = 0;
// the slot the slot view works on: the one held, else the one selected
int slotFocus() { return slotHeld >= 0 ? slotHeld : P.selSlot; }

void releaseHeld(Held& h, int64_t now) {
  if (!h.on) return;
  for (int i = 0; i < h.n; i++) at(now, 0x80 | h.ch, h.note[i], 0);
  h.on = false;
}

void sound(Held& h, const uint8_t* notes, int n, uint8_t ch, uint8_t vel, int64_t now, int strumUs = 8000) {
  releaseHeld(h, now);
  h.n = (uint8_t)(n > 8 ? 8 : n);
  h.ch = ch;
  for (int i = 0; i < h.n; i++) {
    h.note[i] = notes[i];
    at(now + (int64_t)i * strumUs, 0x90 | ch, notes[i], vel);
  }
  h.on = true;
}

// sound a chord now (pads never wait for the clock)
void soundChord(Held& h, const Chord& c, uint8_t shape, int8_t low) {
  uint8_t v[8];
  int n = voicing(c, shape, lastV, nLastV, v);
  if (!n) return;
  sound(h, v, n, (uint8_t)chordCh(), 96, nowUs());
  memcpy(lastV, v, n);
  nLastV = (uint8_t)n;
  lastChord = c;
  lastLow = low;
  capCur.c = c;
  capCur.shape = shape;
  capCur.low = low;
  capCur.flags = hg::HF_SET;
  capSeen = true;
}

// ================================================================== HUNT
enum HuntSub : uint8_t { HS_NOTES, HS_OFFERS, HS_HOLD, HS_HOLD_AB, HS_HOLD_HOME };
uint8_t huntSub = HS_NOTES;
int huntBar = 0;
uint16_t fitMask = 0, clashMask = 0;
uint16_t lastProbe = 0;
int8_t lastProbePc = -1;
struct Offer { int8_t rootAbs; uint8_t qual; int8_t lowAbs; };
Offer offers[3];
int nOffers = 0, offerSel = 0;
int8_t holdPc = -1;
Held drone;

int padPc(int pad) { return pad - 4; }             // pads 4..15 = C..B

void probeSuggestion(int& tryPc, bool& pair) {
  float Ps[12];
  posterior(P.ns, Ps);
  int r;
  uint16_t m = nextProbe(Ps, &r, clashMask);
  tryPc = r;
  pair = __builtin_popcount(m) == 2;
}

void auditionOffer() {
  if (offerSel < 0 || offerSel >= nOffers) return;
  const Offer& o = offers[offerSel];
  Chord c{(int8_t)mod12(o.rootAbs - K_home), o.qual, 0};
  soundChord(held[0], c, SH_THIN, (int8_t)mod12(o.lowAbs - K_home));
  uint8_t low = (uint8_t)(36 + 12 * (P.oct - 2) + o.lowAbs);
  sound(drone, &low, 1, (uint8_t)bassCh(), 90, nowUs());
}

void makeOffers(int lowAbs) {
  float Ps[12];
  posterior(P.ns, Ps);
  struct C { int8_t r; uint8_t q; float s; } c[6] = {
      {(int8_t)lowAbs, Q_BRIGHT, 0},         {(int8_t)lowAbs, Q_DARK, 0},
      {(int8_t)mod12(lowAbs - 4), Q_BRIGHT, 0}, {(int8_t)mod12(lowAbs - 3), Q_DARK, 0},
      {(int8_t)mod12(lowAbs - 7), Q_BRIGHT, 0}, {(int8_t)mod12(lowAbs - 7), Q_DARK, 0}};
  for (auto& x : c) {
    Chord ch{x.r, x.q, 1};
    x.s = fitChance(Ps, chordTones(ch, 0)) + (x.r == lowAbs ? 0.05f : 0);   // over its own root a hair first
  }
  for (int i = 0; i < 6; i++)
    for (int j = i + 1; j < 6; j++)
      if (c[j].s > c[i].s) {
        C t = c[i];
        c[i] = c[j];
        c[j] = t;
      }
  nOffers = 3;
  for (int i = 0; i < 3; i++) offers[i] = {c[i].r, c[i].q, (int8_t)lowAbs};
  offerSel = 0;
}

void huntKeep() {                                   // YES on an offer: the bar is found
  const Offer& o = offers[offerSel];
  rebase();
  hg::Slot s;
  s.c = {(int8_t)mod12(o.rootAbs - K_home), o.qual, 0};
  s.low = o.rootAbs == o.lowAbs ? -1 : (int8_t)mod12(o.lowAbs - K_home);
  s.shape = SH_THIN;
  s.flags = hg::HF_SET;
  if (P.loop.len < 4) P.loop.len = 4;
  P.loop.s[huntBar % P.loop.len] = s;
  pushTake(TK_HUNT);
  char nm[12];
  chordName(s.c, K_home, K_coll, nm, sizeof nm);
  uiToast("BAR %d: %s", huntBar % P.loop.len + 1, nm);
  huntBar = (huntBar + 1) % P.loop.len;
  fitMask = clashMask = 0;
  huntSub = HS_NOTES;
  releaseHeld(held[0], nowUs());
  releaseHeld(drone, nowUs());
}

// ================================================================= LISTEN
const int N_EV = 384;
hl::Ev* evs = nullptr;               // LISTEN's notes [N_EV], in PSRAM
uint32_t* evUs = nullptr;
int nev = 0;
struct Open { uint8_t ch, pitch, vel; float on; uint32_t onUs; };
Open open_[32];
int nOpen = 0;
hl::Result lres;
bool lresValid = false;
uint32_t lastAnalysisMs = 0;
int listenChoice = 0;                              // bassline only: 0 plain, 1 twist
int64_t tap1Us = 0;
bool listenClock = true;

float pos16(int64_t nowUs) {
  int32_t su = clk::stepUs();
  return (float)clk::step() + (su > 0 ? (float)(nowUs - clk::stepAtUs()) / su : 0);
}

void listenClear() {
  nev = nOpen = 0;
  lresValid = false;
  memset(P.ns.listen, 0, sizeof P.ns.listen);
  P.heard.clear();
  tap1Us = 0;
}

void analyzeNow() {
  if (nev < 4) return;
  static hl::Ev* tmp = nullptr;
  if (!tmp) tmp = (hl::Ev*)bigAlloc(N_EV * sizeof(hl::Ev));
  float ms16 = clk::stepUs() / 1000.0f;
  if (listenClock) {
    memcpy(tmp, evs, nev * sizeof(hl::Ev));
  } else {                                          // no clock: find the grid first
    static uint32_t* on = nullptr;
    static uint8_t* w = nullptr;
    if (!on) on = (uint32_t*)bigAlloc(N_EV * 4), w = (uint8_t*)bigAlloc(N_EV);
    for (int i = 0; i < nev; i++) on[i] = evUs[i], w[i] = evs[i].pitch < 55 ? 3 : 1;
    float bpm, ph, lead;
    int db;
    if (!hl::detectTempo(on, w, nev, bpm, ph, db, lead)) return;
    float per = 60e6f / bpm / 4;
    ms16 = per / 1000;
    for (int i = 0; i < nev; i++) {
      tmp[i] = evs[i];
      float origin = tap1Us ? (float)(uint32_t)tap1Us : ph + db * per;
      tmp[i].on = ((float)evUs[i] - origin) / per + 1600;   // keep it positive
      tmp[i].dur = evs[i].dur * 1000.0f / per;               // dur held in ms without a clock
    }
    if (!tap1Us && lead < 0.2f) uiToast("TAP \"1\" ON KNOB 2");
  }
  int bassChSel = P.listenBassCh ? P.listenBassCh - 1 : -1;
  if (!hl::analyze(tmp, nev, bassChSel, -1, ms16, lres)) return;
  lresValid = true;
  memcpy(P.ns.listen, lres.listenL, sizeof P.ns.listen);
  P.heard = lres.home;
  if (lres.bars >= 2) P.manualHome = -1;           // enough heard: the key follows it
  rebase();
  markSong();
}

// =================================================================== GROW
enum GrowSub : uint8_t { GS_IDLE, GS_CANDS, GS_SKEL };
uint8_t growSub = GS_IDLE;
hg::Loop cand[3], growBase, skelLoop;
int nCand = 0, candSel = 0, candPage = 0;
uint32_t growSeed = 1;

// ================================================================= MELODY
bool tapArmed = false, tapRec = false;
uint32_t tapBar = 0;
uint16_t tapMask = 0;
uint16_t tapRhythm[16];
bool haveTap = false;
uint32_t melSeed = 1;
Held melHeld[16];

void buildBarHarm(hm::BarHarm* b, int n) {
  uint8_t prev[8];
  int np = 0;
  for (int i = 0; i < n; i++) {
    const hg::Slot& s = P.loop.s[i % (P.loop.len ? P.loop.len : 1)];
    hm::BarHarm& h = b[i];
    h = hm::BarHarm();
    if (!(s.flags & hg::HF_SET) || s.c.qual == Q_NONE) continue;
    h.rootPc = chordRootPc(s.c, K_home);
    h.qual = s.c.qual;
    h.tones = chordTones(s.c, K_home);
    h.full = chordTonesFull(s.c, K_home);
    h.homeChord = !s.c.planed && s.c.root == 0;
    h.nShape = (uint8_t)voicing(s.c, s.shape, prev, np, h.shape);
    memcpy(prev, h.shape, h.nShape);
    np = h.nShape;
  }
}

void makeLine(bool fromTap) {
  int bars = P.loop.len ? P.loop.len : 4;
  if (bars > 16) bars = 16;
  hm::BarHarm b[16];
  buildBarHarm(b, bars);
  hm::MCtx x = mctx();
  x.seed = melSeed = melSeed * 1103515245u + 12345u + millis();
  uint16_t r[16];
  for (int i = 0; i < 16; i++) r[i] = tapRhythm[0];
  hm::generate(x, b, bars, fromTap && haveTap ? r : nullptr, P.line);
  P.melodyOn = 1;
  markSong();
}

// ============================================================== playback
bool playingFlag = false, ownsClock = false;
uint32_t startBar = 0;
int8_t playSlotNow = -1;
bool previewOn = false;                            // GROW: auditioning a candidate / mood
hg::Loop* playLoop() { return previewOn ? (growSub == GS_SKEL ? &skelLoop : &cand[candSel]) : &P.loop; }
uint8_t vc[16][2][8], nvc[16][2];
bool printArmed = false, printing = false;
int printBars = 0;
bool abPending = false, abOnB = false;
hg::Loop abSaved;
bool noDown = false;
uint32_t noAt = 0, noStepAt = 0;
bool noUsedForUndo = false;
uint8_t styleOutway() { return ht::N_STABS; }      // the rhythm list ends with "out of the way"

void computeVoicings(const hg::Loop& l) {
  uint8_t prev[8];
  int np = 0;
  for (int pass = 0; pass < 2; pass++)              // twice round, so slot 1 leads from the last
    for (int i = 0; i < l.len; i++) {
      const hg::Slot& s = l.s[i];
      for (int h = 0; h < 2; h++) {
        nvc[i][h] = 0;
        if (!(s.flags & hg::HF_SET)) continue;
        if (h == 1 && !(s.flags & hg::HF_SPLIT)) continue;
        const Chord& c = h ? s.c2 : s.c;
        nvc[i][h] = (uint8_t)voicing(c, h ? s.shape2 : s.shape, prev, np, vc[i][h]);
        if (nvc[i][h]) {
          memcpy(prev, vc[i][h], nvc[i][h]);
          np = nvc[i][h];
        }
      }
    }
}

void stopLoop() {
  playingFlag = false;
  printing = printArmed = false;
  playSlotNow = -1;
  silenceCh((uint8_t)chordCh());
  silenceCh(P.melCh);
  if (ownsClock) {
    ownsClock = false;
    if (!clk::external()) clk::stopInternal();     // the Move took over: its transport is its own
  }
}

void startLoop(int fromSlot = 0) {
  if (!P.loop.len) return;
  uint32_t bar = clk::step() / 16;
  if (!clk::running()) {
    clk::startInternal();
    ownsClock = true;
    bar = 0;
    startBar = 0 - (uint32_t)fromSlot;
  } else {
    startBar = bar + 1 - (uint32_t)fromSlot;        // from the next bar line
  }
  playingFlag = true;
  computeVoicings(*playLoop());
}

const Chord* chordAt(const hg::Loop& l, int slot, int pos, int8_t* low) {
  const hg::Slot& s = l.s[slot];
  if (!(s.flags & hg::HF_SET)) return nullptr;
  if (low) *low = s.low;
  return ((s.flags & hg::HF_SPLIT) && pos >= 8) ? &s.c2 : &s.c;
}

void playStep(uint32_t step, int64_t t, int32_t dur) {
  hg::Loop& l = *playLoop();
  if (!l.len) return;
  uint32_t bar = step / 16;
  int pos = step % 16;
  int slot = (int)((bar - startBar) % l.len);
  int next = (slot + 1) % l.len;
  playSlotNow = (int8_t)slot;
  uint8_t ch = (uint8_t)chordCh();
  bool sustain = l.rhythm == 0, outway = l.rhythm >= styleOutway();
  uint16_t mask = outway ? 0x0101 : rhythmMask(STABS[l.rhythm < N_STABS ? l.rhythm : 0].steps);
  bool pushedNext = (l.s[next].flags & hg::HF_PUSH) && (l.s[next].flags & hg::HF_SET);
  bool pushedThis = (l.s[slot].flags & hg::HF_PUSH);
  int earlyAt = sustain || outway ? 14 : 15;        // an 8th early for pads, a 16th for stabs
  auto hit = [&](int sl, int half, int64_t when, int64_t len, uint8_t vel) {
    int n = nvc[sl][half];
    if (!n) return;
    const uint8_t* v = vc[sl][half];
    if (outway) {                                   // stay out of the way: shared notes, a shell, the top
      const hg::Slot& s = l.s[sl];
      const Chord& c = half ? s.c2 : s.c;
      const hg::Slot& nx = l.s[(sl + 1) % l.len];
      uint16_t shared = chordTones(c, K_home) & chordTones(nx.c, K_home);
      uint8_t pick[4];
      int k = 0;
      for (int i = 0; i < n && k < 3; i++)
        if (shared & (1 << (v[i] % 12))) pick[k++] = v[i];
      if (!k && n >= 2) pick[k++] = v[0], pick[k++] = v[n - 1];
      else if (k < 3) pick[k++] = v[n - 1];
      for (int i = 0; i < k; i++) {
        at(when + i * 8000, 0x90 | ch, pick[i], vel);
        at(when + len, 0x80 | ch, pick[i], 0);
      }
      return;
    }
    for (int i = 0; i < n; i++) {
      at(when + i * 8000, 0x90 | ch, v[i], vel);    // strum 8 ms per note, low to high
      at(when + len, 0x80 | ch, v[i], 0);
    }
  };
  // Early: the next chord comes in before the bar line
  if (pushedNext && pos == earlyAt) {
    int64_t len = sustain || outway ? (int64_t)dur * (16 - earlyAt + ((l.s[next].flags & hg::HF_SPLIT) ? 8 : 16)) - 3000
                                    : (int64_t)dur * 3 / 2;
    if (l.rhythm == 7) len = (int64_t)dur * 17;     // pad push: tied over the bar
    hit(next, 0, t, len, 100);
    return;
  }
  if (pushedThis && pos == 0) return;               // already sounding since the push
  if (pushedNext && pos > earlyAt) return;
  if (!(mask & (1 << pos))) {
    if (!((l.s[slot].flags & hg::HF_SPLIT) && pos == 8 && (sustain || outway))) return;
  }
  int half = ((l.s[slot].flags & hg::HF_SPLIT) && pos >= 8) ? 1 : 0;
  int64_t len;
  if (sustain || outway) {
    int end = half || !(l.s[slot].flags & hg::HF_SPLIT) ? 16 : 8;
    if (pushedNext && end == 16) end = earlyAt;
    len = (int64_t)dur * (end - pos) - 3000;
  } else {
    len = (int64_t)dur * 3 / 2;                     // gate: one and a half steps
    if (l.rhythm == 7) len = (int64_t)dur * 17;
  }
  bool firstHit = true;
  for (int p = half ? 8 : 0; p < pos; p++)
    if (mask & (1 << p)) firstHit = false;
  uint8_t vel = (pos % 4 == 0 || firstHit) ? 100 : 70;
  hit(slot, half, t, len, vel);
}

void playMelody(uint32_t step, int64_t t, int32_t dur) {
  if (!P.melodyOn || !P.line.n || !P.line.bars) return;
  uint32_t bar = step / 16;
  int linePos = (int)(((bar - startBar) % P.line.bars) * 16 + step % 16);
  for (int i = 0; i < P.line.n; i++) {
    const hm::Note& n = P.line.note[i];
    if (n.step != linePos) continue;
    at(t, 0x90 | P.melCh, n.pitch, n.vel);
    at(t + (int64_t)dur * n.len - 3000, 0x80 | P.melCh, n.pitch, 0);
  }
}

// ================================================================== saves
void saveSong() {
  File f = LittleFS.open("/song.bin", "w");
  if (!f) return;
  f.write((const uint8_t*)&P, sizeof P);
  f.close();
}

void loadSong() {
  File f = LittleFS.open("/song.bin", "r");
  if (!f) return;
  Profile t;
  if (f.read((uint8_t*)&t, sizeof t) == sizeof t && t.version == P.version) P = t;
  f.close();
}

struct TakesFile { uint8_t version = 2; uint16_t serialNext; Take t[N_TAKES]; };

void saveTakes() {
  File f = LittleFS.open("/takes.bin", "w");
  if (!f) return;
  uint8_t v = 2;
  f.write(&v, 1);
  f.write((const uint8_t*)&serialNext, 2);
  f.write((const uint8_t*)T, N_TAKES * sizeof(Take));
  f.close();
}

void loadTakes() {
  File f = LittleFS.open("/takes.bin", "r");
  if (!f) return;
  uint8_t v = 0;
  uint16_t sn = 0;
  if (f.read(&v, 1) == 1 && v == 2 && f.read((uint8_t*)&sn, 2) == 2) {
    Take* tmp = (Take*)bigAlloc(N_TAKES * sizeof(Take));
    if (tmp && f.read((uint8_t*)tmp, N_TAKES * sizeof(Take)) == (int)(N_TAKES * sizeof(Take))) {
      memcpy(T, tmp, N_TAKES * sizeof(Take));
      serialNext = sn;
    }
    free(tmp);
  }
  f.close();
}

// ================================================================== misc
void selectNextEmpty() {
  int len = P.loop.len ? P.loop.len : 4;
  for (int k = 1; k <= len; k++) {
    int i = (P.selSlot + k) % len;
    if (!(P.loop.s[i].flags & hg::HF_SET)) {
      P.selSlot = (int8_t)i;
      return;
    }
  }
  P.selSlot = (int8_t)((P.selSlot + 1) % len);
}

void slotName(const hg::Slot& s, char* out, int cap) {
  if (!(s.flags & hg::HF_SET)) {
    strlcpy(out, ".", cap);
    return;
  }
  char a[8], b[8];
  chordName(s.c, K_home, K_coll, a, sizeof a);
  if (s.flags & hg::HF_SPLIT) {
    chordName(s.c2, K_home, K_coll, b, sizeof b);
    snprintf(out, cap, "%.3s|%.3s", a, b);
  } else if (s.low >= 0) {
    snprintf(out, cap, "%.4s/%s", a, noteName(K_home + s.low, K_coll));
  } else {
    strlcpy(out, a, cap);
  }
}

const char* keyWord() {
  static char b[20];
  snprintf(b, sizeof b, "%s %s", noteName(K_home, K_coll), K_manual ? MOOD_WORD[P.mood] : modeWord(modeOf(K_home, K_coll)));
  return b;
}

}  // namespace

// ================================================================== API
void begin(bool liveSafe) {
  live = liveSafe;
  if (!T) T = (Take*)bigAlloc(N_TAKES * sizeof(Take));
  if (!evs) evs = (hl::Ev*)bigAlloc(N_EV * sizeof(hl::Ev)), evUs = (uint32_t*)bigAlloc(N_EV * 4);
  for (int i = 0; i < N_TAKES; i++) T[i] = Take();
  serialNext = 1;
  lastTakeIdx = -1;
  P.ns.clear();
  P.hunt.clear();
  P.heard.clear();
  P.manualHome = (int8_t)engine::paramGet(engine::P_ROOT);
  P.home = P.manualHome;
  P.loop = hg::Loop();
  P.line = hm::Line();
  loadSong();
  loadTakes();
  deriveKey();
  if (P.home != K_home && !K_manual) P.home = (int8_t)K_home;
  memset(ref, 0, sizeof ref);
}

void enter(uint8_t m) {
  if (live && m != HM_PLAY) m = HM_PLAY;
  if (hmode == HM_HUNT && m != HM_HUNT) {
    releaseHeld(drone, nowUs());
    huntSub = HS_NOTES;
  }
  hmode = m < HM_COUNT ? m : HM_PLAY;
  inHarm = true;
  takesUp = false;
  if (hmode != HM_GROW && previewOn) {
    previewOn = false;
    growSub = GS_IDLE;
    computeVoicings(P.loop);
  }
  rebase();
}

void leave() {
  inHarm = false;
  takesUp = false;
  slotLayerOn = false;
  for (auto& h : held) releaseHeld(h, nowUs());
  releaseHeld(drone, nowUs());
}

uint8_t mode() { return hmode; }
bool liveSafe() { return live; }
bool playing() { return playingFlag; }
bool ownsHarmony() { return inHarm || playingFlag; }

bool harmony(engine::Harmony& h) {
  if (!ownsHarmony()) return false;
  memset(&h, 0, sizeof h);
  h.keyPc = (uint8_t)K_home;
  uint16_t set = collMask(K_coll);
  for (int i = 0; i < 12; i++)
    if (set & (1 << mod12(K_home + i))) h.scaleMask |= 1 << i;
  Chord c = lastChord;
  int8_t low = lastLow;
  const hg::Loop& l = *playLoop();
  if (playingFlag && playSlotNow >= 0) {
    int pos = clk::step() % 16;
    const Chord* pc = chordAt(l, playSlotNow, pos, &low);
    if (pc) c = *pc;
  }
  if (c.qual == Q_NONE) c = {0, (uint8_t)(K_dark ? Q_DARK : Q_BRIGHT), 0};
  auto fill = [&](const Chord& ch, int8_t lw, engine::Chord& out) {
    memset(&out, 0, sizeof out);
    out.valid = true;
    int root = chordRootPc(ch, K_home);
    int bassPc = lw >= 0 ? mod12(K_home + lw) : root;
    out.rootPc = (uint8_t)bassPc;
    uint16_t t = chordTones(ch, K_home);
    // the chord's other notes, counted from the low note
    out.third = out.fifth = out.seventh = -1;
    for (int iv = 1; iv < 12; iv++)
      if (t & (1 << mod12(bassPc + iv))) {
        if (out.third < 0 && iv <= 5) out.third = (int8_t)iv;
        else if (out.fifth < 0 && iv >= 5) out.fifth = (int8_t)iv;
      }
    if (out.fifth < 0) out.fifth = 7;
    if (ch.qual == Q_DOM || ch.qual == Q_HANG || ch.qual == Q_DARK) out.seventh = (int8_t)mod12(root + 10 - bassPc);
    chordName(ch, K_home, K_coll, out.name, sizeof out.name);
  };
  fill(c, low, h.cur);
  int n = l.len < 8 ? l.len : 8;
  for (int i = 0; i < n; i++) {
    const hg::Slot& s = l.s[i];
    if (s.flags & hg::HF_SET) fill(s.c, s.low, h.prog[h.progN++]);
  }
  h.progPos = playingFlag && playSlotNow >= 0 && playSlotNow < 8 ? playSlotNow : -1;
  return true;
}

void tick(int64_t now, uint32_t ms) {
  flushQueue(now);
  // NO held: step back through the takes, one every 0.6 s
  if (noDown && ms - noAt >= 600 && ms - noStepAt >= 600) {
    noStepAt = ms;
    noUsedForUndo = true;
    int idx[N_TAKES];
    int n = takesByAge(idx);
    int cur = undoSerial < 0 ? (n ? T[idx[0]].serial : -1) : undoSerial;
    for (int i = 0; i < n; i++)
      if (T[idx[i]].serial < cur) {
        loadTake(idx[i]);
        undoSerial = T[idx[i]].serial;
        uiToast("BACK TO TAKE %d", T[idx[i]].serial);
        if (playingFlag) computeVoicings(*playLoop());
        break;
      }
  }
  if (!listenClock && hmode == HM_LISTEN && inHarm && ms - lastAnalysisMs > 2000) {
    lastAnalysisMs = ms;
    analyzeNow();
  }
  if ((songDirty || takesDirty) && ms - changedAt > SAVE_IDLE_MS) {
    if (songDirty) saveSong();
    if (takesDirty) saveTakes();
    songDirty = takesDirty = false;
  }
}

void onStep(uint32_t step, int64_t t, int32_t dur) {
  int pos = step % 16;
  uint32_t bar = step / 16;
  if (pos == 0) {
    // capture: what was heard in the bar that just ended
    bool anyHeld = false;
    for (auto& h : held) anyHeld |= h.on;
    hg::Slot s;
    if (capSeen || anyHeld) s = capCur;
    cap[capHead] = s;
    capHead = (capHead + 1) % 64;
    capSeen = anyHeld;
    // A/B on the bar line
    if (abPending) {
      abPending = false;
      int idx[N_TAKES];
      int n = takesByAge(idx);
      if (!abOnB && n >= 2) {
        abSaved = P.loop;
        P.loop = T[idx[1]].loop;
        abOnB = true;
      } else if (abOnB) {
        P.loop = abSaved;
        abOnB = false;
      }
      computeVoicings(*playLoop());
    }
    // Print to Move: from this bar, once round
    if (printArmed) {
      printArmed = false;
      printing = true;
      printBars = 0;
      playingFlag = true;
      startBar = bar;
      computeVoicings(*playLoop());
    } else if (printing && ++printBars >= P.loop.len) {
      stopLoop();
      uiToast("PRINTED %d BARS", P.loop.len);
    }
    // MELODY: tap the rhythm for one bar
    if (tapArmed) {
      tapArmed = false;
      tapRec = true;
      tapBar = bar;
      tapMask = 0;
    } else if (tapRec && bar != tapBar) {
      tapRec = false;
      if (tapMask) {
        tapRhythm[0] = tapMask;
        haveTap = true;
        makeLine(true);
        uiToast("A LINE FROM YOUR RHYTHM");
      } else {
        uiToast("NO TAPS HEARD");
      }
    }
    if (hmode == HM_LISTEN && inHarm && listenClock && nev >= 4) analyzeNow();
  }
  if (playingFlag) {
    playStep(step, t, dur);
    playMelody(step, t, dur);
  }
}

void onTransport(bool run) {
  if (!run) {
    if (playingFlag && !ownsClock) {
      playingFlag = false;
      playSlotNow = -1;
      silenceCh((uint8_t)chordCh());
      silenceCh(P.melCh);
    }
  } else if (printArmed) {
    // printArmed starts on the first bar line, which comes with step 0
  }
}

void midiIn(uint8_t st, uint8_t d1, uint8_t d2, int64_t now) {
  if (!inHarm || hmode != HM_LISTEN) return;
  uint8_t type = st & 0xF0, ch = st & 15;
  listenClock = clk::external() && clk::running();
  float p = listenClock ? pos16(now) : 0;
  if (type == 0x90 && d2) {
    if (nOpen < 32) open_[nOpen++] = {ch, d1, d2, p, (uint32_t)now};
    return;
  }
  if (type != 0x80 && type != 0x90) return;
  for (int i = 0; i < nOpen; i++) {
    Open& o = open_[i];
    if (o.ch != ch || o.pitch != d1) continue;
    if (nev >= N_EV) {                               // full: keep the newest
      memmove(evs, evs + 64, (nev - 64) * sizeof evs[0]);
      memmove(evUs, evUs + 64, (nev - 64) * sizeof evUs[0]);
      nev -= 64;
    }
    hl::Ev& e = evs[nev];
    e.pitch = o.pitch;
    e.vel = o.vel;
    e.ch = o.ch;
    e.on = listenClock ? o.on : 0;
    e.dur = listenClock ? p - o.on : (float)((uint32_t)now - o.onUs) / 1000.0f;   // ms without a clock
    evUs[nev] = o.onUs;
    nev++;
    open_[i] = open_[--nOpen];
    break;
  }
}

// ---------------------------------------------------------------- controls
void pad(int p, bool down) {
  if (p < 0 || p > 15) return;
  int64_t now = nowUs();
  if (!down) {
    if (hmode == HM_HUNT && (huntSub == HS_HOLD)) return;   // the hold test drones on
    releaseHeld(held[p], now);
    releaseHeld(melHeld[p], now);
    return;
  }
  lastPad = (int8_t)p;
  lastFromSlot = -1;
  if (tapRec) {                                      // MELODY: this press is part of the rhythm
    int32_t su = clk::stepUs();
    int s = su > 0 ? (int)lroundf((float)(now - clk::stepAtUs()) / su) + (int)(clk::step() % 16) : 0;
    tapMask |= 1 << (s & 15);
  }
  if (p < 4) {                                       // the four shapes of the last chord
    static const uint8_t SH[4] = {SH_THIN, SH_FULL, SH_WIDE, SH_FLIP};
    P.shape = SH[p];
    if (lastChord.qual != Q_NONE) soundChord(held[p], lastChord, P.shape, lastLow);
    if (P.selSlot >= 0 && (P.loop.s[P.selSlot].flags & hg::HF_SET) && slotLayerOn) {
      P.loop.s[P.selSlot].shape = P.shape;
      pushTake(TK_SHAPE);
    }
    markSong();
    return;
  }
  switch (hmode) {
    case HM_HUNT: {
      int pc = padPc(p);
      uint8_t notes[2];
      int n = 0;
      int base = 36 + 12 * (P.oct - 2);
      if (huntSub == HS_HOLD) {                      // the hold test: a drone until another
        holdPc = (int8_t)pc;
        notes[0] = (uint8_t)(base + pc);
        sound(drone, notes, 1, (uint8_t)bassCh(), 90, now);
        return;
      }
      int tryPc;
      bool pair;
      probeSuggestion(tryPc, pair);
      notes[n++] = (uint8_t)(base + pc);
      if (pair && pc == tryPc) notes[n++] = (uint8_t)(base + pc + 7);   // the suggested power chord
      lastProbe = (uint16_t)((1 << pc) | (n == 2 ? 1 << mod12(pc + 7) : 0));
      lastProbePc = (int8_t)pc;
      sound(held[p], notes, n, (uint8_t)bassCh(), 96, now, 0);
      if (huntSub == HS_OFFERS) huntSub = HS_NOTES;  // a new note: back to hunting
      return;
    }
    case HM_MELODY: {
      uint8_t pitch[12];
      hm::safePads(mctx(), pitch);
      uint8_t n = pitch[p - 4];
      sound(melHeld[p], &n, 1, P.melCh, 100, now, 0);
      return;
    }
    default: {                                       // PLAY, GROW, LISTEN: one-finger chords
      Chord pads[12];
      int n = chordPads(K_home, K_coll, pads);
      if (p - 4 >= n) return;
      soundChord(held[p], pads[p - 4], P.shape, -1);
      return;
    }
  }
}

void slotLayer(bool on) {
  slotLayerOn = on;
  if (!on) {
    if (slotHeld >= 0) releaseHeld(slotHeldNotes, nowUs());
    slotHeld = slotHeld2 = -1;
  }
}
bool inSlotLayer() { return slotLayerOn; }

void slotPad(int slot, bool down, bool shift) {
  if (slot < 0 || slot > 15) return;
  int64_t now = nowUs();
  if (!down) {
    if (slot == slotHeld) {
      releaseHeld(slotHeldNotes, now);
      slotHeld = slotHeld2 >= 0 ? slotHeld2 : -1;
      slotHeld2 = -1;
    } else if (slot == slotHeld2) {
      slotHeld2 = -1;
    }
    return;
  }
  if (shift) {                                       // SHIFT + slot: play from there
    if (slot < P.loop.len) {
      if (playingFlag) startBar = clk::step() / 16 + 1 - slot;
      else startLoop(slot);
      uiToast("PLAY FROM SLOT %d", slot + 1);
    }
    return;
  }
  if (slotHeld >= 0 && slotHeld != slot) {           // two slots: copy the first to the second
    if (live) return;
    hg::Slot s = P.loop.s[slotHeld];
    s.flags &= ~hg::HF_LOCK;
    P.loop.s[slot] = s;
    if (slot >= P.loop.len) P.loop.len = slot >= 8 ? 16 : 8;
    slotHeld2 = (int8_t)slot;
    pushTake(TK_EDIT);
    if (playingFlag) computeVoicings(*playLoop());
    uiToast("SLOT %d COPIED TO %d", slotHeld + 1, slot + 1);
    return;
  }
  slotHeld = (int8_t)slot;
  P.selSlot = (int8_t)slot;
  suggestIdx = 0;
  lastFromSlot = (int8_t)slot;
  const hg::Slot& s = P.loop.s[slot];
  if ((s.flags & hg::HF_SET) && s.c.qual != Q_NONE) soundChord(slotHeldNotes, s.c, s.shape, s.low);
}

void yes() {
  if (takesUp) {                                     // on a take: star it
    int idx[N_TAKES];
    int n = takesByAge(idx);
    if (takeSel < n) {
      Take& t = T[idx[takeSel]];
      t.star = !t.star;
      takesDirty = true;
      changedAt = millis();
      uiToast(t.star ? "* TAKE %d STARRED" : "TAKE %d UNSTARRED", t.serial);
    }
    return;
  }
  if (slotLayerOn && slotFocus() >= 0) {             // slots: YES locks the slot
    int sl = slotFocus();
    hg::Slot& s = P.loop.s[sl];
    s.flags ^= hg::HF_LOCK;
    pushTake(TK_EDIT);
    uiToast(s.flags & hg::HF_LOCK ? "SLOT %d LOCKED" : "SLOT %d UNLOCKED", sl + 1);
    return;
  }
  switch (hmode) {
    case HM_HUNT:
      if (huntSub == HS_OFFERS) {
        huntKeep();
      } else if (huntSub == HS_NOTES && lastProbe) {
        P.manualHome = -1;
        vote(P.ns, lastProbe, true);
        int pc = lastProbePc;
        if (__builtin_popcount(lastProbe) == 1) {    // a single low note that sits: this bar's low note
          fitMask |= 1 << pc;
          P.hunt.barStart[pc] += 1;
          P.hunt.bassDur[pc] += 1;
          if (huntBar == 0) {
            memset(P.hunt.first, 0, sizeof P.hunt.first);
            P.hunt.first[pc] = 1;
          }
          rebase();
          makeOffers(pc);
          huntSub = HS_OFFERS;
          auditionOffer();
        } else {
          fitMask |= lastProbe;
          rebase();
          uiToast("FITS. NOW THE LOW NOTE");
        }
        lastProbe = 0;
        markSong();
      } else if (huntSub == HS_HOLD && holdPc >= 0) {   // never sounds wrong: home or its fifth
        nOffers = 2;
        offers[0] = {holdPc, Q_BRIGHT, holdPc};
        offers[1] = {holdPc, Q_DARK, holdPc};
        offerSel = 0;
        huntSub = HS_HOLD_AB;
        auditionOffer();
      } else if (huntSub == HS_HOLD_AB) {
        Chord c{offers[offerSel].rootAbs, offers[offerSel].qual, 1};
        vote(P.ns, chordTones(c, 0), true);
        uint8_t q = offers[offerSel].qual;
        nOffers = 2;                                  // one more A/B: home, or its fifth?
        offers[0] = {holdPc, q, holdPc};
        offers[1] = {(int8_t)mod12(holdPc - 7), q, (int8_t)mod12(holdPc - 7)};
        offerSel = 0;
        huntSub = HS_HOLD_HOME;
        auditionOffer();
      } else if (huntSub == HS_HOLD_HOME) {
        int h = offers[offerSel].rootAbs;
        P.manualHome = -1;
        P.hunt.first[h] += 8;
        P.hunt.barStart[h] += 4;
        rebase();
        huntSub = HS_NOTES;
        releaseHeld(drone, nowUs());
        releaseHeld(held[0], nowUs());
        uiToast("HOME: %s", keyWord());
        markSong();
      }
      return;
    case HM_LISTEN: {
      if (live || !lresValid) return;
      rebase();
      int L = lres.loop;
      int len = L <= 4 ? 4 : 8;
      hg::Loop l = P.loop;
      l.len = (uint8_t)len;
      for (int i = 0; i < len; i++) {
        const hl::BarChord& b = lres.bassOnly ? (listenChoice ? lres.twist[i % L] : lres.plain[i % L]) : lres.chord[i % L];
        hg::Slot s;
        if (b.root >= 0) {
          s.c = {(int8_t)mod12(b.root - K_home), b.qual, 0};
          s.flags = hg::HF_SET;
          if (b.low >= 0 && b.low != b.root) s.low = (int8_t)mod12(b.low - K_home);
        }
        l.s[i] = s;
      }
      P.loop = l;
      pushTake(TK_LISTEN);
      uiToast("THE LOOP, FROM WHAT I HEARD");
      if (playingFlag) computeVoicings(*playLoop());
      return;
    }
    case HM_GROW:
      if (growSub == GS_CANDS && nCand) {
        P.loop = cand[candSel];
        pushTake(TK_GROW);
        uiToast("KEPT");
      } else if (growSub == GS_SKEL) {
        P.loop = skelLoop;
        pushTake(TK_SKEL);
        uiToast("KEPT: %s", SKELETONS[P.skel].mood);
      } else {
        return;
      }
      growSub = GS_IDLE;
      previewOn = false;
      if (playingFlag) computeVoicings(*playLoop());
      return;
    case HM_MELODY:                                   // YES: tap a rhythm over the next bar
      if (live) return;
      if (!playingFlag) startLoop();
      tapArmed = true;
      uiToast("TAP A RHYTHM IN THE NEXT BAR");
      return;
    default: {                                        // PLAY: the last chord into the slot
      if (lastChord.qual == Q_NONE) {
        uiToast("PLAY A CHORD, THEN YES");
        return;
      }
      if (P.loop.len < 4) P.loop.len = 4;
      int i = P.selSlot >= 0 ? P.selSlot % 16 : 0;
      if (i >= P.loop.len) P.loop.len = i >= 8 ? 16 : 8;
      hg::Slot& s = P.loop.s[i];
      if (s.flags & hg::HF_LOCK) {
        uiToast("SLOT %d IS LOCKED", i + 1);
        return;
      }
      s.c = lastChord;
      s.shape = P.shape;
      s.low = lastLow;
      s.flags = (uint8_t)((s.flags & hg::HF_PUSH) | hg::HF_SET);
      pushTake(TK_EDIT);
      char nm[12];
      chordName(s.c, K_home, K_coll, nm, sizeof nm);
      uiToast("SLOT %d: %s", i + 1, nm);
      selectNextEmpty();
      if (playingFlag) computeVoicings(*playLoop());
      return;
    }
  }
}

void no(bool down) {
  if (down) {
    noDown = true;
    noAt = noStepAt = millis();
    noUsedForUndo = false;
    return;
  }
  noDown = false;
  if (noUsedForUndo) return;                         // it was held: that was undo
  if (takesUp) {
    takesUp = false;
    return;
  }
  if (slotLayerOn && slotFocus() >= 0) {             // slots: NO clears the slot
    if (live) return;
    int sl = slotFocus();
    hg::Slot& s = P.loop.s[sl];
    if (s.flags & hg::HF_LOCK) return;
    s = hg::Slot();
    pushTake(TK_EDIT);
    if (playingFlag) computeVoicings(*playLoop());
    uiToast("SLOT %d CLEARED", sl + 1);
    return;
  }
  switch (hmode) {
    case HM_HUNT:
      if (huntSub == HS_OFFERS || huntSub >= HS_HOLD_AB) {   // none of these: hunt again
        huntSub = huntSub == HS_OFFERS ? HS_NOTES : HS_HOLD;
        releaseHeld(held[0], nowUs());
        releaseHeld(drone, nowUs());
        return;
      }
      if (huntSub == HS_NOTES && lastProbe) {
        P.manualHome = -1;
        vote(P.ns, lastProbe, false);
        clashMask |= lastProbe;
        lastProbe = 0;
        rebase();
        markSong();
      }
      return;
    case HM_LISTEN:
      listenClear();
      uiToast("LISTENING AFRESH");
      return;
    case HM_GROW: {
      if (live) {
        uiToast("LIVE-SAFE: NO REROLLS");
        return;
      }
      if (growSub == GS_SKEL) {
        growSub = GS_IDLE;
        previewOn = false;
        if (playingFlag) computeVoicings(*playLoop());
        return;
      }
      if (growSub != GS_CANDS) {
        growBase = P.loop;
        candPage = 0;
        growSeed = growSeed * 2654435761u + millis();
      } else {
        candPage++;
      }
      int len = growBase.len < 8 ? 8 : growBase.len;
      hg::Ctx x = ctx(growSeed);
      nCand = hg::candidates(growBase, len, x, candPage, cand, 3);
      if (!nCand) {                                  // this page ran out: fresh ideas
        growSeed++;
        candPage = 0;
        x.seed = growSeed;
        nCand = hg::candidates(growBase, len, x, 0, cand, 3);
      }
      growSub = GS_CANDS;
      candSel = 0;
      previewOn = nCand > 0;
      if (!playingFlag) startLoop();
      else computeVoicings(*playLoop());
      return;
    }
    case HM_MELODY:
      if (live) return;
      makeLine(haveTap);
      uiToast("A NEW LINE");
      return;
    default: {                                        // PLAY: clear the selected slot
      int i = P.selSlot;
      if (i < 0 || i >= P.loop.len) return;
      if (P.loop.s[i].flags & hg::HF_LOCK) return;
      P.loop.s[i] = hg::Slot();
      pushTake(TK_EDIT);
      if (playingFlag) computeVoicings(*playLoop());
      uiToast("SLOT %d CLEARED", i + 1);
      return;
    }
  }
}

void suggestTurn(int d) {                          // slots view + MAIN turn: ranked suggestions
  int sl = slotFocus();
  if (live || sl < 0 || sl > 15) return;
  hg::Slot& s = P.loop.s[sl];
  if (s.flags & hg::HF_LOCK) {
    uiToast("SLOT %d IS LOCKED", sl + 1);
    return;
  }
  Chord sg[8];
  int n = hg::suggest(P.loop, sl, ctx(), sg);
  if (!n) return;
  suggestIdx = ((suggestIdx + d) % n + n) % n;
  s.c = sg[suggestIdx];
  s.flags = (uint8_t)((s.flags & ~hg::HF_SPLIT) | hg::HF_SET);
  if (sl >= P.loop.len) P.loop.len = sl >= 8 ? 16 : 8;
  soundChord(slotHeldNotes, s.c, s.shape, s.low);
  pushTake(TK_SUGGEST);
  if (playingFlag) computeVoicings(*playLoop());
  char nm[12];
  chordName(s.c, K_home, K_coll, nm, sizeof nm);
  uiToast("SUGGESTION %d/%d: %s", suggestIdx + 1, n, nm);
}

void setLength(int d) {                            // loop length 4, 8, 16
  static const uint8_t L[3] = {4, 8, 16};
  int i = P.loop.len >= 16 ? 2 : P.loop.len >= 8 ? 1 : 0;
  i = constrain(i + d, 0, 2);
  if (L[i] == P.loop.len) return;
  hg::Loop out;
  hg::Ctx x = ctx();
  x.spice = 0;                                      // longer: the loop repeats literally
  if (L[i] > P.loop.len) hg::generate(P.loop, L[i], x, out);
  else out = P.loop, out.len = L[i];
  P.loop = out;
  if (P.selSlot >= P.loop.len) P.selSlot = 0;
  pushTake(TK_LEN);
  if (playingFlag) computeVoicings(*playLoop());
}

void browseMood(int d) {                           // GROW: a mood from the skeleton library, auditioned
  if (live) return;
  P.skel = (uint8_t)((P.skel + d + N_SKELETONS) % N_SKELETONS);
  hg::skeleton(P.skel, ctx(), skelLoop);
  skelLoop.rhythm = P.loop.rhythm;
  growSub = GS_SKEL;
  previewOn = true;
  if (!playingFlag) startLoop();
  else computeVoicings(*playLoop());
  markSong();
}

void value(int d) {
  if (!d) return;
  if (takesUp) {
    int idx[N_TAKES];
    int n = takesByAge(idx);
    takeSel = constrain(takeSel + d, 0, n ? n - 1 : 0);
    return;
  }
  if (slotLayerOn) {
    suggestTurn(d);
    return;
  }
  switch (hmode) {
    case HM_HUNT:
      if (huntSub == HS_OFFERS || huntSub >= HS_HOLD_AB) {
        offerSel = ((offerSel + d) % nOffers + nOffers) % nOffers;
        auditionOffer();
      } else {
        P.oct = (uint8_t)constrain(P.oct + d, 1, 4);
        markSong();
      }
      return;
    case HM_LISTEN:
      listenChoice = listenChoice ? 0 : 1;
      return;
    case HM_GROW:
      if (growSub == GS_CANDS && nCand) {
        candSel = ((candSel + d) % nCand + nCand) % nCand;
        computeVoicings(*playLoop());
      } else if (growSub == GS_SKEL) {
        browseMood(d);
      } else {
        P.spice = (uint8_t)constrain(P.spice + d, 0, 7);
        markSong();
      }
      return;
    case HM_MELODY:
      P.mel.density = constrain(P.mel.density + d, 0, 7);
      markSong();
      return;
    default: {                                        // PLAY: the shape
      P.shape = (uint8_t)(((P.shape + d) % SH_COUNT + SH_COUNT) % SH_COUNT);
      if (lastChord.qual != Q_NONE) soundChord(held[0], lastChord, P.shape, lastLow);
      markSong();
      return;
    }
  }
}

void click() {                                      // MAIN click: play / stop (TAKES: load)
  if (takesUp) {
    int idx[N_TAKES];
    int n = takesByAge(idx);
    if (takeSel < n) {
      loadTake(idx[takeSel]);
      undoSerial = T[idx[takeSel]].serial;
      if (playingFlag) computeVoicings(*playLoop());
      uiToast("TAKE %d LOADED", T[idx[takeSel]].serial);
    }
    takesUp = false;
    return;
  }
  if (playingFlag) stopLoop();
  else startLoop();
}

// ---------------------------------------------------- the SET knob's list
enum SetId : uint8_t { ST_TUNE, ST_OCT, ST_HOLD, ST_BASSCH, ST_PLAIN, ST_HOME, ST_COLOUR, ST_RHYTHM, ST_TOP,
                       ST_LEN, ST_SPICE, ST_MOOD, ST_VOICE, ST_DENS, ST_RANGE, ST_NOTES, ST_LINE, ST_SHAPE };
const uint8_t SET_HUNT[] = {ST_TUNE, ST_HOLD, ST_LEN};             // MAIN turns the octave
const uint8_t SET_LISTEN[] = {ST_BASSCH, ST_PLAIN, ST_LEN};
const uint8_t SET_PLAY[] = {ST_HOME, ST_COLOUR, ST_RHYTHM, ST_TOP, ST_LEN};
const uint8_t SET_GROW[] = {ST_MOOD, ST_LEN, ST_RHYTHM, ST_HOME, ST_COLOUR};   // MAIN turns Spice
const uint8_t SET_MELODY[] = {ST_VOICE, ST_RANGE, ST_NOTES, ST_LINE};   // MAIN turns the density

const uint8_t* setList(int& n) {
  switch (hmode) {
    case HM_HUNT: n = sizeof SET_HUNT; return SET_HUNT;
    case HM_LISTEN: n = sizeof SET_LISTEN; return SET_LISTEN;
    case HM_GROW: n = sizeof SET_GROW; return SET_GROW;
    case HM_MELODY: n = sizeof SET_MELODY; return SET_MELODY;
    default: n = sizeof SET_PLAY; return SET_PLAY;
  }
}

int setCount() {
  int n;
  setList(n);
  return n;
}

void setInfo(int i, char* label, int lcap, char* val, int vcap) {
  int n;
  const uint8_t* L = setList(n);
  if (n <= 0) return;
  uint8_t id = L[((i % n) + n) % n];
  static const char* const TOPW[4] = {"free", "5th", "3rd", "9th"};
  switch (id) {
    case ST_TUNE: strlcpy(label, "TUNE", lcap); snprintf(val, vcap, "%+d c", P.tune); break;
    case ST_OCT: strlcpy(label, "OCTAVE", lcap); snprintf(val, vcap, "%d", P.oct); break;
    case ST_HOLD: strlcpy(label, "HOLD TEST", lcap); strlcpy(val, huntSub >= HS_HOLD ? "on" : "off", vcap); break;
    case ST_BASSCH:
      strlcpy(label, "BASS IN", lcap);
      if (P.listenBassCh) snprintf(val, vcap, "ch %d", P.listenBassCh);
      else strlcpy(val, "auto", vcap);
      break;
    case ST_PLAIN: strlcpy(label, "BASS ONLY", lcap); strlcpy(val, listenChoice ? "twist" : "plain", vcap); break;
    case ST_HOME: strlcpy(label, "HOME", lcap); strlcpy(val, noteName(K_home, K_coll), vcap); break;
    case ST_COLOUR: strlcpy(label, "COLOUR", lcap); strlcpy(val, MOOD_WORD[P.mood], vcap); break;
    case ST_RHYTHM:
      strlcpy(label, "RHYTHM", lcap);
      strlcpy(val, P.loop.rhythm < N_STABS ? STABS[P.loop.rhythm].name : "OUT OF WAY", vcap);
      break;
    case ST_TOP: strlcpy(label, "TOP NOTE", lcap); strlcpy(val, TOPW[P.topPick & 3], vcap); break;
    case ST_LEN: strlcpy(label, "LENGTH", lcap); snprintf(val, vcap, "%d", P.loop.len ? P.loop.len : 4); break;
    case ST_SPICE: strlcpy(label, "SPICE", lcap); snprintf(val, vcap, "%d/7", P.spice); break;
    case ST_MOOD: strlcpy(label, "MOOD", lcap); strlcpy(val, SKELETONS[P.skel].mood, vcap); break;
    case ST_VOICE: strlcpy(label, "VOICE", lcap); strlcpy(val, hm::voiceWord(P.mel.voice), vcap); break;
    case ST_DENS: strlcpy(label, "DENSITY", lcap); snprintf(val, vcap, "%d", P.mel.density); break;
    case ST_RANGE: strlcpy(label, "RANGE", lcap); snprintf(val, vcap, "%d", P.mel.range); break;
    case ST_NOTES: strlcpy(label, "NOTES", lcap); strlcpy(val, P.mel.penta ? "5 safe" : "all 7", vcap); break;
    case ST_LINE: strlcpy(label, "LINE", lcap); strlcpy(val, P.melodyOn ? "on" : "off", vcap); break;
    default: label[0] = val[0] = 0;
  }
}

void setAdjust(int i, int d) {
  int n;
  const uint8_t* L = setList(n);
  if (n <= 0 || !d) return;
  uint8_t id = L[((i % n) + n) % n];
  switch (id) {
    case ST_TUNE:                                    // tune until the beating stops
      P.tune = (int8_t)constrain(P.tune + d, -50, 50);
      sendTune();
      break;
    case ST_OCT: P.oct = (uint8_t)constrain(P.oct + d, 1, 4); break;
    case ST_HOLD:                                    // the hold test, for a loop with no clear bass
      if (d > 0 && huntSub < HS_HOLD) {
        huntSub = HS_HOLD;
        holdPc = -1;
        uiToast("HOLD TEST: TAP A NOTE, LET IT DRONE");
      } else if (d < 0 && huntSub >= HS_HOLD) {
        huntSub = HS_NOTES;
        releaseHeld(drone, nowUs());
      }
      return;
    case ST_BASSCH: P.listenBassCh = (uint8_t)constrain(P.listenBassCh + d, 0, 16); break;
    case ST_PLAIN: listenChoice = listenChoice ? 0 : 1; return;
    case ST_HOME:                                    // home, by hand: the chords move with it
      P.manualHome = (int8_t)mod12(K_home + d);
      P.home = P.manualHome;
      deriveKey();
      if (playingFlag) computeVoicings(*playLoop());
      break;
    case ST_COLOUR:
      P.mood = (uint8_t)(((P.mood + d) % MOOD_N + MOOD_N) % MOOD_N);
      if (P.manualHome < 0) P.manualHome = (int8_t)K_home;
      P.home = P.manualHome;
      deriveKey();
      if (playingFlag) computeVoicings(*playLoop());
      break;
    case ST_RHYTHM:
      P.loop.rhythm = (uint8_t)(((P.loop.rhythm + d) % (N_STABS + 1) + N_STABS + 1) % (N_STABS + 1));
      if (previewOn) cand[candSel].rhythm = skelLoop.rhythm = P.loop.rhythm;
      break;
    case ST_TOP:
      P.topPick = (uint8_t)(((P.topPick + d) % 4 + 4) % 4);
      if (playingFlag) computeVoicings(*playLoop());
      break;
    case ST_LEN: setLength(d); return;
    case ST_SPICE: P.spice = (uint8_t)constrain(P.spice + d, 0, 7); break;
    case ST_MOOD: browseMood(d); return;
    case ST_VOICE: P.mel.voice = (uint8_t)(((P.mel.voice + d) % hm::V_COUNT + hm::V_COUNT) % hm::V_COUNT); break;
    case ST_DENS: P.mel.density = constrain(P.mel.density + d, 0, 7); break;
    case ST_RANGE: P.mel.range = constrain(P.mel.range + d, 0, 4); break;
    case ST_NOTES: P.mel.penta = !P.mel.penta; break;
    case ST_LINE:
      P.melodyOn = !P.melodyOn;
      if (!P.melodyOn) silenceCh(P.melCh);
      break;
  }
  markSong();
}

void yesNoWords(char* y, char* n, int cap) {        // the names of YES and NO, right now
  const char* Y = "YES";
  const char* N = "NO";
  if (takesUp) Y = "STAR", N = "BACK";
  else if (slotLayerOn) Y = "LOCK", N = "CLEAR";
  else switch (hmode) {
    case HM_HUNT:
      if (huntSub == HS_OFFERS) Y = "KEEP", N = "NONE";
      else if (huntSub == HS_HOLD) Y = "RESTS", N = "-";
      else if (huntSub >= HS_HOLD_AB) Y = "THIS", N = "BACK";
      else Y = "FITS", N = "CLASH";
      break;
    case HM_LISTEN: Y = lresValid ? "USE IT" : "-", N = "CLEAR"; break;
    case HM_GROW:
      if (growSub == GS_CANDS) Y = "KEEP", N = "MORE";
      else if (growSub == GS_SKEL) Y = "KEEP", N = "BACK";
      else Y = "-", N = "IDEAS";
      break;
    case HM_MELODY: Y = "TAP", N = "NEW"; break;
    default: Y = "ADD", N = "CLEAR";
  }
  strlcpy(y, Y, cap);
  strlcpy(n, N, cap);
}

void tapOne() {                                     // tap "1": the next 16th is the bar start
  clk::resync();
  tap1Us = nowUs();
  uiToast("\"1\" IS HERE");
}

void shiftYes() {                                     // the last bars played -> a take
  int len = P.loop.len ? P.loop.len : 4;
  int last = -1;
  for (int k = 1; k <= 64; k++) {
    int i = (capHead - k + 64) % 64;
    if (cap[i].flags & hg::HF_SET) {
      last = i;
      break;
    }
  }
  if (last < 0) {
    uiToast("NOTHING CAPTURED YET");
    return;
  }
  hg::Loop l = P.loop;
  l.len = (uint8_t)len;
  for (int k = 0; k < len; k++) l.s[k] = cap[(last - (len - 1) + k + 64) % 64];
  P.loop = l;
  pushTake(TK_CAPTURE);
  if (playingFlag) computeVoicings(*playLoop());
  uiToast("CAPTURE: THE LAST %d BARS", len);
}

void shiftNo() {
  abPending = true;
  uiToast(abOnB ? "BACK TO A ON THE BAR" : "B ON THE BAR LINE");
}

void print() {
  if (!P.loop.len) return;
  if (playingFlag) stopLoop();
  printArmed = true;
  if (!clk::running()) uiToast("PRINT: STARTS WITH THE MOVE");
  else uiToast("PRINT FROM THE NEXT BAR");
  computeVoicings(*playLoop());
}

void takes(bool open) {
  takesUp = open;
  takeSel = 0;
}
bool takesOpen() { return takesUp; }

void stopAll() {
  stopLoop();
  for (auto& h : held) releaseHeld(h, nowUs());
  releaseHeld(drone, nowUs());
}

void newSong() {                                    // a new sample: forget what was heard and voted
  pushTake(TK_EDIT);                                  // the old loop stays in TAKES
  P.loop = hg::Loop();
  P.selSlot = 0;
  if (playingFlag) stopLoop();
  P.ns.clear();
  P.hunt.clear();
  P.heard.clear();
  P.tune = 0;
  P.manualHome = (int8_t)K_home;
  huntBar = 0;
  huntSub = HS_NOTES;
  fitMask = clashMask = 0;
  listenClear();
  sendTune();
  deriveKey();
  markSong();
  uiToast("NEW SONG: THE KEY IS OPEN");
}

void setMelodyCh(int ch) {
  silenceCh(P.melCh);
  P.melCh = (uint8_t)constrain(ch, 0, 15);
  markSong();
}
int melodyCh() { return P.melCh; }

// ===================================================================== view
void view(HarmView& v) {
  memset(&v, 0, sizeof v);
  deriveKey();
  v.hmode = hmode;
  v.takes = takesUp;
  v.slotLayer = slotLayerOn;
  v.liveSafe = live;
  v.playing = playingFlag;
  v.melodyOn = P.melodyOn;
  v.tune = P.tune;
  v.spice = P.spice;
  v.density = (uint8_t)P.mel.density;
  v.range = (uint8_t)P.mel.range;
  v.voice = P.mel.voice;
  v.penta = P.mel.penta;
  v.rhythm = P.loop.rhythm;
  v.oct = P.oct;
  v.shape = P.shape;
  v.keyManual = K_manual;
  strlcpy(v.key, keyWord(), sizeof v.key);
  Sure su = sure(P.ns);
  v.surePct = (uint8_t)(K_manual ? 100 : su.pct);
  v.sureBars = (uint8_t)(K_manual ? 5 : su.bars);
  v.tie = !K_manual && su.tie;
  if (!K_manual) {
    if (su.tie && su.splitPc >= 0)
      snprintf(v.ask, sizeof v.ask, "%s OR %s?", noteName(su.splitPc, su.leader), noteName(su.splitAltPc, su.runner));
    else if (su.nEqual >= 3)
      snprintf(v.ask, sizeof v.ask, "5 NOTES: 2 LEFT OPEN");
    else if (su.tie)
      snprintf(v.ask, sizeof v.ask, "TWO ARE EQUALLY LIKELY");
    // two homes in one note set: no pitch test, drone each
    float Ps[12], H[12];
    posterior(P.ns, Ps);
    HomeObs o;
    combinedObs(o);
    homePosterior(o, Ps, H);
    int a = -1, b = -1;
    for (int pc = 0; pc < 12; pc++)
      if (a < 0 || H[pc] > H[a]) a = pc;
    for (int pc = 0; pc < 12; pc++)
      if (pc != a && (b < 0 || H[pc] > H[b])) b = pc;
    if (!v.ask[0] && a >= 0 && b >= 0 && H[a] > 0 && H[a] < 0.6f && H[a] < 2 * H[b])
      snprintf(v.ask, sizeof v.ask, "DRONE %s OR %s: WHICH RESTS?", noteName(a, K_coll), noteName(b, K_coll));
  }
  const hg::Loop& l = *playLoop();
  v.loopLen = l.len;
  v.playSlot = playingFlag ? playSlotNow : -1;
  v.selSlot = slotLayerOn && slotHeld >= 0 ? slotHeld : P.selSlot;
  v.cursor = hmode == HM_HUNT ? (int8_t)huntBar : P.selSlot;
  for (int i = 0; i < 16; i++) {
    const hg::Slot& s = l.s[i];
    v.slotFlags[i] = s.flags;
    if (!(s.flags & hg::HF_SET)) continue;
    slotName(s, v.slot[i], sizeof v.slot[i]);
  }
  if (printing || printArmed) snprintf(v.printing, sizeof v.printing, printArmed ? "PRINT: ARMED" : "PRINT %d/%d", printBars + 1, l.len);
  v.ab = abOnB ? 'B' : 'A';

  // the grid touched last: slots while the knob is held, else the sounds
  v.gridIsSlots = slotLayerOn;
  v.lastPad = lastPad;
  if (slotLayerOn) {
    for (int i = 0; i < 16; i++) {
      uint8_t f = l.s[i].flags;
      v.grid[i] = i >= l.len ? GC_OFF : (f & hg::HF_LOCK) ? GC_LOCK : (f & hg::HF_SET) ? GC_DIM : GC_OFF;
      if (i == v.playSlot) v.grid[i] = GC_PLAY;
      if (i == v.selSlot) v.grid[i] = GC_SEL;
      snprintf(v.pad[i], sizeof v.pad[i], "%d", i + 1);
    }
  } else {
    static const char* const SHN[4] = {"THIN", "FULL", "WIDE", "FLIP"};
    static const uint8_t SH[4] = {SH_THIN, SH_FULL, SH_WIDE, SH_FLIP};
    for (int i = 0; i < 4; i++) {
      strlcpy(v.pad[i], SHN[i], sizeof v.pad[i]);
      v.grid[i] = P.shape == SH[i] ? GC_SEL : GC_SHAPE;
    }
    Chord cur = lastChord;
    if (playingFlag && playSlotNow >= 0) {
      const Chord* c = chordAt(l, playSlotNow, clk::step() % 16, nullptr);
      if (c) cur = *c;
    }
    uint16_t curTones = cur.qual != Q_NONE ? chordTones(cur, K_home) : 0;
    if (cur.qual != Q_NONE) chordName(cur, K_home, K_coll, v.nowChord, sizeof v.nowChord);
    if (hmode == HM_HUNT) {
      int tryPc;
      bool pair;
      probeSuggestion(tryPc, pair);
      for (int pc = 0; pc < 12; pc++) {
        int p = pc + 4;
        snprintf(v.pad[p], sizeof v.pad[p], "%s%s", noteName(pc, K_coll), pair && pc == tryPc ? "5" : "");
        v.grid[p] = (clashMask & (1 << pc)) ? GC_CLASH : (fitMask & (1 << pc)) ? GC_FIT : pc == tryPc ? GC_TRY : GC_DIM;
      }
    } else if (hmode == HM_MELODY) {
      uint8_t pitch[12];
      hm::safePads(mctx(), pitch);
      for (int i = 0; i < 12; i++) {
        strlcpy(v.pad[i + 4], noteName(pitch[i], K_coll), sizeof v.pad[i + 4]);
        v.grid[i + 4] = (curTones & (1 << (pitch[i] % 12))) ? GC_STRONG : GC_DIM;
      }
    } else {
      Chord pads[12];
      int n = chordPads(K_home, K_coll, pads);
      for (int i = 0; i < 12; i++) {
        if (i >= n) continue;
        chordName(pads[i], K_home, K_coll, v.pad[i + 4], sizeof v.pad[i + 4]);
        hg::Ctx x = ctx();
        v.grid[i + 4] = pads[i] == cur ? GC_PLAY : hg::outside(pads[i], x) ? GC_TRY : GC_DIM;
      }
    }
    if (lastPad >= 0 && v.grid[lastPad] != GC_CLASH) v.grid[lastPad] = GC_SEL;
  }

  // takes
  {
    int idx[N_TAKES];
    int n = takesByAge(idx);
    v.takeN = (uint8_t)n;
    if (undoSerial >= 0)
      for (int i = 0; i < n; i++)
        if (T[idx[i]].serial == undoSerial) v.takeIdx = (uint8_t)i;
    v.takeSel = (int8_t)takeSel;
    int first = takeSel > 2 ? takeSel - 2 : 0;
    for (int r = 0; r < 5 && first + r < n; r++) {
      const Take& t = T[idx[first + r]];
      char line[28] = "";
      int k = snprintf(line, sizeof line, "%3d%s ", t.serial, first + r == 0 ? "*" : " ");
      for (int i = 0; i < t.loop.len && i < 4 && k < 24; i++) {
        char nm[8];
        Chord c = t.loop.s[i].c;
        if (!(t.loop.s[i].flags & hg::HF_SET)) strlcpy(nm, ".", sizeof nm);
        else chordName(c, t.home, collForMood(t.home, t.mood), nm, sizeof nm);
        k += snprintf(line + k, sizeof line - k, "%-4s", nm);
      }
      if (t.star) strlcat(line, " *", sizeof line);
      strlcpy(v.takeList[r], line, sizeof v.takeList[r]);
    }
    if (first + 0 < n) v.takeStar = T[idx[takeSel < n ? takeSel : 0]].star;
    v.takeSel = (int8_t)(takeSel - first);
  }

  // per mode: headline, lines, knobs
  auto setK = [&](const char* vl, const char* vt, const char* pl, const char* pt, const char* kl, const char* kt,
                  const char* ck) {
    strlcpy(v.valueLabel, vl, sizeof v.valueLabel);
    strlcpy(v.valueText, vt, sizeof v.valueText);
    strlcpy(v.pushLabel, pl, sizeof v.pushLabel);
    strlcpy(v.pushText, pt, sizeof v.pushText);
    strlcpy(v.k2Label, kl, sizeof v.k2Label);
    strlcpy(v.k2Text, kt, sizeof v.k2Text);
    strlcpy(v.clickText, ck, sizeof v.clickText);
  };
  char b1[12], b2[12], b3[12];
  switch (hmode) {
    case HM_HUNT: {
      v.sub = huntSub;
      int moveBar = clk::running() ? (int)((clk::step() / 16) % (l.len ? l.len : 4)) + 1 : 0;
      if (huntSub == HS_OFFERS)
        snprintf(v.title, sizeof v.title, "BAR %d  LOW NOTE %s", huntBar + 1, noteName(offers[0].lowAbs, K_coll));
      else if (huntSub >= HS_HOLD)
        snprintf(v.title, sizeof v.title, "HOLD TEST%s%s", holdPc >= 0 ? ": " : "", holdPc >= 0 ? noteName(holdPc, K_coll) : "");
      else if (moveBar)
        snprintf(v.title, sizeof v.title, "HUNT BAR %d/%d  (MOVE %d)", huntBar + 1, l.len ? l.len : 4, moveBar);
      else
        snprintf(v.title, sizeof v.title, "HUNT BAR %d/%d", huntBar + 1, l.len ? l.len : 4);
      if (huntSub == HS_OFFERS || huntSub >= HS_HOLD_AB) {
        v.nLines = (uint8_t)nOffers;
        for (int i = 0; i < nOffers; i++) {
          const Offer& o = offers[i];
          if (huntSub == HS_HOLD_HOME)
            snprintf(v.lines[i], sizeof v.lines[i], "%s IS HOME", noteName(o.rootAbs, K_coll));
          else if (o.rootAbs == o.lowAbs)
            snprintf(v.lines[i], sizeof v.lines[i], "%s %s", noteName(o.rootAbs, K_coll), qualWord(o.qual));
          else
            snprintf(v.lines[i], sizeof v.lines[i], "%s %s over %s", noteName(o.rootAbs, K_coll), qualWord(o.qual),
                     noteName(o.lowAbs, K_coll));
        }
        v.lineSel = (int8_t)offerSel;
      } else {                                         // the guesses, honestly
        float Ps[12], H[12];
        posterior(P.ns, Ps);
        HomeObs o;
        combinedObs(o);
        int order[12];
        for (int k = 0; k < 12; k++) order[k] = k;
        for (int a = 0; a < 12; a++)
          for (int b = a + 1; b < 12; b++)
            if (Ps[order[b]] > Ps[order[a]]) {
              int t = order[a];
              order[a] = order[b];
              order[b] = t;
            }
        for (int i = 0; i < 3; i++) {
          int k = order[i];
          float one[12] = {0};
          one[k] = 1;
          homePosterior(o, one, H);
          int h = -1;
          for (int pc = 0; pc < 12; pc++)
            if ((collMask(k) & (1 << pc)) && (h < 0 || H[pc] > H[h])) h = pc;
          float sum = 0;
          for (int pc = 0; pc < 12; pc++) sum += H[pc];
          if (sum <= 0) h = mod12(collTonic(k) + 9);
          int pct = (int)(Ps[k] * 100 + 0.5f);
          snprintf(v.lines[i], sizeof v.lines[i], "%s %s %d%%", noteName(h, k), modeWord(modeOf(h, k)), pct > 95 ? 95 : pct);
        }
        v.nLines = 3;
        v.lineSel = -1;
      }
      snprintf(b1, sizeof b1, "%d", P.oct);
      snprintf(b2, sizeof b2, "%+d C", P.tune);
      setK(huntSub == HS_OFFERS || huntSub >= HS_HOLD_AB ? "CHORD" : "OCTAVE",
           huntSub == HS_OFFERS || huntSub >= HS_HOLD_AB ? "" : b1, "", "", "TUNE", b2,
           huntSub >= HS_HOLD ? "HUNT" : "HOLD TEST");
      break;
    }
    case HM_LISTEN: {
      v.sub = lresValid ? 1 : 0;
      if (!lresValid) {
        snprintf(v.title, sizeof v.title, "LISTENING  %d NOTES", nev);
        strlcpy(v.lines[0], "PLAY THE LOOP INTO ME:", sizeof v.lines[0]);
        strlcpy(v.lines[1], "MOVE TRACK MIDI OUT, OR", sizeof v.lines[1]);
        strlcpy(v.lines[2], "A KEYBOARD ON USB", sizeof v.lines[2]);
        v.nLines = 3;
      } else {
        snprintf(v.title, sizeof v.title, "HEARD %d BARS  LOOP %d", lres.bars, lres.loop);
        int L = lres.loop < 4 ? lres.loop : 4;
        for (int row = 0; row < (lres.bassOnly ? 2 : 1); row++) {
          char line[28] = "";
          int k = snprintf(line, sizeof line, lres.bassOnly ? (row ? "TWIST " : "PLAIN ") : "");
          for (int i = 0; i < L && k < 26; i++) {
            const hl::BarChord& c = lres.bassOnly ? (row ? lres.twist[i] : lres.plain[i]) : lres.chord[i];
            char nm[8];
            if (c.root < 0) strlcpy(nm, "-", sizeof nm);
            else snprintf(nm, sizeof nm, "%s%s%s", noteName(c.root, lres.coll),
                          c.qual == Q_DOM ? "7" : qualSuffix(c.qual), (c.ext & hl::X_7) && c.qual != Q_DOM ? "7" : "");
            k += snprintf(line + k, sizeof line - k, "%-5s", nm);
          }
          strlcpy(v.lines[row], line, sizeof v.lines[row]);
        }
        v.nLines = lres.bassOnly ? 2 : 1;
        v.lineSel = lres.bassOnly ? (int8_t)listenChoice : -1;
      }
      snprintf(b1, sizeof b1, "%s", P.listenBassCh ? "" : "AUTO");
      if (P.listenBassCh) snprintf(b1, sizeof b1, "CH %d", P.listenBassCh);
      setK(lres.bassOnly ? "PLAIN/TWIST" : "", "", "BASS", b1, "TAP 1", "", "CLEAR");
      break;
    }
    case HM_GROW: {
      v.sub = growSub;
      if (growSub == GS_CANDS) {
        snprintf(v.title, sizeof v.title, "GROW %d>%d  SPICE %d/7", growBase.len, cand[0].len, P.spice);
        for (int i = 0; i < nCand; i++) {               // the last four slots, changes marked '
          char line[28] = "";
          int k = snprintf(line, sizeof line, "%c ", 'A' + i + candPage * 3);
          int m = growBase.len ? growBase.len : 4;
          for (int s = cand[i].len - 4; s < cand[i].len && k < 26; s++) {
            char nm[10];
            slotName(cand[i].s[s], nm, sizeof nm);
            bool changed = !(cand[i].s[s].c == growBase.s[s % m].c) || (cand[i].s[s].flags & hg::HF_SPLIT) ||
                           cand[i].s[s].low != growBase.s[s % m].low;
            k += snprintf(line + k, sizeof line - k, "%s%s ", nm, changed ? "'" : "");
          }
          strlcpy(v.lines[i], line, sizeof v.lines[i]);
        }
        v.nLines = (uint8_t)nCand;
        v.lineSel = (int8_t)candSel;
      } else if (growSub == GS_SKEL) {
        snprintf(v.title, sizeof v.title, "MOOD %d/%d", P.skel + 1, N_SKELETONS);
        for (int i = 0; i < 3; i++) {
          int k = (P.skel - 1 + i + N_SKELETONS) % N_SKELETONS;
          strlcpy(v.lines[i], SKELETONS[k].mood, sizeof v.lines[i]);
        }
        v.nLines = 3;
        v.lineSel = 1;
      } else {
        snprintf(v.title, sizeof v.title, "GROW  SPICE %d/7", P.spice);
        static const char* const SP[8] = {"exactly what you entered", "shape only", "shape, early rhythm",
                                          "way back at the ends", "+ third swaps, splits", "+ one outside chord",
                                          "+ one shape, many basses", "+ slide shapes"};
        strlcpy(v.lines[0], SP[P.spice & 7], sizeof v.lines[0]);
        strlcpy(v.lines[1], "NO: 3 NEW LOOPS", sizeof v.lines[1]);
        strlcpy(v.lines[2], "PUSH+TURN: A MOOD", sizeof v.lines[2]);
        v.nLines = 3;
        v.lineSel = -1;
      }
      snprintf(b1, sizeof b1, "%d/7", P.spice);
      setK("SPICE", b1, "MOOD", SKELETONS[P.skel].mood, "PICK", growSub == GS_CANDS ? "A B C" : "", playingFlag ? "STOP" : "PLAY");
      break;
    }
    case HM_MELODY: {
      v.sub = tapRec ? 2 : tapArmed ? 1 : 0;
      snprintf(v.title, sizeof v.title, "MELODY  %s", hm::voiceWord(P.mel.voice));
      strlcpy(v.lines[0], tapRec ? "TAPPING... ANY PAD" : tapArmed ? "TAP FROM THE NEXT BAR" : P.line.n ? "NO: ANOTHER LINE" : "CLICK: TAP A RHYTHM",
              sizeof v.lines[0]);
      snprintf(v.lines[1], sizeof v.lines[1], "%s  %d NOTES  %s", P.mel.penta ? "5 NOTES" : "7 NOTES", P.line.n,
               P.melodyOn ? "ON" : "OFF");
      v.nLines = 2;
      v.lineSel = -1;
      snprintf(b1, sizeof b1, "%d", P.mel.density);
      snprintf(b2, sizeof b2, "%d", P.mel.range);
      setK("DENSITY", b1, "RANGE", b2, "VOICE", hm::voiceWord(P.mel.voice), "TAP RHYTHM");
      break;
    }
    default: {
      v.sub = 0;
      int filled = 0;
      for (int i = 0; i < l.len; i++) filled += (l.s[i].flags & hg::HF_SET) ? 1 : 0;
      snprintf(v.title, sizeof v.title, "PLAY  %d/%d SLOTS", filled, l.len ? l.len : 4);
      strlcpy(v.lines[0], lastChord.qual != Q_NONE ? "YES: INTO THE NEXT SLOT" : "A PAD, THEN YES", sizeof v.lines[0]);
      v.nLines = 1;
      v.lineSel = -1;
      const char* rh = P.loop.rhythm < N_STABS ? STABS[P.loop.rhythm].name : "OUT OF WAY";
      snprintf(b3, sizeof b3, "%s", noteName(K_home, K_coll));
      setK("SHAPE", shapeWord(P.shape), "RHYTHM", rh, "HOME", b3, playingFlag ? "STOP" : "PLAY");
      break;
    }
  }
  if (slotLayerOn) {
    if (slotHeld >= 0) setK("SUGGEST", "", "LENGTH", "", v.k2Label, v.k2Text, v.clickText);
    else {
      snprintf(b1, sizeof b1, "%d", l.len);
      setK("", "", "LENGTH", b1, v.k2Label, v.k2Text, v.clickText);
    }
  }
}

void printState(Print& p) {
  deriveKey();
  Sure su = sure(P.ns);
  p.printf("harmony: mode %d  key %s (%s, sure %d%%)  tune %+d c  spice %d  loop %d slots  %s\n", hmode, keyWord(),
           K_manual ? "by hand" : "inferred", su.pct, P.tune, P.spice, P.loop.len, playingFlag ? "PLAYING" : "stopped");
  for (int i = 0; i < P.loop.len; i++) {
    char a[12], r[12];
    chordName(P.loop.s[i].c, K_home, K_coll, a, sizeof a);
    chordRoman(P.loop.s[i].c, r, sizeof r);
    p.printf("  %2d %-6s %-7s %s%s%s\n", i + 1, (P.loop.s[i].flags & hg::HF_SET) ? a : ".", r,
             shapeWord(P.loop.s[i].shape), (P.loop.s[i].flags & hg::HF_LOCK) ? " LOCK" : "",
             (P.loop.s[i].flags & hg::HF_PUSH) ? " EARLY" : "");
  }
  int idx[N_TAKES];
  p.printf("takes: %d   melody: %d notes %s on ch %d\n", takesByAge(idx), P.line.n, P.melodyOn ? "on" : "off", P.melCh + 1);
}

}  // namespace harm
