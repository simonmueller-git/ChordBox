// config.h — ChordBox: pins and tuning constants (ESP32-S3 or Pico)
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------- pins --
#if defined(ARDUINO_ARCH_RP2040)
// Raspberry Pi Pico (fallback build). Micro-USB = USB-MIDI to the Move.
// Display + encoder module (header GND VDD SCL SDA RES DC CS BLK TRA TRB PSH K0):
//   VDD 3V3 (pin 36), GND any GND, SCL GP18, SDA GP19 (SPI0), RES GP20, DC GP21,
//   CS GP22, BLK GP26, TRA GP27, TRB GP28, PSH GP16, K0 GP17
// Keypad (header NC 1 2 3 4 5 6 7 8 NC; 1-4 = C1-C4, 5-8 = R1-R4): pins 1-8 on GP2-GP9.
#define PIN_LCD_SCK   18
#define PIN_LCD_MOSI  19
#define PIN_LCD_RST   20
#define PIN_LCD_DC    21
#define PIN_LCD_CS    22
#define PIN_LCD_BL    26
#define LCD_SPI_HOST  0
#define PIN_ENC_A     27
#define PIN_ENC_B     28
#define PIN_ENC_SW    16
#define PIN_KEY0      17
static const uint8_t KP_LINES[] = {2, 3, 4, 5, 6, 7, 8, 9};        // keypad pins 1-8

#else
// ESP32-S3-DevKitC-1 style, N16R8 (octal PSRAM owns GPIO 35/36/37; 45 and 46
// are strapping pins and stay free; 0 is one too, but a keypad wire on it only
// ever connects to other inputs at power-up, so the board still boots normally). Native USB-C = USB-MIDI to the Move,
// the other USB-C (COM) stays free for power, flashing and the console.
//
// LEFT header, top to bottom: 3V3 3V3 RST 4 5 6 7 15 16 17 18 8 3 46 9 10 11 12 13 14 5V GND
// RIGHT header: GND TX RX 1 2 42 41 40 39 38 37 36 35 0 45 48 47 21 20 19 GND GND
//
// Display + encoder module (Estardyn 2.0" ST7789 + EC11 + KEY0), header
// GND VDD SCL SDA RES DC CS BLK TRA TRB PSH K0:
//   GND -> GND (top of the right header), VDD -> 3V3 (2nd pin on the left);
//   SCL..K0 -> 4 5 6 7 15 16 17 18 8 3: one straight 10-wire run starting at
//   the 4th pin of the left header (RST, the 3rd, is skipped).
//   GPIO 3 only matters at boot if a JTAG eFuse is burned; it isn't here.
// Keypad (header NC 1 2 3 4 5 6 7 8 NC, seen from the front; pins 1-4 are the
//   columns C1-C4, 5-8 the rows R1-R4; the NC pins stay unconnected):
//   pins 1-7 -> 1 2 42 41 40 39 38, the right header's 4th-10th pins in order,
//   pin 8 -> 0 (14th). Every pair of wires is scanned, so KEYPAD SETUP can
//   remap any other wiring. Until the second knob is fitted, pin 8 may also
//   still sit on 47 (17th, the v1 wiring): both are scanned then.
// Wiring v2 adds:
//   Second knob (round EC11 board, header GND S1 S2 KEY 5V): GND -> GND (right
//   header 21st), S1 -> 21 (18th), S2 -> 47 (17th), KEY -> 48 (16th), and its
//   "5V" pin -> 3V3 (it only feeds the pull-ups). Found at power-up by its
//   pull-ups; GPIO 48 is also the RGB light's data pin, so the light stays off.
//   I2C OLED with 4 buttons (header GND VCC SCL SDA K4 K3 K2 K1): GND -> GND
//   (left header's last pin), VCC -> 3V3 (1st pin, shared), SCL 9, SDA 10,
//   K4 11, K3 12, K2 13, K1 14: the left header's 15th-20th pins in order.
#define PIN_LCD_SCK   4
#define PIN_LCD_MOSI  5
#define PIN_LCD_RST   6
#define PIN_LCD_DC    7
#define PIN_LCD_CS    15
#define PIN_LCD_BL    16
#define LCD_SPI_HOST  SPI2_HOST
#define PIN_ENC_A     17
#define PIN_ENC_B     18
#define PIN_ENC_SW    8
#define PIN_KEY0      3
static const uint8_t KP_LINES[] = {1, 2, 42, 41, 40, 39, 38, 0};   // keypad pins 1-8
#define KP_ALT_LINE   47           // keypad pin 8's old pin, scanned while no second knob sits there
#define HAS_EXTRAS    1            // the v2 parts: second knob, OLED + buttons
#define PIN_ENC2_A    21
#define PIN_ENC2_B    47
#define PIN_ENC2_SW   48
#define PIN_OLED_SCL  9
#define PIN_OLED_SDA  10
static const uint8_t BTN_PINS[4] = {14, 13, 12, 11};                // the OLED's K1-K4
#endif
#ifndef HAS_EXTRAS
#define HAS_EXTRAS    0            // Pico: no second knob or OLED
#endif
#define KP_MAX_KEYPADS 1           // one keypad; a second would need its own wires

// ------------------------------------------------------------- input --
#define ENC_COUNTS_PER_DETENT 4    // EC11 / KY-040: a full quadrature cycle per click
#define HOLD_MS       300          // encoder: tap vs hold (function layer)
#define BTN_HOLD_MS   450          // OLED < >: held this long = change genre
#define KEY0_SHOW_MS  250          // KEY0 held this long: the PERFORM layer shows
#define PAD_PRESS_MS  1            // a press counts once two scans agree: ~1-2 ms to the note
#define PAD_DEBOUNCE_MS 6          // a release must stay open this long, so bounce can't retrigger

// ------------------------------------------------------------- display --
#define UI_FRAME_MS   16           // redraw at most ~60 fps, only when needed
#define UI_ANIM_MS    33           // ~30 fps while the bass is running

// --------------------------------------------------------------- timing --
#define FOCUS_MS      1400         // big readout after a push-turn
#define TOAST_MS      1600
#define SAVE_IDLE_MS  4000         // flash writes only after this much quiet
#define EXT_CLOCK_TIMEOUT_MS 400   // no clock for this long = the Move stopped sending

// ----------------------------------------------------------------- MIDI --
#define DEFAULT_CHORD_CH   0       // 0-based: MIDI ch 1
#define DEFAULT_BASS_CH    1       // MIDI ch 2

// ---- network passwords: put your own in firmware/local_config.h (git-ignored,
// see local_config.example.h). With no OTA_PASSWORD, updates over Wi-Fi stay off.
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef HOTSPOT_PASSWORD
#define HOTSPOT_PASSWORD "chordbox-move"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
