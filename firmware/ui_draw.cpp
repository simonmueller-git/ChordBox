// ui_draw.cpp — pure rendering: a UiModel in, a 320x240 frame out.
// No Arduino or chip calls here, so tests/host renders the real screens on
// the Mac with this exact code.
//
//   status  (18)  program chip, key, bank/genre + page, tempo, beat
//   main   (200)  CHORD: 4x4 chords (the keypad) + the chord you played
//                 BASS:  the step ring + this page's 16 basslines as glyphs
//                 EDIT:  piano roll of one bar; FUNCTION layer; settings; setup
//   legend  (22)  what the knob does right now: turn / push + turn
//
// Colour is meaning: blue KEY, green HARMONY, grey FEEL, yellow GENERATE,
// orange BASS, red accents. Black ground, off-white ink, nothing else.

#define LGFX_USE_V1
#include "ui_draw.h"
#include "lgfx/utility/lgfx_qrcode.h"
#include "menu.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

static lgfx::LGFX_Sprite* S;

// ------------------------------------------------------------ palette --
// Palette slots in a 4-bit sprite (38 KB, fits the Pico too). Keep them
// plain `int`: LovyanGFX reads the C++ type and would take uint16_t as RGB565.
enum : int {
  BG, PANEL, LINE, DIM, MID, INK,
  BLUE, GREEN, GREY, YELLOW, SYSG, ORANGE, RED, BLUE_DK, ORANGE_DK, GREEN_DK,
  N_COLOURS
};

const uint8_t UI_PALETTE[][3] = {
    {0, 0, 0},        {18, 19, 22},    {42, 43, 48},    {104, 105, 111},
    {160, 161, 166},  {239, 237, 232}, {61, 139, 255},  {47, 210, 122},
    {201, 204, 209},  {255, 210, 63},  {138, 139, 144}, {255, 107, 31},
    {255, 59, 48},    {16, 40, 79},    {74, 29, 8},     {13, 58, 36},
};
const int UI_PALETTE_N = N_COLOURS;

static const int GCOL[6] = {BLUE, GREEN, GREY, YELLOW, SYSG, ORANGE};

void uiInitSprite(lgfx::LGFX_Sprite& s) {
  s.setColorDepth(4);
  s.createSprite(W, H);
  s.createPalette();
  for (int i = 0; i < N_COLOURS; i++)
    s.setPaletteColor(i, UI_PALETTE[i][0], UI_PALETTE[i][1], UI_PALETTE[i][2]);
}

// ------------------------------------------------------------- layout --
static const int Y_MAIN = 18, H_MAIN = 200, Y_LEG = 218;
static const float PI_F = 3.14159265f;

using namespace lgfx::v1::fonts;

// ------------------------------------------------------------ helpers --
static void text(const char* s, int x, int y, const lgfx::IFont* f, int c,
                 lgfx::textdatum_t d = lgfx::top_left) {
  S->setFont(f);
  S->setTextColor(c);
  S->setTextDatum(d);
  S->drawString(s, x, y);
}

static int textW(const char* s, const lgfx::IFont* f) {
  S->setFont(f);
  return S->textWidth(s);
}

static const char* fit(const char* in, const lgfx::IFont* f, int maxW, char* buf, size_t cap) {
  strncpy(buf, in, cap - 1);
  buf[cap - 1] = 0;
  for (size_t n = strlen(buf); n > 1 && textW(buf, f) > maxW; n--) buf[n - 1] = 0;
  return buf;
}

static void upper(const char* in, char* out, size_t cap) {
  size_t i = 0;
  for (; in[i] && i + 1 < cap; i++) out[i] = std::toupper((unsigned char)in[i]);
  out[i] = 0;
}

static void chip(const char* s, int x, int y, int fg, int bg, int* wOut = nullptr) {
  int w = textW(s, &Font0) + 8;
  S->fillRoundRect(x, y, w, 13, 2, bg);
  text(s, x + 4, y + 3, &Font0, fg);
  if (wOut) *wOut = w;
}

static void star(int cx, int cy, int r, int c) {
  float px[10], py[10];
  for (int i = 0; i < 10; i++) {
    float a = -PI_F / 2 + i * PI_F / 5;
    float rr = (i & 1) ? r * 0.45f : r;
    px[i] = cx + cosf(a) * rr;
    py[i] = cy + sinf(a) * rr;
  }
  for (int i = 0; i < 10; i += 2)
    S->fillTriangle(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], px[(i + 9) % 10], py[(i + 9) % 10], c);
  S->fillCircle(cx, cy, r * 0.45f, c);
}

static void polyline(const int* xy, int n, int x, int y, int c) {
  for (int i = 0; i + 1 < n; i++) S->drawLine(x + xy[2 * i], y + xy[2 * i + 1], x + xy[2 * i + 2], y + xy[2 * i + 3], c);
}

static void arrowHead(int x, int y, int dx, int dy, int c) {   // small filled head pointing (dx,dy)
  S->fillTriangle(x + dx * 3, y + dy * 3, x - dy * 3 - dx, y + dx * 3 - dy, x + dy * 3 - dx, y - dx * 3 - dy, c);
}

