#!/bin/sh
# The Linux demo-instrument on this computer; Tick + DemoMeter under Wine (own prefix): once over the network
# (127.0.0.1:5025), once over COM1, which Wine maps to demo-instrument's pseudo-terminal. Each Tick session must
# hold the dialogue (identification, settings, gain change, readings).
#   tests/wine-test-demo-linux.sh WINVER SCRATCH      (needs make all tools-test demo, build/demo/demo-instrument)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-winxp}; scratch=${2:?scratch folder}
WINE=${WINE:-$HOME/opt/wine-11.20-amd64-wow64/bin/wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX="$scratch/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$scratch"
[ -d "$WINEPREFIX/drive_c/windows" ] || (unset DISPLAY; "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w)
"$here/build/demo/demo-instrument" > "$scratch/instrument.log" 2>&1 &
inst=$!
trap 'kill $inst 2>/dev/null' EXIT
sleep 1
pts=$(sed -n 's/.*pseudo-terminal \(\/dev\/pts\/[0-9]*\).*/\1/p' "$scratch/instrument.log" | head -1)
echo "instrument pid $inst, serial line $pts"
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
"$WINE" reg add 'HKLM\Software\Wine\Ports' /v COM1 /d "$pts" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
t="$WINEPREFIX/drive_c/demo"; rm -rf "$t"; mkdir -p "$t"
cp "$here/build/Tick.exe" "$here/build/tickctl.exe" "$here/build/demo/DemoMeter.exe" "$t/"
for mode in net com; do
    args="/addr=127.0.0.1 /connect /gain4"; [ $mode = com ] && args="/com1 /connect /gain4"
    cat > "$scratch/run-$mode.sh" <<EOS
cd "$t"
"$WINE" Tick.exe /run "C:\\\\demo\\\\DemoMeter.exe" $args &
sleep 12
import -window root "$scratch/screen-$mode.png" 2>/dev/null || true
"$WINE" tickctl.exe stop 2>/dev/null; sleep 2
"$WINE" tickctl.exe quit 2>/dev/null; sleep 2
"$WINESERVER" -k
EOS
    timeout 200 xvfb-run -a -s "-screen 0 1024x768x24" sh "$scratch/run-$mode.sh" > "$scratch/run-$mode.log" 2>&1 || true
    ev=$(ls -d "$t"/TickSessions/*/ | tail -1)events.jsonl
    echo "== $mode: $ev"
    python3 -I - "$ev" <<'PY'
import json,sys
ev=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
calls=sorted({e.get('call') for e in ev if e.get('kind')=='data'})
txt=[bytes.fromhex(e['hex']).decode('latin1').strip() for e in ev if e.get('kind')=='data' and e.get('hex')]
cfg=[e for e in ev if e.get('kind')=='config']
print('calls:', calls, '| config:', cfg[0] if cfg else None)
print('first:', txt[:6], '| GAIN 4:', any(t.startswith('GAIN 4') for t in txt), '| readings:', sum(t.startswith('M ') for t in txt))
ok = any(t.startswith('*IDN?') for t in txt) and any(t.startswith('GAIN 4') for t in txt) and sum(t.startswith('M ') for t in txt) >= 5
print('PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
PY
done
