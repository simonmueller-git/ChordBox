// app.cpp — how one knob and 16 pads drive two instruments
//
//   turn          browse: chord pages (CHORD) or bassline pages (BASS)
//   push + turn   the focused setting (BANK / GENRE until you pick another)
//   click         CHORD <-> BASS (EDIT and the settings list: back)
//   hold          the FUNCTION layer: the pads become 16 functions, shown on
//                 screen; tap one to act or to focus it, turn while holding
//                 to change it, let go to return
//
// KEY0 (the button beside the knob) is SHIFT, always held, never toggled:
//   hold          the PERFORM layer: the live controls on every screen
//   on its own    pressed and let go without touching anything: bass on / off
//   + D taps      tap tempo (TEMPO in the PERFORM layer)
//   + click       UNDO the last change (with TEMP EDITS on: KEEP the edits)
//   + hold knob   STOP ALL: silence, latch off, loop and bass stopped
// A chord pad held:
//   turn          its inversion        push + turn   its quality (maj, m, 7, 9, sus...)
//   then KEY0     the chord REPEATs on the clock until you let go
//   + click       STAR its row (a bass line held + click: STAR that line)
// PERFORM SCENE (#): tap loads the scene, hold it 0.8 s to save it
//
// With two keypads, keypad A always plays chords and keypad B always drives
// the bass (patterns, or steps in EDIT), whatever the screen shows.
//
// Wiring v2 (ESP32):
// The second knob shapes chords, with no pad to hold:
//   turn          the inversion every chord plays in (the sounding one moves too)
//   push + turn   the sounding chord's quality     click   VOICING (next)
// The OLED's four buttons (MORE: BUTTON SETUP learns them) are the spec's YES / NO / MODE:
//   *  YES (fits, keep, lock; on a take: star)    #  NO (clash, reject, reroll; held: undo)
//   <  >  MODE: CHORD BASS HUNT LISTEN PLAY GROW MELODY  (held: TAKES; in BASS: the genre)
//
// The Harmony Engine (v2.0, mode M_HARM, harm.cpp). One keypad, so the spec's two
// grids share it: the pads are the SOUNDS (top row thin/full/wide/flip, 12 below
// notes or chords); hold the main knob and they are the 16 SLOTS:
//   main knob     turn VALUE (octave / shape / spice / density), push+turn the
//                 mode's second value, click play/stop (HUNT: hold test; MELODY: tap a rhythm)
//   knob held     slots: tap = audition + select, hold a slot + turn = ranked
//                 suggestions, two slots = copy, slot + YES lock, slot + NO clear,
//                 KEY0 + slot = play from there, turn with no slot = loop length
//   knob 2        HUNT tune (click: tap "1"), PLAY home (push+turn: mood, click: top
//                 note), GROW pick A/B/C, MELODY voice (push+turn: line on/off, click: 5/7)
//   KEY0 + YES    capture -> a take     KEY0 + NO   A/B on the bar line
//   KEY0 + click  PRINT to the Move     KEY0 at power-on   live-safe: PLAY only

#include "bigmem.h"
#include "app.h"
#include "bass.h"
#include "clock.h"
#include "config.h"
#include "engine.h"
#include "harm.h"
#include "input.h"
#include "menu.h"
#include "harm_theory.h"
#include "platform.h"
#include "web.h"
#include <LittleFS.h>

