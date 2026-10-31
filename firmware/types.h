// types.h — input events in, UI model out
#pragma once
#include <stdint.h>

// ------------------------------------------------------------- input --
enum EvType : uint8_t {
  EV_PAD_DOWN,     // a = pad 0..31 (keypad A 0..15, keypad B 16..31), row-major
  EV_PAD_UP,       // a = pad
  EV_RAW_DOWN,     // calibration only: a = raw scan index (keypad*16 + row*4 + col)
  EV_TURN,         // b = detents, encoder not pressed
  EV_PUSH_TURN,    // b = detents, encoder held down
  EV_CLICK,        // short press, no turn
  EV_HOLD,         // held past HOLD_MS without turning: function layer opens
  EV_RELEASE,      // released after a hold or a push-turn
  EV_SET,          // console: a = chord param, b = value
  EV_BSET,         // console: a = bass param, b = value
  EV_ACTION,       // console: a = action id
  EV_KEY,          // KEY0 beside the knob: b = 1 pressed, 0 released
  EV_TURN2,        // second knob (chord shape): b = detents, not pressed
  EV_PUSH_TURN2,   // second knob held down and turned
  EV_CLICK2,       // second knob tapped
  EV_HOLD2,        // second knob held 0.5 s without turning (released: no click)
  EV_KNOB2,        // second knob pressed (b = 1) / let go (b = 0): the bass station's SHIFT
  EV_BTN,          // OLED button: a = K1..K4 as 0..3, b = 1 pressed, 0 released
};

struct Event { EvType type; uint8_t a; int16_t b; uint32_t tUs; };

// ---------------------------------------------------------- UI model --
// The app (core 1 on the ESP32) owns the truth and publishes a copy of
// this struct; the screen task only reads its own copy, so drawing can
// never hold up a note.

enum Mode : uint8_t { M_CHORD, M_BASS, M_EDIT, M_MORE, M_CALIB, M_HARM };   // M_HARM: the Harmony Engine (HarmView.hmode)
enum Overlay : uint8_t { OV_NONE, OV_FUNC, OV_PERF, OV_PHONE, OV_MODES };
enum Group : uint8_t { G_KEY, G_HARM, G_FEEL, G_GEN, G_SYS, G_BASS };
enum ParamKind : uint8_t { K_NUM, K_BIPOLAR, K_LIST, K_ACTION };

struct ParamView {
  char label[13];
  char value[12];
  uint8_t group, kind;
  int16_t pos, lo, hi;
  const char* const* opts;   // K_LIST option names (static storage)
  uint8_t nOpts;
};

// ---- chord program
enum PadFlag : uint8_t {
  PF_HAS = 1 << 0,     // a chord lives here
  PF_SOUND = 1 << 1,   // sounding now
  PF_LAST = 1 << 2,    // the last chord played
  PF_WRAP = 1 << 3,    // short row: this pad repeats chord 1
  PF_BORROW = 1 << 4,  // notes outside the chosen scale
  PF_DICED = 1 << 5,   // dice changed this chord
};

struct PadCell {
  char root[4];
  char qual[8];
  char rn[9];
  uint8_t flags;
};

struct ChordView {
  char bank[14];
  uint16_t page, pages;
  uint8_t bankIdx, nBanks;
  bool played, latch;
  char sym[14];        // "Fm9"
  char rn[10];         // "iv9"
  char rowTag[24];
  uint8_t notes[10], nNotes;
  uint8_t sounding;
  PadCell pad[16];
  bool rowFav[4];
  char motion[16];     // "UP 1/16" while the arp runs, "" when off
  bool repeat, echo;
  uint8_t loopState, loopN, loopPos;   // 0 idle, 1 recording, 2 looping
};

// ---- bass program
enum StepFlag : uint8_t {
  SF_NOTE = 1 << 0,
  SF_ACCENT = 1 << 1,
  SF_GHOST = 1 << 2,
  SF_SLIDE = 1 << 3,
  SF_TIE = 1 << 4,
  SF_SHORT = 1 << 5,
  SF_LONG = 1 << 6,
  SF_SKIP = 1 << 7,    // DENSITY (or full DUCK) skips it this time round: drawn hollow
};

struct StepView {
  int8_t semis;        // pitch above the pattern root (-24..+24)
  uint8_t flags;
  char role[3];        // "R", "5", "b2", ...
};

