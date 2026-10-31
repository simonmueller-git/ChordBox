// menu.h — the MODE menu's tiles: the pads become these when MODE is pressed
#pragma once
#include <stdint.h>

enum MenuItem : uint8_t {
  MI_CHORD, MI_BASS, MI_HUNT, MI_LISTEN, MI_PLAY, MI_GROW, MI_MELODY, MI_TAKES,
  MI_SETTINGS, MI_PHONE, MI_NEWSONG, MI_BUTTONS, MI_KEYPAD, MI_PRINT, MI_CAPTURE, MI_AB, MI_COUNT
};
static const char* const MENU_NAME[MI_COUNT] = {"CHORD", "BASS", "HUNT", "LISTEN", "PLAY", "GROW", "MELODY",
                                                "TAKES", "SETUP", "PHONE", "NEW SONG", "BUTTONS", "KEYPAD",
                                                "PRINT", "CAPTURE", "A/B"};
static const char* const MENU_HELP[MI_COUNT] = {
    "find progressions from the banks", "basslines that follow your chords", "find a sample's chords by ear",
    "chords and key from incoming MIDI", "play chords into the 16-slot loop", "grow 4 chords into 8 or 16",
    "a melody on top of the loop", "every version you kept", "every setting", "the phone page",
    "forget the key: a new sample", "teach the 4 buttons, top to bottom", "teach the keypad's wiring",
    "the loop once, for the Move to record", "the bars you just played -> the loop", "the previous version, on the bar"};
