// Render real ChordBox screens on the Mac: the real engine, bass, clock and
// app produce the model, the real ui_draw.cpp draws it. Raw RGB565 frames
// go to /tmp/chordbox_screens; tests/host/render.sh turns them into PNGs.
#include "harness.h"
#include "../../firmware/ui_draw.h"
#include "../../firmware/ui_oled.h"
#include "../../firmware/harm.h"

static lgfx::LGFX_Sprite spr, rgb;

static void snap(const char* name) {
  UiModel m = model();
  uiRender(spr, m, millis());
  spr.pushSprite(&rgb, 0, 0);                   // palette -> RGB565 for the dump
  std::string p = std::string("/tmp/chordbox_screens/") + name + ".rgb565";
  FILE* f = fopen(p.c_str(), "wb");
  fwrite(rgb.getBuffer(), 1, W * H * 2, f);
  fclose(f);
  printf("%s\n", name);
}

static lgfx::LGFX_Sprite oled;
static void snapOled(const char* name, UiModel* over = nullptr) {   // 128x64, one byte a pixel
  UiModel m = over ? *over : model();
  if (!oled.getBuffer()) {
    oled.setColorDepth(1);
    oled.createSprite(OLED_W, OLED_H);
  }
  oledRender(oled, m, millis());
  std::string p = std::string("/tmp/chordbox_screens/") + name + ".oled";
  FILE* f = fopen(p.c_str(), "wb");
  for (int y = 0; y < OLED_H; y++)
    for (int x = 0; x < OLED_W; x++) fputc(oled.readPixelValue(x, y) ? 1 : 0, f);
  fclose(f);
  printf("%s\n", name);
}

static double STEP = 60e6 / 124 / 4;

