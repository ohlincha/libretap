#!/bin/sh
# End-to-end test with the Windows Tick under Wine: Tick.exe /run fakevendor.exe on COM1, COM1 = a pseudo-terminal
# of relay.py (a pretend instrument that also copies both directions to two more pseudo-terminals), LibreTap's
# serial tap (LibreTapCLI record) on those two. Then the Tick session is imported and matched: every byte must
# line up and the clock offset must be small.
#   tests/scripts/e2e-wine.sh BUILD_DIR [SCRATCH]
# Needs: a Wine (WINE=..., default ~/opt/wine-11.20-amd64-wow64/bin/wine), xvfb-run, python3, and Tick's
# build/Tick.exe, build/tickctl.exe, build/fakevendor.exe (in ../tick; not modified here). Own WINEPREFIX.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${1:?build dir}" && pwd)
S=${2:-/tmp/libretap-e2e-wine}
tick="$here/../../../tick/build"
WINE=${WINE:-$HOME/opt/wine-11.20-amd64-wow64/bin/wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX="$S/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
for f in "$tick/Tick.exe" "$tick/tickctl.exe" "$tick/fakevendor.exe" "$WINE"; do
    [ -e "$f" ] || { echo "SKIP: missing $f"; exit 77; }
done
command -v xvfb-run >/dev/null || { echo "SKIP: no xvfb-run"; exit 77; }
rm -rf "$S/run" "$S/ws"; mkdir -p "$S/run" "$S/ws"
[ -d "$WINEPREFIX/drive_c/windows" ] || (unset DISPLAY; "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w)
"$WINE" reg add 'HKLM\Software\Wine\Ports' /v COM1 /d "$S/com1-link" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
t="$WINEPREFIX/drive_c/e2e"; rm -rf "$t"; mkdir -p "$t"
cp "$tick/Tick.exe" "$tick/tickctl.exe" "$tick/fakevendor.exe" "$t/"

python3 -I "$here/relay.py" "$S/com1-link" 90 > "$S/run/relay.txt" &
relay=$!
sleep 1
A=$(sed -n 's/^A //p' "$S/run/relay.txt"); B=$(sed -n 's/^B //p' "$S/run/relay.txt")
export LIBRETAP_HOME="$S/ws" LIBRETAP_CONTROL=libretap-e2e-wine LIBRETAP_NO_SYSTEM_CAMERAS=1
"$build/LibreTapCLI" record --a "$A" --b "$B" --seconds 60 > "$S/run/record.txt" 2>&1 &
rec=$!
sleep 2
cat > "$S/run/wine.sh" <<EOS
cd "$t"
"$WINE" Tick.exe /run "C:\\\\e2e\\\\fakevendor.exe" COM1 6 300 &
sleep 14
"$WINE" tickctl.exe close 2>/dev/null; sleep 3; "$WINE" tickctl.exe close 2>/dev/null; sleep 2
"$WINESERVER" -k
EOS
timeout 120 xvfb-run -a -s "-screen 0 1024x768x24" sh "$S/run/wine.sh" > "$S/run/wine.log" 2>&1 || true
"$build/LibreTapCLI" stop >/dev/null 2>&1 || true
wait $rec || true
kill $relay 2>/dev/null || true
session=$(ls -d "$t"/TickSessions/*/ | head -1)
[ -n "$session" ] || { echo "FAIL: Tick wrote no session"; cat "$S/run/wine.log"; exit 1; }
"$build/LibreTapCLI" import latest "$session" --json > "$S/run/import.json"
"$build/LibreTapCLI" show latest --json > "$S/run/show.json"
python3 -I - "$S/run/import.json" "$S/run/show.json" <<'EOP'
import json, sys
imp = json.load(open(sys.argv[1])); show = json.load(open(sys.argv[2]))
print("match:", imp.get("message"))
assert imp.get("matched"), "clocks not matched"
assert abs(imp["offset"]) < 0.2, "offset %r too large (same computer)" % imp["offset"]
streams = {}
for r in show["rows"]:
    if r["kind"] == "data":
        key = (r["session"], r["who"].split(" (")[0])
        streams[key] = streams.get(key, "") + r["hex"]
tap_a = streams.get((0, "PC → Instrument"), ""); tap_b = streams.get((0, "Instrument → PC"), "")
wrote = streams.get((1, "Program wrote"), ""); read = streams.get((1, "Program read"), "")
print("tap A %d bytes, program wrote %d; tap B %d, program read %d" % (len(tap_a) // 2, len(wrote) // 2, len(tap_b) // 2, len(read) // 2))
assert wrote and tap_a == wrote, "the PC's bytes differ"
assert read and tap_b == read, "the instrument's bytes differ"
print("OK: every byte lines up, offset %.4f s" % imp["offset"])
EOP
