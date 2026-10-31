// Copy to local_config.h (git-ignored) and set your own passwords.
#pragma once
#define HOTSPOT_PASSWORD "at-least-8-characters"   // the board's own Wi-Fi, when it can't join yours
#define OTA_PASSWORD "choose-a-long-one"           // ./build.sh esp32 ota; leave it out to turn OTA off
