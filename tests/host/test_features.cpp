// Host test: the instrument's behaviour end to end — chords, the bass on the
// Move's clock, and the one-knob controls — using the real firmware code.
#include "harness.h"
#include <algorithm>
#include <cmath>
#include <set>

using engine::P_BANK;
using engine::P_ROOT;
using engine::P_SCALE;
using engine::P_STRM;
using engine::P_VAR;
using engine::P_DICE;

static const double STEP124 = 60e6 / 124 / 4;   // one 16th at 124 BPM, in us
static const int CH_CHORD = 0, CH_BASS = 1;

static int pcOf(int note) { return note % 12; }

int main() {
  g_files["/favs.txt"] = "P\tF01\ti bVI bIII bVII\n";
  bootAll();
  UiModel m = model();
  CHECK(m.mode == M_CHORD, "boots into CHORD");

  // ------------------------------------------------------------ chords --
  engine::paramSet(P_ROOT, 5);                  // F minor
  engine::paramSet(P_SCALE, 2);
  engine::paramSet(P_BANK, 13);                 // Dark
  engine::paramSet(engine::P_VRND, 0);          // exact velocities for this check
  engine::paramSet(engine::P_WEIGHT, 0);
  g_midi.clear();
  ev(EV_PAD_DOWN, 0);
  auto on = notesOn(CH_CHORD);
  CHECK(on.size() >= 3 && on[0].d2 == 100, "pad 1 plays a chord now, at VELOCITY 100 (%zu notes)", on.size());
  int64_t firstAt = on.empty() ? 0 : on[0].t;
  CHECK(!on.empty() && firstAt - (g_us - 1000) < 1000, "no waiting for any clock: sounds within 1 ms");
  ev(EV_PAD_UP, 0);
  CHECK(notesOff(CH_CHORD).size() == on.size(), "release: every note off");

  engine::paramSet(P_STRM, -100);
  g_midi.clear();
  ev(EV_PAD_DOWN, 1);
  runFor(300000);
  on = notesOn(CH_CHORD);
  double span = on.size() > 1 ? (on.back().t - on[0].t) / 1000.0 : 0;
  CHECK(on.size() >= 3 && on[0].d1 > on.back().d1 && span >= 80, "strum down: high to low over %.0f ms", span);
  ev(EV_PAD_UP, 1);
  engine::paramSet(P_STRM, 0);

  // ------------------------------------------------------------ clock --
  g_midi.clear();
  moveClock(124, 3000000, true);                // START + 3 s of Move clock
  m = model();
  CHECK(m.clk.ext && m.clk.running, "follows the Move's clock (MOVE, running)");
  CHECK(m.clk.bpm10 >= 1235 && m.clk.bpm10 <= 1245, "reads the tempo: %.1f BPM", m.clk.bpm10 / 10.0);
  auto bon = notesOn(CH_BASS);
  CHECK(bon.size() > 10, "bass plays on channel 2 (%zu notes in 3 s)", bon.size());

  // notes land on the 16th grid (swing 50 = straight)
  bass::paramSet(bass::BP_SWING, 50);
  bass::paramSet(bass::BP_GENRE, 0);            // TECHNO, page 0
  ev(EV_ACTION, app::actionIndex("mode"), 1);   // -> BASS (MODE menu, pad 2)
  CHECK(model().mode == M_BASS, "click switches to BASS");
  bass::paramSet(bass::BP_LEN, 0);              // as written: one bar
  ev(EV_PAD_DOWN, 5);                           // ROLLER, queued for the next bar
  ev(EV_PAD_UP, 5);
  m = model();
  CHECK(m.bass.queuedPad == 5 || m.bass.playPad == 5, "pattern queued for the next bar");
  moveClock(124, 2500000);
  m = model();
  CHECK(!strcmp(m.bass.name, "ROLLER"), "ROLLER took over at the bar (%s)", m.bass.name);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP124 * 16 * 2));  // two bars
  bon = notesOn(CH_BASS);
  CHECK(bon.size() >= 22 && bon.size() <= 26, "ROLLER: 12 notes a bar (%zu in two bars)", bon.size());
  // grid: gaps between notes are multiples of a 16th
  int offGrid = 0;
  for (size_t i = 1; i < bon.size(); i++) {
    double d = (bon[i].t - bon[i - 1].t) / STEP124;
    if (fabs(d - round(d)) > 0.08) offGrid++;
  }
  CHECK(offGrid == 0, "every note on the 16th grid (%d off)", offGrid);

  // follows the chord you play: Ab (bIII) on the pads -> bass roots on Ab
  ev(EV_ACTION, app::actionIndex("mode"), 0);   // -> CHORD
  engine::paramSet(P_BANK, 13);
  engine::paramSet(P_DICE, 0);
  ChordView cv;
  engine::chordView(cv);
  int abPad = -1;
  for (int p = 0; p < 16; p++)
    if (!strcmp(cv.pad[p].root, "Ab") && !strcmp(cv.pad[p].qual, "")) abPad = p;
  CHECK(abPad >= 0, "found an Ab major chord on the Dark page (pad %d)", abPad + 1);
  ev(EV_PAD_DOWN, abPad);
  ev(EV_PAD_UP, abPad);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP124 * 16));
  bon = notesOn(CH_BASS);
  std::set<int> pcs;
  for (auto& n : bon) pcs.insert(pcOf(n.d1));
  CHECK(!bon.empty() && pcs.count(8) && pcs.size() == 1, "bass follows the chord: every root now Ab (%zu notes)", bon.size());

  // PROG: walks the row, one chord per bar
  bass::paramSet(bass::BP_FOLLOW, 1);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP124 * 16 * 4));
  bon = notesOn(CH_BASS);
  pcs.clear();
  for (auto& n : bon) pcs.insert(pcOf(n.d1));
  CHECK(pcs.size() >= 2, "FOLLOW PROG: the bass walks the progression (%zu roots over 4 bars)", pcs.size());
  bass::paramSet(bass::BP_FOLLOW, 0);

  // swing: every second 16th arrives late
  bass::paramSet(bass::BP_SWING, 66);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP124 * 16));
  bon = notesOn(CH_BASS);
  double late = 0;
  int nLate = 0;
  for (auto& n : bon) {
    double pos = (n.t - bon[0].t) / STEP124;
    double frac = pos - floor(pos + 0.001);
    if (frac > 0.05) {
      late += frac;
      nLate++;
    }
  }
  CHECK(nLate > 0 && late / nLate > 0.25 && late / nLate < 0.40, "SWING 66: offbeat 16ths %.2f of a step late", nLate ? late / nLate : 0);
  bass::paramSet(bass::BP_SWING, 50);

  // DUCK 100: nothing on the kick
  bass::paramSet(bass::BP_GENRE, 0);
  ev(EV_ACTION, app::actionIndex("mode"), 1);   // -> BASS (MODE menu, pad 2)
  bass::pageMove(0);
  ev(EV_PAD_DOWN, 12);                          // PULSE8 (all on the 8ths, kicks too)
  ev(EV_PAD_UP, 12);
  moveClock(124, (int64_t)(STEP124 * 20));
  bass::paramSet(bass::BP_DUCK, 100);
  g_midi.clear();
  moveClock(124, (int64_t)(STEP124 * 16));
  bon = notesOn(CH_BASS);
  CHECK(bon.size() == 4, "DUCK 100 on PULSE8: only the 4 off-kick 8ths remain (%zu)", bon.size());
  bass::paramSet(bass::BP_DUCK, 0);

  // slides overlap into the next note (glide on a mono synth)
  bass::paramSet(bass::BP_GENRE, 1);            // ACID
  ev(EV_PAD_DOWN, 0);                           // SQUELCH
  ev(EV_PAD_UP, 0);
  moveClock(124, (int64_t)(STEP124 * 20));
  g_midi.clear();
  moveClock(124, (int64_t)(STEP124 * 16));
  int overlaps = 0;
  int sounding = -1;
  for (auto& e : g_midi) {
    if ((e.st & 15) != CH_BASS) continue;
    if ((e.st & 0xF0) == 0x90 && e.d2) {
      if (sounding >= 0 && sounding != e.d1) overlaps++;
      sounding = e.d1;
    } else if (e.d1 == sounding) {
      sounding = -1;
    }
  }
  CHECK(overlaps >= 2, "ACID slides: next note starts before the last ends (%d overlaps)", overlaps);

  // STOP from the Move: silence, and it stays silent
  clk::midiRealtime(0xFC, g_us);
  runFor(20000);
  bool offSent = !notesOff(CH_BASS).empty();
  g_midi.clear();
  moveClock(124, 500000);
  CHECK(offSent && notesOn(CH_BASS).empty(), "Move STOP: bass note released, no more notes");

  // ---------------------------------------------------- preview, no Move --
  g_us += 2000000;                              // clock gone quiet
  runFor(10000);
  CHECK(!model().clk.ext, "no clock for a while: back to its own tempo");
  g_midi.clear();
  ev(EV_PAD_DOWN, 3);                           // audition a line with the Move stopped
  ev(EV_PAD_UP, 3);
  runFor(1500000);
  CHECK(notesOn(CH_BASS).size() > 4 && model().clk.running, "tap a bassline with no Move: it previews at TEMPO");
  ev(EV_PAD_DOWN, 3);
  ev(EV_PAD_UP, 3);
  g_midi.clear();
  runFor(500000);
  CHECK(!model().clk.running && notesOn(CH_BASS).empty(), "tap it again: preview stops");

  // ------------------------------------------------- one-knob controls --
  ev(EV_HOLD);
  m = model();
  CHECK(m.overlay == OV_FUNC && !strcmp(m.func[0].label, "TEMPO"), "hold: BASS function layer (%s...)", m.func[0].label);
  int sw0 = bass::paramGet(bass::BP_SWING);
  ev(EV_PAD_DOWN, 1);                           // SWING
  ev(EV_PAD_UP, 1);
  ev(EV_TURN, 0, 6);                            // turning while holding sets it
  ev(EV_RELEASE);
  CHECK(bass::paramGet(bass::BP_SWING) == sw0 + 6, "hold + SWING pad + turn: swing %d -> %d", sw0, bass::paramGet(bass::BP_SWING));
  ev(EV_PUSH_TURN, 0, 2);
  CHECK(bass::paramGet(bass::BP_SWING) == sw0 + 8, "push + turn keeps editing the focus (%d)", bass::paramGet(bass::BP_SWING));

  // a chord held through a hold: its release must still release it
  ev(EV_ACTION, app::actionIndex("mode"), 0);   // -> CHORD
  g_midi.clear();
  ev(EV_PAD_DOWN, 5);
  ev(EV_CLICK);                                 // a chord held + click: REPEAT
  ev(EV_PAD_UP, 5);
  runFor(400000);
  CHECK(notesOn(CH_CHORD).size() == notesOff(CH_CHORD).size() && !notesOn(CH_CHORD).empty(),
        "a chord repeated by MAIN click still releases");
  ev(EV_CLICK);                                 // the chord MENU, on the screen
  for (int i = 0; i < 4; i++) ev(EV_TURN, 0, 1);  // MAIN turns to DICE
  ev(EV_CLICK);                                 // open it
  int dice0 = engine::paramGet(engine::P_DICE);
  ev(EV_TURN, 0, 2);
  g_midi.clear();
  ev(EV_PAD_DOWN, 6);                           // the pads keep playing chords meanwhile
  ev(EV_PAD_UP, 6);
  bool padsPlay = !notesOn(CH_CHORD).empty();
  CHECK(model().kmOpen && model().kmEdit && !strcmp(model().focus.label, "DICE") && engine::paramGet(engine::P_DICE) != dice0 &&
            padsPlay,
        "MAIN click = the MENU on screen: turn to DICE, click, turn sets it; the pads still play");
  ev(EV_CLICK);
  ev(EV_HOLD);
  ev(EV_RELEASE);
  CHECK(!model().kmOpen, "MAIN hold closes it");
  engine::paramSet(engine::P_DICE, dice0);

  // VAR comes back
  engine::paramSet(P_DICE, 100);
  engine::paramSet(P_VAR, 1);
  engine::chordView(cv);
  std::string a = std::string(cv.pad[0].root) + cv.pad[0].qual + cv.pad[1].root + cv.pad[1].qual;
  engine::paramSet(P_VAR, 2);
  engine::chordView(cv);
  std::string b = std::string(cv.pad[0].root) + cv.pad[0].qual + cv.pad[1].root + cv.pad[1].qual;
  engine::paramSet(P_VAR, 1);
  engine::chordView(cv);
  std::string c = std::string(cv.pad[0].root) + cv.pad[0].qual + cv.pad[1].root + cv.pad[1].qual;
  CHECK(a == c && a != b, "VAR 1 -> 2 -> 1 returns to the same chords (%s | %s)", a.c_str(), b.c_str());
  engine::paramSet(P_DICE, 0);

  // two keypads: keypad B is always the bass
  g_keypads = 2;
  ev(EV_PAD_DOWN, 16 + 5);
  ev(EV_PAD_UP, 16 + 5);
  runFor(200000);
  CHECK(model().mode == M_CHORD && model().clk.running, "2 keypads: keypad B starts a bassline while CHORD stays on screen");
  ev(EV_PAD_DOWN, 16 + 5);
  ev(EV_PAD_UP, 16 + 5);
  runFor(100000);
  g_keypads = 1;

  // ------------------------------------------------------------ edit --
  ev(EV_ACTION, app::actionIndex("mode"), 1);   // -> BASS (MODE menu, pad 2)
  ev(EV_HOLD);
  ev(EV_PAD_DOWN, 14);                          // EDIT
  ev(EV_PAD_UP, 14);
  ev(EV_RELEASE);
  CHECK(model().mode == M_EDIT, "EDIT opens");
  ev(EV_HOLD);
  ev(EV_PAD_DOWN, 8);                           // CLEAR
  ev(EV_PAD_UP, 8);
  ev(EV_RELEASE);
  ev(EV_PAD_DOWN, 2);                           // tap an empty step: a root
  ev(EV_PAD_UP, 2);
  m = model();
  CHECK((m.bass.step[2].flags & SF_NOTE) && !strcmp(m.bass.step[2].role, "R"), "tap an empty step: root note");
  ev(EV_PAD_DOWN, 2);                           // hold it and turn: the fifth
  ev(EV_TURN, 0, 3);
  ev(EV_PAD_UP, 2);
  m = model();
  CHECK(!strcmp(m.bass.step[2].role, "5"), "hold a step + turn: now %s", m.bass.step[2].role);
  ev(EV_PAD_DOWN, 2);                           // tap a note: gone
  ev(EV_PAD_UP, 2);
  CHECK(!(model().bass.step[2].flags & SF_NOTE), "tap a note again: removed");
  ev(EV_CLICK);
  CHECK(model().mode == M_BASS, "click leaves EDIT");

  // --------------------------------------------------- stars and files --
  bass::star();
  CHECK(g_files["/bassfav.txt"].find("\tP") != std::string::npos || g_files["/bassfav.txt"].find("P\t") != std::string::npos,
        "bass STAR writes bassfav.txt");
  ev(EV_ACTION, app::actionIndex("mode"), 0);   // -> CHORD
  engine::star();
  CHECK(g_files["/favs.txt"].find("F02") != std::string::npos, "chord STAR writes favs.txt (F02)");

  // --------------------------------------------------- keypad setup --
  ev(EV_ACTION, app::actionIndex("more"));      // SETUP
  CHECK(model().mode == M_MORE, "MORE opens the settings list (%d items)", model().moreN);
  for (int i = 0; i < 40; i++) ev(EV_TURN, 0, 1);  // to the last item: BUTTON SETUP,
  ev(EV_TURN, 0, -1);                           // and one up: KEYPAD SETUP
  ev(EV_CLICK);
  CHECK(model().mode == M_CALIB, "KEYPAD SETUP starts");
  ev(EV_RAW_DOWN, 27);                          // keys arrive as the pair of wires they join
  ev(EV_RAW_DOWN, 27);                          // the same key again doesn't take the next pad
  CHECK(model().calNext == 1, "KEYPAD SETUP: a key already taken is ignored");
  for (int i = 1; i < 16; i++) ev(EV_RAW_DOWN, 16 - i);
  CHECK(model().mode != M_CALIB && g_pairOfPad[0] == 27 && g_pairOfPad[1] == 15 && g_pairOfPad[15] == 1,
        "16 presses: every pad learns its pair of wires, any pinout");

  // ----------------------------------------------------------- KEY0 --
  bool before = bass::running();
  ev(EV_KEY, 0, 1);                             // long press on its own
  runFor(700000);
  CHECK(model().overlay == OV_PERF, "KEY0 held: the PERFORM layer shows (%s ...)", model().func[0].label);
  ev(EV_KEY, 0, 0);
  bool after = bass::running();
  ev(EV_KEY, 0, 1);
  runFor(700000);
  ev(EV_KEY, 0, 0);
  CHECK(after != before && bass::running() == before && model().overlay == OV_NONE,
        "KEY0 long press alone: bass on, then off");

  int strum0 = engine::paramGet(P_STRM);
  g_midi.clear();
  ev(EV_KEY, 0, 1);                             // SHIFT + pad 1 = STRUM, + turn = set it
  ev(EV_PAD_DOWN, 0);
  ev(EV_PAD_UP, 0);
  ev(EV_TURN, 0, 3);
  runFor(700000);
  ev(EV_KEY, 0, 0);
  bool noNotes = true;
  for (auto& mm : g_midi) if ((mm.st & 0xF0) == 0x90 && mm.d2 && (mm.st & 15) == CH_CHORD) noNotes = false;
  CHECK(engine::paramGet(P_STRM) != strum0 && noNotes && bass::running() == before,
        "KEY0 + pad + turn: STRUM changes, no chord plays, bass untouched");
  engine::paramSet(P_STRM, strum0);

  float bpm0 = clk::internalBpm();
  bool bassBefore = bass::running();
  ev(EV_KEY, 0, 1);                             // hold KEY0, tap D (TEMPO) four times, 500 ms apart
  for (int i = 0; i < 4; i++) {
    ev(EV_PAD_DOWN, 15);
    ev(EV_PAD_UP, 15);
    runFor(498000);
  }
  ev(EV_KEY, 0, 0);
  CHECK(fabsf(clk::internalBpm() - 120.0f) < 1.5f && bass::running() == bassBefore,
        "KEY0 + D taps: tap tempo %.1f BPM (was %.1f), bass untouched", clk::internalBpm(), bpm0);

  // ---------------------------------------------------- screen flip --
  ev(EV_ACTION, app::actionIndex("more"));      // SETUP
  int flipAt = -1;
  for (int i = 0; i < model().moreN; i++)
    if (!strcmp(model().more[i].label, "SCREEN FLIP")) flipAt = i;
  for (int i = 0; i < flipAt; i++) ev(EV_TURN, 0, 1);
  ev(EV_PUSH_TURN, 0, 1);
  CHECK(flipAt >= 0 && model().screenFlip, "MORE: SCREEN FLIP turns the picture over");
  ev(EV_PUSH_TURN, 0, 1);
  ev(EV_CLICK);

  // --------------------------------------------------- the library --
  int cur = 0, total = 0;
  std::set<uint32_t> shapes;
  for (int g = 0; g < 16; g++) {
    bass::paramSet(bass::BP_GENRE, g);
    for (int p = 0; p < 12; p++) {
      BassView bv;
      bass::bassView(bv);
      for (int k = 0; k < 16; k++) {
        if (!(bv.pad[k].flags & 1)) continue;
        total++;
        if (!(bv.pad[k].flags & 2)) cur++;
        shapes.insert(((uint32_t)g << 16) ^ bv.pad[k].onsets ^ ((uint32_t)bv.pad[k].accents << 7));
      }
      bass::pageMove(1);
    }
  }
  CHECK(cur >= 140, "hand-written basslines in the first pages: %d", cur);
  CHECK(shapes.size() > total * 0.6, "variety: %zu different rhythms in %d lines browsed", shapes.size(), total);

  printf(g_fails ? "\n%d FAILED\n" : "\nall passed\n", g_fails);
  return g_fails ? 1 : 0;
}
