#!/bin/bash
# Render the real UI code on the Mac -> tests/host/screens/*.png + contact sheet.
set -e
shopt -s nullglob
cd "$(dirname "$0")"
ROOT=../..
L=~/Documents/Arduino/libraries/LovyanGFX/src
OBJ=/tmp/chordbox_lgfx_obj
mkdir -p "$OBJ" /tmp/chordbox_screens screens
SDL="-I/opt/homebrew/include -I/opt/homebrew/include/SDL2"
if [ ! -f "$OBJ/.done" ]; then                    # LovyanGFX compiles once
  for f in $L/lgfx/Fonts/efont/*.c $L/lgfx/Fonts/IPA/*.c $L/lgfx/utility/*.c $L/lgfx/v1/lv_font/*.c; do
    clang -c -O1 -w -DLGFX_SDL -I$L $SDL "$f" -o "$OBJ/$(basename "$f").o"
  done
  for f in $L/lgfx/v1/*.cpp $L/lgfx/v1/misc/*.cpp $L/lgfx/v1/panel/Panel_Device.cpp $L/lgfx/v1/panel/Panel_FrameBufferBase.cpp $L/lgfx/v1/platforms/sdl/*.cpp; do
    clang++ -c -std=c++17 -O1 -w -DLGFX_SDL -I$L $SDL "$f" -o "$OBJ/$(basename "$f").o"
  done
  touch "$OBJ/.done"
fi
clang++ -std=c++17 -O1 -w -DLGFX_SDL -Ishim -I$ROOT/firmware -I$L $SDL \
  render_screens.cpp $ROOT/firmware/engine.cpp $ROOT/firmware/bass.cpp $ROOT/firmware/clock.cpp $ROOT/firmware/app.cpp $ROOT/firmware/web.cpp $ROOT/firmware/ui_draw.cpp $ROOT/firmware/ui_oled.cpp $ROOT/firmware/harm.cpp $ROOT/firmware/harm_theory.cpp $ROOT/firmware/harm_listen.cpp $ROOT/firmware/harm_grow.cpp $ROOT/firmware/harm_melody.cpp $OBJ/*.o \
  -L/opt/homebrew/lib -lSDL2 -o /tmp/chordbox_render
rm -f /tmp/chordbox_screens/*.rgb565 /tmp/chordbox_screens/*.oled
/tmp/chordbox_render >/dev/null
python3 - <<'EOF'
import glob, os
from PIL import Image
W, H, S = 320, 240, 2
files = sorted(glob.glob("/tmp/chordbox_screens/*.rgb565"))
imgs = []
for p in files:
    raw = open(p, "rb").read()
    img = Image.new("RGB", (W, H)); px = img.load()
    for i in range(W * H):
        v = (raw[2*i] << 8) | raw[2*i+1]                 # sprite memory is big endian
        px[i % W, i // W] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    img = img.resize((W * S, H * S), Image.NEAREST)
    name = os.path.basename(p).replace(".rgb565", ".png")
    img.save("screens/" + name); imgs.append(img)
cols = 3; rows = (len(imgs) + cols - 1) // cols; g = 16
sheet = Image.new("RGB", (cols * (W*S + g) + g, rows * (H*S + g) + g), (40, 41, 46))
for i, im in enumerate(imgs):
    sheet.paste(im, (g + (i % cols) * (W*S + g), g + (i // cols) * (H*S + g)))
sheet.save("screens/_contact_sheet.png")
for p in sorted(glob.glob("/tmp/chordbox_screens/*.oled")):     # the bass OLED: white on black, 4x
    raw = open(p, "rb").read()
    img = Image.new("RGB", (128, 64)); px = img.load()
    for i in range(128 * 64):
        px[i % 128, i // 128] = (235, 240, 255) if raw[i] else (0, 0, 0)
    img.resize((512, 256), Image.NEAREST).save("screens/" + os.path.basename(p).replace(".oled", ".png"))
print(len(imgs), "screens -> tests/host/screens/")
EOF
