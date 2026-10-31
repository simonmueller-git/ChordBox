#!/bin/bash
# Build / flash ChordBox.
#   ./build.sh [pico|esp32]              regenerate data headers + compile (default: esp32)
#   ./build.sh [pico|esp32] flash [port] compile + flash
#   ./build.sh esp32 ota [host]          compile + flash over Wi-Fi (default chordbox.local)
set -e
cd "$(dirname "$0")"
CLI=$(command -v arduino-cli || echo "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli")
PICO_URL=https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

TARGET=esp32
case "$1" in pico|esp32) TARGET=$1; shift ;; esac

if [ "$TARGET" = pico ]; then
  # 240 MHz overclock for a quicker screen (freq=133 is stock). Adafruit
  # TinyUSB stack for USB-MIDI. 256 KB LittleFS for favourites and settings.
  FQBN="rp2040:rp2040:rpipico:flash=2097152_262144,freq=240,usbstack=tinyusb"
  FLAGS=""
  PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
else
  # USBMode=default = TinyUSB on the native port; CDCOnBoot=cdc keeps a console
  # there too. min_spiffs: two app slots + a small LittleFS for favs.txt.
  FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=min_spiffs,CDCOnBoot=cdc,USBMode=default,UploadSpeed=921600"
  FLAGS=""
  PORTS="/dev/cu.usbmodem* /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/ttyACM* /dev/ttyUSB*"
fi
OUT="build/$TARGET"

python3 tools/gen_data.py
"$CLI" compile --fqbn "$FQBN" --additional-urls "$PICO_URL" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --build-property "compiler.c.extra_flags=$FLAGS" \
  --output-dir "$OUT" firmware

if [ "$1" = "ota" ]; then
  # the password you set in firmware/local_config.h (without one, the firmware has OTA off)
  OTA_PASS=$(sed -n 's/^#define OTA_PASSWORD *"\(.*\)".*/\1/p' firmware/local_config.h 2>/dev/null)
  [ -n "$OTA_PASS" ] || { echo "set OTA_PASSWORD in firmware/local_config.h first"; exit 1; }
  ESPOTA=$(find ~/Library/Arduino15/packages/esp32/hardware/esp32 ~/.arduino15/packages/esp32/hardware/esp32 -name espota.py 2>/dev/null | head -1)
  HOST=${2:-chordbox.local}
  IP=$(dscacheutil -q host -a name "$HOST" 2>/dev/null | awk '/ip_address/ {print $2; exit}')
  echo "updating ChordBox at ${IP:-$HOST} over Wi-Fi"
  exec python3 "$ESPOTA" -i "${IP:-$HOST}" -p 3232 -a "$OTA_PASS" -f "$OUT/firmware.ino.bin" -r
fi
[ "$1" = "flash" ] || exit 0
P=${2:-$(ls $PORTS 2>/dev/null | head -1)}
[ -n "$P" ] || { echo "no board port found"; exit 1; }
echo "flashing $TARGET on $P"
if ! "$CLI" upload --fqbn "$FQBN" --additional-urls "$PICO_URL" -p "$P" --input-dir "$OUT" firmware; then
  [ "$TARGET" = pico ] || exit 1
  # macOS sometimes doesn't mount the Pico's boot drive in time: load over USB instead
  PT=$(find ~/Library/Arduino15/packages/rp2040/tools -name picotool -type f -perm +111 | head -1)
  sleep 2
  "$PT" load -x "$OUT/firmware.ino.uf2"
fi