// what a bass line is like, worked out from its notes (bass.cpp describe)
enum BassTrait : uint8_t {
  BT_MELODIC = 1 << 0, BT_ROOT = 1 << 1, BT_OCT = 1 << 2, BT_SLIDE = 1 << 3,
  BT_CHROM = 1 << 4, BT_LONG = 1 << 5, BT_GHOST = 1 << 6, BT_ACCENT = 1 << 7,
};
inline void bassTraitWords(uint8_t t, char* out, size_t cap, int most) {   // the first `most`, by weight
  static const char* const W[8] = {"MELODIC", "ROOT", "OCTAVES", "SLIDES", "CHROMA", "LONG", "GHOSTS", "ACCENTS"};
  out[0] = 0;
  for (int i = 0, n = 0; i < 8 && n < most; i++)
    if (t & (1 << i)) {
      if (n++) strlcat(out, " ", cap);
      strlcat(out, W[i], cap);
    }
}

struct BassPadCell {
  char name[9];
  uint16_t onsets;     // first bar, bit i = step i has a note
  uint16_t accents;
  uint16_t held;       // first bar, bit i = step i holds the note before
  uint8_t flags;       // 1 = exists, 2 = generated, 4 = starred
  char feel[9];        // ROLLING, OFFBEAT, ACID ...
  uint8_t traits;      // BassTrait
  uint8_t energy;      // 1..5
  int8_t semis[16];    // first bar: rough height of each note (-12..24)
};

struct BassView {
  char bank[12];       // genre
  uint16_t page, pages;
  uint8_t bankIdx, nBanks;
  bool genPage;        // this page is generated, not hand-written
  char name[16];       // playing pattern
  char genre[12];
  uint8_t len;         // steps, 16..64
  StepView step[64];
  int8_t playPad, queuedPad;   // pads on this page, or -1
  BassPadCell pad[16];
  uint8_t curNote;     // MIDI note sounding now, 0 = none
  char noteName[6];
  char follow[6];      // CHORD / PROG / KEY
  char chordName[12];  // chord the bass is following
  bool muted;
  uint8_t ch;
  char next[10];       // the line queued for the next bar, "" = none
  uint16_t line;       // the playing line's number in its genre (1-based, 0 = none)
  uint8_t patBar;      // the bar of the line playing now (2- and 4-bar lines start where they start)
  bool any;            // the station is on ANY genre
  char feel[9];        // the playing line, described
  uint8_t traits, energy;
  char nextFeel[9];    // the line queued for the next bar, "" = none
};

struct ClockView {
  bool ext;            // following the Move's MIDI clock
  bool running;
  uint16_t bpm10;
  uint32_t step;       // 16ths since start
  uint32_t stepAtMs;   // when that step began (for smooth playheads)
  uint16_t stepMs;
  uint8_t swing;
};

// ---- function layer / settings / calibration
struct FuncCell {
  char label[8];
  char value[10];      // current value, for settings (empty for actions)
  uint8_t icon, group, kind;   // kind: K_ACTION for actions, else a param
  bool on;             // toggles: LATCH, PLAY, ...
};

enum Icon : uint8_t {
  I_NONE, I_KEY, I_SCALE, I_OCT, I_BANK, I_DICE, I_VAR, I_VOICE, I_STRUM,
  I_HUMAN, I_LATCH, I_STOP, I_MORE, I_STAR, I_GEN, I_MUTATE, I_FAVS,
  I_TEMPO, I_SWING, I_GENRE, I_FOLLOW, I_LEN, I_GATE, I_SLIDE, I_ACCENT,
  I_DENS, I_DUCK, I_EDIT, I_PLAY, I_NOTE, I_ROTL, I_ROTR, I_UP, I_DOWN,
  I_CLEAR, I_RAND, I_UNDO, I_DONE, I_TIE, I_BAR,
  I_SOFT, I_ARP, I_RHYTHM, I_ECHO, I_REPEAT, I_REC, I_SCENE,
};

// ---- the Harmony Engine (v2.0): HUNT, LISTEN, PLAY, GROW, MELODY, TAKES
enum HMode : uint8_t { HM_HUNT, HM_LISTEN, HM_PLAY, HM_GROW, HM_MELODY, HM_COUNT };
enum GridCell : uint8_t {        // one pad of the 4x4 minimap
  GC_OFF, GC_DIM, GC_FIT, GC_CLASH, GC_TRY, GC_SEL, GC_PLAY, GC_LOCK, GC_STRONG, GC_SHAPE,
};

