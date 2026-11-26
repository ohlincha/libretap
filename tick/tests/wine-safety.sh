#!/bin/sh
# Worst-case tests of Tick under Wine (see SAFETY.md):
#  A  Tick is killed (taskkill /f) while it listens to a program -> the program keeps
#     working and finishes; the session is readable up to the kill.
#  B  the listened-to program is killed -> Tick notes it and keeps recording.
#  C  the session folder disappears while recording ("USB stick pulled") -> Tick keeps
#     running, reports the error, the program is not disturbed.
#  D  Tick is closed normally while the program runs -> the program keeps working.
# Usage: tests/wine-safety.sh WINVER SCRATCH   (same environment as wine-test.sh)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-win2k}
scratch=${2:-/tmp/tick-wine-test}
WINE=${WINE:-wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX=${WINEPREFIX:-$scratch/pfx} WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
"$WINE" reg add 'HKLM\Software\Wine\Ports' /v COM1 /d "$scratch/com1-link" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
t="$WINEPREFIX/drive_c/safety"
rm -rf "$t"; mkdir -p "$t"
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$here/build/fakevendor.exe" "$t/"
out="$scratch/safety-$ver"; rm -rf "$out"; mkdir -p "$out"
export WINE WINESERVER here scratch out t

cat > "$scratch/safety-$ver.sh" <<'EOS'
cd "$t"
vendor='C:\safety\fakevendor.exe'
python3 -I "$here/tests/fake_instrument.py" "$scratch/com1-link" 240 &
sleep 1
echo "== A: kill Tick while listening"
"$WINE" Tick.exe /run "$vendor" COM1 20 300 &
sleep 5
"$WINE" taskkill /f /im Tick.exe
sleep 25
echo "A fakevendor.log:"; cat fakevendor.log; rm -f fakevendor.log

echo "== B: kill the program while listening"
"$WINE" Tick.exe /run "$vendor" COM1 40 300 &
sleep 6
import -window root "$out/B-listening.png"
"$WINE" taskkill /f /im fakevendor.exe
sleep 3
"$WINE" tickctl.exe close
sleep 2

echo "== C: session folder removed while recording and listening"
"$WINE" Tick.exe /run "$vendor" COM1 20 300 &
sleep 5
d=$(ls -d TickSessions/*/ | tail -1); echo "removing $d"; rm -rf "$d"
sleep 4
import -window root "$out/C-after-removal.png"
sleep 20
echo "C fakevendor.log:"; cat fakevendor.log; rm -f fakevendor.log
"$WINE" tickctl.exe close
sleep 2

echo "== D: close Tick normally while the program runs"
"$WINE" Tick.exe /run "$vendor" COM1 20 300 &
sleep 5
"$WINE" tickctl.exe close
sleep 25
echo "D fakevendor.log:"; cat fakevendor.log
"$WINESERVER" -k
EOS
timeout 400 xvfb-run -a -s "-screen 0 1024x768x24" sh "$scratch/safety-$ver.sh" 2>&1 \
    | grep -vE "^[0-9a-f]{4}:|libEGL" | tee "$out/run.log"
for d in "$t"/TickSessions/*/; do
    echo "-- $d"
    python3 -I "$here/tests/check_session.py" "$d" || echo "CHECK FAILED"
    grep -E '"kind":"(status|error)"' "$d/events.jsonl" | cut -c1-160
done
