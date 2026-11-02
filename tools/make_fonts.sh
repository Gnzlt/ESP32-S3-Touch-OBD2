#!/usr/bin/env bash
# Regenerates the LVGL bitmap fonts in main/ui/assets/fonts/ from Barlow (SIL OFL 1.1,
# https://github.com/jpt/barlow). The generated .c files are committed, so a build needs
# neither Node nor the TTFs. Needs: curl, npx (Node).
#   tools/make_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CACHE="${TMPDIR:-/tmp}/obd2-dash-fonts"
OUT=main/ui/assets/fonts
CONV="npx --yes lv_font_conv@1.5.3"
mkdir -p "$CACHE" "$OUT"

for f in barlowcondensed/BarlowCondensed-SemiBold.ttf barlowcondensed/BarlowCondensed-Medium.ttf \
         barlow/Barlow-Medium.ttf barlow/Barlow-SemiBold.ttf; do
    [ -f "$CACHE/$(basename $f)" ] || curl -sSfL -o "$CACHE/$(basename $f)" \
        "https://raw.githubusercontent.com/google/fonts/main/ofl/$f"
done

COND_SB="$CACHE/BarlowCondensed-SemiBold.ttf"
COND_MD="$CACHE/BarlowCondensed-Medium.ttf"
TEXT_MD="$CACHE/Barlow-Medium.ttf"
TEXT_SB="$CACHE/Barlow-SemiBold.ttf"

ASCII="0x20-0x7E"
EXTRA="0xB0,0xB1,0x2013,0x2026"      # ° ± – …
DIGITS="0x30-0x39,0x2D,0x2E,0x20"    # 0-9 - . space

gen() {   # name ttf size ranges
    local name=$1 ttf=$2 size=$3 ranges=$4
    $CONV --font "$ttf" -r "$ranges" --size "$size" --bpp 4 --format lvgl --no-compress \
        --lv-font-name "$name" -o "$OUT/$name.c" >/dev/null
    perl -pi -e "s|\Q$CACHE/\E||g" "$OUT/$name.c"   # keep local paths out of the header
    echo "  $OUT/$name.c"
}

echo "Generating fonts:"
gen font_num_120 "$COND_SB" 120 "$DIGITS"                         # speed
gen font_num_80  "$COND_SB" 80  "$DIGITS,0x4E,0x2B,0x25,0xB0"     # big values, gear (N), + % °
gen font_num_48  "$COND_SB" 48  "$ASCII,$EXTRA"                   # medium values
gen font_num_32  "$COND_MD" 32  "$ASCII,$EXTRA"                   # small values
gen font_txt_24  "$TEXT_SB" 24  "$ASCII,$EXTRA"                   # titles
gen font_txt_18  "$TEXT_MD" 18  "$ASCII,$EXTRA"                   # labels
gen font_txt_14  "$TEXT_MD" 14  "$ASCII,$EXTRA"                   # captions