// ------------------------------------------------------------ icons --
// 16x16 line icons, drawn from primitives.
static void icon(int id, int x, int y, int c) {
  switch (id) {
    case I_SOFT:                                   // a gentle swell
      for (int i = 0; i < 14; i++) S->drawPixel(x + 1 + i, y + 12 - (int)(6 * sinf(i * PI_F / 13)), c);
      S->drawFastHLine(x + 1, y + 13, 14, c);
      break;
    case I_ARP:                                    // rising steps
      for (int i = 0; i < 4; i++) S->fillRect(x + 1 + i * 4, y + 11 - i * 3, 3, 3, c);
      break;
    case I_RHYTHM:                                 // x.x..x
      for (int i : {0, 2, 5}) S->fillRect(x + 1 + i * 2 + i, y + 5, 3, 7, c);
      S->drawFastHLine(x + 1, y + 13, 14, c);
      break;
    case I_ECHO:                                   // fading repeats
      S->fillRect(x + 1, y + 3, 3, 10, c);
      S->fillRect(x + 6, y + 5, 3, 8, c);
      S->drawRect(x + 11, y + 8, 3, 5, c);
      break;
    case I_REPEAT:                                 // ||||
      for (int i = 0; i < 5; i++) S->fillRect(x + 1 + i * 3, y + 3, 2, 10, c);
      break;
    case I_REC:
      S->fillCircle(x + 8, y + 8, 6, c);
      break;
    case I_SCENE:                                  // a stack of three
      for (int i = 0; i < 3; i++) S->drawRect(x + 1 + i * 2, y + 1 + i * 4, 10, 6, c);
      break;
    case I_KEY:
      S->drawRect(x + 1, y + 2, 14, 12, c);
      S->drawFastVLine(x + 5, y + 2, 12, c);
      S->drawFastVLine(x + 10, y + 2, 12, c);
      S->fillRect(x + 4, y + 2, 3, 7, c);
      S->fillRect(x + 9, y + 2, 3, 7, c);
      break;
    case I_SCALE:
      for (int i = 0; i < 4; i++) S->fillRect(x + 1 + i * 4, y + 12 - i * 3, 3, 3 + i * 3, c);
      break;
    case I_OCT:
      S->drawFastVLine(x + 5, y + 3, 11, c);
      arrowHead(x + 5, y + 3, 0, -1, c);
      S->drawFastVLine(x + 11, y + 2, 11, c);
      arrowHead(x + 11, y + 13, 0, 1, c);
      break;
    case I_BANK:
      for (int i = 0; i < 3; i++) S->drawRoundRect(x + 1 + i * 2, y + 7 - i * 3, 10, 7, 1, c);
      break;
    case I_DICE:
    case I_RAND:
      S->drawRoundRect(x + 1, y + 1, 14, 14, 3, c);
      S->fillCircle(x + 5, y + 5, 1, c);
      S->fillCircle(x + 11, y + 11, 1, c);
      S->fillCircle(x + 8, y + 8, 1, c);
      if (id == I_DICE) {
        S->fillCircle(x + 11, y + 5, 1, c);
        S->fillCircle(x + 5, y + 11, 1, c);
      }
      break;
    case I_VAR:
      S->drawArc(x + 8, y + 8, 6, 5, 200, 340, c);
      S->drawArc(x + 8, y + 8, 6, 5, 20, 160, c);
      arrowHead(x + 14, y + 7, 0, 1, c);
      arrowHead(x + 2, y + 9, 0, -1, c);
      break;
    case I_VOICE:
      S->fillRect(x + 2, y + 3, 12, 2, c);
      S->fillRect(x + 4, y + 7, 10, 2, c);
      S->fillRect(x + 2, y + 11, 8, 2, c);
      break;
    case I_STRUM:
      for (int i = 0; i < 4; i++) S->fillCircle(x + 3 + i * 3, y + 12 - i * 3, 1, c);
      S->drawLine(x + 2, y + 15, x + 14, y + 3, c);
      break;
    case I_HUMAN: {
      int w[] = {0, 8, 3, 3, 6, 12, 9, 12, 12, 6, 15, 8};
      polyline(w, 6, x, y + 1, c);
      break;
    }
    case I_LATCH:
      S->fillRoundRect(x + 3, y + 7, 10, 8, 1, c);
      S->drawArc(x + 8, y + 7, 4, 3, 180, 360, c);
      break;
    case I_STOP: S->fillRect(x + 3, y + 3, 10, 10, c); break;
    case I_MORE:
      for (int i = 0; i < 3; i++) S->fillCircle(x + 3 + i * 5, y + 8, 1, c);
      break;
    case I_STAR: star(x + 8, y + 8, 7, c); break;
    case I_GEN:
      S->fillTriangle(x + 8, y + 1, x + 6, y + 8, x + 10, y + 8, c);
      S->fillTriangle(x + 8, y + 15, x + 6, y + 8, x + 10, y + 8, c);
      S->fillTriangle(x + 1, y + 8, x + 8, y + 6, x + 8, y + 10, c);
      S->fillTriangle(x + 15, y + 8, x + 8, y + 6, x + 8, y + 10, c);
      break;
    case I_MUTATE:
      for (int i = 0; i < 14; i++) {
        int a = (int)(4 * sinf(i * 0.45f)), b = -a;
        S->drawPixel(x + 1 + i, y + 8 + a, c);
        S->drawPixel(x + 1 + i, y + 8 + b, c);
        if (i % 3 == 1) S->drawFastVLine(x + 1 + i, y + 8 - abs(a), abs(a) * 2, c);
      }
      break;
    case I_FAVS:
      S->fillCircle(x + 5, y + 6, 3, c);
      S->fillCircle(x + 11, y + 6, 3, c);
      S->fillTriangle(x + 2, y + 7, x + 14, y + 7, x + 8, y + 14, c);
      break;
    case I_TEMPO:
      S->drawTriangle(x + 8, y + 1, x + 2, y + 15, x + 14, y + 15, c);
      S->drawLine(x + 8, y + 12, x + 12, y + 4, c);
      break;
    case I_SWING:
      S->fillCircle(x + 3, y + 11, 2, c);
      S->fillCircle(x + 12, y + 11, 2, c);
      S->drawArc(x + 8, y + 10, 5, 5, 200, 340, c);
      break;
    case I_GENRE:
      S->drawLine(x + 1, y + 8, x + 7, y + 2, c);
      S->drawLine(x + 7, y + 2, x + 14, y + 2, c);
      S->drawLine(x + 14, y + 2, x + 14, y + 14, c);
      S->drawLine(x + 14, y + 14, x + 7, y + 14, c);
      S->drawLine(x + 7, y + 14, x + 1, y + 8, c);
      S->fillCircle(x + 10, y + 8, 1, c);
      break;
    case I_FOLLOW:
      S->drawRoundRect(x + 1, y + 5, 8, 6, 3, c);
      S->drawRoundRect(x + 7, y + 5, 8, 6, 3, c);
      break;
    case I_LEN:
      S->drawRect(x + 1, y + 5, 14, 6, c);
      for (int i = 0; i < 4; i++) S->drawFastVLine(x + 3 + i * 3, y + 5, i % 2 ? 2 : 3, c);
      break;
    case I_GATE: {
      int w[] = {0, 12, 3, 12, 3, 4, 10, 4, 10, 12, 15, 12};
      polyline(w, 6, x, y, c);
      break;
    }
    case I_SLIDE:
      S->fillCircle(x + 3, y + 12, 2, c);
      S->fillCircle(x + 13, y + 4, 2, c);
      S->drawLine(x + 4, y + 11, x + 12, y + 5, c);
      break;
    case I_ACCENT:
      S->drawLine(x + 3, y + 3, x + 12, y + 8, c);
      S->drawLine(x + 3, y + 13, x + 12, y + 8, c);
      S->drawLine(x + 3, y + 4, x + 11, y + 8, c);
      S->drawLine(x + 3, y + 12, x + 11, y + 8, c);
      break;
    case I_DENS:
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) S->fillCircle(x + 3 + i * 5, y + 3 + j * 5, (i + j) % 2 ? 1 : 2, c);
      break;
    case I_DUCK: {
      int w[] = {0, 3, 4, 3, 5, 13, 15, 3};
      polyline(w, 4, x, y, c);
      S->fillCircle(x + 3, y + 3, 2, c);
      break;
    }
    case I_EDIT:
      S->drawLine(x + 3, y + 13, x + 12, y + 4, c);
      S->drawLine(x + 4, y + 13, x + 13, y + 4, c);
      S->drawLine(x + 3, y + 12, x + 12, y + 3, c);
      S->fillTriangle(x + 1, y + 15, x + 2, y + 11, x + 5, y + 14, c);
      break;
    case I_PLAY: S->fillTriangle(x + 4, y + 2, x + 4, y + 14, x + 14, y + 8, c); break;
    case I_NOTE:
      S->fillCircle(x + 5, y + 12, 3, c);
      S->drawFastVLine(x + 8, y + 2, 10, c);
      S->drawLine(x + 8, y + 2, x + 13, y + 6, c);
      break;
    case I_ROTL:
    case I_ROTR: {
      S->drawArc(x + 8, y + 8, 6, 5, 40, 320, c);
      if (id == I_ROTL) arrowHead(x + 12, y + 3, 1, 0, c);
      else arrowHead(x + 12, y + 13, 1, 0, c);
      break;
    }
    case I_UP:
      S->drawFastVLine(x + 8, y + 3, 12, c);
      arrowHead(x + 8, y + 3, 0, -1, c);
      break;
    case I_DOWN:
      S->drawFastVLine(x + 8, y + 1, 12, c);
      arrowHead(x + 8, y + 13, 0, 1, c);
      break;
    case I_CLEAR:
      S->drawLine(x + 3, y + 3, x + 13, y + 13, c);
      S->drawLine(x + 13, y + 3, x + 3, y + 13, c);
      S->drawLine(x + 4, y + 3, x + 13, y + 12, c);
      S->drawLine(x + 12, y + 3, x + 3, y + 12, c);
      break;
    case I_UNDO:
      S->drawArc(x + 9, y + 9, 5, 4, 270, 90, c);
      S->drawFastHLine(x + 4, y + 4, 5, c);
      arrowHead(x + 3, y + 4, -1, 0, c);
      break;
    case I_DONE:
      S->drawLine(x + 2, y + 8, x + 6, y + 12, c);
      S->drawLine(x + 6, y + 12, x + 14, y + 3, c);
      S->drawLine(x + 2, y + 9, x + 6, y + 13, c);
      S->drawLine(x + 6, y + 13, x + 14, y + 4, c);
      break;
    case I_TIE:
      S->fillCircle(x + 3, y + 11, 2, c);
      S->fillCircle(x + 13, y + 11, 2, c);
      S->drawArc(x + 8, y + 12, 6, 5, 200, 340, c);
      break;
    case I_BAR:
      S->drawFastVLine(x + 3, y + 2, 12, c);
      S->drawFastVLine(x + 12, y + 2, 12, c);
      S->fillCircle(x + 7, y + 6, 1, c);
      S->fillCircle(x + 7, y + 10, 1, c);
      break;
  }
}

