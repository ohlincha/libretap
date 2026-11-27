#!/bin/sh
# Builds the hardware guides: HTML from src/ (drawings), then PDF with headless Chrome.
#   docs/hardware/build-pdfs.sh            (needs python3 and google-chrome or chromium)
set -e
cd "$(dirname "$0")"
python3 -I src/make_guides.py
chrome=$(command -v google-chrome || command -v chromium || command -v chromium-browser)
[ -n "$chrome" ] || { echo "needs google-chrome or chromium" >&2; exit 1; }
for f in serial-tap-build usb-sniffer-build pico-firmware-flashing; do
    [ -f "$f.html" ] || continue
    "$chrome" --headless=new --disable-gpu --no-sandbox --no-pdf-header-footer \
        --print-to-pdf="$PWD/$f.pdf" "file://$PWD/$f.html" 2>/dev/null
    echo "built $f.pdf"
done
