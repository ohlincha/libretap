#!/bin/sh
# DemoMeter + DemoInstrument under Tick in Wine: DemoInstrument answers on the network, Tick starts DemoMeter
# (/run), which connects, polls and changes the gain; the session must hold the network dialogue.
#   tests/wine-test-demo.sh WINVER SCRATCH      (own Wine prefix in SCRATCH; needs make all tools-test demo)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-winxp}; scratch=${2:?scratch folder}
WINE=${WINE:-$HOME/opt/wine-11.20-amd64-wow64/bin/wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX="$scratch/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$scratch"
[ -d "$WINEPREFIX/drive_c/windows" ] || (unset DISPLAY; "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w)
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
t="$WINEPREFIX/drive_c/demo"; rm -rf "$t"; mkdir -p "$t"
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$here/build/demo/DemoMeter.exe" "$here/build/demo/DemoInstrument.exe" "$t/"
cat > "$scratch/run.sh" <<EOS
cd "$t"
"$WINE" DemoInstrument.exe > "$scratch/instrument.log" 2>&1 &
sleep 4
"$WINE" Tick.exe /run "C:\\\\demo\\\\DemoMeter.exe" /connect /gain4 &
sleep 12
import -window root "$scratch/screen.png" 2>/dev/null || true
"$WINE" tickctl.exe stop 2>/dev/null; sleep 2
"$WINE" tickctl.exe quit 2>/dev/null; sleep 2
"$WINESERVER" -k
EOS
timeout 200 xvfb-run -a -s "-screen 0 1024x768x24" sh "$scratch/run.sh" > "$scratch/run.log" 2>&1 || true
ev=$(ls -d "$t"/TickSessions/*/ | tail -1)events.jsonl
echo "session: $ev"
grep -c '"kind":"data"' "$ev" | sed 's/^/data events: /'
grep -o '"call":"[a-z]*"' "$ev" | sort | uniq -c
python3 -I - "$ev" <<'PY'
import json,sys
ev=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
txt=[bytes.fromhex(e['hex']).decode('latin1').strip() for e in ev if e.get('kind')=='data' and e.get('hex')]
print('first:', txt[:6]); print('has GAIN 4:', any(t.startswith('GAIN 4') for t in txt)); print('readings:', sum(t.startswith('M ') for t in txt))
ok = any(t.startswith('*IDN?') for t in txt) and any(t.startswith('GAIN 4') for t in txt) and sum(t.startswith('M ') for t in txt) >= 5
sys.exit(0 if ok else 1)
PY