// ------------------------------------------------------------ keyboard --
static const char* const PC[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

static void keyboard(int x0, int y0, const uint8_t* notes, int n, int kh) {
  int lo = 127;
  for (int i = 0; i < n; i++) lo = std::min(lo, (int)notes[i]);
  int base = (lo / 12) * 12;
  const int KW = 8, BW = 5, BH = kh * 3 / 5;
  static const int8_t WHITE[7] = {0, 2, 4, 5, 7, 9, 11};
  auto on = [&](int note) {
    for (int i = 0; i < n; i++)
      if (notes[i] == note) return true;
    return false;
  };
  for (int o = 0; o < 2; o++)
    for (int w = 0; w < 7; w++)
      S->fillRect(x0 + (o * 7 + w) * KW, y0, KW - 1, kh, on(base + o * 12 + WHITE[w]) ? ORANGE : MID);
  static const int8_t BLACK[5] = {1, 3, 6, 8, 10};
  static const int8_t BPOS[5] = {1, 2, 4, 5, 6};
  for (int o = 0; o < 2; o++)
    for (int b = 0; b < 5; b++)
      S->fillRect(x0 + (o * 7 + BPOS[b]) * KW - BW / 2 - 1, y0, BW, BH,
                  on(base + o * 12 + BLACK[b]) ? ORANGE : BG);
}

static const char* const HM_NAME[HM_COUNT] = {"HUNT", "LISTEN", "PLAY", "GROW", "MELODY"};
static const int HM_COL[HM_COUNT] = {GREEN, BLUE, ORANGE, YELLOW, SYSG};

static void sureBars(int x, int y, int bars, bool tie) {
  for (int i = 0; i < 5; i++) {
    int h = 3 + i * 2;
    if (i < bars) S->fillRect(x + i * 4, y + 11 - h, 3, h, tie ? YELLOW : GREEN);
    else S->drawRect(x + i * 4, y + 11 - h, 3, h, LINE);
  }
}

// -------------------------------------------------------------- status --
static void beatDots(const UiModel& m, int x, int y, uint32_t now) {
  int beat = -1;
  if (m.clk.running) beat = (m.clk.step / 4) % 4;
  for (int i = 0; i < 4; i++) {
    bool lit = i == beat;
    bool fresh = lit && m.clk.stepMs && (now - m.clk.stepAtMs) < m.clk.stepMs && m.clk.step % 4 == 0;
    if (lit) S->fillCircle(x + i * 7, y, fresh ? 3 : 2, i == 0 ? ORANGE : INK);
    else S->drawCircle(x + i * 7, y, 2, LINE);
  }
}

static void drawStatus(const UiModel& m, uint32_t now) {
  S->fillRect(0, 0, W, Y_MAIN, BG);
  bool harm = m.mode == M_HARM;
  const char* prog = m.overlay == OV_MODES ? "MODES" : (m.notes.on && m.mode != M_HARM) ? "NOTES" : m.leftBass ? "BASS" : harm ? (m.harm.takes ? "TAKES" : m.harm.slotLayer ? "SLOTS" : HM_NAME[m.harm.hmode < HM_COUNT ? m.harm.hmode : 2])
                   : m.mode == M_CHORD ? "CHORD" : m.mode == M_BASS ? "BASS" : m.mode == M_EDIT ? "EDIT"
                   : m.mode == M_MORE ? "MORE" : "SETUP";
  int pc = harm ? HM_COL[m.harm.hmode < HM_COUNT ? m.harm.hmode : 2]
                : m.mode == M_CHORD ? BLUE : (m.mode == M_BASS || m.mode == M_EDIT) ? ORANGE : SYSG;
  int w;
  chip(prog, 2, 2, BG, pc, &w);
  int x = 2 + w + 4;
  char key[22];
  if (harm) snprintf(key, sizeof key, "%s", m.harm.key);
  else snprintf(key, sizeof key, "%s %s", m.key, m.scale);
  text(key, x, 3, &DejaVu9, BLUE);
  x += textW(key, &DejaVu9) + 4;
  if (harm) {
    sureBars(x, 3, m.harm.sureBars, m.harm.tie);
    x += 24;
    if (m.harm.liveSafe) {
      int cw;
      chip("LIVE", x, 2, BG, RED, &cw);
      x += cw + 4;
    }
    if (m.harm.tune) {
      char tn[10];
      snprintf(tn, sizeof tn, "%+dc", m.harm.tune);
      text(tn, x, 5, &Font0, YELLOW);
      x += textW(tn, &Font0) + 6;
    }
  } else {
    x += 4;
  }

  char where[24], up[16];
  bool bassSide = m.bassSide;
  if (bassSide) {
    upper(m.bass.bank, up, sizeof up);
    if (m.bass.any) snprintf(where, sizeof where, "ANY: %s", up);
    else snprintf(where, sizeof where, "%s %d/%d", up, m.bass.page + 1, m.bass.pages);
  } else {
    upper(m.chord.bank, up, sizeof up);
    snprintf(where, sizeof where, "%s %d/%d", up, m.chord.page + 1, m.chord.pages);
  }
  // held / moving states, as small chips left of the transport, on every screen
  const char* badge[4];
  int badgeCol[4], nb = 0;
  if (m.chord.latch) { badge[nb] = "LAT"; badgeCol[nb++] = YELLOW; }
  if (m.chord.loopState) { badge[nb] = m.chord.loopState == 1 ? "REC" : "LOOP"; badgeCol[nb++] = m.chord.loopState == 1 ? RED : GREEN; }
  if (m.chord.repeat) { badge[nb] = "RPT"; badgeCol[nb++] = RED; }
  else if (m.chord.motion[0]) { badge[nb] = "ARP"; badgeCol[nb++] = ORANGE; }
  int badgeW = 0;
  for (int i = 0; i < nb; i++) badgeW += textW(badge[i], &Font0) + 11;
  char fw[24];
  if (harm) where[0] = 0;
  text(fit(where, &DejaVu9, 150 - x + 60 - badgeW, fw, sizeof fw), x, 3, &DejaVu9, INK);
  if (bassSide && m.bass.genPage) {
    int gx = x + textW(fw, &DejaVu9) + 4;
    if (gx + 30 <= 226 - badgeW) chip("GEN", gx, 2, BG, YELLOW);   // only if the badges leave room
  }

  // tempo and transport, right-aligned
  char bpm[12];
  snprintf(bpm, sizeof bpm, "%d", (m.clk.bpm10 + 5) / 10);
  int rx = W - 3;
  beatDots(m, rx - 22, 8, now);
  rx -= 30;
  int bw = textW(bpm, &DejaVu9);
  text(bpm, rx - bw, 3, &DejaVu9, INK);
  rx -= bw + 4;
  const char* src = m.clk.ext ? "MOVE" : "INT";
  int sw = textW(src, &Font0);
  text(src, rx - sw, 5, &Font0, m.clk.ext ? GREEN : DIM);
  rx -= sw + 5;
  if (m.clk.running) S->fillTriangle(rx - 6, 3, rx - 6, 13, rx, 8, m.bass.muted ? DIM : ORANGE);
  else S->fillRect(rx - 6, 4, 6, 8, DIM);
  rx -= 10;
  for (int i = nb - 1; i >= 0; i--) {
    int cw = textW(badge[i], &Font0) + 8;
    chip(badge[i], rx - cw, 2, BG, badgeCol[i]);
    rx -= cw + 3;
  }
  S->drawFastHLine(0, Y_MAIN - 1, W, LINE);
}

// -------------------------------------------------------------- legend --
// The two knobs, always: MAIN (left knob) turn + click, SET (middle knob) the
// setting it turns, with dots for which of the list (a click steps to the next).
static void drawLegend(const UiModel& m) {
  S->fillRect(0, Y_LEG, W, H - Y_LEG, BG);
  S->drawFastHLine(0, Y_LEG, W, LINE);
  int y = Y_LEG + 6;
  if (m.mode == M_CALIB) {                     // setup: progress; the knob only clicks/holds here
    char pb[20];
    snprintf(pb, sizeof pb, "KEYPAD %c", 'A' + m.calKeypad);
    text(pb, 4, y + 2, &Font0, MID);
    snprintf(pb, sizeof pb, "%d/16", m.calNext);
    text(pb, 10 + textW("KEYPAD A", &Font0), y, &DejaVu9, INK);
    return;
  }
  // MAIN: turn, then what a click does
  S->drawCircle(8, y + 5, 5, MID);
  S->drawFastVLine(8, y, 4, INK);
  int x = 17;
  char b[16];
  if (m.turnLabel[0]) {
    text(m.turnLabel, x, y + 2, &Font0, MID);
    x += textW(m.turnLabel, &Font0) + 3;
  }
  if (m.turnValue[0]) {
    text(fit(m.turnValue, &DejaVu9, 70, b, sizeof b), x, y, &DejaVu9, INK);
    x += textW(b, &DejaVu9) + 6;
  }
  if (m.clickLabel[0] && x < 140) {
    S->fillRoundRect(x, y, textW(m.clickLabel, &Font0) + 8, 12, 2, PANEL);
    text(m.clickLabel, x + 4, y + 2, &Font0, INK);
  }
  // SET: the setting, its value, and where it sits in the list
  int sx = 166;
  S->fillCircle(sx, y + 5, 5, BLUE);
  S->fillCircle(sx, y + 5, 2, BG);
  sx += 9;
  if (m.setLabel[0]) {
    text(fit(m.setLabel, &Font0, 64, b, sizeof b), sx, y + 2, &Font0, MID);
    sx += textW(b, &Font0) + 4;
  }
  int dotsW = m.setN > 1 ? m.setN * 5 : 0;
  text(fit(m.setValue, &DejaVu9, W - 6 - dotsW - sx, b, sizeof b), sx, y, &DejaVu9, INK);
  for (int i = 0; i < m.setN && m.setN > 1; i++) {
    int dx = W - 4 - (m.setN - i) * 5 + 2;
    if (i == m.setPos) S->fillCircle(dx, y + 5, 2, BLUE);
    else S->drawPixel(dx, y + 5, DIM);
  }
}

// --------------------------------------------------------------- NOTES --
// The pads play single bass notes: which sit, which clash, and the key they point to.
static void drawNotes(const UiModel& m) {
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  const auto& n = m.notes;
  const int CW = 46, CH = 44, G = 3, X0 = 4, Y0 = Y_MAIN + 6;
  for (int p = 0; p < 16; p++) {
    int note = n.base + p, pc = note % 12;
    int x = X0 + (p % 4) * (CW + G), y = Y0 + (p / 4) * (CH + G);
    bool fit = n.fit & (1 << pc), clash = n.clash & (1 << pc), last = note == n.last;
    int fill = last ? INK : fit ? GREEN_DK : PANEL;
    S->fillRoundRect(x, y, CW, CH, 3, fill);
    S->drawRoundRect(x, y, CW, CH, 3, fit ? GREEN : clash ? RED : LINE);
    if (clash) S->drawLine(x + 5, y + 5, x + CW - 6, y + CH - 6, RED);
    char nm[8];
    snprintf(nm, sizeof nm, "%s%d", PC[pc], note / 12 - 2);
    text(nm, x + CW / 2, y + CH / 2, &DejaVu12, last ? BG : INK, lgfx::middle_center);
  }
  int rx = X0 + 4 * (CW + G) + 6;
  text("NOTES", rx, Y0, &Font0, ORANGE);
  char big[8] = "-";
  if (n.last >= 0) snprintf(big, sizeof big, "%s%d", PC[n.last % 12], n.last / 12 - 2);
  text(big, rx, Y0 + 14, &DejaVu24, INK);
  text("THE KEY", rx, Y0 + 52, &Font0, DIM);
  char kb[30];
  text(fit(n.key, &DejaVu9, W - rx - 4, kb, sizeof kb), rx, Y0 + 64, &DejaVu9, BLUE);
  text("BUTTONS", rx, Y0 + 92, &Font0, DIM);
  text("FITS  CLASH", rx, Y0 + 104, &Font0, INK);
  text("MIDDLE KNOB", rx, Y0 + 122, &Font0, DIM);
  text("turn: octave", rx, Y0 + 134, &Font0, INK);
  text("click: use key", rx, Y0 + 146, &Font0, INK);
  text("LINES: chords", rx, Y0 + 164, &Font0, DIM);
}

// ----------------------------------------------------------- MODE menu --
// The pads become the modes: the tiles sit where the pads are.
static void drawModes(const UiModel& m) {
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  static const int TC[MI_COUNT] = {BLUE, ORANGE, GREEN, BLUE, ORANGE, YELLOW, SYSG, MID, DIM, DIM, DIM, DIM, DIM};
  const int TW = 76, TH = 38, G = 3, X0 = 4, Y0 = Y_MAIN + 4;
  for (int i = 0; i < 16; i++) {
    int x = X0 + (i % 4) * (TW + G), y = Y0 + (i / 4) * (TH + G);
    if (i >= MI_COUNT) {
      S->drawRoundRect(x, y, TW, TH, 3, PANEL);
      continue;
    }
    bool sel = i == m.modeSel;
    if (sel) S->fillRoundRect(x, y, TW, TH, 3, TC[i]);
    else S->drawRoundRect(x, y, TW, TH, 3, TC[i]);
    char n[4];
    snprintf(n, sizeof n, "%d", i + 1);
    text(n, x + 4, y + 3, &Font0, sel ? BG : DIM);
    text(MENU_NAME[i], x + TW / 2, y + TH / 2 + 3, &DejaVu9, sel ? BG : INK, lgfx::middle_center);
  }
  int i = m.modeSel < MI_COUNT ? m.modeSel : 0;
  char line[48];
  snprintf(line, sizeof line, "%s: %s", MENU_NAME[i], MENU_HELP[i]);
  char f[48];
  text(fit(line, &Font0, W - 8, f, sizeof f), 4, Y0 + 4 * (TH + G) + 2, &Font0, INK);
}

// --------------------------------------------------------------- toast --
static void drawToast(const UiModel& m, uint32_t now) {
  if (!m.toast[0] || now >= m.toastUntil) return;
  bool starred = m.toast[0] == '*';
  const char* t = starred ? m.toast + 2 : m.toast;
  int tw = textW(t, &DejaVu12), w = tw + 20 + (starred ? 14 : 0);
  int x = (W - w) / 2, y = Y_LEG - 26;
  S->fillRoundRect(x, y, w, 20, 3, INK);
  if (starred) star(x + 14, y + 10, 6, ORANGE);
  text(t, x + 10 + (starred ? 14 : 0), y + 10, &DejaVu12, BG, lgfx::middle_left);
}

// --------------------------------------------------------------- CHORD --
static void miniBass(const UiModel& m, int x, int y, int w, uint32_t now) {
  // the running bassline, one bar, so CHORD never loses sight of it
  const BassView& b = m.bass;
  if (!b.len) return;
  int bar = m.clk.running ? b.patBar % std::max(1, b.len / 16) : 0;
  int cw = w / 16;
  int cur = m.clk.running ? m.clk.step % 16 : -1;
  for (int i = 0; i < 16; i++) {
    const StepView& s = b.step[bar * 16 + i];
    int cx = x + i * cw;
    int h = (s.flags & SF_NOTE) ? 4 + std::max(0, std::min(12, (s.semis + 12) / 3)) : 1;
    int col = !(s.flags & SF_NOTE) ? LINE : (s.flags & SF_ACCENT) ? RED : (s.flags & SF_GHOST) ? DIM : INK;
    if (i == cur) S->fillRect(cx, y - 18, cw - 1, 20, ORANGE_DK);
    S->fillRect(cx, y - h, cw - 2, h, col);
    if (i % 4 == 0) S->fillRect(cx, y + 3, cw - 2, 1, DIM);
  }
  (void)now;
}

static void drawChord(const UiModel& m, uint32_t now) {
  const ChordView& c = m.chord;
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  const int CW = 47, CH = 48, X0 = 2, Y0 = Y_MAIN + 2;
  for (int p = 0; p < 16; p++) {
    const PadCell& cell = c.pad[p];
    int x = X0 + (p % 4) * (CW + 1), y = Y0 + (p / 4) * (CH + 1);
    bool pressed = m.padMask & (1u << p);
    if (!(cell.flags & PF_HAS)) {
      S->drawRect(x, y, CW, CH, PANEL);
      continue;
    }
    bool sound = cell.flags & PF_SOUND;
    int fg = sound ? BG : (cell.flags & PF_WRAP) ? DIM : INK;
    S->fillRect(x, y, CW, CH, sound ? INK : PANEL);
    if (pressed && !sound) S->drawRect(x, y, CW, CH, BLUE);
    if ((cell.flags & PF_LAST) && !sound) S->fillRect(x, y + CH - 3, CW, 3, ORANGE);
    char rn[8];
    int rc = sound ? BG : (cell.flags & PF_DICED) ? GREEN : DIM;
    text(fit(cell.rn, &Font0, (cell.flags & PF_BORROW) ? CW - 12 : CW - 5, rn, sizeof rn), x + 3, y + 3, &Font0, rc);
    if (cell.flags & PF_BORROW) S->drawCircle(x + CW - 5, y + 6, 2, sound ? BG : MID);
    text(cell.root, x + 3, y + 14, &DejaVu18, fg);
    text(cell.qual, x + 4, y + 36, &Font0, sound ? BG : MID);
  }
  for (int r = 0; r < 4; r++)
    if (c.rowFav[r]) star(X0 + 4 * (CW + 1) - 9, Y0 + r * (CH + 1) + CH - 10, 4, YELLOW);

  // hero: the chord you just played
  int hx = 200;
  if (!c.played) {
    text("ChordBox", hx, Y_MAIN + 18, &DejaVu18, INK);
    text("PLAY A PAD", hx + 1, Y_MAIN + 48, &Font0, DIM);
    text("CLICK = MENU", hx + 1, Y_MAIN + 62, &Font0, DIM);
    text("HOLD = MODES", hx + 1, Y_MAIN + 76, &Font0, DIM);
  } else {
    char root[4] = {c.sym[0], 0, 0, 0};
    int rl = c.sym[1] == 'b' ? 2 : 1;
    if (rl == 2) root[1] = 'b';
    text(root, hx, Y_MAIN + 6, &DejaVu40, c.sounding ? INK : MID);
    text(c.sym + rl, hx + 2 + textW(root, &DejaVu40), Y_MAIN + 10, &DejaVu18, c.sounding ? INK : MID);
    text(c.rn, hx + 1, Y_MAIN + 54, &DejaVu12, GREEN);
    char tag[24];
    upper(c.rowTag, tag, sizeof tag);
    char tf[24];
    text(fit(tag, &Font0, 116, tf, sizeof tf), hx + 1, Y_MAIN + 72, &Font0, DIM);
    keyboard(hx + 1, Y_MAIN + 86, c.notes, c.nNotes, 34);
    char names[40] = "";
    for (int i = 0; i < c.nNotes && strlen(names) < 30; i++) {
      strncat(names, PC[c.notes[i] % 12], sizeof names - strlen(names) - 1);
      strncat(names, " ", sizeof names - strlen(names) - 1);
    }
    text(names, hx + 1, Y_MAIN + 124, &Font0, MID);
  }
  // what is held or moving, big enough to read from a stage
  int bx = hx, bw;
  if (c.latch) {
    chip("LATCH", bx, Y_MAIN + 138, BG, YELLOW, &bw);
    bx += bw + 3;
  }
  if (c.loopState) {
    char lb[12];
    if (c.loopState == 1) snprintf(lb, sizeof lb, "REC %d/8", c.loopN);
    else snprintf(lb, sizeof lb, "LOOP %d/%d", c.loopPos, c.loopN);
    chip(lb, bx, Y_MAIN + 138, BG, c.loopState == 1 ? RED : GREEN, &bw);
    bx += bw + 3;
  }
  if (c.repeat) chip("REPEAT", bx, Y_MAIN + 138, BG, RED, &bw);
  else if (c.motion[0]) {
    char ab[20];
    snprintf(ab, sizeof ab, "ARP %s", c.motion);
    if (bx + textW(ab, &Font0) + 8 > W) snprintf(ab, sizeof ab, "ARP");
    chip(ab, bx, Y_MAIN + 138, BG, ORANGE, &bw);
  }

  // the bass, still running underneath
  int by = Y_MAIN + 190;
  bool bassOn = m.clk.running && !m.bass.muted;
  text("BASS", hx, Y_MAIN + 156, &Font0, bassOn ? ORANGE : DIM);
  if (m.bass.feel[0]) text(m.bass.feel, hx + 30, Y_MAIN + 156, &Font0, bassOn ? INK : MID);
  if (m.bass.chordName[0]) text(m.bass.chordName, W - 4, Y_MAIN + 156, &Font0, GREEN, lgfx::top_right);
  miniBass(m, hx, by, 116, now);
}

// ---------------------------------------------------------------- BASS --
// A line as one bar of 16 columns: a note is a bar, taller = higher (root, then its
// octave above); a held note runs on along the top. The playing line uses the live
// steps (its bar now, the chord it follows, DENSITY's skips).
static int barH(int semis, int h) { return 3 + (std::max(-12, std::min(24, semis)) + 12) * (h - 3) / 36; }

static void rollNote(int x, int base, int h, int semis, int len, int col, bool hollow) {
  int bh = barH(semis, h);
  if (hollow) S->drawRect(x, base - bh, 3, bh, col);
  else S->fillRect(x, base - bh, 3, bh, col);
  if (len > 1) S->drawFastHLine(x + 3, base - bh, len * 4 - 4, col);
}

static void rollCell(const BassPadCell& c, int x, int base, int h, int on) {
  for (int s = 0; s < 16; s += 4) S->drawPixel(x + s * 4 + 1, base + 2, DIM);   // the beats
  for (int s = 0; s < 16; s++) {
    if (!(c.onsets & (1 << s))) continue;
    int len = 1;
    while (s + len < 16 && (c.held & (1 << (s + len)))) len++;
    rollNote(x + s * 4, base, h, c.semis[s], len, (c.accents & (1 << s)) ? RED : on, false);
  }
}

static void rollLive(const BassView& b, int bar, int x, int base, int h, int cur) {
  for (int s = 0; s < 16; s += 4) S->drawPixel(x + s * 4 + 1, base + 2, ORANGE);
  for (int s = 0; s < 16; s++) {
    const StepView& st = b.step[bar * 16 + s];
    if (!(st.flags & SF_NOTE)) continue;
    int len = 1;
    while (s + len < 16 && (b.step[bar * 16 + s + len].flags & SF_TIE)) len++;
    int col = s == cur ? INK : (st.flags & SF_ACCENT) ? RED : (st.flags & SF_GHOST) ? MID : ORANGE;
    rollNote(x + s * 4, base, h, st.semis, len, col, st.flags & SF_SKIP);   // hollow: DENSITY skips it
  }
  if (cur >= 0) S->fillRect(x + cur * 4, base + 1, 3, 2, INK);
}

static void energyBars(int x, int y, int e, int lit) {   // 5 rising bars, right-aligned at x
  for (int i = 0; i < 5; i++) {
    int bh = 3 + i * 2;
    S->fillRect(x - (5 - i) * 4, y + 11 - bh, 3, bh, i < e ? lit : LINE);
  }
}

// The bass page: 16 lines, laid out like the pads, each one what it is (not its name).
static void drawBass(const UiModel& m, uint32_t now) {
  const BassView& b = m.bass;
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  const int CW = 79, CH = 49, X0 = 0, Y0 = Y_MAIN + 1;
  bool blink = (now / 250) % 2;
  int bars = std::max(1, b.len / 16);
  int bar = m.clk.running ? b.patBar % bars : 0;
  int cur = m.clk.running ? (int)(m.clk.step % 16) : -1;
  bool padsAreLines = m.mode == M_BASS;
  for (int p = 0; p < 16; p++) {
    const BassPadCell& c = b.pad[p];
    int x = X0 + (p % 4) * (CW + 1), y = Y0 + (p / 4) * (CH + 1);
    if (!(c.flags & 1)) {
      S->drawRect(x, y, CW, CH, PANEL);
      continue;
    }
    bool playing = p == b.playPad;
    bool queued = p == b.queuedPad;
    bool pressed = padsAreLines && (m.keypads == 1 ? (m.padMask & (1u << p)) : (m.padMask & (1u << (16 + p))));
    S->fillRect(x, y, CW, CH, playing ? ORANGE_DK : PANEL);
    if (playing) S->drawRect(x, y, CW, CH, ORANGE);
    else if (queued && blink) S->drawRect(x, y, CW, CH, ORANGE);
    else if (pressed) S->drawRect(x, y, CW, CH, BLUE);
    text(c.feel, x + 5, y + 4, &DejaVu9, playing ? INK : queued ? ORANGE : (c.flags & 2) ? MID : GREY);
    char tw[12];
    bassTraitWords(c.traits, tw, sizeof tw, 1);
    text(tw, x + 5, y + 16, &Font0, playing ? ORANGE : DIM);
    energyBars(x + CW - 3, y + 3, c.energy, playing ? ORANGE : DIM);
    if (c.flags & 4) star(x + CW - 27, y + 9, 3, YELLOW);
    if (playing && b.len) rollLive(b, bar, x + 7, y + CH - 6, 20, cur);
    else rollCell(c, x + 7, y + CH - 6, 20, (c.flags & 2) ? MID : INK);
    if (playing && bars > 1)                       // which bar of a 2- or 4-bar line
      for (int i = 0; i < bars; i++) S->fillRect(x + CW - 4 - (bars - i) * 4, y + CH - 4, 3, 2, i == bar ? INK : ORANGE);
  }
}

// ---------------------------------------------------------------- EDIT --
static void drawEdit(const UiModel& m, uint32_t now) {
  const BassView& b = m.bass;
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  const int X0 = 8, SW = 19, YT = Y_MAIN + 16, YB = Y_MAIN + 132;
  int bar = m.editBar;
  char head[32];
  snprintf(head, sizeof head, "%s  BAR %d/%d", b.name, bar + 1, std::max(1, (int)m.editBars));
  text(head, X0, Y_MAIN + 3, &Font0, MID);
  text(b.chordName, W - 6, Y_MAIN + 3, &Font0, GREEN, lgfx::top_right);
  auto yOf = [&](int semis) { return YB - 6 - (std::max(-12, std::min(24, semis)) + 12) * (YB - YT - 12) / 36; };

  for (int i = 0; i <= 16; i++)
    S->drawFastVLine(X0 + i * SW - 1, YT, YB - YT, i % 4 == 0 ? LINE : PANEL);
  S->drawFastHLine(X0, yOf(0) + 3, 16 * SW, PANEL);
  S->drawFastHLine(X0, yOf(12) + 3, 16 * SW, PANEL);
  text("R", 1, yOf(0), &Font0, DIM);
  text("O", 1, yOf(12), &Font0, DIM);

  if (m.editStep >= 0) S->fillRect(X0 + m.editStep * SW, YT, SW - 1, YB - YT + 44, BLUE_DK);
  int cur = m.clk.running ? m.clk.step % 16 : -1;
  bool curBar = m.clk.running && m.bass.patBar % std::max(1, (int)m.editBars) == bar;

  for (int i = 0; i < 16; i++) {
    const StepView& s = b.step[bar * 16 + i];
    int x = X0 + i * SW;
    if (!(s.flags & SF_NOTE)) continue;
    int len = 1;
    while (i + len < 16 && (b.step[bar * 16 + i + len].flags & SF_TIE)) len++;
    int w = (s.flags & SF_SHORT) ? SW / 2 : (s.flags & SF_LONG) || len > 1 ? SW * len - 2 : SW - 3;
    int y = yOf(s.semis);
    int col = (s.flags & SF_ACCENT) ? RED : (s.flags & SF_GHOST) ? DIM : INK;
    if (curBar && i == cur) col = ORANGE;
    S->fillRect(x + 1, y, w, 7, col);
    if (s.flags & SF_SLIDE) {
      int k = i + 1;
      while (k < 16 && !(b.step[bar * 16 + k].flags & SF_NOTE)) k++;
      if (k < 16) S->drawLine(x + w, y + 3, X0 + k * SW + 1, yOf(b.step[bar * 16 + k].semis) + 3, ORANGE);
    }
    // velocity lane
    int vh = (s.flags & SF_ACCENT) ? 14 : (s.flags & SF_GHOST) ? 4 : 9;
    S->fillRect(x + 3, YB + 20 - vh, SW - 7, vh, col);
    text(s.role, x + SW / 2, YB + 24, &Font0, MID, lgfx::top_center);
  }
  if (curBar && cur >= 0) {
    float frac = m.clk.stepMs ? std::min(1.0f, (float)(now - m.clk.stepAtMs) / m.clk.stepMs) : 0;
    int px = X0 + (int)((cur + frac) * SW);
    S->drawFastVLine(px, YT, YB - YT, ORANGE);
  }
  for (int i = 0; i < 16; i++) {
    char n[4];
    snprintf(n, sizeof n, "%d", i + 1);
    text(n, X0 + i * SW + SW / 2, YB + 36, &Font0, i == m.editStep ? INK : DIM, lgfx::top_center);
  }
  // the selected step, in words
  if (m.editStep >= 0) {
    const StepView& s = b.step[bar * 16 + m.editStep];
    char info[48];
    if (s.flags & SF_NOTE)
      snprintf(info, sizeof info, "STEP %02d  %s%s%s%s", m.editStep + 1, s.role,
               (s.flags & SF_ACCENT) ? "  ACCENT" : (s.flags & SF_GHOST) ? "  GHOST" : "",
               (s.flags & SF_SLIDE) ? "  SLIDE" : "", (s.flags & SF_SHORT) ? "  SHORT" : "");
    else snprintf(info, sizeof info, "STEP %02d  %s", m.editStep + 1, (s.flags & SF_TIE) ? "TIE" : "REST");
    text(info, X0, Y_MAIN + 186, &Font0, INK);
  }
}

// ------------------------------------------------------------ FUNCTION --
static void dial(const ParamView& k, int cx, int cy, int r, int col) {
  S->drawArc(cx, cy, r, r - 3, 135, 405, LINE);
  float span = std::max(1, k.hi - k.lo);
  float f = (k.pos - k.lo) / span;
  if (k.kind == K_BIPOLAR) {
    float a0 = 270, a1 = 135 + f * 270;
    if (a1 >= a0) S->fillArc(cx, cy, r, r - 3, a0, a1, col);
    else S->fillArc(cx, cy, r, r - 3, a1, a0, col);
  } else if (f > 0) {
    S->fillArc(cx, cy, r, r - 3, 135, 135 + f * 270, col);
  }
  float a = (135 + f * 270) * PI_F / 180;
  S->drawLine(cx, cy, cx + cosf(a) * (r - 6), cy + sinf(a) * (r - 6), INK);
  S->fillCircle(cx, cy, 2, INK);
}

static void drawFunc(const UiModel& m) {
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  const int CW = 57, CH = 48, X0 = 2, Y0 = Y_MAIN + 2;
  for (int i = 0; i < 16; i++) {
    const FuncCell& c = m.func[i];
    int x = X0 + (i % 4) * (CW + 1), y = Y0 + (i / 4) * (CH + 1);
    int gc = GCOL[c.group < 6 ? c.group : 4];
    bool focus = i == m.funcFocus;
    bool pressed = !m.kmOpen && (m.padMask & (1u << (m.keypads == 2 && m.mode != M_CHORD ? 16 + i : i)));
    S->fillRect(x, y, CW, CH, focus ? (m.kmOpen && m.kmEdit ? ORANGE : gc) : PANEL);
    if (pressed) S->drawRect(x, y, CW, CH, INK);
    int fg = focus ? BG : gc;
    icon(c.icon, x + 5, y + 5, fg);
    if (c.kind == K_ACTION) {
      if (c.on) S->fillCircle(x + CW - 7, y + 7, 3, focus ? BG : ORANGE);
      else S->drawCircle(x + CW - 7, y + 7, 3, focus ? BG : LINE);
    }
    text(c.label, x + 5, y + 25, &Font0, focus ? BG : INK);
    if (c.value[0]) {
      char v[10];
      text(fit(c.value, &Font0, CW - 8, v, sizeof v), x + 5, y + 36, &Font0, focus ? BG : MID);
    }
  }
  // the focused setting, big
  const ParamView& k = m.focus;
  int px = 238, gc = GCOL[k.group < 6 ? k.group : 4];
  text(k.label, px + 2, Y_MAIN + 8, &Font0, gc);
  char v[14];
  text(fit(k.value, &DejaVu18, 78, v, sizeof v), px + 2, Y_MAIN + 22, &DejaVu18, INK);
  if (k.kind == K_LIST && k.opts && k.nOpts <= 12) {
    int sel = k.pos - k.lo;
    for (int d = -2; d <= 2; d++) {
      int i = sel + d;
      if (i < 0 || i >= k.nOpts) continue;
      char o[10];
      text(fit(k.opts[i], &Font0, 70, o, sizeof o), px + 4, Y_MAIN + 90 + d * 13, &Font0, d == 0 ? INK : d == 1 || d == -1 ? MID : DIM);
      if (d == 0) S->fillTriangle(px - 2, Y_MAIN + 90, px - 2, Y_MAIN + 96, px + 2, Y_MAIN + 93, gc);
    }
  } else {
    dial(k, px + 40, Y_MAIN + 100, 30, gc);
  }
  if (m.kmOpen) {                                // worked by MAIN alone; the pads keep playing
    text(m.overlay == OV_PERF ? "PERFORM 2/2" : "MENU 1/2", px + 2, Y_MAIN + 136, &Font0, ORANGE);
    text(m.kmEdit ? "TURN: SET" : "TURN: MOVE", px + 2, Y_MAIN + 148, &Font0, INK);
    text(m.kmEdit ? "CLICK: DONE" : "CLICK: OPEN", px + 2, Y_MAIN + 160, &Font0, INK);
    text("HOLD: CLOSE", px + 2, Y_MAIN + 172, &Font0, DIM);
    text("PADS: CHORDS", px + 2, Y_MAIN + 184, &Font0, DIM);
    return;
  }
  if (m.overlay == OV_PERF) {                   // KEY0 held
    text("PERFORM", px + 2, Y_MAIN + 136, &Font0, ORANGE);
    text("PAD+TURN: SET", px + 2, Y_MAIN + 148, &Font0, DIM);
    text("LET GO ALONE", px + 2, Y_MAIN + 164, &Font0, DIM);
    text("BASS ON/OFF", px + 2, Y_MAIN + 176, &Font0, DIM);
    return;
  }
  text("TAP A PAD", px + 2, Y_MAIN + 160, &Font0, DIM);
  text("TURN TO SET", px + 2, Y_MAIN + 172, &Font0, DIM);
  text("LET GO: BACK", px + 2, Y_MAIN + 184, &Font0, DIM);
}

// ---------------------------------------------------------- PHONE LINK --
static void qr(const char* text, int x, int y, int size) {   // light square, dark modules
  static uint8_t buf[512];
  QRCode q;
  for (uint8_t v = 1; v <= 8; v++) {
    if (lgfx_qrcode_getBufferSize(v) > sizeof buf) break;
    if (lgfx_qrcode_initText(&q, buf, v, 0, text) != 0) continue;
    int m = size / (q.size + 2);                   // one module of quiet zone each side
    int w = m * (q.size + 2), ox = x + (size - w) / 2, oy = y + (size - w) / 2;
    S->fillRect(ox, oy, w, w, INK);
    for (int yy = 0; yy < q.size; yy++)
      for (int xx = 0; xx < q.size; xx++)
        if (lgfx_qrcode_getModule(&q, xx, yy)) S->fillRect(ox + (xx + 1) * m, oy + (yy + 1) * m, m, m, BG);
    return;
  }
}

static void drawPhone(const UiModel& m) {
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  const auto& w = m.wifi;
  text("PHONE LINK", 8, Y_MAIN + 6, &DejaVu12, INK);
  if (!w.on) {
    text(w.ssid[0] || w.url[0] ? "STARTING..." : "WIFI IS OFF OR NOT ON THIS BOARD", 8, Y_MAIN + 34, &Font0, MID);
    text("MORE > WIFI: ON, THEN RESTART", 8, Y_MAIN + 50, &Font0, DIM);
    text("ANY KEY: BACK", 8, Y_MAIN + 180, &Font0, DIM);
    return;
  }
  // travelling: the code joins the hotspot; at home: the code opens the page
  char code[80];
  if (w.hotspot) snprintf(code, sizeof code, "WIFI:T:WPA;S:%s;P:%s;;", w.ssid, w.pass);
  else strlcpy(code, w.url, sizeof code);
  qr(code, W - 150, Y_MAIN + 22, 146);
  int y = Y_MAIN + 30;
  text(w.hotspot ? "OWN HOTSPOT" : "WIFI", 8, y, &Font0, DIM);
  text(w.ssid, 8, y + 11, &DejaVu12, BLUE);
  y += 36;
  if (w.hotspot) {
    text("PASSWORD", 8, y, &Font0, DIM);
    text(w.pass, 8, y + 11, &DejaVu12, INK);
    y += 36;
  }
  text("OPEN", 8, y, &Font0, DIM);
  text(w.url + (strncmp(w.url, "http://", 7) ? 0 : 7), 8, y + 11, &DejaVu12, ORANGE);
  text("or chordbox.local", 8, y + 30, &Font0, MID);
  y += 50;
  char ph[24];
  snprintf(ph, sizeof ph, "%d PHONE%s CONNECTED", w.phones, w.phones == 1 ? "" : "S");
  text(ph, 8, y, &Font0, w.phones ? GREEN : DIM);
  text(w.hotspot ? "SCAN: JOIN, THEN OPEN" : "SCAN WITH THE CAMERA", W - 146, Y_MAIN + 172, &Font0, DIM);
  text("ANY KEY: BACK", 8, Y_MAIN + 186, &Font0, DIM);
}

// ---------------------------------------------------------------- MORE --
static void drawMore(const UiModel& m) {
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  char t[24];
  snprintf(t, sizeof t, "%s SETTINGS", m.moreTitle);
  text(t, 8, Y_MAIN + 5, &DejaVu12, INK);
  const int RH = 17, Y0 = Y_MAIN + 26, ROWS = 10;
  int first = std::max(0, std::min((int)m.moreSel - ROWS / 2, (int)m.moreN - ROWS));
  for (int r = 0; r < ROWS && first + r < m.moreN; r++) {
    int i = first + r;
    const ParamView& k = m.more[i];
    int y = Y0 + r * RH;
    bool sel = i == m.moreSel;
    int gc = GCOL[k.group < 6 ? k.group : 4];
    if (sel) S->fillRect(4, y - 1, W - 8, RH, INK);
    S->fillRect(8, y + 5, 3, 5, sel ? BG : gc);
    text(k.label, 16, y + 2, &DejaVu9, sel ? BG : INK);
    text(k.value, W - 10, y + 2, &DejaVu9, sel ? BG : (k.kind == K_ACTION ? DIM : MID), lgfx::top_right);
  }
  if (m.moreN > ROWS) {                           // scroll bar
    int h = H_MAIN - 34, bh = h * ROWS / m.moreN;
    int by = Y0 + (h - bh) * first / std::max(1, m.moreN - ROWS);
    S->fillRect(W - 3, by, 2, bh, DIM);
  }
}

// --------------------------------------------------------------- SETUP --
static void drawCalib(const UiModel& m, uint32_t now) {
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  char t[24];
  snprintf(t, sizeof t, "KEYPAD %c SETUP", 'A' + m.calKeypad);
  text(t, 8, Y_MAIN + 6, &DejaVu12, INK);
  char n[8];
  snprintf(n, sizeof n, "%d", m.calNext + 1);
  text("PRESS PAD", 8, Y_MAIN + 40, &Font0, MID);
  text(n, 8, Y_MAIN + 54, &DejaVu40, ORANGE);
  text("LEFT TO RIGHT,", 8, Y_MAIN + 110, &Font0, DIM);
  text("TOP ROW FIRST", 8, Y_MAIN + 122, &Font0, DIM);
  text("CLICK: KEY IS DEAD, SKIP", 8, Y_MAIN + 146, &Font0, DIM);
  text("HOLD: CANCEL", 8, Y_MAIN + 158, &Font0, DIM);
  const int CW = 36, X0 = 160, Y0 = Y_MAIN + 20;
  bool blink = (now / 200) % 2;
  for (int p = 0; p < 16; p++) {
    int x = X0 + (p % 4) * (CW + 4), y = Y0 + (p / 4) * (CW + 4);
    if (m.calDone & (1 << p)) S->fillRect(x, y, CW, CW, GREEN_DK);
    else if (p == m.calNext) S->fillRect(x, y, CW, CW, blink ? ORANGE : ORANGE_DK);
    else S->drawRect(x, y, CW, CW, LINE);
    char s[4];
    snprintf(s, sizeof s, "%d", p + 1);
    text(s, x + CW / 2, y + CW / 2, &Font0, p == m.calNext ? BG : DIM, lgfx::middle_center);
  }
}


// ------------------------------------------------------ HARMONY ENGINE --
// Left: the mode's headline, the one question, the guesses / offers /
// candidates. Right: the 4x4 grid touched last (sounds, or slots while the
// knob is held). Bottom: the loop, 16 slots, playing one lit.
static void miniGrid(const HarmView& h, int x0, int y0, uint32_t now) {
  const int CW = 28, CH = 22, G = 2;
  bool blink = (now / 250) % 2;
  for (int p = 0; p < 16; p++) {
    int x = x0 + (p % 4) * (CW + G), y = y0 + (p / 4) * (CH + G);
    uint8_t c = h.grid[p];
    int fill = -1, edge = LINE, ink = MID;
    switch (c) {
      case GC_OFF: edge = PANEL; ink = DIM; break;
      case GC_DIM: fill = PANEL; ink = INK; break;
      case GC_FIT: fill = GREEN_DK; edge = GREEN; ink = GREEN; break;
      case GC_CLASH: edge = RED; ink = RED; break;
      case GC_TRY: fill = blink ? ORANGE_DK : PANEL; edge = ORANGE; ink = ORANGE; break;
      case GC_SEL: fill = INK; edge = INK; ink = BG; break;
      case GC_PLAY: fill = ORANGE; edge = ORANGE; ink = BG; break;
      case GC_LOCK: fill = PANEL; edge = YELLOW; ink = YELLOW; break;
      case GC_STRONG: fill = GREEN_DK; edge = GREEN; ink = INK; break;
      case GC_SHAPE: fill = BLUE_DK; edge = BLUE_DK; ink = BLUE; break;
    }
    if (fill >= 0) S->fillRoundRect(x, y, CW, CH, 3, fill);
    S->drawRoundRect(x, y, CW, CH, 3, edge);
    char lab[8];
    strncpy(lab, h.pad[p], sizeof lab - 1);
    lab[sizeof lab - 1] = 0;
    if (c == GC_CLASH) {
      S->drawLine(x + 4, y + 4, x + CW - 5, y + CH - 5, RED);
    }
    text(lab, x + CW / 2, y + CH / 2 + 1, &Font0, ink, lgfx::middle_center);
  }
  text(h.gridIsSlots ? "SLOTS" : "SOUNDS", x0, y0 + 4 * (CH + G), &Font0, DIM);
}

static void loopStrip(const HarmView& h, int y0, uint32_t now) {
  const int CW = 39, CH = 34;
  int len = h.loopLen ? h.loopLen : 4;
  int rows = len > 8 ? 2 : 1;
  for (int i = 0; i < 16; i++) {
    int r = i / 8, c = i % 8;
    if (r >= rows) break;
    int x = 2 + c * (CW + 1), y = y0 + r * (CH + 2);
    bool inLoop = i < len;
    uint8_t f = h.slotFlags[i];
    bool set = f & 8;
    bool play = i == h.playSlot, sel = i == h.selSlot && !play;
    if (!inLoop) {
      S->drawRect(x, y, CW, CH, PANEL);
      continue;
    }
    S->fillRect(x, y, CW, CH, play ? ORANGE : set ? PANEL : BG);
    S->drawRect(x, y, CW, CH, sel ? INK : (f & 2) ? YELLOW : LINE);
    if (sel) S->drawRect(x + 1, y + 1, CW - 2, CH - 2, INK);
    char n[4];
    snprintf(n, sizeof n, "%d", i + 1);
    text(n, x + 3, y + 3, &Font0, play ? BG : DIM);
    if (f & 2) S->fillRect(x + CW - 7, y + 3, 4, 4, play ? BG : YELLOW);      // locked
    if (f & 1) S->fillTriangle(x + 2, y + CH - 3, x + 6, y + CH - 3, x + 2, y + CH - 8, play ? BG : BLUE);   // early
    if (set) {
      char nm[10];
      text(fit(h.slot[i], &DejaVu9, CW - 4, nm, sizeof nm), x + CW / 2, y + 21, &DejaVu9, play ? BG : INK,
           lgfx::middle_center);
    } else {
      text("-", x + CW / 2, y + 21, &DejaVu9, DIM, lgfx::middle_center);
    }
    if (h.hmode == HM_HUNT && i == h.cursor) S->fillRect(x + 4, y + CH - 3, CW - 8, 2, GREEN);
  }
}

static void drawHarm(const UiModel& m, uint32_t now) {
  const HarmView& h = m.harm;
  S->fillRect(0, Y_MAIN, W, H_MAIN, BG);
  int x = 6, y = Y_MAIN + 5;
  char buf[32];
  if (h.takes) {                                   // TAKES: newest first, starred ones stay
    text("TAKES", x, y, &DejaVu12, INK);
    snprintf(buf, sizeof buf, "%d KEPT", h.takeN);
    text(buf, x + 54, y + 3, &Font0, DIM);
    for (int r = 0; r < 5; r++) {
      if (!h.takeList[r][0]) break;
      int ry = y + 20 + r * 16;
      bool sel = r == h.takeSel;
      if (sel) S->fillRect(2, ry - 2, 196, 15, INK);
      text(h.takeList[r], x, ry, &Font0, sel ? BG : INK);
    }
    text("CLICK LOADS  YES STARS", x, y + 100, &Font0, DIM);
  } else {
    char t[30];
    text(fit(h.title, &DejaVu12, 192, t, sizeof t), x, y, &DejaVu12, INK);
    y += 20;
    if (h.ask[0]) {
      S->fillRoundRect(x - 2, y - 2, 194, 15, 2, YELLOW);
      text(fit(h.ask, &Font0, 186, t, sizeof t), x + 2, y + 2, &Font0, BG);
      y += 17;
    }
    for (int i = 0; i < h.nLines && i < 4; i++) {
      bool sel = i == h.lineSel;
      if (sel) S->fillRect(2, y - 2, 196, 16, INK);
      text(fit(h.lines[i], &DejaVu9, 188, t, sizeof t), x, y, &DejaVu9, sel ? BG : (h.lineSel < 0 ? MID : INK));
      y += 17;
    }
  }
  miniGrid(h, W - 4 * 30 - 2, Y_MAIN + 4, now);
  // the loop
  int ly = Y_MAIN + 126;
  text("LOOP", 4, ly - 12, &Font0, DIM);
  char lb[40];
  snprintf(lb, sizeof lb, "%d SLOTS  %s%s  A|B %c", h.loopLen, h.playing ? "PLAYING" : "STOPPED",
           h.melodyOn ? " + LINE" : "", h.ab);
  text(lb, 34, ly - 12, &Font0, h.playing ? ORANGE : DIM);
  if (h.printing[0]) chip(h.printing, W - 70, ly - 14, BG, RED);
  loopStrip(h, ly, now);
}

// -------------------------------------------------------------- entry --
void uiRender(lgfx::LGFX_Sprite& sprite, const UiModel& m, uint32_t now, void (*between)()) {
  S = &sprite;
  drawStatus(m, now);
  if (between) between();
  if (m.overlay == OV_MODES) drawModes(m);
  else if (m.overlay == OV_FUNC || m.overlay == OV_PERF) drawFunc(m);
  else if (m.overlay == OV_PHONE) drawPhone(m);
  else {
    switch (m.mode) {
      case M_CHORD: m.notes.on ? drawNotes(m) : m.leftBass ? drawBass(m, now) : drawChord(m, now); break;
      case M_BASS: m.notes.on ? drawNotes(m) : drawBass(m, now); break;
      case M_EDIT: drawEdit(m, now); break;
      case M_MORE: drawMore(m); break;
      case M_CALIB: drawCalib(m, now); break;
      case M_HARM: drawHarm(m, now); break;
    }
  }
  if (between) between();
  drawLegend(m);
  drawToast(m, now);
}

bool uiAnimating(const UiModel& m, uint32_t now) {
  if (m.clk.running) return true;
  if (m.mode == M_HARM) return true;               // suggestions blink
  if ((m.mode == M_BASS || m.leftBass) && m.bass.queuedPad >= 0) return true;
  if (m.mode == M_CALIB) return true;
  (void)now;
  return false;
}
