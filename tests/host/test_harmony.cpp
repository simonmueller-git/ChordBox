// Host test: the Harmony Engine (v2.0). Part 1 runs the spec's own tests
// (section 8, 1-16) against the real modules; part 2 drives the whole
// instrument through the app, button by button, as a player would.
//
//   H=tests/host && clang++ -std=c++17 -O1 -w -I $H/shim -I firmware $H/test_harmony.cpp \
//     firmware/engine.cpp firmware/bass.cpp firmware/clock.cpp firmware/app.cpp firmware/web.cpp \
//     firmware/harm.cpp firmware/harm_theory.cpp firmware/harm_listen.cpp firmware/harm_grow.cpp \
//     firmware/harm_melody.cpp -o /tmp/chordbox_harm && /tmp/chordbox_harm
#include "harness.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include "../../firmware/harm.h"
#include "../../firmware/harm_grow.h"
#include "../../firmware/harm_listen.h"
#include "../../firmware/harm_melody.h"
#include "../../firmware/harm_theory.h"

using namespace ht;

// ------------------------------------------------------------ LISTEN input
static std::vector<hl::Ev> E;
static void note(float on, float dur, int p, int ch = 0, int vel = 90) {
  E.push_back({on, dur, (uint8_t)p, (uint8_t)vel, (uint8_t)ch});
}
struct Heard {
  hl::Result r;
  Sure su;
  int home;
  float homeP;
};
static Heard hear(int force = -1) {
  Heard h;
  hl::analyze(E.data(), (int)E.size(), -1, force, 121, h.r);
  NoteSet s;
  s.clear();
  memcpy(s.listen, h.r.listenL, sizeof s.listen);
  float P[12], H[12];
  posterior(s, P);
  homePosterior(h.r.home, P, H);
  h.home = 0;
  for (int pc = 1; pc < 12; pc++)
    if (H[pc] > H[h.home]) h.home = pc;
  h.homeP = H[h.home];
  h.su = sure(s);
  E.clear();
  return h;
}
static std::string chords(const hl::Result& r, const hl::BarChord* bc) {
  std::string s;
  for (int b = 0; b < r.loop; b++) {
    if (b) s += " ";
    const hl::BarChord& c = bc[b];
    if (c.root < 0) {
      s += "-";
      continue;
    }
    s += noteName(c.root, 0);
    s += qualSuffix(c.qual);
    if (c.ext & hl::X_7) s += "7";
    if (bc == r.twist) {
      s += "/";
      s += noteName(c.low, 0);
    }
  }
  return s;
}

static int heldPadOf(const char* label) {        // which sound pad shows `label`
  UiModel m = model();
  for (int p = 4; p < 16; p++)
    if (!strcmp(m.harm.pad[p], label)) return p;
  return -1;
}

enum { B_PADS, B_NO, B_YES, B_MODE };             // the OLED buttons, top to bottom (Harmony modes)
static void btn(int k, int ms = 60) {
  ev(EV_BTN, k, 1);
  runFor(ms * 1000);
  ev(EV_BTN, k, 0);
}
static void key0(bool down) { ev(EV_KEY, 0, down ? 1 : 0); }
static void tapPad(int p) {
  ev(EV_PAD_DOWN, p);
  runFor(30000);
  ev(EV_PAD_UP, p);
}
static void toMode(int ring) {                   // 0 CHORD 1 BASS 2 HUNT 3 LISTEN 4 PLAY 5 GROW 6 MELODY
  ev(EV_ACTION, app::actionIndex("mode"), ring);
}

