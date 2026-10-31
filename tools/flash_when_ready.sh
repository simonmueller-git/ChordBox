#!/bin/bash
# Waits for the ESP32-S3 to show up in ROM download mode (hold BOOT, tap RST)
# and flashes build/ as soon as it does. Needed once: the factory firmware
# ignores the automatic reset; ChordBox itself supports it afterwards.
cd "$(dirname "$0")/.."
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=min_spiffs,CDCOnBoot=cdc,USBMode=default,UploadSpeed=921600"
ET=$(ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/ | tail -1)esptool
OLD=${1:-/dev/cu.usbmodem1234561}
deadline=$(( $(date +%s) + ${2:-3600} ))
echo "waiting for download mode (hold BOOT, tap RST) ..."
while [ "$(date +%s)" -lt "$deadline" ]; do
  for P in /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/cu.wchusbserial*; do
    [ -e "$P" ] || continue
    if [ "$P" = "$OLD" ]; then continue; fi
    if "$ET" --port "$P" --before no-reset --after no-reset chip-id >/tmp/chordbox_chip.txt 2>&1; then
      cat /tmp/chordbox_chip.txt | grep -E "Chip|Features|Crystal|MAC"
      echo "flashing $P"
      "$CLI" upload --fqbn "$FQBN" -p "$P" --input-dir build chordbox_s3 && echo "FLASH OK" && exit 0
      echo "FLASH FAILED"; exit 1
    fi
  done
  sleep 1
done
echo "timed out"; exit 2
