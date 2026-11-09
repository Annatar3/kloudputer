#!/bin/sh
# Rerunnable: builds a preview from the firmware's real UI headers (theme.h, ui.h, menu.h...) with M5GFX compiled
# for this machine, renders every screen to tests/build/ui/out/*.png (240x135) plus a 3x contact sheet, and fails if
# a paint or redraw check fails or a setup QR code doesn't read back as its text.
#   tests/ui/preview.sh [src-dir]
# Needs g++, OpenSSL headers, python3 with Pillow, and the libraries PlatformIO fetched.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="${1:-$ROOT/src}"
LIBS="$ROOT/.pio/libdeps/cardputer-adv"
GFX="$LIBS/M5GFX/src"
[ -d "$GFX" ] || { echo "M5GFX is missing: run 'pio pkg install -e cardputer-adv' first" >&2; exit 2; }
BUILD="$ROOT/tests/build/ui"
mkdir -p "$BUILD/gfx" "$BUILD/out"

# M5GFX for the host, compiled once: the framebuffer platform never opens a device here, it only gives us sprites.
FLAGS="-O1 -w -DLGFX_LINUX_FB -DLGFX_USE_V1 -I$GFX"
for f in lgfx/v1/LGFXBase.cpp lgfx/v1/lgfx_fonts.cpp lgfx/v1/lgfx_v1.cpp lgfx/v1/lgfx_v1_panel.cpp \
         lgfx/v1/lgfx_v1_platforms.cpp lgfx/utility/lgfx_miniz.c lgfx/utility/lgfx_pngle.c lgfx/utility/lgfx_qoi.c \
         lgfx/utility/lgfx_qrcode.c lgfx/utility/lgfx_tjpgd.c lgfx/Fonts/efont/lgfx_efont_cn.c \
         lgfx/Fonts/efont/lgfx_efont_ja.c lgfx/Fonts/efont/lgfx_efont_kr.c lgfx/Fonts/efont/lgfx_efont_tw.c \
         lgfx/Fonts/IPA/lgfx_font_japan.c; do
  o="$BUILD/gfx/$(basename "${f%.*}").o"
  [ -f "$o" ] && continue
  case "$f" in
    *.cpp) g++ -std=gnu++17 $FLAGS -c "$GFX/$f" -o "$o" ;;
    *) gcc $FLAGS -c "$GFX/$f" -o "$o" ;;
  esac
done

g++ -std=gnu++17 -O1 -w -DLGFX_LINUX_FB -DLGFX_USE_V1 -I"$GFX" -I"$SRC" -I"$ROOT/tests/host" -isystem "$LIBS/ArduinoJson/src" \
  -o "$BUILD/preview" "$HERE/preview.cpp" "$BUILD"/gfx/*.o -lpthread -lcrypto
rm -f "$BUILD"/out/*.ppm "$BUILD"/out/*.png
(cd "$BUILD/out" && ../preview)
python3 -I - "$BUILD/out" <<'PY'
import glob, os, sys
from PIL import Image, ImageDraw
out = sys.argv[1]
shots = sorted(glob.glob(f"{out}/*.ppm"))
for p in shots:
    Image.open(p).save(p[:-4] + ".png")
    os.remove(p)
pngs = sorted(glob.glob(f"{out}/[0-9]*.png"))
S, PAD, LABEL, COLS = 3, 24, 18, 2
w, h = 240 * S, 135 * S
rows = (len(pngs) + COLS - 1) // COLS
sheet = Image.new("RGB", (PAD + COLS * (w + PAD), PAD + rows * (h + LABEL + PAD)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, p in enumerate(pngs):
    x = PAD + (i % COLS) * (w + PAD)
    y = PAD + (i // COLS) * (h + LABEL + PAD)
    d.text((x, y), os.path.basename(p)[:-4], fill=(230, 230, 230))
    sheet.paste(Image.open(p).resize((w, h), Image.NEAREST), (x, y + LABEL))
sheet.save(f"{out}/contact_sheet.png")
print(f"{len(pngs)} screens -> {out}/contact_sheet.png")
PY
(cd "$BUILD/out" && python3 -I "$HERE/qrdecode.py" 13_setup_join.png 'WIFI:T:WPA;S:Kloudputer-1A2B;P:k7mnp2xq4r;;' &&
  python3 -I "$HERE/qrdecode.py" 14_setup_open.png 'http://192.0.2.1/')
