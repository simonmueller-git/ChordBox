// ui_common.cpp — LCD settings file and framebuffer screenshots
#include "ui_common.h"
#include <LittleFS.h>

void lcdPrefsLoad(LcdPrefs& p) {
  File f = LittleFS.open("/lcd.bin", "r");
  if (!f) return;
  LcdPrefs t;
  if (f.read((uint8_t*)&t, sizeof t) == sizeof t) p = t;
  f.close();
}

void lcdPrefsSave(const LcdPrefs& p) {
  File f = LittleFS.open("/lcd.bin", "w");
  if (!f) return;
  f.write((const uint8_t*)&p, sizeof p);
  f.close();
}

bool lcdPrefsCommand(LcdPrefs& p, const String& arg, Print& out) {
  bool changed = true;
  if (arg == "invert") p.invert = !p.invert;
  else if (arg == "bgr") p.bgr = !p.bgr;
  else if (arg == "rot") p.rot = p.rot == 1 ? 3 : 1;
  else if (arg.startsWith("mhz ")) p.mhz = constrain(arg.substring(4).toInt(), 10, 80);
  else if (arg.startsWith("bright ")) p.bright = constrain(arg.substring(7).toInt(), 5, 255);
  else changed = false;
  if (changed) lcdPrefsSave(p);
  out.printf("lcd: rot %d  invert %d  bgr %d  %d MHz  bright %d%s\n", p.rot, p.invert, p.bgr, p.mhz, p.bright,
             changed ? "  (saved)" : "   (lcd invert|bgr|rot|mhz N|bright N)");
  return changed;
}

// palette frame -> RGB565 big endian, one row at a time
void shotSprite(lgfx::LGFX_Sprite& s, Print& out) {
  uint16_t lut[16] = {};
  for (int i = 0; i < UI_PALETTE_N; i++) {
    uint16_t v = ((UI_PALETTE[i][0] & 0xF8) << 8) | ((UI_PALETTE[i][1] & 0xFC) << 3) | (UI_PALETTE[i][2] >> 3);
    lut[i] = (v >> 8) | (v << 8);
  }
  out.printf("SHOT %d %d RGB565BE\n", W, H);
  static uint16_t row[W];
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) row[x] = lut[s.readPixelValue(x, y) & 15];
    out.write((const uint8_t*)row, sizeof row);
  }
  out.println();
}