namespace {

enum Owner : uint8_t { O_CHORD, O_BASS, O_ACT, O_SYS, O_NONE };

enum Act : uint8_t {
  A_NONE, A_LATCH, A_ALLOFF, A_MORE, A_STAR, A_GEN, A_MUTATE, A_FAVS,
  A_BSTAR, A_BMUTATE, A_EDIT, A_PLAY,
  A_E_ACC, A_E_SLIDE, A_E_TIE, A_E_OCTUP, A_E_OCTDN, A_E_ROTL, A_E_ROTR,
  A_E_CLEAR, A_E_RAND, A_E_UNDO, A_E_DONE, A_E_BAR,
  A_CALIB, A_PANIC, A_PLAIN, A_RESYNC, A_UNSTAR,
  A_LOOP, A_STOPALL, A_UNDO, A_SAVESCENE, A_SCENEN, A_SAVESCENEN, A_KEY0, A_PHONE,
  A_BTNSETUP, A_HMODE, A_NEWSONG,
};

enum Sys : uint8_t { S_FLIP, S_ENCDIR, S_SCENE, S_TEMP, S_CLOCK, S_INCH, S_WIFI, S_MELCH };

struct FuncDef {
  const char* label;
  uint8_t icon, group, owner, id;
};


const FuncDef CHORD_LAYER[16] = {
    {"ROOT", I_KEY, G_KEY, O_CHORD, engine::P_ROOT},       {"SCALE", I_SCALE, G_KEY, O_CHORD, engine::P_SCALE},
    {"OCTAVE", I_OCT, G_KEY, O_CHORD, engine::P_OCT},      {"BANK", I_BANK, G_KEY, O_CHORD, engine::P_BANK},
    {"DICE", I_DICE, G_HARM, O_CHORD, engine::P_DICE},     {"VAR", I_VAR, G_HARM, O_CHORD, engine::P_VAR},
    {"VOICING", I_VOICE, G_HARM, O_CHORD, engine::P_VOIC}, {"STRUM", I_STRUM, G_FEEL, O_CHORD, engine::P_STRM},
    {"HUMAN", I_HUMAN, G_FEEL, O_CHORD, engine::P_HUM},    {"LATCH", I_LATCH, G_FEEL, O_ACT, A_LATCH},
    {"ALL OFF", I_STOP, G_SYS, O_ACT, A_ALLOFF},   {"MORE", I_MORE, G_SYS, O_ACT, A_MORE},
    {"STAR", I_STAR, G_GEN, O_ACT, A_STAR},        {"NEW", I_GEN, G_GEN, O_ACT, A_GEN},
    {"MUTATE", I_MUTATE, G_GEN, O_ACT, A_MUTATE},  {"FAVS", I_FAVS, G_GEN, O_ACT, A_FAVS},
};

const FuncDef BASS_LAYER[16] = {
    {"TEMPO", I_TEMPO, G_BASS, O_BASS, bass::BP_TEMPO},  {"SWING", I_SWING, G_FEEL, O_BASS, bass::BP_SWING},
    {"OCTAVE", I_OCT, G_BASS, O_BASS, bass::BP_OCT},     {"GENRE", I_GENRE, G_BASS, O_BASS, bass::BP_GENRE},
    {"FOLLOW", I_FOLLOW, G_HARM, O_BASS, bass::BP_FOLLOW}, {"LENGTH", I_LEN, G_GEN, O_BASS, bass::BP_LEN},
    {"GATE", I_GATE, G_FEEL, O_BASS, bass::BP_GATE},     {"SLIDE", I_SLIDE, G_FEEL, O_BASS, bass::BP_SLIDE},
    {"ACCENT", I_ACCENT, G_FEEL, O_BASS, bass::BP_ACCENT}, {"DENSITY", I_DENS, G_GEN, O_BASS, bass::BP_DENS},
    {"DUCK", I_DUCK, G_FEEL, O_BASS, bass::BP_DUCK},     {"MORE", I_MORE, G_SYS, O_ACT, A_MORE},
    {"STAR", I_STAR, G_GEN, O_ACT, A_BSTAR},       {"MUTATE", I_MUTATE, G_GEN, O_ACT, A_BMUTATE},
    {"EDIT", I_EDIT, G_BASS, O_ACT, A_EDIT},       {"PLAY", I_PLAY, G_BASS, O_ACT, A_PLAY},
};

const FuncDef EDIT_LAYER[16] = {
    {"ACCENT", I_ACCENT, G_FEEL, O_ACT, A_E_ACC},  {"SLIDE", I_SLIDE, G_FEEL, O_ACT, A_E_SLIDE},
    {"TIE", I_TIE, G_FEEL, O_ACT, A_E_TIE},        {"BAR", I_BAR, G_BASS, O_ACT, A_E_BAR},
    {"OCT UP", I_UP, G_HARM, O_ACT, A_E_OCTUP},    {"OCT DN", I_DOWN, G_HARM, O_ACT, A_E_OCTDN},
    {"<< ROT", I_ROTL, G_HARM, O_ACT, A_E_ROTL},   {"ROT >>", I_ROTR, G_HARM, O_ACT, A_E_ROTR},
    {"CLEAR", I_CLEAR, G_SYS, O_ACT, A_E_CLEAR},   {"NEW LINE", I_RAND, G_GEN, O_ACT, A_E_RAND},
    {"UNDO", I_UNDO, G_SYS, O_ACT, A_E_UNDO},      {"MORE", I_MORE, G_SYS, O_ACT, A_MORE},
    {"STAR", I_STAR, G_GEN, O_ACT, A_BSTAR},       {"MUTATE", I_MUTATE, G_GEN, O_ACT, A_BMUTATE},
    {"PLAY", I_PLAY, G_BASS, O_ACT, A_PLAY},       {"DONE", I_DONE, G_BASS, O_ACT, A_E_DONE},
};

// KEY0 held: the same 16 live controls on every screen
const FuncDef PERF_LAYER[16] = {
    {"STRUM", I_STRUM, G_FEEL, O_CHORD, engine::P_STRM},   {"HUMAN", I_HUMAN, G_FEEL, O_CHORD, engine::P_HUM},
    {"SOFT", I_SOFT, G_FEEL, O_CHORD, engine::P_SOFT},     {"B.OCT", I_OCT, G_BASS, O_BASS, bass::BP_OCT},
    {"ARP", I_ARP, G_GEN, O_CHORD, engine::P_ARP},         {"RATE", I_TEMPO, G_GEN, O_CHORD, engine::P_ARATE},
    {"RHYTHM", I_RHYTHM, G_GEN, O_CHORD, engine::P_RHY},   {"ECHO", I_ECHO, G_GEN, O_CHORD, engine::P_ECHO},
    {"PLAY", I_PLAY, G_BASS, O_ACT, A_PLAY},               {"REPEAT", I_REPEAT, G_GEN, O_CHORD, engine::P_RPT},
    {"SWING", I_SWING, G_FEEL, O_BASS, bass::BP_SWING},    {"DENSITY", I_DENS, G_GEN, O_BASS, bass::BP_DENS},
    {"LATCH", I_LATCH, G_FEEL, O_ACT, A_LATCH},            {"REC", I_REC, G_BASS, O_ACT, A_LOOP},
    {"SCENE", I_SCENE, G_SYS, O_SYS, S_SCENE},             {"TEMPO", I_TEMPO, G_BASS, O_BASS, bass::BP_TEMPO},
};

struct MoreItem { const char* label; uint8_t owner, id; };

const MoreItem CHORD_MORE[] = {
    {"PHONE LINK", O_ACT, A_PHONE}, {"NEW SONG", O_ACT, A_NEWSONG},
    {nullptr, O_CHORD, engine::P_VEL},  {nullptr, O_CHORD, engine::P_VLD},  {nullptr, O_CHORD, engine::P_CHCH},
    {nullptr, O_CHORD, engine::P_DARK}, {nullptr, O_CHORD, engine::P_TENS}, {nullptr, O_CHORD, engine::P_LEN},
    {nullptr, O_CHORD, engine::P_MUT},
    {nullptr, O_CHORD, engine::P_VRND}, {nullptr, O_CHORD, engine::P_WEIGHT},
    {nullptr, O_CHORD, engine::P_SDIR}, {nullptr, O_CHORD, engine::P_AGATE}, {nullptr, O_CHORD, engine::P_AOCT},
    {nullptr, O_CHORD, engine::P_RRAMP}, {nullptr, O_CHORD, engine::P_EFB}, {nullptr, O_CHORD, engine::P_LOOPLEN},
    {"PLAIN", O_ACT, A_PLAIN},  {"UNSTAR ROW", O_ACT, A_UNSTAR},
    {"SCENE", O_SYS, S_SCENE}, {"SAVE SCENE", O_ACT, A_SAVESCENE}, {"UNDO", O_ACT, A_UNDO},
    {"TEMP EDITS", O_SYS, S_TEMP}, {"CLOCK", O_SYS, S_CLOCK}, {"MIDI IN CH", O_SYS, S_INCH},
    {"WIFI", O_SYS, S_WIFI}, {"MELODY CH", O_SYS, S_MELCH},
    {"PANIC", O_ACT, A_PANIC},  {"SCREEN FLIP", O_SYS, S_FLIP}, {"KNOB DIR", O_SYS, S_ENCDIR},
    {"KEYPAD SETUP", O_ACT, A_CALIB}, {"BUTTON SETUP", O_ACT, A_BTNSETUP},
};
const MoreItem BASS_MORE[] = {
    {"PHONE LINK", O_ACT, A_PHONE},  {nullptr, O_BASS, bass::BP_OCT}, {nullptr, O_BASS, bass::BP_LEN},
    {nullptr, O_BASS, bass::BP_CH},    {nullptr, O_BASS, bass::BP_NUDGE}, {nullptr, O_BASS, bass::BP_FILL},
    {nullptr, O_BASS, bass::BP_RATE},  {nullptr, O_BASS, bass::BP_QUANT}, {nullptr, O_BASS, bass::BP_VEL},
    {"RESYNC BAR", O_ACT, A_RESYNC},
    {"SCENE", O_SYS, S_SCENE}, {"SAVE SCENE", O_ACT, A_SAVESCENE}, {"UNDO", O_ACT, A_UNDO},
    {"TEMP EDITS", O_SYS, S_TEMP}, {"CLOCK", O_SYS, S_CLOCK}, {"MIDI IN CH", O_SYS, S_INCH},
    {"WIFI", O_SYS, S_WIFI}, {"MELODY CH", O_SYS, S_MELCH},
    {"PANIC", O_ACT, A_PANIC},
    {"SCREEN FLIP", O_SYS, S_FLIP}, {"KNOB DIR", O_SYS, S_ENCDIR}, {"KEYPAD SETUP", O_ACT, A_CALIB},
    {"BUTTON SETUP", O_ACT, A_BTNSETUP},
};

// ---- state
uint8_t mode = M_CHORD, backMode = M_CHORD;
bool overlay = false;
uint8_t focusOwner[3] = {O_CHORD, O_BASS, O_BASS};   // per CHORD / BASS / EDIT
uint8_t focusId[3] = {engine::P_BANK, bass::BP_GENRE, bass::BP_SWING};
int8_t funcFocus = -1;
uint32_t focusUntil = 0;
bool padConsumed[32];
bool moveUp = false;
bool phoneInfo = false;             // the PHONE LINK screen is up

// KEY0 = SHIFT
bool shiftDown = false, shiftUsed = false, perf = false;   // perf: the PERFORM layer is up
bool repeatKey = false;             // KEY0 went down over a held chord: REPEAT, not SHIFT
uint32_t shiftAt = 0;
uint8_t perfOwner = O_CHORD, perfId = engine::P_STRM;
uint32_t chordDown = 0;             // chord pads physically held (bit = pad)
int8_t lastChordPad = -1;
uint32_t bassDown = 0;              // bass lines held on the BASS screen
int8_t scenePad = -1;               // PERFORM SCENE held: tap loads, 0.8 s saves
uint32_t sceneAt = 0;

// where you are, /where.bin: restored at power-up
struct Where {
  uint8_t version = 1, mode = M_CHORD;
  char bank[16] = "";
  int16_t page = 0, bBank = -1, bPage = 0, bPlayBank = -1, bPlayIdx = -1;
};
Where whereSaved;
uint32_t whereChangedAt = 0;
bool whereDirty = false;

// device settings, /app.bin
struct AppPrefs {
  uint8_t version = 2;
  uint8_t temp = 0;                 // TEMP EDITS: PERFORM edits revert when KEY0 lets go
  uint8_t clockInt = 0;             // CLOCK: 0 AUTO (follow the Move), 1 INTERNAL
  uint8_t inCh = 0;                 // MIDI IN CH: 0 off, 1-16
  uint8_t scene = 0;                // the scene SCENE loads and SAVE SCENE writes
  uint8_t wifi = 1;                 // the phone page (ESP32): home network or its own hotspot
} AP;

// UNDO: the last setting changed, and what it was
struct { bool valid; uint8_t owner, id; int16_t old; } undoRec = {false, 0, 0, 0};
uint32_t undoAt = 0;

// TEMP EDITS: every setting as it was when KEY0 went down
int16_t tempChord[engine::P_COUNT], tempBass[bass::BP_COUNT];
bool tempKeep = false;

// scenes, /scenes.bin
struct Scene { uint8_t used; int16_t chord[engine::P_COUNT], bass[bass::BP_COUNT]; };
const int N_SCENES = 8;

// edit
int8_t editSel = 0;
uint8_t editBar = 0;
int8_t heldStep = -1;
bool heldExisting = false, heldTurned = false;

// more
uint8_t moreSel = 0;

// calibration
uint8_t calKp = 0, calNext = 0;
uint16_t calDone = 0;
int8_t calPair[32];                              // pad -> the pair of keypad wires it joins

// OLED buttons, named by where they sit beside the OLED, top to bottom
// (/buttons2.bin, learnt by BUTTON SETUP: press them top to bottom)
enum BtnPos : uint8_t { P_TOP, P_2ND, P_3RD, P_BOTTOM };
uint8_t btnRole[4] = {P_TOP, P_2ND, P_3RD, P_BOTTOM};  // K1..K4 -> position, top to bottom
uint8_t btnLearn = 0;               // 1..4: waiting for the top, 2nd, 3rd, bottom button

// the MODE menu (pads become the modes), PADS (the keypad's other layer), the SET knob's place
bool modeMenu = false;
uint8_t modeSel = 0;
bool padsAlt = false;
bool knobHeld = false;
int8_t harmNoK = -1;               // the button holding Harmony NO down
bool perfLatch = false;             // MAIN click in CHORD: MENU -> PERFORM -> closed (no SHIFT needed)
bool repeatClick = false;           // a chord held + MAIN click: REPEAT until the pad lets go
int8_t repeatPad = -1;

// the bass station: the OLED, its four buttons and the middle knob, in CHORD and BASS
bool bassSet = true;                // the middle knob always turns a bass setting (v2.3)
uint8_t bassSetIdx = 0;
const uint8_t BASS_SETS[] = {bass::BP_OCT, bass::BP_DENS, bass::BP_SWING, bass::BP_LEN, bass::BP_GATE,
                             bass::BP_ACCENT, bass::BP_SLIDE, bass::BP_DUCK, bass::BP_FOLLOW, bass::BP_TEMPO};
bool knob2Down = false, knob2Used = false;   // the middle knob held = the station's SHIFT
bool leftBass = false;              // the left screen shows the bass (the middle was used last)
uint32_t upDownAt = 0;              // UP / DOWN held: they repeat
int8_t upDownK = -1;

// the chord MENU on the left screen, worked by MAIN alone (the pads stay keys)
bool kmOpen = false, kmEdit = false;
uint8_t kmCursor = 0;               // 0-15 the chord functions, 16-31 PERFORM
const int N_BASS_SETS = sizeof BASS_SETS;

// NOTES: the pads play single bass notes, to find the key by ear
bool notesOn = false, notesMutedBass = false;
int8_t notesOct = 0;                // -2..+2: the pads start at C1 (MIDI 36) + 12 x this
int8_t notesLast = -1, notesHeld[16];
uint16_t notesFit = 0, notesClash = 0;
ht::NoteSet notesNs;
ht::HomeObs notesObs;
bool notesAny = false;
uint8_t setIdx[8];                  // per screen: CHORD, BASS, EDIT, then the five harmony modes
uint32_t btnAt[4];
bool btnIsDown[4], btnFired[4];

// toast
char toastBuf[32] = "";
uint32_t toastUntil = 0;

// model
UiModel* Mp = nullptr;                           // the model being built, and the published copy:
UiModel* Gp = nullptr;                           // 4.8 KB each, kept in PSRAM
MODEL_LOCK_DECL
uint32_t seq = 0;
bool dirty = true;
uint32_t lastPublishMs = 0;
uint32_t lastStepSeen = 0xFFFFFFFF;

int modeSlot() { return mode == M_CHORD ? 0 : mode == M_BASS ? 1 : 2; }
uint8_t& fOwner() { return perf ? perfOwner : focusOwner[modeSlot()]; }   // what the knob edits
uint8_t& fId() { return perf ? perfId : focusId[modeSlot()]; }
bool chordScreen() { return mode == M_CHORD || (mode == M_MORE && backMode == M_CHORD); }

const FuncDef* layer() {
  if (kmOpen) return kmCursor < 16 ? CHORD_LAYER : PERF_LAYER;
  if (perf) return PERF_LAYER;
  if (mode == M_EDIT || (mode == M_MORE && backMode == M_EDIT)) return EDIT_LAYER;
  if (mode == M_BASS || (mode == M_MORE && backMode == M_BASS)) return BASS_LAYER;
  return CHORD_LAYER;
}

const MoreItem* moreList(int& n) {
  if (backMode != M_BASS && backMode != M_EDIT) {
    n = sizeof CHORD_MORE / sizeof CHORD_MORE[0];
    return CHORD_MORE;
  }
  n = sizeof BASS_MORE / sizeof BASS_MORE[0];
  return BASS_MORE;
}

void saveAppPrefs() {
  File f = LittleFS.open("/app.bin", "w");
  if (!f) return;
  f.write((const uint8_t*)&AP, sizeof AP);
  f.close();
}

void loadAppPrefs() {
  File f = LittleFS.open("/app.bin", "r");
  if (!f) return;
  AppPrefs t;
  if (f.read((uint8_t*)&t, sizeof t) == sizeof t && t.version == AP.version) AP = t;
  f.close();
}

bool sceneRead(int i, Scene& s) {
  File f = LittleFS.open("/scenes.bin", "r");
  memset(&s, 0, sizeof s);
  if (!f) return false;
  Scene all[N_SCENES];
  bool ok = f.read((uint8_t*)all, sizeof all) == sizeof all;
  f.close();
  if (ok) s = all[i];
  return ok && s.used;
}

void sceneSave(int i) {
  Scene all[N_SCENES];
  memset(all, 0, sizeof all);
  File f = LittleFS.open("/scenes.bin", "r");
  if (f) {
    if (f.read((uint8_t*)all, sizeof all) != sizeof all) memset(all, 0, sizeof all);
    f.close();
  }
  Scene& s = all[i];
  s.used = 1;
  for (int p = 0; p < engine::P_COUNT; p++) s.chord[p] = engine::paramGet(p);
  for (int p = 0; p < bass::BP_COUNT; p++) s.bass[p] = bass::paramGet(p);
  f = LittleFS.open("/scenes.bin", "w");
  if (f) {
    f.write((const uint8_t*)all, sizeof all);
    f.close();
  }
  uiToast("SCENE %d SAVED", i + 1);
}

// put chord and bass settings back (a scene, or TEMP EDITS reverting); the
// MIDI channels and NUDGE belong to the setup, so they stay
void applySettings(const int16_t* chord, const int16_t* bs) {
  for (int p = 0; p < engine::P_COUNT; p++)
    if (p != engine::P_CHCH && chord[p] != engine::paramGet(p)) engine::paramSet(p, chord[p]);
  if (bs[bass::BP_GENRE] != bass::paramGet(bass::BP_GENRE)) bass::paramSet(bass::BP_GENRE, bs[bass::BP_GENRE]);
  for (int p = 0; p < bass::BP_COUNT; p++)
    if (p != bass::BP_CH && p != bass::BP_NUDGE && p != bass::BP_GENRE && bs[p] != bass::paramGet(p))
      bass::paramSet(p, bs[p]);
}

void sceneLoad(int i) {
  Scene s;
  if (!sceneRead(i, s)) {
    uiToast("SCENE %d EMPTY: SAVE IN MORE", i + 1);
    return;
  }
  applySettings(s.chord, s.bass);
  uiToast("SCENE %d", i + 1);
}

Where whereNow() {
  Where w;
  w.mode = (mode == M_BASS || (mode == M_EDIT) || (mode == M_MORE && backMode != M_CHORD)) ? M_BASS : M_CHORD;
  int pg;
  engine::place(w.bank, sizeof w.bank, pg);
  w.page = pg;
  int bb, bp, pb, pi;
  bass::place(bb, bp, pb, pi);
  w.bBank = bb;
  w.bPage = bp;
  w.bPlayBank = pb;
  w.bPlayIdx = pi;
  return w;
}

void whereTick(uint32_t now) {                   // saved 3 s after it stops changing
  Where w = whereNow();
  if (memcmp(&w, &whereSaved, sizeof w)) {
    whereSaved = w;
    whereDirty = true;
    whereChangedAt = now;
  }
  if (whereDirty && now - whereChangedAt > 3000) {
    File f = LittleFS.open("/where.bin", "w");
    if (f) {
      f.write((const uint8_t*)&whereSaved, sizeof whereSaved);
      f.close();
    }
    whereDirty = false;
  }
}

void whereRestore() {
  File f = LittleFS.open("/where.bin", "r");
  if (!f) return;
  Where w;
  bool ok = f.read((uint8_t*)&w, sizeof w) == sizeof w && w.version == Where().version;
  f.close();
  if (!ok) return;
  engine::setPlace(w.bank, w.page);
  bass::setPlace(w.bBank, w.bPage, w.bPlayBank, w.bPlayIdx);
  if (w.mode == M_BASS) mode = M_BASS;
  whereSaved = whereNow();
}

int paramValue(uint8_t owner, uint8_t id) {
  return owner == O_CHORD ? engine::paramGet(id) : bass::paramGet(id);
}

void setParam(uint8_t owner, uint8_t id, int v) {
  if (owner == O_CHORD) engine::paramSet(id, v);
  else bass::paramSet(id, v);
}

void undo() {
  if (!undoRec.valid) {
    uiToast("NOTHING TO UNDO");
    return;
  }
  int now = paramValue(undoRec.owner, undoRec.id);
  setParam(undoRec.owner, undoRec.id, undoRec.old);
  undoRec.old = now;                             // undo again = redo
  ParamView k;
  if (undoRec.owner == O_CHORD) engine::paramView(undoRec.id, k);
  else bass::paramView(undoRec.id, k);
  uiToast("UNDO  %s %s", k.label, k.value);
}

void paramViewOf(uint8_t owner, uint8_t id, ParamView& k) {
  memset(&k, 0, sizeof k);
  if (owner == O_CHORD) engine::paramView(id, k);
  else if (owner == O_BASS) bass::paramView(id, k);
  else if (owner == O_SYS) {
    static const char* const FLIP_N[] = {"OFF", "ON"};
    static const char* const DIR_N[] = {"NORMAL", "REVERSED"};
    static const char* const CLOCK_N[] = {"AUTO", "INTERNAL"};
    static const char* const SCENE_N[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    static const char* const INCH_N[] = {"OFF", "1", "2", "3", "4", "5", "6", "7", "8",
                                         "9", "10", "11", "12", "13", "14", "15", "16"};
    k.group = G_SYS;
    k.kind = K_LIST;
    k.lo = 0;
    k.hi = 1;
    k.nOpts = 2;
    if (id == S_SCENE) {
      strlcpy(k.label, "SCENE", sizeof k.label);
      k.pos = AP.scene;
      k.hi = N_SCENES - 1;
      k.nOpts = N_SCENES;
      k.opts = SCENE_N;
    } else if (id == S_TEMP) {
      strlcpy(k.label, "TEMP EDITS", sizeof k.label);
      k.pos = AP.temp;
      k.opts = FLIP_N;
    } else if (id == S_CLOCK) {
      strlcpy(k.label, "CLOCK", sizeof k.label);
      k.pos = AP.clockInt;
      k.opts = CLOCK_N;
    } else if (id == S_INCH) {
      strlcpy(k.label, "MIDI IN CH", sizeof k.label);
      k.pos = AP.inCh;
      k.hi = 16;
      k.nOpts = 17;
      k.opts = INCH_N;
    } else if (id == S_MELCH) {
      strlcpy(k.label, "MELODY CH", sizeof k.label);
      k.pos = harm::melodyCh() + 1;
      k.lo = 1;
      k.hi = 16;
      k.nOpts = 17;
      k.opts = INCH_N;
    } else if (id == S_WIFI) {
      strlcpy(k.label, "WIFI", sizeof k.label);
      k.pos = AP.wifi;
      k.opts = FLIP_N;
    } else if (id == S_FLIP) {
      strlcpy(k.label, "SCREEN FLIP", sizeof k.label);
      k.pos = input::screenFlip();
      k.opts = FLIP_N;
    } else {
      strlcpy(k.label, "KNOB DIR", sizeof k.label);
      k.pos = input::encReversed();
      k.opts = DIR_N;
    }
    strlcpy(k.value, k.opts[k.pos], sizeof k.value);
    if (id == S_WIFI && AP.wifi) strlcpy(k.value, web::status(), sizeof k.value);
  }
}

void adjust(uint8_t owner, uint8_t id, int d) {
  if ((owner == O_CHORD || owner == O_BASS) && d) {  // remember the old value for UNDO
    uint32_t now = millis();
    if (!undoRec.valid || undoRec.owner != owner || undoRec.id != id || now - undoAt > 1500)
      undoRec = {true, owner, id, (int16_t)paramValue(owner, id)};
    undoAt = now;
  }
  if (owner == O_CHORD) engine::paramAdjust(id, d);
  else if (owner == O_BASS) bass::paramAdjust(id, d);
  else if (owner == O_SYS && d) {
    switch (id) {
      case S_SCENE: AP.scene = ((AP.scene + d) % N_SCENES + N_SCENES) % N_SCENES; break;
      case S_TEMP: AP.temp = !AP.temp; break;
      case S_CLOCK:
        AP.clockInt = !AP.clockInt;
        clk::setFollowMove(!AP.clockInt);
        break;
      case S_INCH: AP.inCh = constrain(AP.inCh + d, 0, 16); break;
      case S_MELCH: harm::setMelodyCh(constrain(harm::melodyCh() + d, 0, 15)); return;
      case S_WIFI:
        AP.wifi = !AP.wifi;
        uiToast(AP.wifi ? "WIFI ON AFTER A RESTART" : "WIFI OFF AFTER A RESTART");
        break;
      case S_FLIP: input::setScreenFlip(!input::screenFlip()); break;
      default: input::setEncReversed(!input::encReversed());
    }
    if (id == S_FLIP || id == S_ENCDIR) input::saveSettings();
    else saveAppPrefs();
  }
}

// ---------------------------------------------------------------- actions --
void setMode(uint8_t m) {
  modeMenu = false;
  if (m == M_MORE && mode != M_MORE) {
    backMode = mode;
    moreSel = 0;
  }
  mode = m;
  overlay = false;
  heldStep = -1;
}

void startCalibration() {
  memset(calPair, -1, sizeof calPair);
  calKp = 0;
  calNext = 0;
  calDone = 0;
  input::calibrate(true);
  mode = M_CALIB;
  overlay = false;
}

void finishCalibration() {
  input::setPads(calPair, 16 * input::keypads());
  input::calibrate(false);
  input::saveSettings();
  uiToast("KEYPAD SAVED");
  mode = backMode == M_MORE ? M_CHORD : backMode;
}

void action(uint8_t a) {
  switch (a) {
    case A_LATCH: engine::toggleLatch(); break;
    case A_ALLOFF:
      engine::allOff();
      uiToast("ALL OFF");
      break;
    case A_MORE: setMode(M_MORE); break;
    case A_STAR: engine::star(); break;
    case A_GEN: engine::gen(); break;
    case A_MUTATE: engine::mutate(); break;
    case A_FAVS: engine::recall(); break;
    case A_BSTAR: bass::star(); break;
    case A_BMUTATE: bass::mutate(); break;
    case A_EDIT:
      editBar = 0;
      editSel = 0;
      setMode(M_EDIT);
      break;
    case A_PLAY: bass::togglePlay(); break;
    case A_E_ACC: bass::editAccent(editBar, editSel); break;
    case A_E_SLIDE: bass::editSlide(editBar, editSel); break;
    case A_E_TIE: bass::editTie(editBar, editSel); break;
    case A_E_OCTUP: bass::editOctave(editBar, editSel, 1); break;
    case A_E_OCTDN: bass::editOctave(editBar, editSel, -1); break;
    case A_E_ROTL: bass::editRotate(-1); break;
    case A_E_ROTR: bass::editRotate(1); break;
    case A_E_CLEAR: bass::editClear(); break;
    case A_E_RAND: bass::editRandom(); break;
    case A_E_UNDO:
      if (!bass::editUndo()) uiToast("NOTHING TO UNDO");
      break;
    case A_E_DONE: setMode(M_BASS); break;
    case A_E_BAR:
      editBar = (editBar + 1) % max(1, bass::bars());
      uiToast("BAR %d / %d", editBar + 1, bass::bars());
      break;
    case A_CALIB: startCalibration(); break;
    case A_PANIC:
      engine::panic();
      bass::onTransport(bass::running());   // flushes the bass voice
      uiToast("PANIC");
      break;
    case A_PLAIN: engine::plain(); break;
    case A_RESYNC:
      clk::resync();
      uiToast("NEXT 16TH = BAR START");
      break;
    case A_UNSTAR: engine::unstar(); break;
    case A_LOOP: engine::loopButton(); break;
    case A_STOPALL:
      engine::stopAll();
      harm::stopAll();
      if (bass::running() && !bass::muted()) bass::togglePlay();
      uiToast("STOP ALL");
      break;
    case A_UNDO: undo(); break;
    case A_SAVESCENE: sceneSave(AP.scene); break;
    case A_PHONE:
      setMode(backMode == M_MORE ? M_CHORD : backMode);
      phoneInfo = true;
      break;
    case A_NEWSONG:
      harm::newSong();
      setMode(backMode == M_MORE ? M_CHORD : backMode);
      break;
    case A_BTNSETUP:
      setMode(backMode == M_MORE ? M_CHORD : backMode);
      btnLearn = 1;
      uiToast("PRESS THE * BUTTON");
      break;
  }
}

// ------------------------------------------------------------ OLED buttons --
void loadButtons() {
  File f = LittleFS.open("/buttons2.bin", "r");
  if (!f) {
    btnLearn = 1;                                // first start with v2.1: learn where they sit
    return;
  }
  uint8_t r[4];
  bool ok = f.read(r, 4) == 4;
  f.close();
  uint8_t seen = 0;
  for (int i = 0; ok && i < 4; i++) {
    if (r[i] > 3) ok = false;
    else seen |= 1 << r[i];
  }
  if (ok && seen == 0x0F) memcpy(btnRole, r, 4);
}

void saveButtons() {
  File f = LittleFS.open("/buttons2.bin", "w");
  if (!f) return;
  f.write(btnRole, 4);
  f.close();
}

static const char* const BTN_POS[4] = {"TOP", "2ND", "3RD", "BOTTOM"};

void btnLearnPress(int k) {                      // BUTTON SETUP: button k is the one asked for
  int role = btnLearn - 1;
  for (int i = 0; i < 4; i++)
    if (i != k && btnRole[i] == role) btnRole[i] = btnRole[k];   // swap, so every role stays taken
  btnRole[k] = role;
  if (++btnLearn > 4) {
    btnLearn = 0;
    saveButtons();
    uiToast("BUTTONS: MODE PADS NO YES");
  } else {
    uiToast("PRESS THE %s BUTTON", BTN_POS[btnLearn - 1]);
  }
}

// the mode ring: CHORD BASS | HUNT LISTEN PLAY GROW MELODY
static const char* const RING[7] = {"CHORD", "BASS", "HUNT", "LISTEN", "PLAY", "GROW", "MELODY"};

int ringIndex() {
  uint8_t m = (mode == M_MORE || mode == M_CALIB) ? backMode : mode;
  if (m == M_HARM) return 2 + harm::mode();
  return (m == M_BASS || m == M_EDIT) ? 1 : 0;
}

void gotoRing(int i, int dir) {
  i = ((i % 7) + 7) % 7;
  if (harm::liveSafe() && i > 1 && i != 2 + HM_PLAY) {   // live-safe: CHORD, BASS and PLAY only
    int from = ringIndex();
    i = dir > 0 ? (from <= 1 ? 2 + HM_PLAY : 0) : dir < 0 ? (from == 0 ? 2 + HM_PLAY : from - 1) : 2 + HM_PLAY;
    if (from == 1 && dir < 0) i = 0;
  }
  padsAlt = false;
  overlay = false;
  harm::slotLayer(false);
  if (i <= 1) {
    if (mode == M_HARM) harm::leave();
    setMode(i ? M_BASS : M_CHORD);
  } else {
    if (mode != M_HARM) engine::stopAll();       // the old chord engine rests
    harm::enter((uint8_t)(i - 2));
    setMode(M_HARM);
  }
}

// ---- the MODE menu: everything one press away
bool menuAllowed(int i) {
  if (!harm::liveSafe()) return true;
  return !(i == MI_HUNT || i == MI_LISTEN || i == MI_GROW || i == MI_MELODY || i == MI_NEWSONG);
}

void menuOpen(int i) {
  modeMenu = false;
  if (i < 0 || i >= MI_COUNT) return;
  if (!menuAllowed(i)) {
    uiToast("LIVE-SAFE: PLAY ONLY");
    return;
  }
  if (i <= MI_MELODY) {
    gotoRing(i, 0);
    uiToast("%s", MENU_NAME[i]);
    return;
  }
  switch (i) {
    case MI_TAKES:
      if (mode != M_HARM) gotoRing(2 + HM_PLAY, 0);
      harm::takes(true);
      break;
    case MI_SETTINGS: action(A_MORE); break;
    case MI_PHONE: action(A_PHONE); break;
    case MI_NEWSONG: action(A_NEWSONG); break;
    case MI_BUTTONS:
      btnLearn = 1;
      uiToast("PRESS THE TOP BUTTON");
      break;
    case MI_KEYPAD: action(A_CALIB); break;
    case MI_PRINT: harm::print(); break;
    case MI_CAPTURE: harm::shiftYes(); break;
    case MI_AB: harm::shiftNo(); break;
  }
}

void pushTurn(int d);
int heldChord();

void setPadsAlt(bool on) {                       // PADS: sounds <-> slots (Harmony) / functions
  padsAlt = on;
  if (mode == M_HARM) harm::slotLayer(on || knobHeld);
  else overlay = on || knobHeld;
}

bool stationMode() { return mode != M_HARM; }   // CHORD, BASS, EDIT, MORE: the middle is the bass

// ---- NOTES: single bass notes, and the key they point to
int notesBase() { return 36 + 12 * notesOct; }

void notesKey(int& home, int& k, int& pct) {      // home first, then the key that suits it best
  float P[12], H[12];
  ht::posterior(notesNs, P);
  ht::homePosterior(notesObs, P, H);
  float pmax = 0, hsum = 0;
  for (int i = 0; i < 12; i++) pmax = P[i] > pmax ? P[i] : pmax, hsum += H[i];
  home = 0;
  for (int pc = 1; pc < 12; pc++)
    if (H[pc] > H[home]) home = pc;
  k = -1;
  float best = -1e30f;
  for (int i = 0; i < 12; i++) {
    if (P[i] < pmax * 0.9f) continue;
    if (hsum > 0 && !(ht::collMask(i) & (1 << home))) continue;
    float sc[12];
    ht::homeScores(notesObs, i, sc);
    float v = hsum > 0 ? sc[home] : 0;
    if (k < 0 || v > best) best = v, k = i;
  }
  if (k < 0) {
    k = 0;
    for (int i = 1; i < 12; i++)
      if (P[i] > P[k]) k = i;
  }
  if (hsum <= 0) home = (ht::collTonic(k) + 9) % 12;
  pct = (int)(P[k] * 100 + 0.5f);
  if (pct > 95) pct = 95;
}

const char* modeName(int m) {
  static const char* const N[7] = {"major", "dorian", "phrygian", "lydian", "mixolydian", "minor", "locrian"};
  return m >= 0 && m < 7 ? N[m] : "?";
}

void notesKeyText(char* out, int cap) {
  if (!notesAny) {
    strlcpy(out, "play, then FITS / CLASH", cap);
    return;
  }
  int home, k, pct;
  notesKey(home, k, pct);
  snprintf(out, cap, "%s %s %d%%", ht::noteName(home, k), modeName(ht::modeOf(home, k)), pct);
}

void notesToggle() {
  notesOn = !notesOn;
  memset(notesHeld, -1, sizeof notesHeld);
  if (notesOn) {
    if (bass::running() && !bass::muted()) {       // the line rests while you find notes
      bass::togglePlay();
      notesMutedBass = true;
    }
    uiToast("NOTES: PADS PLAY SINGLE BASS NOTES");
  } else {
    if (notesMutedBass && bass::running() && bass::muted()) bass::togglePlay();
    notesMutedBass = false;
    uiToast("PADS: CHORDS");
  }
}

void notesVerdict(bool fits) {
  if (notesLast < 0) {
    uiToast("PLAY A NOTE FIRST");
    return;
  }
  int pc = notesLast % 12;
  ht::vote(notesNs, (uint16_t)(1 << pc), fits);
  notesAny = true;
  if (fits) {
    if (!notesFit) notesObs.first[pc] = 1;          // the first note that sits leans to home
    notesFit |= 1 << pc;
    notesClash &= ~(1 << pc);
    notesObs.bassDur[pc] += 1;
    notesObs.barStart[pc] += 1;
  } else {
    notesClash |= 1 << pc;
    notesFit &= ~(1 << pc);
  }
  char k[28];
  notesKeyText(k, sizeof k);
  uiToast("%s", k);
}

void notesUseKey() {                              // the key found becomes the key of chords and bass
  if (!notesAny) {
    uiToast("FITS / CLASH A FEW NOTES FIRST");
    return;
  }
  int home, k, pct;
  notesKey(home, k, pct);
  static const uint8_t SCALE_OF_MODE[7] = {1, 3, 4, 7, 5, 2, 2};   // MAJ DOR PHR LYD MIX MIN (locrian: MIN)
  int m = ht::modeOf(home, k);
  engine::paramSet(engine::P_ROOT, home);
  engine::paramSet(engine::P_SCALE, SCALE_OF_MODE[m < 0 ? 5 : m]);
  uiToast("KEY: %s %s", ht::noteName(home, k), modeName(m));
  notesNs.clear();
  notesObs.clear();
  notesFit = notesClash = 0;
  notesAny = false;
}

void notesPad(int p, bool down) {
  uint8_t ch = (uint8_t)bass::paramGet(bass::BP_CH);
  if (!down) {
    if (notesHeld[p] >= 0) midiOut(0x80 | ch, notesHeld[p], 0);
    notesHeld[p] = -1;
    return;
  }
  int n = notesBase() + p;
  if (n < 0 || n > 127) return;
  midiOut(0x90 | ch, n, 100);
  notesHeld[p] = (int8_t)n;
  notesLast = (int8_t)n;
}

// ---- the station's knob and buttons (CHORD and BASS: the middle is only the bass)
bool stationShift() { return knob2Down || shiftDown; }

void stationTurn(int d) {                         // the middle knob: the bass setting (NOTES: octave)
  if (notesOn) {
    notesOct = (int8_t)constrain(notesOct + d, -2, 2);
    return;
  }
  if (chordScreen() && heldChord() >= 0) {        // a chord held: its type
    engine::quality(heldChord(), d);
    return;
  }
  bass::paramAdjust(BASS_SETS[bassSetIdx % N_BASS_SETS], d);
}

void stationPushTurn(int d) {                     // held and turned: flip through the genres
  knob2Used = true;
  leftBass = true;
  bass::genreStep(d);
}

void stationClick() {
  if (knob2Used) return;                          // it was the SHIFT for a button
  if (notesOn) {
    notesUseKey();
    return;
  }
  bassSetIdx = (uint8_t)((bassSetIdx + 1) % N_BASS_SETS);   // the next setting
}

void stationHold() {}

// ---- the chord MENU on the screen: MAIN turn = move, click = open (a setting: turn it, click
// again to leave it), hold = close. 16 chord functions, then the 16 PERFORM controls.
const FuncDef& kmCell() { return (kmCursor < 16 ? CHORD_LAYER : PERF_LAYER)[kmCursor % 16]; }

void kmClose() {
  kmOpen = kmEdit = false;
}

void kmSelect() {
  const FuncDef& f = kmCell();
  if (f.owner == O_ACT) {
    if (f.id == A_MORE) kmClose();
    action(f.id);
    return;
  }
  kmEdit = !kmEdit;
  if (f.owner == O_BASS && f.id == bass::BP_TEMPO && kmEdit) {   // TEMPO: a tap is a tap tempo too
    clk::tap(nowUs());
  }
}

void kmTurn(int d) {
  if (kmEdit) {
    const FuncDef& f = kmCell();
    adjust(f.owner, f.id, d);
    return;
  }
  kmCursor = (uint8_t)(((kmCursor + d) % 32 + 32) % 32);
}

void button(int k, bool down, uint32_t now) {
  if (k < 0 || k > 3) return;
  btnIsDown[k] = down;
  if (down && btnLearn) {
    btnLearnPress(k);
    btnFired[k] = true;
    return;
  }
  uint8_t r = btnRole[k];
  if (!down) {
    if (harmNoK == k) {                           // Harmony NO: always tell the engine it let go
      harmNoK = -1;
      harm::no(false);
      return;
    }
    if (upDownK == k) upDownK = -1;
    if (btnFired[k]) return;
    btnFired[k] = true;                           // released before a hold: the tap
    if (mode == M_HARM || modeMenu || notesOn) return;
    if (r == P_3RD) bass::genreStep(1);           // GENRE (held: the previous one)
    else if (r == P_BOTTOM) action(A_PLAY);       // ON/OFF (held: STAR the line)
    return;
  }
  btnAt[k] = now;
  btnFired[k] = true;
  if (modeMenu) {
    if (r == P_3RD) menuOpen(modeSel);            // OPEN
    else modeMenu = false;                        // CLOSE
    return;
  }
  if (mode == M_HARM) {                           // SLOTS, NO, YES, MODES
    if (shiftDown) {
      shiftUsed = true;
      if (r == P_BOTTOM) action(A_MORE);
      else if (r == P_TOP) harm::print();
      else if (r == P_2ND) harm::shiftNo();
      else harm::shiftYes();
      return;
    }
    if (r == P_BOTTOM) {
      if (harm::takesOpen()) harm::takes(false);
      modeMenu = true;
      modeSel = (uint8_t)ringIndex();
    } else if (r == P_TOP) {
      if (harm::takesOpen()) harm::takes(false);
      setPadsAlt(!padsAlt);
    } else if (r == P_2ND) {
      harmNoK = (int8_t)k;
      harm::no(true);
    } else {
      harm::yes();
    }
    return;
  }
  // the bass station (browsing lines shows their page on the left screen)
  if (stationShift()) {                           // the middle knob held (or SHIFT): the second jobs
    knob2Used = true;
    if (shiftDown) shiftUsed = true;
    leftBass = r != P_BOTTOM && !notesOn;
    if (r == P_TOP) bass::pageJump(-1);           // PAGE UP
    else if (r == P_2ND) bass::pageJump(1);       // PAGE DOWN
    else if (r == P_3RD) bass::surprise();        // RANDOM
    else notesToggle();                           // NOTES
    return;
  }
  if (notesOn) {                                  // FITS, CLASH, USE KEY, ON/OFF
    if (r == P_TOP) notesVerdict(true);
    else if (r == P_2ND) notesVerdict(false);
    else if (r == P_3RD) notesUseKey();
    else action(A_PLAY);
    return;
  }
  if (r == P_TOP || r == P_2ND) {                 // UP / DOWN: the line before / after (held: repeats)
    leftBass = true;
    bass::lineStep(r == P_TOP ? -1 : 1);
    upDownK = (int8_t)k;
    upDownAt = now;
    return;
  }
  if (r == P_3RD) leftBass = true;
  btnFired[k] = false;                            // GENRE, ON/OFF: on release, or held
}

void buttonsTick(uint32_t now) {
  for (int k = 0; k < 4; k++) {
    if (upDownK == k && btnIsDown[k] && now - upDownAt >= BTN_HOLD_MS) {   // UP / DOWN repeat
      upDownAt = now - BTN_HOLD_MS + 180;
      bass::lineStep(btnRole[k] == P_TOP ? -1 : 1);
      dirty = true;
      continue;
    }
    if (!btnIsDown[k] || btnFired[k] || now - btnAt[k] < BTN_HOLD_MS) continue;
    btnFired[k] = true;
    if (mode == M_HARM) continue;                 // NO's hold is the engine's (undo)
    uint8_t r = btnRole[k];
    if (r == P_3RD) bass::genreStep(-1);          // GENRE held: the previous genre
    else if (r == P_BOTTOM) bass::star();         // ON/OFF held: STAR the line
    dirty = true;
  }
}

// the names of the four buttons, right now (drawn beside them on the OLED)
void softLabels(UiModel& m) {
  const char* L[4] = {"-", "-", "-", "MODES"};
  char yes[9] = "YES", no[9] = "NO";
  if (btnLearn) {
    for (int i = 0; i < 4; i++) strlcpy(m.soft[i], i == btnLearn - 1 ? "< PRESS" : "", sizeof m.soft[i]);
    return;
  }
  if (shiftDown && mode == M_HARM) {
    L[3] = "SETUP";
    L[0] = mode == M_HARM ? "PRINT" : "-";
    L[1] = mode == M_HARM ? "A/B" : "UNDO";
    L[2] = mode == M_HARM ? "CAPTURE" : "-";
  } else if (modeMenu) {
    L[1] = "CLOSE";
    L[2] = "OPEN";
    L[3] = "CLOSE";
  } else if (mode == M_HARM) {
    L[0] = padsAlt ? "SOUNDS" : "SLOTS";
    harm::yesNoWords(yes, no, sizeof yes);
    L[1] = no;
    L[2] = yes;
  } else if (stationShift()) {                   // the middle knob held
    L[0] = "PG UP";
    L[1] = "PG DN";
    L[2] = "RANDOM";
    L[3] = notesOn ? "LINES" : "NOTES";
  } else if (notesOn) {
    L[0] = "FITS";
    L[1] = "CLASH";
    L[2] = "USE KEY";
    L[3] = "ON/OFF";
  } else {
    L[0] = "UP";
    L[1] = "DOWN";
    L[2] = "GENRE";
    L[3] = "ON/OFF";
  }
  for (int i = 0; i < 4; i++) strlcpy(m.soft[i], L[i], sizeof m.soft[i]);
}

// ---------------------------------------------------------- the SET knob --
void pushTurn(int d);
int heldChord();
// Turn = change one setting, click = the next setting. No pushing and turning.
enum SetSpecial : uint8_t { SP_FOCUS, SP_INV, SP_QUAL, SP_VOICING };
struct SetItem { uint8_t owner, id; };            // owner O_NONE = special
const SetItem SET_CHORD[] = {{O_NONE, SP_FOCUS}, {O_NONE, SP_INV}, {O_NONE, SP_QUAL}, {O_NONE, SP_VOICING},
                             {O_CHORD, engine::P_ROOT}, {O_CHORD, engine::P_SCALE}, {O_CHORD, engine::P_OCT}};
const SetItem SET_BASS[] = {{O_NONE, SP_FOCUS}, {O_BASS, bass::BP_OCT}, {O_BASS, bass::BP_LEN},
                            {O_BASS, bass::BP_SWING}, {O_BASS, bass::BP_FOLLOW}, {O_BASS, bass::BP_DENS}};

int setSlot() {
  if (mode == M_HARM) return 3 + harm::mode();
  return mode == M_BASS ? 1 : mode == M_EDIT ? 2 : 0;
}

const SetItem* legacySet(int& n) {
  if (mode == M_BASS) {
    n = sizeof SET_BASS / sizeof SET_BASS[0];
    return SET_BASS;
  }
  n = sizeof SET_CHORD / sizeof SET_CHORD[0];
  return SET_CHORD;
}

int setCountNow() {
  if (mode == M_HARM) return harm::setCount();
  if (mode == M_EDIT || mode == M_MORE || mode == M_CALIB) return 1;
  int n;
  legacySet(n);
  return n;
}

void setLabelNow(char* label, int lc, char* val, int vc) {
  label[0] = val[0] = 0;
  if (stationMode() && mode != M_MORE && mode != M_EDIT && mode != M_CALIB && !shiftDown) {
    if (notesOn) {                                // the middle knob in NOTES: the pads' octave
      strlcpy(label, "OCTAVE", lc);
      snprintf(val, vc, "C%d", notesBase() / 12 - 2);
      return;
    }
    if (bassSet) {
      ParamView k;
      bass::paramView(BASS_SETS[bassSetIdx % N_BASS_SETS], k);
      strlcpy(label, k.label, lc);
      strlcpy(val, k.value, vc);
      return;
    }
    BassView bv;
    bass::bassView(bv);
    strlcpy(label, "BASS", lc);
    snprintf(val, vc, "%s %s", bv.name[0] ? bv.name : "-", !bass::running() ? "off" : bass::muted() ? "muted" : "on");
    return;
  }
  int i = setIdx[setSlot()];
  if (perf || shiftDown) {                        // the PERFORM layer: its focused control
    ParamView k;
    paramViewOf(fOwner(), fId(), k);
    strlcpy(label, k.label, lc);
    strlcpy(val, k.value, vc);
    return;
  }
  if (mode == M_HARM) {
    harm::setInfo(i, label, lc, val, vc);
    return;
  }
  if (mode == M_MORE) {
    strlcpy(label, "CHANGE", lc);
    return;
  }
  if (mode == M_EDIT) {
    strlcpy(label, "NOTE", lc);
    return;
  }
  int n;
  const SetItem* L = legacySet(n);
  const SetItem& it = L[i % n];
  if (it.owner == O_NONE) {
    switch (it.id) {
      case SP_FOCUS: {
        ParamView k;
        paramViewOf(focusOwner[modeSlot()], focusId[modeSlot()], k);
        strlcpy(label, k.label, lc);
        strlcpy(val, k.value, vc);
        return;
      }
      case SP_INV:
        strlcpy(label, "INVERSION", lc);
        snprintf(val, vc, "%+d", engine::inversion());
        return;
      case SP_QUAL:
        strlcpy(label, "CHORD TYPE", lc);
        strlcpy(val, "hold a pad", vc);
        return;
      case SP_VOICING: {
        ParamView k;
        engine::paramView(engine::P_VOIC, k);
        strlcpy(label, "VOICING", lc);
        strlcpy(val, k.value, vc);
        return;
      }
    }
  }
  ParamView k;
  paramViewOf(it.owner, it.id, k);
  strlcpy(label, k.label, lc);
  strlcpy(val, k.value, vc);
}

void setTurn(int d) {
  if (!d) return;
  if (shiftDown || perf) {                        // SHIFT held: the PERFORM focus
    shiftUsed = perf = true;
    adjust(fOwner(), fId(), d);
    focusUntil = millis() + FOCUS_MS;
    return;
  }
  if (modeMenu) {
    modeSel = (uint8_t)(((modeSel + d) % MI_COUNT + MI_COUNT) % MI_COUNT);
    return;
  }
  if (mode == M_CALIB) return;
  if (mode == M_MORE || mode == M_EDIT || overlay) {   // the selected setting / step / function
    pushTurn(d);
    return;
  }
  if (mode == M_HARM) {
    harm::setAdjust(setIdx[setSlot()], d);
    return;
  }
  if (chordScreen() && heldChord() >= 0) {        // a chord held: its type
    engine::quality(heldChord(), d);
    return;
  }
  int n;
  const SetItem* L = legacySet(n);
  const SetItem& it = L[setIdx[setSlot()] % n];
  if (it.owner == O_NONE) {
    switch (it.id) {
      case SP_FOCUS: pushTurn(d); return;
      case SP_INV: engine::shapeTurn(d); return;
      case SP_QUAL: engine::shapeQuality(d); return;
      case SP_VOICING: {
        ParamView k;
        engine::paramView(engine::P_VOIC, k);
        int v = engine::paramGet(engine::P_VOIC) + d;
        if (v > k.hi) v = k.lo;
        if (v < k.lo) v = k.hi;
        engine::paramSet(engine::P_VOIC, v);
        return;
      }
    }
  }
  adjust(it.owner, it.id, d);
  focusUntil = millis() + FOCUS_MS;
}

void setClick() {                                 // the next setting in the list
  if (modeMenu) {
    menuOpen(modeSel);
    return;
  }
  int n = setCountNow();
  if (n <= 1) return;
  uint8_t& i = setIdx[setSlot()];
  i = (uint8_t)((i + 1) % n);
  char lb[12], vb[16];
  setLabelNow(lb, sizeof lb, vb, sizeof vb);
  uiToast("SET: %s", lb);
}

bool bassScreen() { return mode == M_BASS && input::keypads() == 1; }

int heldBass() {
  for (int p = 0; p < 32; p++)
    if (bassDown & (1u << p)) return p % 16;
  return -1;
}

// ------------------------------------------------------------------ pads --
bool chordPads(int pad) {                      // does this pad play chords right now?
  if (input::keypads() == 2) return pad < 16;
  return mode == M_CHORD || (mode == M_MORE && backMode == M_CHORD);
}

void bassPadDown(int local) {
  if (mode == M_EDIT) {
    heldStep = local;
    heldTurned = false;
    editSel = local;
    BassView v;
    bass::bassView(v);
    int i = editBar * 16 + local;
    heldExisting = i < v.len && (v.step[i].flags & SF_NOTE);
    if (!heldExisting) bass::editToggle(editBar, local);   // empty step: a root note
    return;
  }
  bass::padDown(local);
}

void bassPadUp(int local) {
  if (mode == M_EDIT && heldStep == local) {
    if (heldExisting && !heldTurned) bass::editToggle(editBar, local);   // tap on a note: remove
    heldStep = -1;
  }
}

void funcPad(int local) {
  const FuncDef& f = layer()[local];
  if (f.owner == O_ACT) {
    if (padsAlt && !perf) setPadsAlt(false);     // MENU latched: an action closes it
    action(f.id);
    return;
  }
  funcFocus = local;
  fOwner() = f.owner;
  fId() = f.id;
  focusUntil = millis() + FOCUS_MS;
  if (f.owner == O_SYS && f.id == S_SCENE) {   // SCENE: tap loads, hold saves (see tick)
    scenePad = local;
    sceneAt = millis();
  }
  if (f.owner == O_BASS && f.id == bass::BP_TEMPO) {  // tapping TEMPO taps the tempo
    clk::tap(nowUs());
    bass::paramSet(bass::BP_TEMPO, (int)(clk::internalBpm() + 0.5f));
  }
}

void padDown(int pad) {
  if (mode == M_CALIB) return;
  if (modeMenu && mode == M_HARM) {               // MODES in a Harmony mode: a pad opens its tile
    padConsumed[pad] = true;
    menuOpen(pad % 16);
    return;
  }
  if (mode == M_HARM && harm::inSlotLayer()) {   // the knob is held: the pads are the slots
    padConsumed[pad] = true;
    if (shiftDown) shiftUsed = true;
    harm::slotPad(pad % 16, true, shiftDown);
    return;
  }
  if (mode == M_HARM && !shiftDown) {
    padConsumed[pad] = false;
    harm::pad(pad % 16, true);
    return;
  }
  if (mode == M_CHORD && !shiftDown) {         // a pad plays, and the screen comes back to the chords
    leftBass = false;
    modeMenu = false;
    if (kmOpen && !kmEdit) kmClose();            // (still turning a value: keep it, hear it)
  }
  if (shiftDown || perfLatch) {                // KEY0 + pad (or PERFORM opened by a click)
    if (shiftDown) shiftUsed = true;
    perf = true;
    padConsumed[pad] = true;
    funcPad(pad % 16);
    return;
  }
  if (notesOn && !overlay && (mode == M_CHORD || mode == M_BASS)) {   // NOTES: single bass notes
    padConsumed[pad] = true;
    notesPad(pad % 16, true);
    return;
  }
  if (overlay) {
    padConsumed[pad] = true;
    funcPad(pad % 16);
    return;
  }
  padConsumed[pad] = false;
  if (mode == M_MORE) {                        // any pad closes the settings list (and plays, back in CHORD)
    setMode(backMode);
    if (mode != M_CHORD) {
      padConsumed[pad] = true;
      return;
    }
  }
  if (chordPads(pad)) {
    engine::padDown(pad % 16);
    chordDown |= 1u << (pad % 16);
    lastChordPad = pad % 16;
  } else {
    if (mode == M_BASS) bassDown |= 1u << pad;
    bassPadDown(pad % 16);
  }
}

int heldChord() {                              // the chord pad under a finger now, or -1
  if (lastChordPad >= 0 && (chordDown & (1u << lastChordPad))) return lastChordPad;
  for (int p = 0; p < 16; p++)
    if (chordDown & (1u << p)) return p;
  return -1;
}

void padUp(int pad) {
  bassDown &= ~(1u << pad);
  if (mode == M_HARM) {
    if (!padConsumed[pad]) {
      harm::pad(pad % 16, false);
      return;
    }
    harm::slotPad(pad % 16, false, false);       // a slot, or a PERFORM pad: handled below
  }
  if (notesOn) notesPad(pad % 16, false);
  if (repeatClick && repeatPad == pad % 16) {    // the repeated chord lets go
    repeatClick = false;
    repeatPad = -1;
    engine::setRepeat(false);
  }
  if (padConsumed[pad]) {
    padConsumed[pad] = false;
    if (scenePad == pad % 16) {                  // SCENE let go before the hold: load it
      scenePad = -1;
      sceneLoad(AP.scene);
    }
    return;
  }
  if (chordPads(pad)) {
    chordDown &= ~(1u << (pad % 16));
    engine::padUp(pad % 16);
  } else {
    bassPadUp(pad % 16);
  }
}

// --------------------------------------------------------------- encoder --
void turn(int d) {
  if (heldChord() >= 0 && chordScreen()) {    // hold a chord + turn: its inversion
    engine::revoice(heldChord(), d);
    return;
  }
  switch (mode) {
    case M_CHORD: engine::pageMove(d); break;
    case M_BASS: bass::pageMove(d); break;
    case M_EDIT:
      if (heldStep >= 0) {                     // hold a step + turn: its note
        bass::editRole(editBar, heldStep, d);
        heldTurned = true;
      } else {
        editSel = ((editSel + d) % 16 + 16) % 16;
      }
      break;
    case M_MORE: {
      int n;
      moreList(n);
      moreSel = constrain(moreSel + d, 0, n - 1);
      break;
    }
    case M_CALIB: break;
  }
}

void pushTurn(int d) {
  if (mode == M_MORE) {
    int n;
    const MoreItem* L = moreList(n);
    const MoreItem& it = L[moreSel];
    if (it.owner != O_ACT) adjust(it.owner, it.id, d);
    return;
  }
  if (mode == M_EDIT && !overlay) {            // push + turn in EDIT: the selected note
    bass::editRole(editBar, editSel, d);
    return;
  }
  if (mode == M_CALIB) return;
  if (!perf && !overlay && heldChord() >= 0 && chordScreen()) {   // hold a chord + push-turn: quality
    engine::quality(heldChord(), d);
    return;
  }
  adjust(fOwner(), fId(), d);
  focusUntil = millis() + FOCUS_MS;
}

void click() {
  switch (mode) {
    case M_CHORD: setMode(M_BASS); break;
    case M_BASS: setMode(M_CHORD); break;
    case M_EDIT: setMode(M_BASS); break;
    case M_MORE: {
      int n;
      const MoreItem* L = moreList(n);
      if (L[moreSel].owner == O_ACT) action(L[moreSel].id);
      else setMode(backMode);
      break;
    }
    case M_CALIB:                               // skip a dead key
      calDone |= 1 << calNext;
      if (++calNext >= 16) {
        if (calKp + 1 < input::keypads()) {
          calKp++;
          calNext = 0;
          calDone = 0;
        } else {
          finishCalibration();
        }
      }
      break;
  }
}

void rawDown(int raw) {                          // calibration press: raw = wire pair
  if (mode != M_CALIB) return;
  int cur = calKp * 16 + calNext;
  for (int p = 0; p < cur; p++)
    if (calPair[p] == raw) return;               // that key is already taken
  calPair[cur] = raw;
  calDone |= 1 << calNext;
  if (++calNext >= 16) {
    if (calKp + 1 < input::keypads()) {
      calKp++;
      calNext = 0;
      calDone = 0;
    } else {
      finishCalibration();
    }
  }
}

// ------------------------------------------------------------ KEY0 = SHIFT --
void key0(bool down, uint32_t now) {
  if (mode == M_CALIB) return;
  if (down) {
    if (heldChord() >= 0 && chordScreen()) {     // a chord held first: REPEAT it
      repeatKey = true;
      engine::setRepeat(true);
      return;
    }
    shiftDown = true;
    shiftUsed = false;
    shiftAt = now;
    tempKeep = false;
    for (int p = 0; p < engine::P_COUNT; p++) tempChord[p] = engine::paramGet(p);
    for (int p = 0; p < bass::BP_COUNT; p++) tempBass[p] = bass::paramGet(p);
    return;
  }
  if (repeatKey) {
    repeatKey = false;
    engine::setRepeat(false);
    return;
  }
  if (!shiftDown) return;
  shiftDown = false;
  perf = perfLatch;
  if (AP.temp && shiftUsed && !tempKeep) {       // TEMP EDITS: back to how it was
    applySettings(tempChord, tempBass);
    uiToast("TEMP: PUT BACK");
  }
  if (shiftUsed) return;                         // it was SHIFT for a pad or the knob
  action(A_PLAY);                                // KEY0 on its own: bass on / off
}

// ----------------------------------------------------------------- model --
void fillFunc(UiModel& m) {
  const FuncDef* L = layer();
  for (int i = 0; i < 16; i++) {
    FuncCell& c = m.func[i];
    strlcpy(c.label, L[i].label, sizeof c.label);
    c.icon = L[i].icon;
    c.group = L[i].group;
    c.kind = L[i].owner == O_ACT ? K_ACTION : K_NUM;
    c.on = false;
    c.value[0] = 0;
    if (L[i].owner != O_ACT) {
      ParamView pv;
      paramViewOf(L[i].owner, L[i].id, pv);
      strlcpy(c.value, pv.value, sizeof c.value);
    }
    if (L[i].owner == O_ACT) {
      if (L[i].id == A_LATCH) c.on = engine::latched();
      if (L[i].id == A_LOOP) c.on = Mp->chord.loopState != 0;
      if (L[i].id == A_PLAY) c.on = bass::running() && !bass::muted();
      if (L[i].id == A_EDIT) c.on = mode == M_EDIT;
    }
  }
  m.funcFocus = -1;
  for (int i = 0; i < 16; i++)
    if (L[i].owner == fOwner() && L[i].id == fId()) m.funcFocus = i;
  paramViewOf(fOwner(), fId(), m.focus);
  if (kmOpen && mode == M_CHORD) {               // the screen MENU: the cursor is the focus
    m.funcFocus = kmCursor % 16;
    const FuncDef& f = L[kmCursor % 16];
    if (f.owner != O_ACT) paramViewOf(f.owner, f.id, m.focus);
    else {
      memset(&m.focus, 0, sizeof m.focus);
      strlcpy(m.focus.label, f.label, sizeof m.focus.label);
      m.focus.kind = K_ACTION;
    }
  }
}

void publish(uint32_t now) {
  UiModel& m = *Mp;
  m.mode = mode;
  m.overlay = phoneInfo ? OV_PHONE : modeMenu ? OV_MODES : (kmOpen && mode == M_CHORD) ? (kmCursor < 16 ? OV_FUNC : OV_PERF)
            : perf ? OV_PERF : overlay ? OV_FUNC : OV_NONE;
  m.leftBass = leftBass && mode == M_CHORD;
  m.kmOpen = kmOpen && mode == M_CHORD;
  m.kmEdit = kmEdit;
  web::Info wi;
  web::info(wi);
  strlcpy(m.wifi.ssid, wi.ssid, sizeof m.wifi.ssid);
  strlcpy(m.wifi.url, wi.url, sizeof m.wifi.url);
  strlcpy(m.wifi.pass, wi.pass, sizeof m.wifi.pass);
  m.wifi.hotspot = wi.hotspot;
  m.wifi.on = wi.on;
  m.wifi.phones = wi.phones;
  m.bassSide = mode == M_BASS || mode == M_EDIT || (mode == M_CHORD && leftBass && !notesOn) ||
               ((mode == M_MORE || mode == M_CALIB) && backMode != M_CHORD);
  strlcpy(m.key, engine::keyName(), sizeof m.key);
  strlcpy(m.scale, engine::scaleName(), sizeof m.scale);
  m.moveUp = moveUp;
  m.keypads = input::keypads();
  m.screenFlip = input::screenFlip();

  m.clk.ext = clk::external();
  m.clk.running = bass::running();
  m.clk.bpm10 = (uint16_t)(clk::bpm() * 10 + 0.5f);
  m.clk.step = clk::step();
  m.clk.stepAtMs = (uint32_t)(clk::stepAtUs() / 1000);
  m.clk.stepMs = clk::stepUs() / 1000;
  m.clk.swing = bass::paramGet(bass::BP_SWING);

  engine::chordView(m.chord);
  bass::bassView(m.bass);
  fillFunc(m);
  m.focusUntil = focusUntil;

  ParamView k;
  if (mode == M_CHORD) {
    strlcpy(m.turnLabel, "PAGE", sizeof m.turnLabel);
    snprintf(m.turnValue, sizeof m.turnValue, "%d/%d", m.chord.page + 1, m.chord.pages);
  } else if (mode == M_BASS) {
    strlcpy(m.turnLabel, m.bass.genPage ? "GEN PAGE" : "PAGE", sizeof m.turnLabel);
    snprintf(m.turnValue, sizeof m.turnValue, "%d/%d", m.bass.page + 1, m.bass.pages);
  } else if (mode == M_EDIT) {
    strlcpy(m.turnLabel, heldStep >= 0 ? "NOTE" : "STEP", sizeof m.turnLabel);
    snprintf(m.turnValue, sizeof m.turnValue, "%02d", editSel + 1);
  } else {
    strlcpy(m.turnLabel, "SELECT", sizeof m.turnLabel);
    m.turnValue[0] = 0;
  }
  softLabels(m);
  m.shiftHeld = shiftDown || (knob2Down && stationMode());
  m.modeSel = modeSel;
  m.padsAlt = padsAlt;
  setLabelNow(m.setLabel, sizeof m.setLabel, m.setValue, sizeof m.setValue);
  m.setN = (uint8_t)setCountNow();
  m.setPos = (uint8_t)(m.setN ? setIdx[setSlot()] % m.setN : 0);
  if (stationMode() && mode != M_MORE && mode != M_EDIT) {
    m.setN = bassSet ? N_BASS_SETS : 1;
    m.setPos = bassSet ? bassSetIdx % N_BASS_SETS : 0;
  }
  m.bassSet = bassSet;
  m.notes.on = notesOn;
  m.notes.base = (uint8_t)notesBase();
  m.notes.last = notesLast;
  m.notes.fit = notesFit;
  m.notes.clash = notesClash;
  notesKeyText(m.notes.key, sizeof m.notes.key);
  strlcpy(m.clickLabel, mode == M_HARM ? (harm::takesOpen() ? "LOAD" : harm::playing() ? "STOP" : "PLAY")
                        : mode == M_CHORD ? (perfLatch ? "CLOSE" : overlay ? "PERFORM" : "MENU")
                        : mode == M_BASS ? "CHORD" : mode == M_EDIT ? "DONE"
                        : mode == M_MORE ? "OK" : "SKIP", sizeof m.clickLabel);
  if (modeMenu) strlcpy(m.clickLabel, "OPEN", sizeof m.clickLabel);
  if (modeMenu && !stationMode()) {              // Harmony: the middle knob picks a tile too
    strlcpy(m.setLabel, "PICK", sizeof m.setLabel);
    strlcpy(m.setValue, MENU_NAME[modeSel % MI_COUNT], sizeof m.setValue);
    m.setN = 1;
  }
  if (mode == M_HARM) {
    harm::view(m.harm);
    strlcpy(m.turnLabel, m.harm.valueLabel, sizeof m.turnLabel);
    strlcpy(m.turnValue, m.harm.valueText, sizeof m.turnValue);
    strlcpy(m.pushLabel, m.harm.pushLabel, sizeof m.pushLabel);
    strlcpy(m.pushValue, m.harm.pushText, sizeof m.pushValue);
  } else if (mode == M_EDIT) {
    strlcpy(m.pushLabel, "NOTE", sizeof m.pushLabel);
    int i = editBar * 16 + editSel;
    strlcpy(m.pushValue, i < m.bass.len && m.bass.step[i].flags & SF_NOTE ? m.bass.step[i].role : "-", sizeof m.pushValue);
  } else if (mode == M_MORE) {
    strlcpy(m.pushLabel, "CHANGE", sizeof m.pushLabel);
    m.pushValue[0] = 0;
  } else {
    strlcpy(m.pushLabel, m.focus.label, sizeof m.pushLabel);
    strlcpy(m.pushValue, m.focus.value, sizeof m.pushValue);
  }

  if (kmOpen && mode == M_CHORD && !modeMenu) {
    strlcpy(m.turnLabel, kmEdit ? "SET" : "MENU", sizeof m.turnLabel);
    strlcpy(m.turnValue, layer()[kmCursor % 16].label, sizeof m.turnValue);
    strlcpy(m.clickLabel, kmEdit ? "DONE" : "OPEN", sizeof m.clickLabel);
  }
  if (modeMenu) {
    strlcpy(m.turnLabel, "PICK", sizeof m.turnLabel);
    strlcpy(m.turnValue, MENU_NAME[modeSel % MI_COUNT], sizeof m.turnValue);
  } else if (m.leftBass) {                       // the bass page: MAIN (or a pad) brings the chords back
    strlcpy(m.turnLabel, "BACK", sizeof m.turnLabel);
    strlcpy(m.turnValue, "CHORDS", sizeof m.turnValue);
    m.clickLabel[0] = 0;
  }
  strlcpy(m.toast, toastBuf, sizeof m.toast);
  m.toastUntil = toastUntil;
  m.padMask = input::padMask();
  m.editStep = mode == M_EDIT ? editSel : -1;
  m.editBar = editBar;
  m.editBars = bass::bars();

  if (mode == M_MORE) {
    int n;
    const MoreItem* L = moreList(n);
    m.moreN = min(n, 40);
    m.moreSel = moreSel;
    strlcpy(m.moreTitle, backMode == M_CHORD ? "CHORD" : "BASS", sizeof m.moreTitle);
    for (int i = 0; i < m.moreN; i++) {
      if (L[i].owner == O_ACT) {
        memset(&m.more[i], 0, sizeof m.more[i]);
        strlcpy(m.more[i].label, L[i].label, sizeof m.more[i].label);
        strlcpy(m.more[i].value, "CLICK", sizeof m.more[i].value);
        m.more[i].kind = K_ACTION;
        m.more[i].group = G_SYS;
      } else {
        paramViewOf(L[i].owner, L[i].id, m.more[i]);
        if (L[i].label) strlcpy(m.more[i].label, L[i].label, sizeof m.more[i].label);
      }
    }
  }
  m.calKeypad = calKp;
  m.calNext = calNext;
  m.calDone = calDone;
  m.inversion = engine::inversion();
  m.btnLearn = btnLearn;
  m.btnHeld = 0;
  for (int k = 0; k < 4; k++)
    if (btnIsDown[k]) m.btnHeld |= 1 << btnRole[k];
  (void)k;
  (void)now;

  MODEL_LOCK();
  m.seq = ++seq;
  *Gp = m;
  MODEL_UNLOCK();
}

}  // namespace

void uiToast(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(toastBuf, sizeof toastBuf, fmt, ap);
  va_end(ap);
  toastUntil = millis() + TOAST_MS;
  dirty = true;
}

void modelRead(UiModel& out) {
  MODEL_LOCK();
  if (!Gp) {
    memset(&out, 0, sizeof out);
    MODEL_UNLOCK();
    return;
  }
  out = *Gp;
  MODEL_UNLOCK();
}

void app::begin() {
  bigOnce(Mp);
  bigOnce(Gp);
  memset(padConsumed, 0, sizeof padConsumed);
  loadAppPrefs();
  loadButtons();
  harm::begin(input::key0AtBoot());
  clk::setFollowMove(!AP.clockInt);
  whereRestore();
  if (AP.wifi) web::begin();
  if (mode == M_HARM) harm::enter(harm::mode());
  if (input::heldAtBoot()) startCalibration();  // the way in when the pads are miswired
  if (harm::liveSafe()) uiToast("LIVE-SAFE: PLAY ONLY");
  dirty = true;
}

// MIDI from the Move on MIDI IN CH: a note sets the key, a program change the bank
uint8_t app::scenesUsed() {
  uint8_t bits = 0;
  for (int i = 0; i < N_SCENES; i++) {
    Scene s;
    if (sceneRead(i, s)) bits |= 1 << i;
  }
  return bits;
}

uint8_t app::sceneSelected() { return AP.scene; }

void app::midiIn(uint8_t st, uint8_t d1, uint8_t d2) {
  if (!AP.inCh || (st & 0x0F) != AP.inCh - 1) return;
  if ((st & 0xF0) == 0x90 && d2) {
    engine::paramSet(engine::P_ROOT, d1 % 12);
    uiToast("KEY %s FROM MIDI", engine::keyName());
  } else if ((st & 0xF0) == 0xC0) {
    engine::paramSet(engine::P_BANK, d1);
    ChordView v;
    engine::chordView(v);
    uiToast("BANK %s", v.bank);
  }
  dirty = true;
}

void app::setMoveUp(bool up) {
  if (up != moveUp) {
    moveUp = up;
    dirty = true;
  }
}

void app::handle(const Event& e) {
  if (phoneInfo && (e.type == EV_PAD_DOWN || e.type == EV_CLICK || e.type == EV_TURN || e.type == EV_HOLD ||
                    e.type == EV_PUSH_TURN || (e.type == EV_KEY && e.b) || e.type == EV_TURN2 ||
                    e.type == EV_PUSH_TURN2 || e.type == EV_CLICK2 || (e.type == EV_BTN && e.b))) {
    phoneInfo = false;                           // any control closes PHONE LINK
    if (e.type == EV_PAD_DOWN) padConsumed[e.a] = true;
    if (e.type == EV_BTN) btnFired[e.a & 3] = btnIsDown[e.a & 3] = true;
    dirty = true;
    return;
  }
  // the left controls (MAIN, SHIFT) always bring the left screen back to the chords
  bool leftCtl = e.type == EV_TURN || e.type == EV_CLICK || e.type == EV_HOLD || e.type == EV_PUSH_TURN ||
                 (e.type == EV_KEY && e.b);
  if (leftCtl && leftBass) {
    leftBass = false;
    if (e.type != EV_KEY) {                      // that first touch only brings the chords back
      if (e.type == EV_HOLD) knobHeld = true;
      dirty = true;
      return;
    }
  }
  if (mode == M_CHORD && kmOpen && !modeMenu) {  // the chord MENU on the screen
    switch (e.type) {
      case EV_TURN:
      case EV_PUSH_TURN: kmTurn(e.b); dirty = true; return;
      case EV_CLICK: kmSelect(); dirty = true; return;
      case EV_HOLD: kmClose(); knobHeld = true; dirty = true; return;
      case EV_RELEASE: knobHeld = false; dirty = true; return;
      default: break;
    }
  }
  switch (e.type) {
    case EV_PAD_DOWN: padDown(e.a); break;
    case EV_PAD_UP: padUp(e.a); break;
    case EV_RAW_DOWN: rawDown(e.a); break;
    case EV_TURN:
      if (shiftDown || perfLatch) {            // KEY0 + turn (or PERFORM opened by a click): its focus
        if (shiftDown) shiftUsed = true;
        perf = true;
        pushTurn(e.b);
      } else if (modeMenu) {
        modeSel = (uint8_t)(((modeSel + e.b) % MI_COUNT + MI_COUNT) % MI_COUNT);
      } else if (mode == M_HARM) {
        harm::value(e.b);
      } else if (overlay) {
        pushTurn(e.b);                         // turning during a hold edits the focus
      } else {
        turn(e.b);
      }
      break;
    case EV_PUSH_TURN: setTurn(e.b); break;     // pushed and turned: the same as the SET knob
    case EV_CLICK:
      if (modeMenu) {
        menuOpen(modeSel);
      } else if (mode == M_HARM) {               // KEY0 + click: PRINT; click: play / stop
        if (shiftDown) {
          shiftUsed = true;
          harm::print();
        } else {
          harm::click();
        }
      } else if (shiftDown) {                    // KEY0 + click: UNDO (TEMP EDITS: KEEP)
        shiftUsed = true;
        if (AP.temp) {
          tempKeep = true;
          uiToast("KEPT");
        } else {
          undo();
        }
      } else if (chordScreen() && heldChord() >= 0) {   // a chord held + click: REPEAT it till it lets go
        repeatClick = true;
        repeatPad = (int8_t)heldChord();
        engine::setRepeat(true);
      } else if (mode == M_CHORD) {              // click: the chord MENU, on the screen
        kmOpen = true;
        kmEdit = false;
      } else if (bassScreen() && heldBass() >= 0) {     // hold a bass line + click: STAR it
        bass::starAt(heldBass());
      } else {
        click();
      }
      break;
    case EV_HOLD:
      if (shiftDown) {                           // KEY0 + hold the knob: STOP ALL
        shiftUsed = true;
        action(A_STOPALL);
        break;
      }
      if (modeMenu) {                            // held again: close the menu
        modeMenu = false;
        break;
      }
      knobHeld = true;
      if (mode == M_CHORD) {                     // held: MODES, on the screen (the pads stay keys)
        modeMenu = true;
        modeSel = (uint8_t)ringIndex();
        break;
      }
      if (mode == M_HARM) {                      // held: the pads become the 16 slots (or tap PADS)
        harm::slotLayer(true);
        break;
      }
      if (mode != M_MORE && mode != M_CALIB) overlay = true;
      if (mode == M_CALIB) {                    // hold during setup: give up
        input::calibrate(false);
        mode = M_CHORD;
        uiToast("KEYPAD SETUP CANCELLED");
      }
      break;
    case EV_RELEASE:
      knobHeld = false;
      if (mode == M_HARM) harm::slotLayer(padsAlt);
      else if (mode != M_CHORD) overlay = padsAlt;
      break;
    case EV_SET: engine::paramSet(e.a, e.b); break;
    case EV_BSET: bass::paramSet(e.a, e.b); break;
    case EV_ACTION:
      if (e.a == A_SCENEN || e.a == A_SAVESCENEN) {   // from the phone: scene by number
        AP.scene = constrain(e.b, 0, N_SCENES - 1);
        saveAppPrefs();
        if (e.a == A_SCENEN) sceneLoad(AP.scene);
        else sceneSave(AP.scene);
      } else if (e.a == A_KEY0) {
        key0(e.b, millis());
      } else if (e.a == A_HMODE) {
        gotoRing(e.b, 0);
      } else {
        action(e.a);
      }
      break;
    case EV_KEY: key0(e.b, millis()); break;
    case EV_KNOB2:                              // the middle knob down / up: the station's SHIFT
      knob2Down = e.b;
      if (e.b) knob2Used = false;
      break;
    case EV_TURN2:                              // the middle knob: the bass station (Harmony: SET)
      if (stationMode() && mode != M_MORE && mode != M_EDIT && mode != M_CALIB && !shiftDown) stationTurn(e.b);
      else setTurn(e.b);
      break;
    case EV_PUSH_TURN2:
      if (stationMode() && mode != M_MORE && mode != M_EDIT && mode != M_CALIB) stationPushTurn(e.b);
      else setTurn(e.b);
      break;
    case EV_CLICK2:
      if (stationMode() && mode != M_MORE && mode != M_EDIT && mode != M_CALIB) stationClick();
      else setClick();
      break;
    case EV_HOLD2:
      if (stationMode() && mode != M_CALIB) stationHold();
      else if (mode == M_HARM) harm::tapOne();  // Harmony: tap "1", the bar starts here
      break;
    case EV_BTN:
      if (mode != M_CALIB) button(e.a, e.b, millis());
      else btnIsDown[e.a & 3] = e.b;
      break;
  }
  dirty = true;
}

void app::tick(int64_t nowUs, uint32_t now) {
  if (shiftDown && !perf && now - shiftAt >= KEY0_SHOW_MS) {   // KEY0 held: show PERFORM
    perf = true;
    dirty = true;
  }
  if (scenePad >= 0 && now - sceneAt >= 800) {   // SCENE held: save it
    scenePad = -1;
    sceneSave(AP.scene);
  }
  whereTick(now);
  buttonsTick(now);
  harm::tick(nowUs, now);
  if (mode == M_HARM && now - lastPublishMs > 120) dirty = true;   // live readouts (playhead, sure %)
  char note[32];
  if (web::takeNotice(note, sizeof note)) uiToast("%s", note);
  uint32_t st = clk::step();
  if (st != lastStepSeen) {                      // playhead moved
    lastStepSeen = st;
    dirty = true;
  }
  if (dirty || now - lastPublishMs > 250) {
    dirty = false;
    lastPublishMs = now;
    publish(now);
  }
}

int app::actionIndex(const String& n) {
  static const struct { const char* name; uint8_t id; } A[] = {
      {"latch", A_LATCH}, {"alloff", A_ALLOFF}, {"more", A_MORE}, {"star", A_STAR}, {"gen", A_GEN},
      {"mutate", A_MUTATE}, {"favs", A_FAVS}, {"bstar", A_BSTAR}, {"bmutate", A_BMUTATE},
      {"edit", A_EDIT}, {"play", A_PLAY}, {"resync", A_RESYNC}, {"panic", A_PANIC}, {"calib", A_CALIB},
      {"done", A_E_DONE}, {"plain", A_PLAIN}, {"loop", A_LOOP}, {"stopall", A_STOPALL},
      {"undo", A_UNDO}, {"savescene", A_SAVESCENE}, {"scene", A_SCENEN}, {"storescene", A_SAVESCENEN},
      {"key0", A_KEY0}, {"phone", A_PHONE}, {"buttons", A_BTNSETUP}, {"mode", A_HMODE}, {"newsong", A_NEWSONG}};
  for (auto& a : A)
    if (n.equalsIgnoreCase(a.name)) return a.id;
  return -1;
}

void app::printHelp(Print& p) {
  p.println(
      "ChordBox console\n"
      "  state | bass | pattern | harm | favs | mem | perf | reboot\n"
      "  act mode N        0 CHORD 1 BASS 2 HUNT 3 LISTEN 4 PLAY 5 GROW 6 MELODY;  act newsong\n"
      "  pad N / up N      press / release pad N (1-16 keypad A, 17-32 keypad B)\n"
      "  tap N [ms]        press, release after ms (default 200)\n"
      "  turn D | push D   encoder turn / push+turn by D detents\n"
      "  click | hold | release\n"
      "  turn2 D | push2 D | click2   the second knob (chord shape)\n"
      "  btn K [ms]        OLED button K1-K4 (1-4): press, release after ms (default 150)\n"
      "  oled [flip|offset N|off|on]  the bass OLED; input [knob2 auto|on|off]\n"
      "  set PARAM V       chord: ROOT SCALE OCT BANK DICE VAR VOIC STRM HUM VEL VLEAD CH DARK TENS LEN MUT\n"
      "  bset PARAM V      bass:  TEMPO SWING OCT GENRE FOLLOW LEN GATE SLIDE ACCENT DENS DUCK CH NUDGE FILL RATE SWITCH VEL\n"
      "  act NAME          latch alloff more star gen mutate favs bstar bmutate edit play resync panic calib\n"
      "  clock BPM | start | stop | cont   simulate the Move's MIDI clock\n"
      "  shot | lcd [invert|bgr|rot|mhz N]");
}