int main() {

  uiInitSprite(spr);
  rgb.setColorDepth(16);
  rgb.createSprite(W, H);
  g_files["/favs.txt"] = "P\tF01\ti bVI bIII bVII\n";
  bootAll();
  app::setMoveUp(true);
  engine::paramSet(engine::P_ROOT, 5);
  engine::paramSet(engine::P_SCALE, 2);
  engine::paramSet(engine::P_BANK, 11);          // Mysterious
  runFor(2000);
  snap("01_chord_idle");
  snapOled("o1_no_line");

  // the Move starts; ROLLER runs; a chord is played
  bass::paramSet(bass::BP_GENRE, 0);
  moveClock(124, 200000, true);
  ev(EV_ACTION, app::actionIndex("mode"), 1);
  ev(EV_PAD_DOWN, 0);
  ev(EV_PAD_UP, 0);
  ev(EV_ACTION, app::actionIndex("mode"), 0);
  moveClock(124, 2200000);
  ev(EV_PAD_DOWN, 5);
  moveClock(124, (int64_t)(STEP * 5.4));
  snap("02_chord_playing");
  ev(EV_PAD_UP, 5);

  ev(EV_ACTION, app::actionIndex("mode"), 1);                // BASS: the ring
  moveClock(124, (int64_t)(STEP * 6.5));
  snap("03_bass_roller");

  bass::paramSet(bass::BP_GENRE, 1);             // ACID
  ev(EV_PAD_DOWN, 0);
  ev(EV_PAD_UP, 0);
  moveClock(124, (int64_t)(STEP * 16 * 2 + STEP * 9.3));
  snap("04_bass_acid");
  snapOled("o2_acid_playing");

  bass::paramSet(bass::BP_GENRE, 9);             // BASS MUSIC: 2-bar lines
  ev(EV_PAD_DOWN, 0);
  ev(EV_PAD_UP, 0);
  moveClock(124, (int64_t)(STEP * 16 * 3 + STEP * 3.2));
  snap("05_bass_halftime");
  snapOled("o3_two_bars");
  {
    UiModel m = model();
    m.btnLearn = 3;
    snapOled("o4_button_setup", &m);
  }

  bass::paramSet(bass::BP_GENRE, 0);             // generated pages
  for (int i = 0; i < 3; i++) ev(EV_TURN, 0, 1);
  ev(EV_PAD_DOWN, 6);
  ev(EV_PAD_UP, 6);
  moveClock(124, (int64_t)(STEP * 16 * 2 + STEP * 12.6));
  snap("06_bass_generated");

  ev(EV_HOLD);                                   // BASS function layer, SWING in focus
  ev(EV_PAD_DOWN, 1);
  ev(EV_TURN, 0, 4);
  moveClock(124, (int64_t)(STEP * 2));
  snap("07_func_bass");
  ev(EV_PAD_UP, 1);
  ev(EV_RELEASE);

  ev(EV_ACTION, app::actionIndex("mode"), 0);                // CHORD function layer, DICE
  ev(EV_HOLD);
  ev(EV_PAD_DOWN, 4);
  ev(EV_TURN, 0, 7);
  snap("08_func_chord");
  ev(EV_PAD_UP, 4);
  ev(EV_RELEASE);

  ev(EV_ACTION, app::actionIndex("mode"), 1);                // EDIT the playing line
  ev(EV_HOLD);
  ev(EV_PAD_DOWN, 14);
  ev(EV_PAD_UP, 14);
  ev(EV_RELEASE);
  for (int i = 0; i < 5; i++) ev(EV_TURN, 0, 1);
  moveClock(124, (int64_t)(STEP * 7.5));
  snap("09_edit");
  ev(EV_CLICK);

  ev(EV_HOLD);                                   // settings list
  ev(EV_PAD_DOWN, 11);
  ev(EV_PAD_UP, 11);
  ev(EV_RELEASE);
  ev(EV_TURN, 0, 2);
  snap("10_more_bass");
  for (int i = 0; i < 40; i++) ev(EV_TURN, 0, 1);
  ev(EV_TURN, 0, -1);                            // the last is BUTTON SETUP; one up: KEYPAD SETUP
  ev(EV_CLICK);
  for (int i = 0; i < 6; i++) ev(EV_RAW_DOWN, i);
  snap("11_keypad_setup");
  ev(EV_HOLD);                                   // cancel

  ev(EV_ACTION, app::actionIndex("mode"), 1);                // back to BASS, a toast
  bass::star();
  runFor(5000);
  snap("12_bass_starred");

  ev(EV_KEY, 0, 1);                              // KEY0 held: PERFORM
  ev(EV_PAD_DOWN, 0);
  ev(EV_PAD_UP, 0);
  ev(EV_TURN, 0, 4);
  runFor(400000);
  snap("13_perform");
  ev(EV_KEY, 0, 0);

  ev(EV_ACTION, app::actionIndex("mode"), 0);                // CHORD with latch + arp running: the badges
  engine::paramSet(engine::P_ARP, 1);
  engine::toggleLatch();
  ev(EV_PAD_DOWN, 5);
  ev(EV_PAD_UP, 5);
  runFor(3000000);
  snap("14_badges");
  extern bool g_fakeWifi;
  g_fakeWifi = true;
  app::handle(Event{EV_ACTION, (uint8_t)app::actionIndex("phone"), 0, 0});
  runFor(300000);
  snap("15_phone_link");
  ev(EV_CLICK);
  engine::stopAll();
  engine::paramSet(engine::P_ARP, 0);
    // ---- the Harmony Engine (v2.0)
  {
    auto evA = [](const char* name, int v = 0) { ev(EV_ACTION, app::actionIndex(name), v); };
    auto btn = [](int k) { ev(EV_BTN, k, 1); runFor(60000); ev(EV_BTN, k, 0); };
    enum { B_PADS, B_NO, B_YES, B_MODE };
    harm::stopAll();
    evA("newsong");
    evA("mode", 4);                                // PLAY
    for (int i = 0; i < 12 && strncmp(model().harm.key, "F ", 2); i++) ev(EV_TURN2, 0, 1);
    auto padOf = [](const char* l) { UiModel m = model(); for (int p = 4; p < 16; p++) if (!strcmp(m.harm.pad[p], l)) return p; return 4; };
    for (const char* c : {"Fm", "Fm", "Db", "Eb"}) {
      int p = padOf(c);
      ev(EV_PAD_DOWN, p); runFor(30000); ev(EV_PAD_UP, p);
      btn(B_YES);
    }
    ev(EV_CLICK);
    moveClock(124, 200000, true);
    moveClock(124, (int64_t)(STEP * 16 * 2 + STEP * 5));
    ev(EV_PAD_DOWN, padOf("Bbm"));
    snap("16_play");
    snapOled("o5_play");
    ev(EV_PAD_UP, padOf("Bbm"));
    ev(EV_HOLD);
    ev(EV_PAD_DOWN, 2);
    snap("17_slots");
    ev(EV_PAD_UP, 2);
    ev(EV_RELEASE);
    evA("mode", 5);                                // GROW
    for (int i = 0; i < 2; i++) ev(EV_TURN, 0, 1);
    btn(B_NO);
    moveClock(124, (int64_t)(STEP * 9));
    snap("18_grow");
    btn(B_YES);
    ev(EV_CLICK);
    // HUNT, part way: two verdicts in
    evA("newsong");
    evA("mode", 2);
    ev(EV_PAD_DOWN, 4 + 2); runFor(30000); ev(EV_PAD_UP, 4 + 2);
    btn(B_YES);                                        // D fits... as a low note: offers
    btn(B_NO);                                        // none of these
    ev(EV_PAD_DOWN, 4 + 1); runFor(30000); ev(EV_PAD_UP, 4 + 1);
    btn(B_NO);                                        // C# clashes
    snap("19_hunt");
    snapOled("o6_hunt");
    ev(EV_PAD_DOWN, 4 + 2); runFor(30000); ev(EV_PAD_UP, 4 + 2);
    btn(B_YES);
    snap("20_hunt_offers");
    btn(B_YES);
    // LISTEN, after a few bars of MIDI
    evA("mode", 3);
    btn(B_NO);
    int lb[4] = {41, 37, 44, 39}, up[4][3] = {{65, 68, 72}, {65, 68, 73}, {63, 68, 72}, {63, 67, 70}};
    double bar = STEP * 16;
    for (int r = 0; r < 3; r++)
      for (int b = 0; b < 4; b++) {
        harm::midiIn(0x91, lb[b], 100, nowUs());
        for (int x : up[b]) harm::midiIn(0x90, x, 90, nowUs());
        moveClock(124, (int64_t)(bar * 0.9));
        harm::midiIn(0x81, lb[b], 0, nowUs());
        for (int x : up[b]) harm::midiIn(0x80, x, 0, nowUs());
        moveClock(124, (int64_t)(bar * 0.1));
      }
    moveClock(124, (int64_t)bar);
    snap("21_listen");
    btn(B_YES);
    // MELODY
    evA("mode", 6);
    btn(B_NO);
    btn(B_YES);
    moveClock(124, (int64_t)(STEP * 4));
    snap("22_melody");
    // TAKES
    btn(B_MODE);
    ev(EV_PAD_DOWN, 7); ev(EV_PAD_UP, 7);
    snap("23_takes");
    snapOled("o7_takes");
    ev(EV_CLICK);
    btn(B_MODE);
    ev(EV_TURN, 0, 1);
    snap("24_mode_menu");
    snapOled("o8_mode_menu");
    btn(B_NO);
    evA("mode", 4);
    ev(EV_KEY, 0, 1);
    runFor(100000);
    snapOled("o9_shift");
    ev(EV_KEY, 0, 0);
    evA("mode", 0);
    snap("25_chord_v21");
    snapOled("o10_chord");
    // v2.3: the middle is only the bass: UP/DOWN lines, GENRE, ON/OFF; knob held = SHIFT
    moveClock(124, 200000, true);
    for (int i = 0; i < 16; i++) btn(2);          // GENRE round to ANY
    btn(1);
    moveClock(124, (int64_t)(STEP * 22));
    snap("26_chord_station");
    snapOled("o12_station_any");
    ev(EV_KNOB2, 0, 1);
    ev(EV_KNOB2, 0, 0);
    ev(EV_CLICK2);                                // the next setting: DENSITY
    ev(EV_TURN2, 0, 2);
    moveClock(124, (int64_t)(STEP * 4));
    snapOled("o13_bass_setting");
    ev(EV_TURN2, 0, -2);
    ev(EV_KNOB2, 0, 1);                           // held: SHIFT
    runFor(100000);
    snapOled("o15_station_shift");
    btn(3);                                       // NOTES
    ev(EV_KNOB2, 0, 0);
    for (int pc : {5, 8, 0}) { ev(EV_PAD_DOWN, pc); ev(EV_PAD_UP, pc); btn(0); }
    for (int pc : {4, 11}) { ev(EV_PAD_DOWN, pc); ev(EV_PAD_UP, pc); btn(1); }
    ev(EV_PAD_DOWN, 10);
    snap("27_notes");
    snapOled("o14_notes");
    ev(EV_PAD_UP, 10);
    ev(EV_KNOB2, 0, 1);
    btn(3);
    ev(EV_KNOB2, 0, 0);
    ev(EV_TURN, 0, 1);                            // MAIN: back to the chords
    ev(EV_CLICK);                                 // the chord MENU on the screen
    for (int i = 0; i < 3; i++) ev(EV_TURN, 0, 1);
    snap("28_chord_menu");
    ev(EV_HOLD);
    ev(EV_RELEASE);
    ev(EV_HOLD);                                  // MODES on the main screen
    ev(EV_RELEASE);
    snap("29_modes_main");
    snapOled("o16_modes_bass");
    ev(EV_HOLD);
    ev(EV_RELEASE);
    evA("buttons");
    snapOled("o11_buttons");
  }
  return 0;
}