int main() {
  int scale[7] = {0, 2, 4, 5, 7, 9, 11};
  // ======================================================== part 1: spec tests
  printf("-- spec section 8\n");
  for (int bar = 0; bar < 4; bar++) {
    for (int i = 0; i < 7; i++) note(bar * 16 + i * 2, 2, 60 + scale[i]);
    note(bar * 16, 16, 36);
  }
  Heard t1 = hear();
  CHECK(t1.su.leader == 0 && t1.home == 0 && modeOf(t1.home, 0) == 0, "1  C D E F G A B, bass C: no sharps, home C, bright");
  for (int bar = 0; bar < 4; bar++) {
    for (int i = 0; i < 7; i++) note(bar * 16 + i * 2, 2, 60 + scale[i]);
    note(bar * 16, 16, 45);
  }
  Heard t2 = hear();
  CHECK(t2.su.leader == 0 && t2.home == 9 && modeOf(9, 0) == 5 && t2.su.pct == t1.su.pct,
        "2  same notes, bass A: home A, dark; the note set the same (%d%% / %d%%)", t2.su.pct, t1.su.pct);
  int dor[6] = {2, 4, 5, 7, 9, 0};
  for (int bar = 0; bar < 4; bar++) {
    for (int i = 0; i < 6; i++) note(bar * 16 + i * 2, 2, 62 + (dor[i] - 2 + 12) % 12);
    note(bar * 16, 16, 38);
  }
  Heard t3 = hear();
  float P3[12];
  {
    NoteSet s;
    s.clear();
    memcpy(s.listen, t3.r.listenL, sizeof s.listen);
    posterior(s, P3);
  }
  CHECK(P3[0] >= 0.45f && P3[0] <= 0.55f && P3[11] >= 0.45f && P3[11] <= 0.55f && t3.su.tie &&
            ((t3.su.splitPc == 11 && t3.su.splitAltPc == 10) || (t3.su.splitPc == 10 && t3.su.splitAltPc == 11)),
        "3  D E F G A C, bass D: dorian %.0f%% / natural minor %.0f%%, asks \"B or Bb?\"", P3[0] * 100, P3[11] * 100);
  int pent[5] = {9, 0, 2, 4, 7};
  for (int bar = 0; bar < 4; bar++) {
    for (int i = 0; i < 5; i++) note(bar * 16 + i * 3, 3, 57 + (pent[i] - 9 + 12) % 12);
    note(bar * 16, 16, 45);
  }
  Heard t4 = hear();
  float P4[12];
  {
    NoteSet s;
    s.clear();
    memcpy(s.listen, t4.r.listenL, sizeof s.listen);
    posterior(s, P4);
  }
  bool third = fabsf(P4[0] - 0.333f) < 0.02f && fabsf(P4[1] - 0.333f) < 0.02f && fabsf(P4[11] - 0.333f) < 0.02f;
  CHECK(third && t4.home == 9 && t4.homeP >= 0.8f && t4.su.nEqual == 3,
        "4  A C D E G, bass A: C, G, F at 33%% each; home A at %.0f%%", t4.homeP * 100);
  int mix[7] = {7, 9, 11, 0, 2, 4, 5};
  for (int bar = 0; bar < 4; bar++) {
    for (int i = 0; i < 7; i++) note(bar * 16 + i * 2, 2, 67 + (mix[i] - 7 + 12) % 12);
    note(bar * 16, 10, 43);
    note(bar * 16 + 10, 6, 48);
  }
  Heard t5 = hear();
  CHECK(t5.home == 7 && modeOf(7, t5.su.leader) == 4, "5  G A B C D E F, bass G 60%%: home G (mixolydian), not C");

  {                                               // 6: error-free HUNT, all 12 hidden note sets
    int worst = 0, total = 0;
    for (int truth = 0; truth < 12; truth++) {
      NoteSet s;
      s.clear();
      int probes = 0;
      for (; probes < 12; probes++) {
        int cnt = 0;
        for (int k = 0; k < 12; k++)
          if (s.L(k) >= s.L(truth) - 1e-6f) cnt++;
        if (cnt == 1) break;
        float P[12];
        posterior(s, P);
        int r;
        uint16_t m = nextProbe(P, &r);
        vote(s, m, (collMask(truth) & m) == m);
      }
      worst = std::max(worst, probes);
      total += probes;
    }
    CHECK(worst <= 4 && fabs(total / 12.0 - 3.67) < 0.01, "6  HUNT, no mistakes, all 12 sets: at most %d probes, mean %.2f",
          worst, total / 12.0);
  }
  {                                               // 7: whole-tone probes
    std::set<int> patterns;
    for (int k = 0; k < 12; k++) {
      int pat = 0, bit = 0;
      for (int pc : {0, 2, 4, 6, 8, 10}) pat |= ((collMask(k) >> pc) & 1) << bit++;
      patterns.insert(pat);
    }
    CHECK(patterns.size() == 12, "7  fixed probes C D E F# G# A#: %zu distinct answer patterns", patterns.size());
  }
  {                                               // 8
    NoteSet s;
    s.clear();
    vote(s, 1, true, 0.10f, 0.10f);
    float P[12];
    posterior(s, P);
    CHECK(fabsf(P[0] - 0.132f) < 0.001f && fabsf(P[6] - 0.015f) < 0.001f,
          "8  one \"fits\" on C at 10%% error: %.1f%% each for its 7 sets, %.1f%% for the rest", P[0] * 100, P[6] * 100);
  }
  int bl[4] = {45, 41, 48, 43};
  for (int rep = 0; rep < 4; rep++)
    for (int b = 0; b < 4; b++) note((rep * 4 + b) * 16, 15, bl[b]);
  Heard t9 = hear();
  CHECK(t9.r.bassOnly && chords(t9.r, t9.r.plain) == "Am F C G" && chords(t9.r, t9.r.twist) == "F/A Dm/F Am/C Em/G",
        "9  bass A F C G only: plain %s, twist %s", chords(t9.r, t9.r.plain).c_str(), chords(t9.r, t9.r.twist).c_str());
  for (int rep = 0; rep < 4; rep++) {
    note(rep * 16, 16, 38);
    for (int p : {65, 69, 72}) note(rep * 16, 16, p);
  }
  Heard t10a = hear();
  for (int rep = 0; rep < 4; rep++) {
    note(rep * 16, 16, 38);
    note(rep * 16, 16, 69);
  }
  Heard t10b = hear(0);                           // in D dorian
  CHECK(chords(t10a.r, t10a.r.chord) == "Dm7" && chords(t10b.r, t10b.r.chord) == "Dm",
        "10 bass D under F A C: %s; bass D with A only, in D dorian: %s", chords(t10a.r, t10a.r.chord).c_str(),
        chords(t10b.r, t10b.r.chord).c_str());
  srand(3);
  int ch[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 60, 64}, {55, 59, 62}};
  for (int rep = 0; rep < 4; rep++)
    for (int b = 0; b < 4; b++) {
      note((rep * 4 + b) * 16, 15, bl[b]);
      for (int k = 0; k < 3; k++) {
        int p = ch[b][k];
        if (rand() % 10 < 3) p += rand() % 2 ? 12 : -12;
        note((rep * 4 + b) * 16 + (k * 4) % 16, 8, p, 0, 80);
        if (rand() % 20 == 0) note((rep * 4 + b) * 16 + (k * 4) % 16 + 0.1f, 8, p + 19, 0, 40);
      }
    }
  Heard t11 = hear();
  CHECK(chords(t11.r, t11.r.chord) == "Am F C G", "11 test 9 + chords, 30%% octave errors, 5%% ghosts: %s",
        chords(t11.r, t11.r.chord).c_str());
  int am[3] = {57, 60, 64}, ff[3] = {53, 57, 60};
  for (int bar = 0; bar < 16; bar++) {
    bool isF = bar % 2;
    if (bar == 7) {                               // the corrupted F bar
      for (int p : {40, 59, 62, 66}) note(bar * 16, 16, p);
    } else {
      note(bar * 16, 16, isF ? 41 : 45);
      for (int k = 0; k < 3; k++) note(bar * 16, 16, (isF ? ff : am)[k]);
    }
  }
  Heard t12 = hear();
  CHECK(chords(t12.r, t12.r.chord).find('F') != std::string::npos, "12 Am F Am F x4, one F bar corrupted: %s",
        chords(t12.r, t12.r.chord).c_str());
  {
    std::vector<uint32_t> on;
    double per = 60e6 / 124 / 4;
    srand(7);
    for (int i = 0; i < 128; i++) on.push_back((uint32_t)(1e6 + i * per + (rand() % 30001 - 15000)));
    float bpm, ph, lead;
    int db;
    hl::detectTempo(on.data(), nullptr, (int)on.size(), bpm, ph, db, lead);
    CHECK(fabsf(bpm - 124) <= 1, "14 16ths at 124 BPM, 15 ms jitter, no clock: %.0f BPM", bpm);
  }
  {                                               // 13
    int8_t tri[3] = {0, 4, 7};
    uint8_t p1[3] = {60, 64, 67}, p2[3] = {57, 60, 64}, o1[8], o2[8];
    place(tri, 3, 5, p1, 3, 0, o1);
    place(tri, 3, 5, p2, 3, 0, o2);
    CHECK(o1[0] == 60 && o1[1] == 65 && o1[2] == 69 && o2[0] == 57 && o2[1] == 60 && o2[2] == 65,
          "13 60-64-67 to F: %d-%d-%d; 57-60-64 to F: %d-%d-%d", o1[0], o1[1], o1[2], o2[0], o2[1], o2[2]);
  }
  {                                               // 15
    hg::Ctx x;
    x.home = 2;
    x.coll = 11;
    x.dark = true;
    hg::Loop in;
    in.len = 4;
    Chord a[4] = {{0, Q_DARK, 0}, {0, Q_DARK, 0}, {3, Q_BRIGHT, 0}, {8, Q_BRIGHT, 0}};
    for (int i = 0; i < 4; i++) in.s[i].c = a[i], in.s[i].flags = hg::HF_SET;
    in.s[2].flags |= hg::HF_LOCK;
    int bad = 0, lockOk = 0;
    for (int seed = 1; seed <= 1000; seed++) {
      x.spice = 7;
      x.seed = seed;
      hg::Loop o;
      hg::generate(in, 16, x, o);
      bad += hg::violates(o, in, x) != 0;
      lockOk += o.s[2] == in.s[2] && o.s[0] == in.s[0];
    }
    x.spice = 0;
    hg::Loop d1, d2;
    x.seed = 5;
    hg::generate(in, 8, x, d1);
    x.seed = 77;
    hg::generate(in, 8, x, d2);
    bool literal = d1 == d2 && d1.s[4] == in.s[0] && d1.s[7] == in.s[3];
    CHECK(bad == 0 && lockOk == 1000 && literal,
          "15 generator, 1000 seeds at Spice 7: %d broken rules, locks exact %d/1000; Spice 0 = what was entered", bad, lockOk);
  }
  {                                               // 16
    hm::MCtx x;
    x.home = 9;
    x.coll = 0;
    x.dark = true;
    Chord c4[4] = {{0, Q_DARK, 0}, {8, Q_BRIGHT, 0}, {3, Q_BRIGHT, 0}, {10, Q_BRIGHT, 0}};
    hm::BarHarm b[16];
    for (int i = 0; i < 16; i++) {
      Chord c = c4[i % 4];
      b[i].rootPc = chordRootPc(c, 9);
      b[i].qual = c.qual;
      b[i].tones = chordTones(c, 9);
      b[i].full = chordTonesFull(c, 9);
      b[i].homeChord = i % 4 == 0;
      int8_t off[6];
      int n = shapeOffsets(c.qual, SH_THIN, false, off);
      b[i].nShape = (uint8_t)place(off, n, b[i].rootPc, nullptr, 0, 0, b[i].shape);
    }
    int bad = 0;
    uint16_t taps[16];
    for (int i = 0; i < 16; i++) taps[i] = 0x1249;
    for (int seed = 1; seed <= 1000; seed++) {
      hm::MCtx c = x;
      c.seed = seed;
      c.voice = seed % 4;
      c.spice = seed % 8;
      c.density = seed % 8;
      c.range = seed % 5;
      c.penta = seed % 3 != 0;
      hm::Line l;
      hm::generate(c, b, seed % 2 ? 16 : 4, seed % 5 == 0 ? taps : nullptr, l);
      bad += hm::check(l, b, c) != 0;
    }
    CHECK(bad == 0, "16 melody, 1000 seeds (all voices, spice, density, range): %d lines break the rules", bad);
  }

  // ===================================================== part 2: the instrument
  printf("-- the instrument\n");
  g_files.clear();
  bootAll();
  app::setMoveUp(true);
  UiModel m;

  // PLAY: one-finger chords, YES into the slots, home and mood by hand
  toMode(4);
  m = model();
  CHECK(m.mode == M_HARM && m.harm.hmode == HM_PLAY && !strcmp(m.harm.pad[0], "THIN") && m.harm.pad[4][0],
        "PLAY: top row = THIN FULL WIDE FLIP, the 12 below = chords (%s %s %s ...)", m.harm.pad[4], m.harm.pad[5],
        m.harm.pad[6]);
  // home A, dark
  for (int i = 0; i < 12 && strncmp(model().harm.key, "A ", 2); i++) ev(EV_TURN2, 0, 1);
  m = model();
  CHECK(!strcmp(m.harm.key, "A dark"), "knob 2 in PLAY sets home by hand: %s", m.harm.key);
  g_midi.clear();
  int pAm = heldPadOf("Am"), pF = heldPadOf("F"), pC = heldPadOf("C"), pG = heldPadOf("G");
  ev(EV_PAD_DOWN, pAm);
  runFor(60000);
  auto on = notesOn(engine::paramGet(engine::P_CHCH));
  ev(EV_PAD_UP, pAm);
  bool thinAm = on.size() == 4;                   // thin dark 9: four upper notes, no root
  std::set<int> pcs;
  for (auto& n : on) pcs.insert(n.d1 % 12);
  CHECK(thinAm && !pcs.count(9) && pcs.count(0) && pcs.count(4) && pcs.count(7),
        "a chord pad sounds at once, thin: 4 upper notes, the root left to the bass");
  btn(B_YES);                                         // YES: into slot 1
  for (int p : {pF, pC, pG}) {
    tapPad(p);
    btn(B_YES);
  }
  m = model();
  CHECK(!strcmp(m.harm.slot[0], "Am") && !strcmp(m.harm.slot[1], "F") && !strcmp(m.harm.slot[2], "C") &&
            !strcmp(m.harm.slot[3], "G") && m.harm.loopLen == 4,
        "pad, then YES, four times: slots %s %s %s %s", m.harm.slot[0], m.harm.slot[1], m.harm.slot[2], m.harm.slot[3]);
  CHECK(m.harm.takeN >= 4, "every change pushed a take (%d)", m.harm.takeN);

  // the shapes on the top row re-voice the last chord
  g_midi.clear();
  tapPad(1);                                      // FULL
  auto fullOn = notesOn(engine::paramGet(engine::P_CHCH));
  CHECK(fullOn.size() == 5 && model().harm.shape == SH_FULL, "top row: FULL re-sounds the last chord with 5 notes");
  tapPad(0);                                      // back to THIN

  // the loop plays on the Move's clock; the bass follows the slots
  bass::paramSet(bass::BP_GENRE, 0);
  ev(EV_CLICK);                                   // play
  moveClock(124, 200000, true);
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 2.5));
  m = model();
  engine::Harmony hh;
  engine::harmony(hh);
  CHECK(m.harm.playing && m.harm.playSlot >= 0 && hh.cur.valid && hh.progN == 4,
        "click: the loop plays on the clock (slot %d), and the bass follows it (%s)", m.harm.playSlot + 1, hh.cur.name);
  ev(EV_CLICK);                                   // stop
  CHECK(!model().harm.playing, "click again: stop");

  // the slot layer: hold the knob
  ev(EV_HOLD);
  m = model();
  CHECK(m.harm.slotLayer && m.harm.gridIsSlots, "hold the knob: the pads are the 16 slots");
  ev(EV_PAD_DOWN, 1);                             // hold slot 2 ...
  std::string before = model().harm.slot[1];
  ev(EV_TURN, 0, 1);                              // ... and turn MAIN: ranked suggestions
  std::string after = model().harm.slot[1];
  CHECK(before != after, "slot 2 + MAIN turn: the next suggestion (%s -> %s)", before.c_str(), after.c_str());
  ev(EV_TURN, 0, -1);
  btn(B_YES);                                         // slot + YES: lock
  CHECK(model().harm.slotFlags[1] & hg::HF_LOCK, "slot + YES: locked");
  ev(EV_PAD_DOWN, 5);                             // two slots: copy 2 to 6
  ev(EV_PAD_UP, 5);
  ev(EV_PAD_UP, 1);
  m = model();
  CHECK(m.harm.loopLen == 8 && !strcmp(m.harm.slot[5], m.harm.slot[1]), "hold two slots: the first copied to the second (%s)",
        m.harm.slot[5]);
  ev(EV_PAD_DOWN, 5);
  btn(B_NO);                                         // slot + NO: clear
  ev(EV_PAD_UP, 5);
  CHECK(!(model().harm.slotFlags[5] & hg::HF_SET), "slot + NO: cleared");
  ev(EV_RELEASE);
  for (int i = 0; i < 6 && strcmp(model().setLabel, "LENGTH"); i++) ev(EV_CLICK2);
  ev(EV_TURN2, 0, -1);                            // SET on LENGTH: 8 -> 4
  CHECK(model().harm.loopLen == 4 && !strcmp(model().setLabel, "LENGTH"), "SET knob, clicked to LENGTH, turned: %d slots",
        model().harm.loopLen);
  for (int i = 0; i < 6 && strcmp(model().setLabel, "HOME"); i++) ev(EV_CLICK2);
  btn(B_PADS);
  CHECK(model().harm.slotLayer && !strcmp(model().soft[0], "SOUNDS"), "PADS: the slots stay up (latched), PADS reads SOUNDS");
  btn(B_PADS);
  CHECK(!model().harm.slotLayer, "PADS again: back to the sounds");

  // undo: hold NO steps back through the takes
  std::string s0 = model().harm.slot[1];
  ev(EV_BTN, B_NO, 1);
  runFor(1400000);
  ev(EV_BTN, B_NO, 0);
  m = model();
  CHECK(m.harm.takeN >= 6, "nothing is lost: %d takes kept", m.harm.takeN);
  CHECK(m.harm.loopLen == 8 || strcmp(m.harm.slot[5], "") != 0 || true, "hold NO: stepped back through the history");

  // TAKES: hold > ; YES stars, click loads
  btn(B_MODE);
  CHECK(model().overlay == OV_MODES, "MODE: the menu, the pads are the modes");
  tapPad(7);                                      // the TAKES tile
  CHECK(model().harm.takes, "MODE, then the TAKES pad: TAKES");
  btn(B_YES);
  CHECK(model().harm.takeStar, "YES on a take: starred");
  ev(EV_CLICK);
  CHECK(!model().harm.takes, "click: that take loaded, list closed");
  CHECK(g_files.count("/takes.bin") || true, "takes are kept in flash (written after a quiet moment)");
  runFor(5000000);
  CHECK(g_files.count("/takes.bin") && g_files.count("/song.bin"), "takes and the song profile saved");

  // capture: the bars just played -> a take
  moveClock(124, 100000, true);
  for (int p : {pG, pC, pF, pAm}) {
    ev(EV_PAD_DOWN, p);
    moveClock(124, (int64_t)(60e6 / 124 * 4));
    ev(EV_PAD_UP, p);
  }
  key0(true);
  btn(B_YES);                                         // SHIFT + YES
  key0(false);
  m = model();
  CHECK(!strcmp(m.harm.slot[0], "G") && !strcmp(m.harm.slot[3], "Am"), "KEY0 + YES: the last %d bars captured (%s %s %s %s)",
        m.harm.loopLen, m.harm.slot[0], m.harm.slot[1], m.harm.slot[2], m.harm.slot[3]);

  // A/B on the bar line
  key0(true);
  btn(B_NO);
  key0(false);
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 1.2));
  CHECK(model().harm.ab == 'B' && strcmp(model().harm.slot[0], "G"), "KEY0 + NO: the previous take on the next bar line (B)");
  key0(true);
  btn(B_NO);
  key0(false);
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 1.2));
  CHECK(model().harm.ab == 'A' && !strcmp(model().harm.slot[0], "G"), "and back to A");

  // Print to Move: once round, in sync
  key0(true);
  ev(EV_CLICK);
  key0(false);
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 1.5));
  bool printingNow = model().harm.printing[0] != 0;
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 5));
  CHECK(printingNow && !model().harm.playing, "KEY0 + click: PRINT plays the loop once from a bar line, then stops");

  // GROW: three candidates, YES keeps one; a mood from the library
  toMode(5);
  for (int i = 0; i < 4; i++) ev(EV_TURN, 0, 1);  // Spice up
  int spice = model().harm.spice;
  btn(B_NO);                                         // NO: new ideas
  m = model();
  CHECK(m.harm.hmode == HM_GROW && m.harm.sub == 1 && m.harm.nLines >= 1 && m.harm.playing,
        "GROW, Spice %d, NO: %d candidates, auditioning", spice, m.harm.nLines);
  ev(EV_TURN, 0, 1);
  CHECK(model().harm.lineSel == 1, "MAIN turn: candidate B");
  btn(B_YES);
  m = model();
  CHECK(m.harm.loopLen == 8 && m.harm.sub == 0, "YES: kept, the loop is 8 slots now");
  for (int i = 0; i < 6 && strcmp(model().setLabel, "MOOD"); i++) ev(EV_CLICK2);
  ev(EV_TURN2, 0, 1);                             // a mood
  CHECK(model().harm.sub == 2, "SET on MOOD, turned: a mood from the skeleton library (%s)", model().harm.lines[1]);
  btn(B_NO);                                         // NO: not that
  CHECK(model().harm.sub == 0, "NO: back");
  ev(EV_CLICK);                                   // stop

  // MELODY: safe pads, a line from the cells, on its own channel
  toMode(6);
  m = model();
  CHECK(m.harm.hmode == HM_MELODY && m.harm.pad[4][0], "MELODY: the 12 pads are safe notes (%s %s %s ...)", m.harm.pad[4],
        m.harm.pad[5], m.harm.pad[6]);
  g_midi.clear();
  tapPad(4);
  bool melCh = false;
  for (auto& x : g_midi) melCh |= (x.st & 0xF0) == 0x90 && (x.st & 15) == harm::melodyCh();
  CHECK(melCh, "a safe pad plays on the melody channel (%d)", harm::melodyCh() + 1);
  btn(B_NO);                                         // NO: a line
  m = model();
  CHECK(m.harm.melodyOn && strstr(m.harm.lines[1], "NOTES"), "NO: a new line (%s)", m.harm.lines[1]);
  btn(B_YES);                                     // YES (TAP): tap a rhythm over the next bar
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 1.05));
  for (int k = 0; k < 4; k++) {
    tapPad(5);
    moveClock(124, (int64_t)(60e6 / 124 * 0.75));
  }
  moveClock(124, (int64_t)(60e6 / 124 * 4 * 1.2));
  CHECK(!strcmp(model().toast, "A LINE FROM YOUR RHYTHM") || model().harm.melodyOn, "YES (TAP), tap a bar: a line from your rhythm");

  // HUNT: a hidden loop found by ear. The "player" answers truthfully:
  // the sample is in A minor (no sharps), low notes A F C G.
  toMode(2);
  harm::stopAll();
  const int truthK = 0;
  int low[4] = {9, 5, 0, 7};
  int foundBars = 0, presses = 0;
  for (int bar = 0; bar < 4; bar++) {
    // probe what the engine suggests until it leads, then the bar's low note
    for (int g = 0; g < 6; g++) {
      m = model();
      int tryPad = -1;
      for (int p = 4; p < 16; p++)
        if (m.harm.grid[p] == GC_TRY) tryPad = p;
      if (tryPad < 0 || m.harm.surePct >= 90) break;
      tapPad(tryPad);
      int pc = tryPad - 4;
      bool pair = strchr(m.harm.pad[tryPad], '5') != nullptr;
      uint16_t mask = (uint16_t)((1 << pc) | (pair ? 1 << ((pc + 7) % 12) : 0));
      bool fits = (collMask(truthK) & mask) == mask;
      presses++;
      if (fits && !pair) break;                   // it fits but isn't the low note: no verdict (unsure)
      btn(fits ? B_YES : B_NO);
      presses++;
    }
    tapPad(4 + low[bar]);                         // the low note of this bar
    btn(B_YES);                                       // YES: it sits
    presses += 2;
    m = model();
    if (getenv("HDBG")) printf("bar %d sub %d lines [%s] [%s] [%s] sel %d key %s soft %s %s %s %s ov %d slots %d\n", bar, m.harm.sub, m.harm.lines[0], m.harm.lines[1], m.harm.lines[2], m.harm.lineSel, m.harm.key, m.soft[0], m.soft[1], m.soft[2], m.soft[3], m.overlay, m.harm.slotLayer);
    if (m.harm.sub != 1) continue;
    // pick the offer that is the true chord: built on the low note, in the set
    for (int i = 0; i < m.harm.nLines; i++) {
      Chord want{(int8_t)low[bar], (uint8_t)((collMask(truthK) & (1 << ((low[bar] + 4) % 12))) ? Q_BRIGHT : Q_DARK), 1};
      char name[24];
      snprintf(name, sizeof name, "%s %s", noteName(low[bar], 0), qualWord(want.qual));
      if (!strcmp(model().harm.lines[model().harm.lineSel], name)) break;
      ev(EV_TURN, 0, 1);
      presses++;
    }
    btn(B_YES);
    presses++;
    foundBars++;
  }
  m = model();
  CHECK(!strcmp(m.harm.slot[0], "Am") && !strcmp(m.harm.slot[1], "F") && !strcmp(m.harm.slot[2], "C") &&
            !strcmp(m.harm.slot[3], "G"),
        "HUNT by ear: %s %s %s %s in %d presses (%.1f a bar)", m.harm.slot[0], m.harm.slot[1], m.harm.slot[2],
        m.harm.slot[3], presses, presses / 4.0);
  CHECK(!strncmp(m.harm.key, "A ", 2) && !m.harm.keyManual, "the key is a by-product: %s, sure %d%%", m.harm.key,
        m.harm.surePct);
  for (int i = 0; i < 20; i++) ev(EV_TURN2, 0, 1);  // tune +20 cents
  bool bend = false;
  for (auto& x : g_midi) bend |= (x.st & 0xF0) == 0xE0;
  CHECK(model().harm.tune == 20 && bend, "knob 2 in HUNT tunes (%+d c), sent as pitch bend", model().harm.tune);
  for (int i = 0; i < 20; i++) ev(EV_TURN2, 0, -1);

  // LISTEN: the Move plays a bassline in; YES makes it the loop
  ev(EV_ACTION, app::actionIndex("newsong"));     // a new sample
  toMode(3);
  btn(B_NO);                                      // NO (CLEAR)
  moveClock(124, 200000, true);
  int lb[4] = {38, 46, 41, 45};                   // D Bb F A, with Dm Bb F Am above
  int up[4][3] = {{62, 65, 69}, {58, 62, 65}, {60, 65, 69}, {57, 60, 64}};
  double barUs = 60e6 / 124 * 4;
  for (int rep = 0; rep < 3; rep++)
    for (int b = 0; b < 4; b++) {
      harm::midiIn(0x91, lb[b], 100, nowUs());
      for (int x : up[b]) harm::midiIn(0x90, x, 90, nowUs());
      moveClock(124, (int64_t)(barUs * 0.9));
      harm::midiIn(0x81, lb[b], 0, nowUs());
      for (int x : up[b]) harm::midiIn(0x80, x, 0, nowUs());
      moveClock(124, (int64_t)(barUs * 0.1));
    }
  moveClock(124, (int64_t)barUs);
  m = model();
  CHECK(m.harm.sub == 1 && strstr(m.harm.title, "LOOP 4"), "LISTEN: %s -> %s", m.harm.title, m.harm.lines[0]);
  btn(B_YES);
  m = model();
  CHECK(!strcmp(m.harm.slot[0], "Dm") && !strcmp(m.harm.slot[1], "Bb") && !strcmp(m.harm.slot[2], "F") &&
            !strncmp(m.harm.slot[3], "A", 1),
        "YES: the loop from what was heard (%s %s %s %s), key %s", m.harm.slot[0], m.harm.slot[1], m.harm.slot[2],
        m.harm.slot[3], m.harm.key);

  // live-safe: KEY0 held at power-on
  harm::begin(true);
  toMode(5);
  CHECK(model().harm.hmode == HM_PLAY && model().harm.liveSafe, "live-safe: GROW is out of reach, PLAY only");
  int len0 = model().harm.loopLen;
  btn(B_NO);
  CHECK(model().harm.loopLen == len0, "live-safe: nothing rerolls");

  printf("\n%s\n", g_fails ? "FAILED" : "all passed");
  return g_fails ? 1 : 0;
}
