# boot.py — USB MIDI + storage mode select
#
# Default (just plug in): COMPUTER MODE — your Mac can edit files, but the
# device cannot save, so STAR/PRINT show "RO!" and keep favs in RAM only.
#
# Hold CONFIRM (GP2) while plugging in / resetting: PERFORMANCE MODE —
# the device can write favs.txt; the drive appears read-only to a
# computer. Use this when attached to the Move.
#
# boot.py only runs on hard reset (power cycle), not on auto-reload.

import time
import board
import digitalio
import storage
import usb_midi

usb_midi.enable()

b = digitalio.DigitalInOut(board.GP2)
b.direction = digitalio.Direction.INPUT
b.pull = digitalio.Pull.UP
time.sleep(0.05)
if not b.value:                      # CONFIRM held -> performance mode
    storage.remount('/', readonly=False)
b.deinit()
