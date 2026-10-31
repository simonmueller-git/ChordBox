// ui_oled.cpp — the OLED: a UiModel in, a 128x64 one-bit frame out.
// Pure drawing like ui_draw.cpp, so tests/host renders it on the Mac too.
//
// The four buttons sit beside the OLED's right edge, top to bottom, so the
// right-hand column (x 86..127) names what each one does right now, row by
// row: MODE, PADS (or SLOTS / MENU / SOUNDS), NO, YES — and their SHIFT jobs
// while SHIFT is held (drawn inverted). The left 84 pixels show the screen's
// status: the bass line in CHORD / BASS, the loop and key in the Harmony modes.

#define LGFX_USE_V1
#include "ui_oled.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "menu.h"

using namespace lgfx::v1::fonts;

static const int LW = 78;                          // the status area's width

static void txt(lgfx::LGFX_Sprite& s, const char* str, int x, int y, const lgfx::IFont* f,
                lgfx::textdatum_t d = lgfx::top_left, int c = 1) {
  s.setFont(f);
  s.setTextColor(c);
  s.setTextDatum(d);
  s.drawString(str, x, y);
}

static void fitTxt(lgfx::LGFX_Sprite& s, const char* str, int x, int y, int maxW, const lgfx::IFont* f,
                   lgfx::textdatum_t d = lgfx::top_left) {
  char buf[28];
  strncpy(buf, str, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  s.setFont(f);
  for (size_t n = strlen(buf); n > 1 && s.textWidth(buf) > maxW; n--) buf[n - 1] = 0;
  txt(s, buf, x, y, f, d);
}

// bold capitals set tight (the feel words are wider than the column otherwise)
static int tightW(lgfx::LGFX_Sprite& s, const char* str, int track) {
  s.setFont(&FreeSansBold9pt7b);
  int w = 0;
  for (const char* p = str; *p; p++) {
    char c[2] = {*p, 0};
    w += s.textWidth(c) + (p[1] ? track : 0);
  }
  return w;
}

static void tight(lgfx::LGFX_Sprite& s, const char* str, int x, int y, int maxW) {
  int track = -1;
  while (track > -4 && tightW(s, str, track) > maxW) track--;
  for (const char* p = str; *p; p++) {
    char c[2] = {*p, 0};
    txt(s, c, x, y, &FreeSansBold9pt7b);
    x += s.textWidth(c) + track;
  }
}

// the four buttons' names, one row each, beside the buttons
static void softColumn(lgfx::LGFX_Sprite& s, const UiModel& m) {
  s.drawFastVLine(LW + 1, 0, 64, 1);
  for (int i = 0; i < 4; i++) {
    const char* t = m.soft[i];
    if (!t[0] || !strcmp(t, "-")) continue;
    int y = i * 16;
    if (m.shiftHeld) {                             // SHIFT: the second jobs, inverted
      s.fillRect(LW + 3, y + 2, 127 - LW - 3, 12, 1);
      s.setFont(&Font0);
      char b[12];
      strncpy(b, t, sizeof b - 1);
      b[sizeof b - 1] = 0;
      while (strlen(b) > 1 && s.textWidth(b) > 127 - LW - 6) b[strlen(b) - 1] = 0;
      txt(s, b, 126, y + 4, &Font0, lgfx::top_right, 0);
    } else {
      fitTxt(s, t, 126, y + 4, 127 - LW - 4, &Font0, lgfx::top_right);
    }
  }
}

static void learnScreen(lgfx::LGFX_Sprite& s, const UiModel& m) {
  static const char* const POS[4] = {"TOP", "SECOND", "THIRD", "BOTTOM"};
  int r = std::min(3, std::max(0, m.btnLearn - 1));
  txt(s, "BUTTONS", 0, 0, &Font0);
  txt(s, "PRESS THE", 0, 16, &Font0);
  txt(s, POS[r], 0, 28, &FreeSansBold9pt7b);
  txt(s, "BUTTON", 0, 52, &Font0);
  int y = r * 16 + 8;                                // an arrow at that button's row
  s.fillTriangle(124, y, 112, y - 6, 112, y + 6, 1);
  s.drawFastHLine(92, y, 22, 1);
}

static void menuScreen(lgfx::LGFX_Sprite& s, const UiModel& m) {
  int i = m.modeSel < MI_COUNT ? m.modeSel : 0;
  txt(s, "MODE MENU", 0, 0, &Font0);
  fitTxt(s, MENU_NAME[i], 0, 12, LW, &FreeSansBold9pt7b);
  // the help, two short lines
  const char* h = MENU_HELP[i];
  char a[16] = "", b[24] = "";
  const char* sp = strchr(h + (strlen(h) > 13 ? 13 : 0), ' ');
  if (strlen(h) <= 14 || !sp) strncpy(a, h, sizeof a - 1);
  else {
    int n = (int)(sp - h);
    if (n > 15) n = 15;
    memcpy(a, h, n);
    a[n] = 0;
    strncpy(b, sp + 1, sizeof b - 1);
  }
  fitTxt(s, a, 0, 36, LW, &Font0);
  fitTxt(s, b, 0, 46, LW, &Font0);
  char pad[16];
  snprintf(pad, sizeof pad, "PAD %d", i + 1);
  txt(s, pad, 0, 56, &Font0);
}

// the Harmony Engine: mode, the chord now, the key and how sure, the loop, the question
static void harmScreen(lgfx::LGFX_Sprite& s, const UiModel& m, uint32_t now) {
  static const char* const NAME[HM_COUNT] = {"HUNT", "LISTEN", "PLAY", "GROW", "MELODY"};
  const HarmView& h = m.harm;
  char buf[32];
  txt(s, h.takes ? "TAKES" : h.slotLayer ? "SLOTS" : NAME[h.hmode < HM_COUNT ? h.hmode : 2], 0, 0, &Font0);
  for (int i = 0; i < 5; i++) {                      // how sure of the key: five bars
    int bh = 2 + i * 1, x = 62 + i * 4;
    if (i < h.sureBars) s.fillRect(x, 7 - bh, 3, bh + 1, 1);
    else s.drawFastHLine(x, 7, 3, 1);
  }
  fitTxt(s, h.nowChord[0] ? h.nowChord : "-", 0, 11, LW, &FreeSansBold9pt7b);
  fitTxt(s, h.key, 0, 29, LW, &Font0);
  // the loop: one box a slot, the one playing filled, the selected one underlined
  int len = h.loopLen ? h.loopLen : 4, cw = LW / len;
  for (int i = 0; i < len; i++) {
    int x = i * cw;
    bool set = h.slotFlags[i] & 8;
    if (i == h.playSlot) s.fillRect(x + 1, 39, cw - 2, 7, 1);
    else if (set) s.drawRect(x + 1, 39, cw - 2, 7, 1);
    else s.drawFastHLine(x + 1, 45, cw - 2, 1);
    if (i == h.selSlot) s.drawFastHLine(x + 1, 48, cw - 2, 1);
  }
  if (m.toast[0] && now < m.toastUntil) snprintf(buf, sizeof buf, "%s", m.toast);
  else if (h.ask[0]) snprintf(buf, sizeof buf, "%s", h.ask);
  else snprintf(buf, sizeof buf, "%s %s", m.setLabel, m.setValue);
  fitTxt(s, buf, 0, 54, LW, &Font0);
}

// the bass station (CHORD / BASS): the line, its steps, the chord it follows
static const char* const PCN[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

static void notesScreen(lgfx::LGFX_Sprite& s, const UiModel& m) {
  const auto& n = m.notes;
  char buf[32];
  snprintf(buf, sizeof buf, "NOTES  C%d", n.base / 12 - 2);
  txt(s, buf, 0, 0, &Font0);
  if (n.last >= 0) {
    snprintf(buf, sizeof buf, "%s%d", PCN[n.last % 12], n.last / 12 - 2);
    txt(s, buf, 0, 10, &FreeSansBold9pt7b);
  } else {
    txt(s, "tap a pad", 0, 14, &Font0);
  }
  fitTxt(s, n.key, 0, 30, LW, &Font0);
  for (int pc = 0; pc < 12; pc++) {              // the 12 notes: filled = fits, cross = clashes
    int x = pc * 6 + 1, y = 42;
    if (n.fit & (1 << pc)) s.fillRect(x, y, 5, 5, 1);
    else if (n.clash & (1 << pc)) s.drawLine(x, y, x + 4, y + 4, 1), s.drawLine(x + 4, y, x, y + 4, 1);
    else s.drawPixel(x + 2, y + 2, 1);
  }
  txt(s, "click = KEY", 0, 54, &Font0);
}

static void bassScreen(lgfx::LGFX_Sprite& s, const UiModel& m, uint32_t now) {
  if (m.notes.on) {
    notesScreen(s, m);
    return;
  }
  const BassView& b = m.bass;
  bool playing = m.clk.running && !b.muted;
  char buf[40];
  if (b.any) snprintf(buf, sizeof buf, "ANY %s", b.genre);
  else snprintf(buf, sizeof buf, "%s", b.bank);
  fitTxt(s, buf, 0, 0, LW - 28, &Font0);
  const char* st = !m.clk.running ? "OFF" : b.muted ? "MUTE" : "ON";
  if (playing) {
    s.fillRect(LW - 19, 0, 19, 8, 1);
    txt(s, st, LW - 10, 0, &Font0, lgfx::top_center, 0);
  } else {
    txt(s, st, LW - 1, 0, &Font0, lgfx::top_right);
  }
  if (!b.name[0]) {
    txt(s, "NO LINE", 0, 14, &FreeSansBold9pt7b);
    fitTxt(s, "press DOWN", 0, 40, LW, &Font0);
    return;
  }
  // what the line is: its feel (big), its energy, what stands out (names say nothing)
  tight(s, b.feel, 0, 11, LW);
  for (int i = 0; i < 5; i++) {                   // energy, beside what stands out
    int bh = 2 + i * 2;
    if (i < b.energy) s.fillRect(LW - 14 + i * 3, 36 - bh, 2, bh, 1);
    else s.drawPixel(LW - 14 + i * 3, 35, 1);
  }
  if (b.nextFeel[0]) {                            // queued for the next bar
    snprintf(buf, sizeof buf, "NEXT %s", b.nextFeel);
    s.setFont(&Font0);
    s.fillRect(0, 28, s.textWidth(buf) + 4, 9, 1);
    txt(s, buf, 2, 29, &Font0, lgfx::top_left, 0);
  } else {
    bassTraitWords(b.traits, buf, sizeof buf, 2);
    s.setFont(&Font0);
    if (s.textWidth(buf) > LW - 17) bassTraitWords(b.traits, buf, sizeof buf, 1);
    txt(s, buf, 0, 29, &Font0);
  }
  // this bar's steps: 16 columns of 5 pixels (a dot = DENSITY skips it)
  const int Y0 = 40, HT = 11;
  int bars = std::max(1, b.len / 16);
  int bar = m.clk.running ? b.patBar % bars : 0;
  int cur = m.clk.running ? (int)(m.clk.step % 16) : -1;
  int lo = 99, hi = -99;
  for (int i = 0; i < b.len; i++)
    if (b.step[i].flags & SF_NOTE) lo = std::min(lo, (int)b.step[i].semis), hi = std::max(hi, (int)b.step[i].semis);
  if (lo > hi) lo = hi = 0;
  int span = std::max(12, hi - lo);
  for (int k = 0; k < 16; k++) {
    int i = bar * 16 + k;
    if (i >= b.len) break;
    const StepView& stp = b.step[i];
    int x = k * 5;
    if (!(stp.flags & SF_NOTE)) continue;
    int h = 2 + (stp.semis - lo) * (HT - 2) / span;
    if (stp.flags & SF_SKIP) s.drawPixel(x + 1, Y0 + HT - h, 1);
    else if (stp.flags & SF_GHOST) s.drawRect(x, Y0 + HT - h, 4, h, 1);
    else s.fillRect(x, Y0 + HT - h, 4, h, 1);
  }
  if (cur >= 0) s.drawFastHLine(cur * 5, Y0 + HT + 1, 4, 1);
  if (m.toast[0] && now < m.toastUntil) {
    fitTxt(s, m.toast, 0, 54, LW, &Font0);
    return;
  }
  // the bottom line: the knob's setting (turn = change, click = the next one)
  s.fillRect(0, 53, LW - 1, 11, 1);
  s.setFont(&Font0);
  int vw = s.textWidth(m.setValue);
  char lb[12];
  strlcpy(lb, m.setLabel, sizeof lb);
  for (size_t n = strlen(lb); n > 1 && s.textWidth(lb) > LW - 9 - vw; n--) lb[n - 1] = 0;
  txt(s, lb, 2, 55, &Font0, lgfx::top_left, 0);
  txt(s, m.setValue, LW - 3, 55, &Font0, lgfx::top_right, 0);
}

void oledRender(lgfx::LGFX_Sprite& s, const UiModel& m, uint32_t now) {
  s.fillScreen(0);
  if (m.btnLearn) {
    learnScreen(s, m);
    return;
  }
  if (m.overlay == OV_MODES && m.mode == M_HARM) menuScreen(s, m);
  else if (m.mode == M_HARM) harmScreen(s, m, now);
  else bassScreen(s, m, now);
  softColumn(s, m);
}

bool oledAnimating(const UiModel& m, uint32_t now) {
  return m.clk.running || m.mode == M_HARM || m.btnLearn || (m.toast[0] && now < m.toastUntil);
}
