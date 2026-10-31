// Host test: the performance features, end to end through the real app and
// engine — feel, arp, stabs, repeat, echo, re-voice, quality, loop recorder,
// STOP ALL, UNDO, TEMP EDITS, scenes, CLOCK and MIDI IN.
//
//   H=tests/host && clang++ -std=c++17 -O1 -w -I $H/shim -I firmware $H/test_perform.cpp \
//     firmware/engine.cpp firmware/bass.cpp firmware/clock.cpp firmware/app.cpp -o /tmp/chordbox_perf
#include "harness.h"
#include <algorithm>
#include <cmath>
#include <set>

using namespace engine;
static const int CH = 0;
static const double STEP = 60e6 / 124 / 4;       // a 16th at 124 BPM, in us

static std::vector<int64_t> onsets(const std::vector<Msg>& on) {
  std::set<int64_t> t;
  for (auto& m : on) t.insert(m.t);
  return std::vector<int64_t>(t.begin(), t.end());
}
static void key0(bool down) { ev(EV_KEY, 0, down ? 1 : 0); }
static void perform(int cell, int detents = 0) {   // hold KEY0, tap a PERFORM cell, turn
  key0(true);
  ev(EV_PAD_DOWN, cell);
  ev(EV_PAD_UP, cell);
  if (detents) ev(EV_TURN, 0, detents);
  key0(false);
}
static bool moreItem(const char* label) {        // open MORE (SETUP) and go to label
  ev(EV_ACTION, app::actionIndex("more"));
  int at = -1;
  UiModel m = model();
  for (int i = 0; i < m.moreN; i++)
    if (!strcmp(m.more[i].label, label)) at = i;
  for (int i = 0; i < at; i++) ev(EV_TURN, 0, 1);
  return at >= 0;
}

