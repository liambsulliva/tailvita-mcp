#!/bin/bash
# Draws the Tailnet Daemon bubble art with ImageMagick. The Vita wants 8-bit
# palette PNGs at exactly these sizes: icon0 128x128, bg0 840x500, startup
# 280x158. Run from this folder; the PNGs are committed.
set -euo pipefail
cd "$(dirname "$0")"
BG="#10202b"; LINE="#2f6f7f"; NODE="#5fd0c0"; HUB="#f2f5f7"; TEXT="#f2f5f7"; DIM="#8fb3bd"
FONT="DejaVu-Sans-Bold"

# A hub joined to three nodes, drawn in a 128x128 box.
GLYPH=(-stroke "$LINE" -strokewidth 5 -fill none
       -draw "line 64,66 30,34" -draw "line 64,66 100,38" -draw "line 64,66 62,106"
       -stroke none -fill "$NODE"
       -draw "circle 30,34 30,23" -draw "circle 100,38 100,27" -draw "circle 62,106 62,95"
       -fill "$HUB" -draw "circle 64,66 64,51")

convert -size 128x128 "xc:$BG" "${GLYPH[@]}" -colors 64 PNG8:icon0.png

# The start button's frame (startup.png) sits over the middle of this image,
# so the middle is left empty and the words go underneath.
convert -size 840x500 "xc:$BG" \
  -font "$FONT" -fill "$TEXT" -pointsize 30 -gravity south -annotate +0+92 "Tailnet Daemon" \
  -font DejaVu-Sans -fill "$DIM" -pointsize 20 \
  -annotate +0+56 "Open to start the background service." \
  -annotate +0+28 "Hold L while opening to stop it." \
  -colors 64 PNG8:bg0.png

convert -size 280x158 "xc:$BG" \
  \( -size 128x128 xc:none "${GLYPH[@]}" -resize 72x72 \) -geometry +22+43 -composite \
  -font "$FONT" -fill "$TEXT" -pointsize 26 -annotate +108+76 "Tailnet" \
  -font "$FONT" -fill "$NODE" -pointsize 26 -annotate +108+108 "Daemon" \
  -colors 64 PNG8:startup.png