struct HarmView {
  uint8_t hmode, sub;            // sub-state of the mode (offers, hold test, candidates, ...)
  bool takes, slotLayer, liveSafe, playing, melodyOn;
  int8_t tune;                   // cents
  uint8_t spice, density, range, voice, rhythm, oct, shape;
  bool penta;
  char key[20];                  // "D dark", or the leading guess
  uint8_t sureBars, surePct;
  bool tie, keyManual;
  char ask[28];                  // the one question that settles it ("B or Bb?")
  char title[28];                // the mode's headline ("bar 2/4 · low note F")
  char lines[4][28];             // offers, guesses, candidates, moods, listen results
  int8_t lineSel;
  uint8_t nLines;
  uint8_t loopLen;
  int8_t playSlot, selSlot, cursor;   // cursor: HUNT's bar, PLAY's next slot to fill
  char slot[16][8];              // chord names
  uint8_t slotFlags[16];         // hg::F_* (F_SET = filled)
  uint8_t grid[16];              // GridCell, the grid touched last
  char pad[16][6];               // what each sound pad plays
  int8_t lastPad;
  bool gridIsSlots;              // the minimap shows slots (else sounds)
  uint8_t takeIdx, takeN, takeAge; bool takeStar; char ab;
  char takeList[5][28];
  int8_t takeSel;
  char printing[16];             // "PRINT 3/8" while printing to the Move
  char nowChord[10];             // the chord sounding (or last played)
  char valueLabel[10], valueText[12], pushLabel[10], pushText[12];
  char k2Label[10], k2Text[12], clickText[12];
};

struct UiModel {
  uint32_t seq;
  uint8_t mode, overlay;
  bool bassSide;       // the screen belongs to the bass program (BASS, EDIT, its settings)
  char key[4], scale[5];
  bool moveUp;
  uint8_t keypads;     // 1 or 2 wired
  bool screenFlip;     // picture turned 180 degrees (MORE: SCREEN FLIP)
  struct { char ssid[33], url[32], pass[16]; bool hotspot, on; uint8_t phones; } wifi;   // PHONE LINK
  ClockView clk;
  ChordView chord;
  BassView bass;

  FuncCell func[16];   // the layer shown while the encoder is held
  int8_t funcFocus;    // cell whose parameter the encoder edits (-1 none)
  ParamView focus;     // the focused parameter, for the big readout
  uint32_t focusUntil; // readout stays up after a push-turn until then

  char turnLabel[16], turnValue[16];   // what the encoder does, for the legend
  char pushLabel[16], pushValue[16];

  char toast[32];
  uint32_t toastUntil;
  uint32_t padMask;    // pads held right now

  int8_t editStep;     // EDIT: selected step in the bar (0..15)
  uint8_t editBar, editBars;

  uint8_t moreN, moreSel;   // MORE: the settings list
  char moreTitle[12];
  ParamView more[40];

  uint8_t calKeypad, calNext;   // CALIB: which pad to press next
  uint16_t calDone;

  // the v2 parts
  int8_t inversion;    // second knob: the inversion every chord is played in
  uint8_t btnLearn;    // BUTTON SETUP: 1..4 = waiting for * # < >, 0 = off
  uint8_t btnHeld;     // OLED buttons held now, by role (bit 0 * .. bit 3 >)

  HarmView harm;       // the Harmony Engine's screens (mode M_HARM)

  // the controls, named for the screens (v2.1)
  char soft[4][9];     // the four OLED buttons, top to bottom: what each does right now
  bool shiftHeld;      // SHIFT (KEY0) is down: the soft labels show the SHIFT actions
  char clickLabel[12]; // MAIN knob click
  char setLabel[12], setValue[16];   // SET knob: the setting it turns
  uint8_t setPos, setN;              // which of how many (click steps to the next)
  uint8_t modeSel;     // MODE menu: the tile in focus
  bool padsAlt;        // PADS: the keypad shows its other layer (slots / functions)

  // the bass station (v2.2): the OLED + its buttons + the middle knob are the bass
  bool bassSet;        // the middle knob turns the bass settings
  bool leftBass;       // the left screen shows the bass (the middle was used last)
  bool kmOpen, kmEdit; // the chord MENU on the screen, worked by MAIN; a setting being turned
  struct {
    bool on;           // NOTES: the pads play single bass notes
    uint8_t base;      // MIDI note of pad 1
    int8_t last;       // the last note played (-1 none)
    uint16_t fit, clash;   // pitch classes marked FITS / CLASH
    char key[28];      // the key they point to ("F minor 82%")
  } notes;
};