int main() {
  bootAll();
  paramSet(P_ROOT, 5);
  paramSet(P_SCALE, 2);
  paramSet(P_BANK, 13);
  paramSet(P_VRND, 0);
  paramSet(P_HUM, 0);
  paramSet(P_STRM, 0);

  // ------------------------------------------------------------- feel --
  g_midi.clear();
  ev(EV_PAD_DOWN, 0);
  auto on = notesOn(CH);
  std::sort(on.begin(), on.end(), [](const Msg& a, const Msg& b) { return a.d1 < b.d1; });
  CHECK(on.size() >= 3 && on.front().d2 == 104 && on[1].d2 == 90 && on.back().d2 == 112,
        "WEIGHT PIANO: low %d, inner %d, top %d", on.front().d2, on.size() > 1 ? on[1].d2 : 0, on.back().d2);
  ev(EV_PAD_UP, 0);
  paramSet(P_SOFT, 100);
  paramSet(P_WEIGHT, 0);
  g_midi.clear();
  ev(EV_PAD_DOWN, 0);
  on = notesOn(CH);
  CHECK(!on.empty() && on[0].d2 == 45, "SOFT 100: velocity 100 -> %d", on.empty() ? 0 : on[0].d2);
  ev(EV_PAD_UP, 0);
  paramSet(P_SOFT, 0);

  // -------------------------------------------------------------- arp --
  moveClock(124, 500000, true);
  paramSet(P_ARP, 1);                           // UP
  paramSet(P_ARATE, 3);                         // 1/16
  g_midi.clear();
  ev(EV_PAD_DOWN, 0);
  moveClock(124, (int64_t)(STEP * 8));
  on = notesOn(CH);
  auto t = onsets(on);
  int offGrid = 0;
  for (size_t i = 2; i < t.size(); i++) {
    double k = (t[i] - t[1]) / STEP;
    if (fabs(k - lround(k)) * STEP > 1500) offGrid++;
  }
  bool single = on.size() == t.size();
  bool rising = on.size() >= 3 && on[1].d1 > on[0].d1 && on[2].d1 > on[1].d1;
  CHECK(t.size() >= 8 && t.size() <= 10 && single && rising && !offGrid,
        "ARP UP 1/16: %zu single notes, rising, on the Move's grid", t.size());
  ev(EV_PAD_UP, 0);
  moveClock(124, 200000);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP * 4));
  CHECK(notesOn(CH).empty(), "ARP stops when the pad is let go");

  paramSet(P_ARP, 5);                           // CHORD = stabs
  paramSet(P_RHY, 2);                           // OFFBEAT
  moveClock(124, (int64_t)(STEP * (16 - fmod((g_us / STEP), 16))));
  g_midi.clear();
  ev(EV_PAD_DOWN, 0);
  moveClock(124, (int64_t)(STEP * 16));
  on = notesOn(CH);
  t = onsets(on);
  CHECK(t.size() == 4 && on.size() == 4 * (on.size() / 4), "stabs: CHORD + OFFBEAT = %zu chord hits a bar", t.size());
  ev(EV_PAD_UP, 0);
  paramSet(P_ARP, 0);
  paramSet(P_RHY, 0);
  moveClock(124, 200000);

  // ----------------------------------------------------------- repeat --
  paramSet(P_RPT, 3);                           // 1/16
  g_midi.clear();
  ev(EV_PAD_DOWN, 1);
  key0(true);
  bool noPerform = model().overlay != OV_PERF;
  moveClock(124, (int64_t)(STEP * 8));
  bool badge = model().chord.repeat;
  key0(false);
  moveClock(124, 50000);
  t = onsets(notesOn(CH));
  CHECK(noPerform && badge && t.size() >= 9 && !model().chord.repeat,
        "pad held, then KEY0: the chord repeats (%zu hits in half a bar), badge shows", t.size());
  ev(EV_PAD_UP, 1);
  moveClock(124, 100000);

  // ------------------------------------------------------------- echo --
  paramSet(P_ECHO, 1);                          // 1/8
  g_midi.clear();
  ev(EV_PAD_DOWN, 2);
  ev(EV_PAD_UP, 2);
  moveClock(124, (int64_t)(STEP * 8));
  on = notesOn(CH);
  t = onsets(on);
  bool fading = on.size() > 4 && on.back().d2 < on.front().d2;
  double gap = t.size() > 1 ? (t[1] - t[0]) / STEP : 0;
  CHECK(t.size() >= 3 && fading && fabs(gap - 2) < 0.1, "ECHO 1/8: %zu repeats after the press, fading, 2 steps apart", t.size() - 1);
  paramSet(P_ECHO, 0);
  moveClock(124, 1000000);

  // ------------------------------------------------ re-voice, quality --
  g_midi.clear();
  ev(EV_PAD_DOWN, 4);
  on = notesOn(CH);
  int lowest = 127;
  for (auto& m : on) lowest = std::min(lowest, (int)m.d1);
  g_midi.clear();
  ev(EV_TURN, 0, 1);
  auto after = notesOn(CH);
  bool movedUp = false;
  for (auto& m : after) movedUp |= m.d1 == lowest + 12;
  int page0 = model().chord.page;
  CHECK(movedUp && model().chord.page == page0, "hold a chord + turn: inversion (lowest note up an octave), page stays");
  std::string s0 = model().chord.sym;
  ev(EV_PUSH_TURN, 0, 1);
  std::string s1 = model().chord.sym;
  ev(EV_PUSH_TURN, 0, 1);
  std::string s2 = model().chord.sym;
  CHECK(s1 != s2 && s2 == s1 + "m", "hold a chord + push-turn: quality %s -> %s -> %s", s0.c_str(), s1.c_str(), s2.c_str());
  ev(EV_PAD_UP, 4);

  // ------------------------------------------------------ loop recorder --
  perform(13);                                  // REC
  CHECK(model().chord.loopState == 1, "REC arms the loop recorder");
  ev(EV_PAD_DOWN, 0);
  ev(EV_PAD_UP, 0);
  std::string a = model().chord.sym;
  ev(EV_PAD_DOWN, 5);
  ev(EV_PAD_UP, 5);
  std::string b = model().chord.sym;
  perform(13);                                  // play
  std::set<std::string> heard;
  for (int bar = 0; bar < 4; bar++) {
    moveClock(124, (int64_t)(STEP * 16));
    heard.insert(model().chord.sym);
  }
  UiModel m = model();
  CHECK(m.chord.loopState == 2 && m.chord.loopN == 2 && heard.count(a) && heard.count(b),
        "LOOP plays the 2 recorded chords (%s, %s), one a bar", a.c_str(), b.c_str());

  // ---------------------------------------------------------- STOP ALL --
  perform(12);                                  // LATCH on
  ev(EV_PAD_DOWN, 3);
  ev(EV_PAD_UP, 3);
  key0(true);
  ev(EV_HOLD);
  ev(EV_RELEASE);
  key0(false);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP * 16));
  m = model();
  CHECK(!m.chord.latch && m.chord.loopState == 0 && notesOn(CH).empty(),
        "KEY0 + hold the knob: STOP ALL (latch off, loop off, silence)");

  // ------------------------------------------------ UNDO, TEMP EDITS --
  int sw0 = bass::paramGet(bass::BP_SWING);
  perform(10, 3);                               // SWING +3 detents
  int sw1 = bass::paramGet(bass::BP_SWING);
  key0(true);
  ev(EV_CLICK);
  key0(false);
  CHECK(sw1 != sw0 && bass::paramGet(bass::BP_SWING) == sw0, "KEY0 + click: UNDO (swing %d -> %d -> %d)", sw0, sw1,
        bass::paramGet(bass::BP_SWING));

  CHECK(moreItem("TEMP EDITS"), "MORE has TEMP EDITS");
  ev(EV_PUSH_TURN, 0, 1);
  ev(EV_CLICK);
  perform(2, 5);                                // SOFT +5 detents, then let go
  CHECK(paramGet(P_SOFT) == 0, "TEMP EDITS: SOFT goes back when KEY0 lets go");
  key0(true);
  ev(EV_PAD_DOWN, 2);
  ev(EV_PAD_UP, 2);
  ev(EV_TURN, 0, 4);
  ev(EV_CLICK);                                 // KEEP
  key0(false);
  CHECK(paramGet(P_SOFT) == 20, "TEMP EDITS + click: KEEP (SOFT %d)", paramGet(P_SOFT));
  moreItem("TEMP EDITS");
  ev(EV_PUSH_TURN, 0, 1);
  ev(EV_CLICK);

  // ------------------------------------------------------------ scenes --
  moreItem("SAVE SCENE");
  ev(EV_CLICK);                                 // saves scene 1
  ev(EV_PAD_DOWN, 15);                          // any pad closes the list
  ev(EV_PAD_UP, 15);
  bass::paramSet(bass::BP_SWING, 66);
  paramSet(P_SOFT, 80);
  perform(14);                                  // SCENE: tap loads it
  CHECK(bass::paramGet(bass::BP_SWING) == sw0 && paramGet(P_SOFT) == 20 && g_files.count("/scenes.bin"),
        "SAVE SCENE, change things, tap SCENE: all back (swing %d, soft %d)", bass::paramGet(bass::BP_SWING),
        paramGet(P_SOFT));

  // ----------------------------------------------------------- KEY0 0.4 --
  moveClock(124, 100000);
  bool run0 = bass::running() && !bass::muted();
  key0(true);
  moveClock(124, 450000);                        // the Move keeps playing while KEY0 is held
  key0(false);
  CHECK((bass::running() && !bass::muted()) != run0, "KEY0 held 0.45 s alone: bass on / off");

  // ------------------------------------------- quick bass, quick star --
  moveClock(124, 100000);
  bool b0 = bass::running() && !bass::muted();
  key0(true);                                   // a quick tap on its own
  key0(false);
  bool b1 = bass::running() && !bass::muted();
  key0(true);
  key0(false);
  CHECK(b1 != b0 && (bass::running() && !bass::muted()) == b0, "KEY0 tapped on its own: bass on / off");

  ev(EV_PAD_DOWN, 6);
  ev(EV_CLICK);
  bool rpt = model().chord.repeat;
  ev(EV_PAD_UP, 6);
  CHECK(rpt && !model().chord.repeat && model().mode == M_CHORD, "hold a chord + MAIN click: REPEAT until it lets go (no SHIFT)");
  ev(EV_ACTION, app::actionIndex("mode"), 1);   // to BASS
  std::string bf0 = g_files["/bassfav.txt"];
  ev(EV_PAD_DOWN, 2);
  ev(EV_CLICK);
  ev(EV_PAD_UP, 2);
  CHECK(g_files["/bassfav.txt"] != bf0 && model().mode == M_BASS, "hold a bass line + click: STAR that line");
  ev(EV_ACTION, app::actionIndex("mode"), 0);   // back to CHORD

  bass::paramSet(bass::BP_SWING, 60);
  key0(true);                                   // hold SCENE (#) for 0.9 s: save
  ev(EV_PAD_DOWN, 14);
  runFor(900000);
  ev(EV_PAD_UP, 14);
  key0(false);
  bass::paramSet(bass::BP_SWING, 70);
  perform(14);                                  // tap: load
  CHECK(bass::paramGet(bass::BP_SWING) == 60, "PERFORM SCENE: hold saves, tap loads (swing %d)", bass::paramGet(bass::BP_SWING));

  // ------------------------------------------ where you are, after a restart --
  ev(EV_TURN, 0, 2);
  runFor(3500000);
  UiModel w0 = model();
  ev(EV_TURN, 0, 3);
  app::begin();                                 // what a restart does after the engines start
  runFor(2000);
  UiModel w1 = model();
  CHECK(g_files.count("/where.bin") && w1.chord.page == w0.chord.page && !strcmp(w1.chord.bank, w0.chord.bank),
        "restart: back on the same chord page (%s %d)", w1.chord.bank, w1.chord.page + 1);

  // --------------------------------------------- bass length and octave --
  bass::paramSet(bass::BP_GENRE, 0);            // Techno page 1: ROLLER etc. are one bar as written
  bass::paramSet(bass::BP_LEN, 0);
  ev(EV_ACTION, app::actionIndex("mode"), 1);   // BASS
  moveClock(124, (int64_t)(STEP * 16));
  ev(EV_PAD_DOWN, 5);                           // a one-bar line
  ev(EV_PAD_UP, 5);
  moveClock(124, (int64_t)(STEP * 20));
  int one = model().bass.len;
  bass::paramSet(bass::BP_LEN, 1);
  runFor(300000);
  int two = model().bass.len;
  UiModel lm = model();
  bool answers = false;                         // bar 2 answers bar 1 in its last beat
  for (int i = 12; i < 16; i++)
    answers |= lm.bass.step[i].flags != lm.bass.step[16 + i].flags || lm.bass.step[i].semis != lm.bass.step[16 + i].semis;
  bass::paramSet(bass::BP_LEN, 2);
  runFor(300000);
  int four = model().bass.len;
  CHECK(one == 16 && two == 32 && four == 64 && answers,
        "LENGTH: a one-bar line plays 1 / 2 / 4 bars (%d / %d / %d steps), bar 2 answers bar 1", one, two, four);
  int oct0 = bass::paramGet(bass::BP_OCT);
  perform(3, 1);                                // PERFORM A = bass octave
  CHECK(bass::paramGet(bass::BP_OCT) == oct0 + 1, "PERFORM A: bass octave %+d -> %+d", oct0, bass::paramGet(bass::BP_OCT));
  bass::paramSet(bass::BP_OCT, oct0);
  {                                              // the bass goes all the way down (v2.1)
    int lowest[3] = {127, 127, 127}, octs[3] = {0, -2, -4};
    moveClock(124, 200000, true);
    bass::padDown(0);
    moveClock(124, 4000000);
    for (int k = 0; k < 3; k++) {
      bass::paramSet(bass::BP_OCT, octs[k]);
      g_midi.clear();
      moveClock(124, 2000000);
      for (auto& x : notesOn(bass::paramGet(bass::BP_CH))) lowest[k] = std::min(lowest[k], (int)x.d1);
    }
    bass::paramSet(bass::BP_OCT, oct0);
    // DENSITY thins the line: the ring draws the skipped notes hollow, and exactly those stay silent
    bass::paramSet(bass::BP_DENS, -60);
    double bar = 60e6 / 124 * 4;
    int match = 0, bars = 0, skipped = 0;
    for (int k = 0; k < 8; k++) {
      while (clk::step() % 16 != 15) moveClock(124, 20000);   // just before a bar line
      UiModel bm = model();
      int len = bm.bass.len ? bm.bass.len : 16;
      int startPos = ((bm.bass.patBar + 1) % (len / 16)) * 16;   // the line's next bar
      int expect = 0;
      for (int i = startPos; i < startPos + 16; i++) {
        bool note = bm.bass.step[i].flags & SF_NOTE, skip = bm.bass.step[i].flags & SF_SKIP;
        expect += note && !skip;
        skipped += note && skip;
      }
      g_midi.clear();
      uint32_t s0 = clk::step();
      while (clk::step() < s0 + 16) moveClock(124, 5000);       // through the bar's last 16th
      int sent = (int)notesOn(bass::paramGet(bass::BP_CH)).size();
      match += sent == expect;
      bars++;
    }
    bass::paramSet(bass::BP_DENS, 0);
    CHECK(match == bars && skipped > 0, "DENSITY LESS 60: the hollow notes are exactly the silent ones (%d/%d bars, %d skipped)",
          match, bars, skipped);
    CHECK(lowest[1] < 24 && lowest[1] < lowest[0] && lowest[2] >= 0 && lowest[2] < lowest[1],
          "bass OCTAVE goes down to -4: lowest note %d at 0, %d at -2, %d at -4", lowest[0], lowest[1], lowest[2]);
  }
  ev(EV_ACTION, app::actionIndex("mode"), 0);   // back to CHORD

  // -------------------------------------------------------- PHONE LINK --
  moreItem("PHONE LINK");
  ev(EV_CLICK);
  bool shown = model().overlay == OV_PHONE;
  ev(EV_PAD_DOWN, 3);                           // any key closes it, and plays nothing
  g_midi.clear();
  ev(EV_PAD_UP, 3);
  CHECK(shown && model().overlay == OV_NONE && notesOn(CH).empty(), "MORE > PHONE LINK opens the phone screen; any key closes it");

  // ------------------------------------------------------ CLOCK, MIDI IN --
  CHECK(moreItem("MIDI IN CH"), "MORE has MIDI IN CH");
  ev(EV_PUSH_TURN, 0, 1);                       // channel 1
  ev(EV_CLICK);
  app::midiIn(0x90, 62, 100);                   // D on channel 1
  app::midiIn(0x91, 64, 100);                   // E on channel 2: not ours
  CHECK(paramGet(P_ROOT) == 2, "MIDI IN CH 1: a note from the Move sets the key (%s)", keyName());

  moreItem("CLOCK");
  ev(EV_PUSH_TURN, 0, 1);                       // INTERNAL
  ev(EV_CLICK);
  runFor(600000);
  moveClock(124, 500000, true);
  CHECK(!model().clk.ext, "CLOCK INTERNAL: the Move's clock is ignored");

  // ---- v2.3: the middle is only the bass; the pads stay keys; the left is the chords
  ev(EV_ACTION, app::actionIndex("mode"), 0);
  engine::stopAll();
  for (int p : {P_ARP, P_RHY, P_ECHO, P_STRM, P_HUM, P_VRND, P_DICE, P_VLD}) paramSet(p, 0);
  enum { P_TOP, P_2ND, P_3RD, P_BOTTOM };
  auto btn = [](int k, int ms = 60) {
    ev(EV_BTN, k, 1);
    runFor(ms * 1000);
    ev(EV_BTN, k, 0);
  };
  UiModel mm = model();
  CHECK(!strcmp(mm.soft[0], "UP") && !strcmp(mm.soft[1], "DOWN") && !strcmp(mm.soft[2], "GENRE") &&
            !strcmp(mm.soft[3], "ON/OFF"),
        "the bass station's buttons read %s %s %s %s", mm.soft[0], mm.soft[1], mm.soft[2], mm.soft[3]);
  moveClock(124, 200000, true);
  std::string l0 = model().bass.name;
  btn(P_2ND);                                   // DOWN: the next line
  moveClock(124, (int64_t)(STEP * 20));
  std::string l1 = model().bass.name;
  btn(P_TOP);                                   // UP: back
  moveClock(124, (int64_t)(STEP * 20));
  CHECK(l1 != l0 && model().bass.name == l0, "DOWN: the next line (%s -> %s), UP: back (%s)", l0.c_str(), l1.c_str(),
        model().bass.name);
  CHECK(model().leftBass && model().mode == M_CHORD, "using the middle: the left screen shows the bass");
  g_midi.clear();
  ev(EV_PAD_DOWN, 5);
  bool padChord = !notesOn(CH).empty() && !model().leftBass;
  ev(EV_PAD_UP, 5);
  CHECK(padChord, "a pad plays its chord, and the left screen shows the chords again");
  btn(P_2ND);                                   // DOWN: the bass page again
  btn(P_TOP);
  uint16_t pg0 = model().chord.page;
  ev(EV_TURN, 0, 1);                            // touching MAIN: back to the chords
  CHECK(!model().leftBass && model().chord.page == pg0, "the first touch of MAIN brings the chords back, and only that (lb %d, page %d -> %d)", model().leftBass, pg0, model().chord.page);
  std::string h0 = model().bass.name;
  ev(EV_BTN, P_2ND, 1);                         // DOWN held: it repeats
  runFor(1200000);
  ev(EV_BTN, P_2ND, 0);
  moveClock(124, (int64_t)(STEP * 20));
  CHECK(model().bass.line >= 4, "DOWN held: lines go by (now #%d)", model().bass.line);
  bool on0 = bass::running() && !bass::muted();
  btn(P_BOTTOM);
  bool on1 = bass::running() && !bass::muted();
  btn(P_BOTTOM);
  CHECK(on1 != on0 && (bass::running() && !bass::muted()) == on0, "ON/OFF: bass on / off, and back");
  std::string bf1 = g_files["/bassfav.txt"];
  btn(P_BOTTOM, 600);
  CHECK(g_files["/bassfav.txt"] != bf1 && (bass::running() && !bass::muted()) == on0, "ON/OFF held: STAR the line (no toggle)");
  int g0 = bass::paramGet(bass::BP_GENRE);
  btn(P_3RD);
  CHECK(bass::paramGet(bass::BP_GENRE) == g0 + 1, "GENRE: the next genre, a line from it plays (%s)", model().bass.name);
  btn(P_3RD, 600);
  CHECK(bass::paramGet(bass::BP_GENRE) == g0, "GENRE held: the previous genre");

  // the middle knob: turn = the setting, click = the next one
  CHECK(!strcmp(model().setLabel, "OCTAVE"), "the middle knob starts on %s", model().setLabel);
  int o0 = bass::paramGet(bass::BP_OCT);
  ev(EV_TURN2, 0, -1);
  CHECK(bass::paramGet(bass::BP_OCT) == o0 - 1, "middle turn: OCTAVE %+d", bass::paramGet(bass::BP_OCT));
  bass::paramSet(bass::BP_OCT, o0);
  ev(EV_KNOB2, 0, 1);
  ev(EV_KNOB2, 0, 0);
  ev(EV_CLICK2);
  CHECK(!strcmp(model().setLabel, "DENSITY"), "middle click: the next setting, %s (intensity)", model().setLabel);
  for (int i = 0; i < 9; i++) {
    ev(EV_KNOB2, 0, 1);
    ev(EV_KNOB2, 0, 0);
    ev(EV_CLICK2);
  }
  CHECK(!strcmp(model().setLabel, "OCTAVE"), "and round again to OCTAVE");

  // the middle knob held = SHIFT for the station
  ev(EV_KNOB2, 0, 1);
  mm = model();
  CHECK(mm.shiftHeld && !strcmp(mm.soft[0], "PG UP") && !strcmp(mm.soft[2], "RANDOM") && !strcmp(mm.soft[3], "NOTES"),
        "middle knob held: the buttons read %s %s %s %s", mm.soft[0], mm.soft[1], mm.soft[2], mm.soft[3]);
  int ln0 = model().bass.line;
  btn(P_2ND);                                   // PAGE DOWN
  moveClock(124, (int64_t)(STEP * 20));
  int ln1 = model().bass.line;
  std::string r0 = model().bass.name;
  btn(P_3RD);                                   // RANDOM
  moveClock(124, (int64_t)(STEP * 20));
  std::string r1 = model().bass.name;
  int gg = bass::paramGet(bass::BP_GENRE);
  ev(EV_PUSH_TURN2, 0, 1);                      // held + turn: the genres
  int gg1 = bass::paramGet(bass::BP_GENRE);
  ev(EV_KNOB2, 0, 0);
  ev(EV_CLICK2);                                // (the knob's release: no click, it was SHIFT)
  CHECK(ln1 == ln0 + 16 && r1 != r0 && gg1 != gg && !strcmp(model().setLabel, "OCTAVE"),
        "held: PAGE DOWN (#%d -> #%d), RANDOM (%s), turn = genre; letting go doesn't click", ln0, ln1, r1.c_str());
  for (int i = 0; i < 20 && !model().bass.any; i++) btn(P_3RD);
  std::set<std::string> genres;
  std::vector<std::string> lines;
  for (int i = 0; i < 6; i++) {
    btn(P_2ND);
    moveClock(124, (int64_t)(STEP * 20));
    lines.push_back(model().bass.name);
    genres.insert(model().bass.genre);
  }
  btn(P_TOP);
  moveClock(124, (int64_t)(STEP * 20));
  CHECK(model().bass.any && genres.size() >= 3 && model().bass.name == lines[4],
        "GENRE round to ANY: DOWN gives %zu genres in 6, UP replays the one before", genres.size());

  // NOTES: held + the bottom button
  ev(EV_KNOB2, 0, 1);
  btn(P_BOTTOM);
  ev(EV_KNOB2, 0, 0);
  mm = model();
  CHECK(mm.notes.on && !strcmp(mm.soft[0], "FITS") && !strcmp(mm.soft[1], "CLASH") && !strcmp(mm.soft[2], "USE KEY"),
        "middle held + bottom: NOTES; the buttons read %s %s %s", mm.soft[0], mm.soft[1], mm.soft[2]);
  int bch = bass::paramGet(bass::BP_CH);
  g_midi.clear();
  ev(EV_PAD_DOWN, 5);
  bool fOn = false;
  for (auto& x : notesOn(bch)) fOn |= x.d1 == model().notes.base + 5;
  ev(EV_PAD_UP, 5);
  CHECK(fOn, "the pads play single bass notes (F = %d)", model().notes.base + 5);
  btn(P_TOP);
  for (int pc : {8, 0, 10, 3}) {
    ev(EV_PAD_DOWN, pc);
    ev(EV_PAD_UP, pc);
    btn(P_TOP);
  }
  for (int pc : {4, 11, 9}) {
    ev(EV_PAD_DOWN, pc);
    ev(EV_PAD_UP, pc);
    btn(P_2ND);
  }
  std::string key = model().notes.key;
  btn(P_3RD);                                   // USE KEY
  CHECK(key.rfind("F minor", 0) == 0 && paramGet(P_ROOT) == 5 && paramGet(P_SCALE) == 2,
        "FITS / CLASH, then USE KEY: %s for chords and bass", key.c_str());
  ev(EV_TURN2, 0, -1);
  CHECK(model().notes.base == 24, "NOTES: the middle knob shifts the octave (pad 1 = MIDI %d)", model().notes.base);
  ev(EV_TURN2, 0, 1);
  ev(EV_KNOB2, 0, 1);
  btn(P_BOTTOM);                                // LINES
  ev(EV_KNOB2, 0, 0);
  CHECK(!model().notes.on, "held + bottom again: the pads play chords");

  // the left: MAIN click = the chord MENU on the screen; MAIN hold = MODES; the pads stay keys
  ev(EV_TURN, 0, 0);
  if (model().leftBass) ev(EV_TURN, 0, 1), ev(EV_TURN, 0, -1);
  ev(EV_CLICK);
  CHECK(model().kmOpen && model().overlay == OV_FUNC, "MAIN click: the chord MENU, on the screen");
  for (int i = 0; i < 18; i++) ev(EV_TURN, 0, 1);   // past the 16 functions: PERFORM, SOFT
  ev(EV_CLICK);
  int soft0 = paramGet(P_SOFT);
  ev(EV_TURN, 0, 2);
  g_midi.clear();
  ev(EV_PAD_DOWN, 6);
  ev(EV_PAD_UP, 6);
  CHECK(model().overlay == OV_PERF && paramGet(P_SOFT) != soft0 && !notesOn(CH).empty(),
        "turn on to PERFORM SOFT, click, turn: %d -> %d; the pads keep playing chords", soft0, paramGet(P_SOFT));
  paramSet(P_SOFT, soft0);
  ev(EV_CLICK);                                 // DONE
  ev(EV_PAD_DOWN, 6);
  ev(EV_PAD_UP, 6);
  CHECK(!model().kmOpen && model().overlay == OV_NONE, "DONE, then a pad: back to the chords");
  ev(EV_CLICK);
  ev(EV_HOLD);
  ev(EV_RELEASE);
  CHECK(!model().kmOpen && model().overlay == OV_NONE, "MAIN hold: closed");
  ev(EV_HOLD);
  ev(EV_RELEASE);
  CHECK(model().overlay == OV_MODES, "MAIN hold: MODES, on the screen");
  g_midi.clear();
  ev(EV_PAD_DOWN, 4);
  ev(EV_PAD_UP, 4);
  CHECK(!notesOn(CH).empty() && model().overlay == OV_NONE, "MODES up: a pad plays its chord and closes MODES");
  ev(EV_HOLD);
  ev(EV_RELEASE);
  for (int i = 0; i < 4; i++) ev(EV_TURN, 0, 1);  // CHORD -> PLAY
  ev(EV_CLICK);
  mm = model();
  CHECK(mm.mode == M_HARM && !strcmp(mm.soft[0], "SLOTS") && !strcmp(mm.soft[3], "MODES"),
        "MAIN turn + click: PLAY; there the buttons read %s %s %s %s", mm.soft[0], mm.soft[1], mm.soft[2], mm.soft[3]);
  ev(EV_ACTION, app::actionIndex("mode"), 0);
  CHECK(model().mode == M_CHORD, "back to CHORD");

  // the bass page on the left: what each line is, not its name
  btn(P_2ND);
  mm = model();
  int described = 0;
  for (auto& c : mm.bass.pad) described += (c.flags & 1) && c.feel[0] && c.energy >= 1 && c.energy <= 5;
  CHECK(mm.leftBass && mm.bassSide && mm.bass.feel[0] && described == 16 && !strcmp(mm.turnValue, "CHORDS"),
        "DOWN: the bass page on the left, every line described (playing: %s)", mm.bass.feel);
  ev(EV_PAD_DOWN, 3);
  ev(EV_PAD_UP, 3);
  ev(EV_TURN2, 0, 1);
  ev(EV_TURN2, 0, -1);
  CHECK(!model().leftBass, "turning a bass setting leaves the chords on the screen");
  ev(EV_ACTION, app::actionIndex("more"));
  bool inSetup = model().mode == M_MORE;
  g_midi.clear();
  ev(EV_PAD_DOWN, 2);
  bool setupPlays = !notesOn(CH).empty();
  ev(EV_PAD_UP, 2);
  CHECK(inSetup && model().mode == M_CHORD && setupPlays, "SETUP: a pad closes it and plays its chord");

  ev(EV_ACTION, app::actionIndex("buttons"));   // BUTTONS: learn them, top to bottom
  bool asking = model().btnLearn == 1;
  for (int k : {3, 2, 1, 0}) btn(k);            // wired the other way round
  bool saved = g_files.count("/buttons2.bin") && model().btnLearn == 0;
  CHECK(asking && saved && !strcmp(model().soft[0], "UP"), "BUTTONS setup: press top to bottom");
  ev(EV_ACTION, app::actionIndex("buttons"));
  for (int k : {0, 1, 2, 3}) btn(k);
  CHECK(g_files["/buttons2.bin"] == std::string("\0\1\2\3", 4), "and back to the default order");

  printf("\n%s\n", g_fails ? "FAILED" : "all passed");
  return g_fails ? 1 : 0;
}
